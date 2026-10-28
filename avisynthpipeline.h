#ifndef AVISYNTHPIPELINE_H
#define AVISYNTHPIPELINE_H

#include "mediautils.h"
#include <QLockFile>
#include <QTemporaryDir>
#include <memory>

struct AvisynthProfile {
    QString name, filters;
};

// A prepared script is shared by validation, preview, sample and final encoding.
// The runtime and all plugins are supplied through the user's AviSynth autoload setup.
class AvisynthPipeline {
public:
    bool prepare(const QString &source, const VideoMetadata &metadata, const AvisynthProfile &profile,
        const QString &ffmpeg, const QString &ffprobe, int threads, const std::atomic_bool *cancel,
        QString &error, const QString &cacheRoot = {});
    QString scriptPath() const;
    QString originalScriptPath() const;
    QString directory() const { return m_temporary.path(); }
    VideoMetadata filtered;
    QString videoFilter(bool restoreSourceTiming = false) const;
    QStringList colorArguments() const { return m_colorArguments; }
    QString timingDescription() const { return m_timingDescription; }
    QString scriptError() const;
    QString validateEncodedOutput(const QString &output, const QString &ffprobe, const std::atomic_bool *cancel) const;
    static QString quote(const QString &value);
    static QString testInstallation(const QString &ffmpeg, const QString &ffprobe, const std::atomic_bool *cancel);
    static QString validateTimeline(const QList<double> &timestamps, double fps);
private:
    QTemporaryDir m_temporary;
    std::unique_ptr<QLockFile> m_cacheLock;
    QString m_sar = "1/1";
    qint64 m_frames = 0;
    double m_videoStart = 0;
    QString m_timingDescription;
    QStringList m_colorArguments;
};

#endif
