#ifndef TRANSCODEWORKER_H
#define TRANSCODEWORKER_H

#include <QThread>
#include <QStringList>
#include <QVariantMap>
#include <QProcess>
#include <atomic>

class TranscodeWorker : public QThread {
    Q_OBJECT
public:
    TranscodeWorker(const QStringList &fileQueue, const QString &rootDir, const QString &dbPath, const QVariantMap &settings, QObject *parent = nullptr);
    ~TranscodeWorker();

    void stop();

signals:
    void logSignal(const QString &message);
    void progressSignal(const QString &filepath, int percentage, double fps, double speed, const QString &etaStr, double outSizeMb, double projectedSizeMb);
    void statusSignal(const QString &filepath, const QString &status, const QString &details);
    void fileDoneSignal(const QString &filepath, const QString &status, qint64 oldSize, qint64 newSize);
    void finishedSignal();

protected:
    void run() override;

private:
    QStringList m_fileQueue;
    QString m_rootDir;
    QString m_dbPath;
    QVariantMap m_settings;
    std::atomic<bool> m_isRunning;
    QProcess *m_activeProcess;
    bool m_hasFdk;

    bool processFile(const QString &filepath, const QString &ffmpegBin, const QString &ffprobeBin, const QString &trashDir, const QString &errorDir);
    bool runFfmpegProcess(const QStringList &cmd, double duration, const QString &filepath);
    
    void moveToTrash(const QString &filepath, const QString &trashDir);
    void moveToErrors(const QString &filepath, const QString &errorDir);
};

struct VideoMetadata {
    QString vcodec;
    QString acodec;
    int width = 0;
    int height = 0;
    double duration = 0.0;
    double fps = 0.0;
    QString fieldOrder;
    bool hasAudio = false;
};

// Global helper methods matching Python utilities
QString findDependency(const QString &name);
QString computeFastHash(const QString &filepath);
bool probeFileCompliance(const QString &filepath, const QString &ffprobeBin);
VideoMetadata probeMetadata(const QString &filepath, const QString &ffprobeBin);

#endif // TRANSCODEWORKER_H
