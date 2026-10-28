#ifndef FILETRANSACTION_H
#define FILETRANSACTION_H

#include <QLockFile>
#include <QTemporaryFile>
#include <memory>

QString normalizedPath(const QString &path);
bool isJobArtifact(const QString &filename);
QString outputPathFor(const QString &source);

// Owns a unique temporary file and a destination lock. Never overwrites a file.
class FileTransaction {
public:
    FileTransaction(const QString &source, const QString &root);
    ~FileTransaction();
    bool prepare(QString &error);
    QString temporaryPath() const;
    QString finalPath() const { return m_destination; }
    QString backupPath() const { return m_backup; }
    bool commit(QString &error);

private:
    QString m_source, m_root, m_destination, m_backup, m_temporary;
    bool m_keepTemporary = false;
    std::unique_ptr<QLockFile> m_lock;
};

#endif
