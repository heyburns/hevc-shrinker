#ifndef TRANSCODEWORKER_H
#define TRANSCODEWORKER_H

#include "mediautils.h"
#include <QThread>

class TranscodeWorker : public QThread {
    Q_OBJECT
public:
    TranscodeWorker(const QStringList &fileQueue, const QString &rootDir, const QString &dbPath,
        const QVariantMap &settings, QObject *parent = nullptr);
    ~TranscodeWorker() override;
    void stop();
    void setLivePreviewEnabled(bool enabled);

signals:
    void logSignal(const QString &message);
    void progressSignal(const QString &filepath, int percentage, double fps, double speed,
        const QString &etaStr, double outSizeMb, double projectedSizeMb);
    void statusSignal(const QString &filepath, const QString &status, const QString &details);
    void fileDoneSignal(const QString &filepath, const QString &status, qint64 oldSize, qint64 newSize);
    void previewFrameSignal(const QString &filepath, double secs);
    void encodedPreviewSignal(const QString &filepath, const QByteArray &png, double secs);

protected:
    void run() override;

private:
    friend class RegressionTests;
    QStringList m_fileQueue;
    QString m_rootDir, m_dbPath;
    QVariantMap m_settings;
    std::atomic_bool m_cancelled{false}, m_livePreviewEnabled{false};
    bool processFile(const QString &filepath, const QString &ffmpeg, const QString &ffprobe);
    ProcessResult encode(const QString &ffmpeg, const QStringList &arguments,
        double duration, const QString &filepath, const QString &output, double outputFps = 0);
    void finishFile(const QString &filepath, const QString &status, const QString &details,
        qint64 oldSize, qint64 newSize);
};
#endif
