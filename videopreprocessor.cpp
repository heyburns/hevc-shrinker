#include "videopreprocessor.h"
#include "videoencoder.h"
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>

namespace {
bool hasCudaScale(const QString &ffmpeg, const std::atomic_bool *cancel)
{
    if (cancel && cancel->load()) return false;
    static QMutex mutex;
    static QHash<QString, bool> cache;
    QFileInfo executable(ffmpeg);
    if (!executable.isAbsolute()) {
        const QString resolved = QStandardPaths::findExecutable(ffmpeg);
        if (!resolved.isEmpty()) executable.setFile(resolved);
    }
    const QString canonical = executable.canonicalFilePath();
    const QString key = (canonical.isEmpty() ? executable.absoluteFilePath() : canonical)
        + ':' + QString::number(executable.size()) + ':'
        + QString::number(executable.lastModified().toMSecsSinceEpoch());
    {
        QMutexLocker lock(&mutex);
        const auto found = cache.constFind(key);
        if (found != cache.cend()) return found.value();
    }
    const auto result = runProcess(ffmpeg, {"-hide_banner", "-filters"}, cancel, 15000);
    if (!result.ok) return false;
    static const QRegularExpression listed("(?:^|\\n)\\s*[A-Z.]+\\s+scale_cuda(?:\\s|$)");
    const bool supported = listed.match(QString::fromUtf8(result.output + result.diagnostics)).hasMatch();
    {
        QMutexLocker lock(&mutex);
        cache.insert(key, supported);
    }
    return supported;
}
}

namespace VideoPreprocessor {
Plan plan(const QString &ffmpeg, const QVariantMap &settings,
    const VideoMetadata &meta, const VideoGeometry &geometry,
    bool interlaced, bool halveFps, const std::atomic_bool *cancel)
{
    auto cpu = [](const QString &reason) {
        Plan result;
        result.reason = reason;
        return result;
    };
    if (!VideoEncoder::isNvenc(settings)) return cpu("CPU encoder selected.");
    if (settings.value("avs_enabled", false).toBool()) return cpu("AviSynth provides CPU-filtered frames.");
    if (cancel && cancel->load()) return cpu("Cancelled.");
    if (!meta.valid) return cpu("Source metadata is incomplete.");
    if (interlaced) return cpu("Deinterlacing uses CPU preprocessing.");
    if (!std::isfinite(meta.rotation) || qAbs(meta.rotation) > 0.01)
        return cpu("Display rotation uses CPU preprocessing.");
    if (meta.hdr || meta.dolbyVision) return cpu("HDR/Dolby Vision is not eligible for GPU preprocessing.");
    if (!QStringList{"h264", "hevc", "mpeg2video", "vp9", "av1"}.contains(meta.vcodec))
        return cpu("Source codec is not eligible for NVDEC.");
    if (meta.width <= 0 || meta.height <= 0 || (meta.width % 2) || (meta.height % 2)
        || geometry.width <= 0 || geometry.height <= 0 || (geometry.width % 2) || (geometry.height % 2))
        return cpu("Source or output dimensions are not eligible for GPU preprocessing.");
    if (halveFps && (!std::isfinite(meta.fps) || meta.fps <= 0))
        return cpu("Source frame rate is unavailable.");

    QString pixelFormat;
    for (const auto &value : meta.json.value("streams").toArray()) {
        const auto stream = value.toObject();
        if (stream.value("index").toInt(-1) == meta.videoIndex) {
            pixelFormat = stream.value("pix_fmt").toString();
            break;
        }
    }
    // Restrict the first GPU path to conventional 4:2:0 inputs. Other chroma,
    // RGB, and higher-bit-depth sources keep the established software path.
    if (!QStringList{"yuv420p", "yuv420p10le", "nv12", "p010le"}.contains(pixelFormat))
        return cpu("Source pixel format uses CPU preprocessing.");

    int ordinal = 0;
    bool selected = false;
    for (const auto &stream : meta.streams) {
        if (stream.type != "video") continue;
        if (stream.index == meta.videoIndex) {
            selected = !stream.attachedPicture;
            break;
        }
        ++ordinal;
    }
    if (!selected) return cpu("Selected video stream is unavailable.");
    if (!hasCudaScale(ffmpeg, cancel)) return cpu("Selected FFmpeg does not provide scale_cuda.");
    if (cancel && cancel->load()) return cpu("Cancelled.");

    Plan result;
    result.accelerated = true;
    const QString streamSpecifier = ":v:" + QString::number(ordinal);
    // Stream-specific input options leave copied covers and secondary video
    // streams alone. One decoder thread avoids needless software thread pools.
    result.inputOptions = {"-hwaccel" + streamSpecifier, "cuda",
        "-hwaccel_output_format" + streamSpecifier, "cuda",
        "-threads" + streamSpecifier, "1", "-extra_hw_frames" + streamSpecifier, "8"};
    // Force a separate CUDA frame pool even without resizing. This also handles
    // NV12 -> P010 conversion while releasing decoder surfaces to avoid stalls.
    result.filters << QString("scale_cuda=w=%1:h=%2:interp_algo=lanczos:format=p010le:passthrough=0")
        .arg(geometry.width).arg(geometry.height);
    if (geometry.scale) result.filters << "setsar=1";
    // fps changes frame references/timestamps only; pixels stay on the GPU.
    if (halveFps) result.filters << QString("fps=fps=%1:round=down").arg(meta.fps / 2.0, 0, 'f', 8);
    result.reason = "NVDEC decoding and CUDA preprocessing.";
    return result;
}
}
