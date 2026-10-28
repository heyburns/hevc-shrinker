#include "scanworker.h"
#include "filetransaction.h"
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QSet>

ScanWorker::ScanWorker(const QString &root, const QString &dbPath, const QVariantMap &settings, QObject *parent)
    : QThread(parent), m_root(root), m_dbPath(dbPath), m_settings(settings)
{}

ScanWorker::~ScanWorker() { stop(); wait(); }

void ScanWorker::run()
{
    DatabaseManager db(m_dbPath);
    if (!db.isReady()) { error = "Cannot open processing history: " + db.lastError(); return; }
    if (!db.migrateLocalDatabase(QDir(m_root).filePath("processed_files.db"), m_root))
        emit logSignal("Legacy history migration failed; the database was retained: " + db.lastError());
    const QString ffprobe = m_settings.value("ffprobe", findDependency("ffprobe")).toString();
    if (ffprobe.isEmpty()) { error = "FFprobe is missing; scanning cannot continue."; return; }
    const QStringList extensions{"mp4", "mkv", "wmv", "avi", "mov", "flv", "mpeg", "mpg", "vid", "m4v", "asf", "f4v", "divx"};
    QStringList directories{m_root};
    QSet<QString> visited;
    while (!directories.isEmpty() && !m_cancelled) {
        const QString path = directories.takeLast();
        QString canonical = QDir(path).canonicalPath();
#ifdef Q_OS_WIN
        canonical = canonical.toCaseFolded();
#endif
        if (canonical.isEmpty() || visited.contains(canonical)) continue;
        visited.insert(canonical);
        const auto entries = QDir(path).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const auto &entry : entries) {
            if (m_cancelled) break;
            if (entry.isSymLink() || entry.isJunction()) continue;
            if (entry.isDir()) {
                if (entry.fileName().compare(".Trash", Qt::CaseInsensitive) != 0
                    && entry.fileName().compare(".Errors", Qt::CaseInsensitive) != 0)
                    directories.append(entry.absoluteFilePath());
                continue;
            }
            if (!extensions.contains(entry.suffix().toLower()) || isJobArtifact(entry.fileName())) continue;
            ScannedFile file;
            file.absolutePath = entry.absoluteFilePath();
            file.filename = entry.fileName();
            file.relPath = QDir(m_root).relativeFilePath(file.absolutePath);
            file.size = entry.size();
            file.lastModified = entry.lastModified().toMSecsSinceEpoch();
            file.history = db.getProcessedFileInfo(file.absolutePath);
            // Processing history is authoritative, as in the original app.
            // Legacy records describe the input hash, and changing settings
            // or turning off an old AVS profile must never requeue an output.
            if (file.history.found) {
                file.status = "Completed";
                file.detail = "Already processed (in DB)";
            } else {
                const auto cached = db.cachedMetadata(file.absolutePath, file.size, file.lastModified);
                if (!cached.isEmpty()) file.metadata = parseMetadata(QJsonDocument::fromJson(cached).object());
                if (!file.metadata.valid) {
                    file.metadata = probeMetadata(file.absolutePath, ffprobe, &m_cancelled);
                    if (file.metadata.valid && !m_cancelled && !db.cacheMetadata(file.absolutePath, file.size,
                        file.lastModified, QJsonDocument(file.metadata.json).toJson(QJsonDocument::Compact)))
                        emit logSignal("Metadata cache write failed: " + db.lastError());
                }
                if (m_cancelled) break;
                if (!file.metadata.valid) {
                    file.status = "Failed";
                    file.detail = file.metadata.error;
                } else if (isCompliant(file.absolutePath, file.metadata, m_settings)) {
                    file.status = "Skipped";
                    file.detail = "Already compliant (HEVC+AAC)";
                }
            }
            files.append(file);
        }
    }
    // Detect shared destinations before dispatch, including another input that
    // is already compliant. QLockFile additionally protects separate instances.
    QHash<QString, QList<int>> destinations;
    for (int i = 0; i < files.size(); ++i) destinations[normalizedPath(outputPathFor(files[i].absolutePath))].append(i);
    for (auto it = destinations.cbegin(); it != destinations.cend(); ++it) {
        for (int index : it.value()) {
            auto &file = files[index];
            if (file.status != "Pending") continue;
            const QString destination = outputPathFor(file.absolutePath);
            if (it.value().size() > 1 || (normalizedPath(destination) != normalizedPath(file.absolutePath)
                && (QFileInfo::exists(destination) || QFileInfo(destination).isSymLink()))) {
                file.status = "Failed";
                file.detail = "Output name collision; originals retained. Rename the conflicting files before rescanning.";
            }
        }
    }
}
