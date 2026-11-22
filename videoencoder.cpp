#include "videoencoder.h"
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStandardPaths>

namespace VideoEncoder {
bool isNvenc(const QVariantMap &settings)
{
    return settings.value("nvenc", false).toBool();
}

QString name(const QVariantMap &settings)
{
    return isNvenc(settings) ? "hevc_nvenc" : "libx265";
}

QString pixelFormat(const QVariantMap &settings)
{
    return isNvenc(settings) ? "p010le" : "yuv420p10le";
}

QStringList arguments(const QVariantMap &settings)
{
    if (isNvenc(settings)) {
        // Fixed profile selected by the DVD comparison benchmark. NVENC CQ
        // and CPU CRF are different controls; CPU settings do not affect this.
        return {"-c:v:0", "hevc_nvenc", "-profile:v:0", "main10", "-preset:v:0", "p5",
            "-tune:v:0", "hq", "-rc:v:0", "vbr", "-cq:v:0", "35.25", "-b:v:0", "0",
            "-g:v:0", QString::number(NvencGopFrames), "-bf:v:0", "4", "-b_ref_mode:v:0", "middle",
            "-rc-lookahead:v:0", "20", "-spatial-aq:v:0", "1", "-temporal-aq:v:0", "1",
            "-aq-strength:v:0", "8", "-multipass:v:0", "qres"};
    }
    return {"-c:v:0", "libx265", "-profile:v:0", "main10", "-preset:v:0", settings.value("preset", "medium").toString(),
        "-crf:v:0", QString::number(settings.value("crf", 28).toInt()), "-x265-params:v:0",
        QString("no-sao=1:selective-sao=0:aq-mode=1:pmode=1:pme=1:pools=%1").arg(qMax(1, settings.value("threads", 1).toInt()))};
}

ProcessResult capabilities(const QString &ffmpeg, const std::atomic_bool *cancel)
{
    if (cancel && cancel->load()) { ProcessResult result; result.cancelled = true; return result; }
    static QMutex mutex;
    static QHash<QString, ProcessResult> cache;
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
        if (cache.contains(key)) return cache.value(key);
    }
    const auto result = runProcess(ffmpeg, {"-hide_banner", "-encoders"}, cancel, 15000);
    if (result.ok) {
        QMutexLocker lock(&mutex);
        cache.insert(key, result);
    }
    return result;
}

QString validate(const QString &ffmpeg, const QVariantMap &settings, const std::atomic_bool *cancel)
{
    const auto encoders = capabilities(ffmpeg, cancel);
    if (!encoders.ok) return encoders.cancelled ? "Cancelled" : "Cannot check FFmpeg video encoders: " + encoders.error;
    const QString encoder = name(settings);
    const QRegularExpression listed("(?:^|\\n)\\s*[A-Z.]+\\s+" + QRegularExpression::escape(encoder) + "(?:\\s|$)");
    if (!listed.match(QString::fromUtf8(encoders.output)).hasMatch())
        return "FFmpeg does not provide " + encoder + "; original retained.";
    if (!isNvenc(settings)) return {};

    // Listing an encoder does not prove that a compatible GPU and driver are
    // present. Initialize the complete fixed profile before touching outputs.
    QStringList probe{"-v", "error", "-nostdin", "-threads", "1", "-filter_threads", "1",
        "-f", "lavfi", "-i", "color=c=black:s=256x256:r=30",
        "-frames:v", "1", "-an", "-vf", "format=" + pixelFormat(settings)};
    probe << arguments(settings) << "-f" << "null" << "-";
    const auto result = runProcess(ffmpeg, probe, cancel, 15000);
    if (!result.ok) return result.cancelled ? "Cancelled"
        : "The fixed NVIDIA NVENC profile (P5 / CQ 35.25 / Main10) is unavailable with the selected FFmpeg, GPU, or driver; original retained.\n"
            + result.error;
    return {};
}
}
