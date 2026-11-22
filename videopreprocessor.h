#ifndef VIDEOPREPROCESSOR_H
#define VIDEOPREPROCESSOR_H

#include "mediautils.h"

namespace VideoPreprocessor {
struct Plan {
    bool accelerated = false;
    QStringList inputOptions;
    QStringList filters;
    QString reason;
};

// Select a fully GPU-resident ordinary-video path when it is applicable. The
// caller retains its existing CPU path and can retry it if this GPU path fails.
Plan plan(const QString &ffmpeg, const QVariantMap &settings,
    const VideoMetadata &meta, const VideoGeometry &geometry,
    bool interlaced, bool halveFps, const std::atomic_bool *cancel = nullptr);
}

#endif
