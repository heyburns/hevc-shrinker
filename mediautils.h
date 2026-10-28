#ifndef MEDIAUTILS_H
#define MEDIAUTILS_H

#include <QJsonObject>
#include <QProcess>
#include <QVariantMap>
#include <atomic>
#include <functional>

struct ProcessResult {
    bool ok = false;
    bool cancelled = false;
    QByteArray output, diagnostics;
    QString error;
};
ProcessResult runProcess(const QString &program, const QStringList &arguments,
    const std::atomic_bool *cancel = nullptr, int timeoutMs = 60000,
    const std::function<void(const QByteArray &, const QByteArray &)> &onData = {});

struct MediaStream {
    int index = -1;
    QString type, codec;
    bool attachedPicture = false;
};

struct VideoMetadata {
    bool valid = false;
    QString error;
    int videoIndex = -1;
    QString vcodec, acodec;
    int width = 0, height = 0;
    double duration = 0.0, fps = 0.0, rotation = 0.0;
    QString fieldOrder, displayAspectRatio, sampleAspectRatio;
    QString colorTransfer, colorPrimaries, colorSpace, colorRange;
    bool hasAudio = false, hdr = false, dolbyVision = false;
    QList<MediaStream> streams;
    QJsonObject json;
};

struct VideoGeometry {
    int width = 0, height = 0;
    bool scale = false;
};

QString findDependency(const QString &name);
QString computeFastHash(const QString &filepath);
VideoMetadata parseMetadata(const QJsonObject &json);
VideoMetadata probeMetadata(const QString &filepath, const QString &ffprobeBin,
    const std::atomic_bool *cancel = nullptr);
VideoGeometry videoGeometry(const VideoMetadata &meta, bool downscale);
bool isCompliant(const QString &filepath, const VideoMetadata &meta, const QVariantMap &settings);
QString settingsSignature(const QVariantMap &settings);

#endif
