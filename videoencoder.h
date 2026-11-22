#ifndef VIDEOENCODER_H
#define VIDEOENCODER_H

#include "mediautils.h"

// Normal encodes and AviSynth samples share the same video encoder settings.
namespace VideoEncoder {
inline constexpr int NvencGopFrames = 250;

bool isNvenc(const QVariantMap &settings);
QString name(const QVariantMap &settings);
QString pixelFormat(const QVariantMap &settings);
QStringList arguments(const QVariantMap &settings);
ProcessResult capabilities(const QString &ffmpeg, const std::atomic_bool *cancel = nullptr);
QString validate(const QString &ffmpeg, const QVariantMap &settings,
    const std::atomic_bool *cancel = nullptr);
}

#endif
