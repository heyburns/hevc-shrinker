#include "filetransaction.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

QString normalizedPath(const QString &path)
{
    QFileInfo info(path);
    QString parent = info.dir().canonicalPath();
    if (parent.isEmpty()) parent = info.absolutePath();
    QString result = QDir::cleanPath(QDir(parent).filePath(info.fileName()));
#ifdef Q_OS_WIN
    result = result.toCaseFolded();
#endif
    return result;
}

bool isJobArtifact(const QString &filename)
{
    return filename.startsWith(".hevc-shrinker-", Qt::CaseInsensitive)
        || filename.endsWith(".tmp_out.mkv", Qt::CaseInsensitive);
}

QString outputPathFor(const QString &source)
{
    QFileInfo info(source);
    // Preserve the spelling of an existing MKV (including .MKV on Linux).
    return info.suffix().compare("mkv", Qt::CaseInsensitive) == 0
        ? info.absoluteFilePath() : info.dir().filePath(info.completeBaseName() + ".mkv");
}

FileTransaction::FileTransaction(const QString &source, const QString &root)
    : m_source(QFileInfo(source).absoluteFilePath()), m_root(root),
      m_destination(outputPathFor(source))
{}

FileTransaction::~FileTransaction()
{
    if (!m_keepTemporary && !m_temporary.isEmpty()) QFile::remove(m_temporary);
}

bool FileTransaction::prepare(QString &error)
{
    QFileInfo source(m_source);
    if (!source.isFile() || source.isSymLink() || isJobArtifact(source.fileName())) {
        error = "Source is missing, a symbolic link, or a temporary output.";
        return false;
    }
    const QString key = QString::fromLatin1(QCryptographicHash::hash(
        normalizedPath(m_destination).toUtf8(), QCryptographicHash::Sha256).toHex());
    m_lock = std::make_unique<QLockFile>(source.dir().filePath(".hevc-shrinker-" + key + ".lock"));
    // Long encodes must not be considered stale because of their age.
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock(0)) {
        error = "Another job is using this output path, or its lock cannot be created.";
        return false;
    }
    QFileInfo destination(m_destination);
    if ((destination.exists() || destination.isSymLink())
        && normalizedPath(m_source) != normalizedPath(m_destination)) {
        error = "Output already exists; both files were left unchanged: " + m_destination;
        return false;
    }
    QTemporaryFile temporary(source.dir().filePath(".hevc-shrinker-XXXXXX.mkv"));
    if (!temporary.open()) {
        error = "Cannot create temporary output: " + temporary.errorString();
        return false;
    }
    m_temporary = temporary.fileName();
    temporary.setAutoRemove(false);
    // Destroy the QTemporaryFile here. close() alone can retain its native
    // handle on Windows, blocking FFmpeg and the eventual rename.
    return true;
}

QString FileTransaction::temporaryPath() const
{
    return m_temporary;
}

bool FileTransaction::commit(QString &error)
{
    if (m_temporary.isEmpty() || QFileInfo(temporaryPath()).size() <= 0) {
        error = "Temporary output is missing or empty; original retained.";
        return false;
    }
    const QString relative = QDir(m_root).relativeFilePath(m_source);
    if (QDir::isAbsolutePath(relative) || relative == ".." || relative.startsWith("../")) {
        error = "Source is outside the workspace; original retained.";
        return false;
    }
    const QString backupDir = QFileInfo(QDir(m_root).filePath(".Trash/" + relative)).absolutePath();
    if (!QDir().mkpath(backupDir)) {
        error = "Cannot create the original backup directory; original retained.";
        return false;
    }
    const QFileInfo source(m_source);
    m_backup = QDir(backupDir).filePath(source.fileName());
    while (QFileInfo::exists(m_backup) || QFileInfo(m_backup).isSymLink()) {
        m_backup = QDir(backupDir).filePath(source.completeBaseName() + "_"
            + QUuid::createUuid().toString(QUuid::WithoutBraces) + "." + source.suffix());
    }
    const bool samePath = normalizedPath(m_source) == normalizedPath(m_destination);
    if (!samePath && (QFileInfo::exists(m_destination) || QFileInfo(m_destination).isSymLink())) {
        error = "Output appeared while encoding; original retained.";
        return false;
    }

    // Leave a recovery record before moving anything. A crash may leave the
    // original in .Trash, but never destroys the only original or encoded copy.
    const QString journalPath = temporaryPath() + ".recovery.json";
    QSaveFile journal(journalPath);
    const QByteArray json = QJsonDocument(QJsonObject{{"source", m_source},
        {"backup", m_backup}, {"temporary", temporaryPath()}, {"output", m_destination}}).toJson();
    if (!journal.open(QIODevice::WriteOnly) || journal.write(json) != json.size() || !journal.commit()) {
        error = "Cannot save recovery information; original retained.";
        return false;
    }
    m_keepTemporary = true;
    // QFile::rename refuses an existing destination on both Windows and Linux.
    // Qt also handles cross-volume moves; failure never triggers deletion here.
    if (!QFile::rename(m_source, m_backup)) {
        error = "Cannot move original to .Trash; original retained.";
        QFile::remove(journalPath);
        m_keepTemporary = false;
        return false;
    }
    if (!QFile::rename(temporaryPath(), m_destination)) {
        if (QFile::rename(m_backup, m_source)) {
            error = "Cannot publish output; original restored.";
            QFile::remove(journalPath);
            m_keepTemporary = false;
        } else {
            error = "Cannot publish output or restore its original path. Original is safe at "
                + m_backup + "; recovery record: " + journalPath;
        }
        return false;
    }
    QFile::remove(journalPath);
    return true;
}
