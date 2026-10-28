#ifndef SCANWORKER_H
#define SCANWORKER_H

#include "databasemanager.h"
#include "mediautils.h"
#include <QThread>

struct ScannedFile {
    QString absolutePath, filename, relPath;
    qint64 size = 0, lastModified = 0;
    QString status = "Pending", detail = "Waiting...";
    ProcessedFileInfo history;
    VideoMetadata metadata;
};

class ScanWorker : public QThread {
    Q_OBJECT
public:
    ScanWorker(const QString &root, const QString &dbPath, const QVariantMap &settings, QObject *parent = nullptr);
    ~ScanWorker() override;
    void stop() { m_cancelled = true; }
    bool wasCancelled() const { return m_cancelled; }
    // Read these only after QThread::finished/wait().
    QList<ScannedFile> files;
    QString error;
signals:
    void logSignal(const QString &message);
protected:
    void run() override;
private:
    QString m_root, m_dbPath;
    QVariantMap m_settings;
    std::atomic_bool m_cancelled{false};
};

#endif
