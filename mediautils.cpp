#include "mediautils.h"
#include "videoencoder.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <cmath>

ProcessResult runProcess(const QString &program, const QStringList &arguments,
    const std::atomic_bool *cancel, int timeoutMs,
    const std::function<void(const QByteArray &, const QByteArray &)> &onData)
{
    ProcessResult result;
    if (cancel && cancel->load()) { result.cancelled = true; return result; }
    QProcess process;
    bool started = false;
    QObject::connect(&process, &QProcess::started, &process, [&]() { started = true; });
    QElapsedTimer elapsed;
    elapsed.start();
    process.start(program, arguments, QIODevice::ReadOnly);
    while (process.state() == QProcess::Starting) {
        process.waitForStarted(50);
        if ((cancel && cancel->load()) || elapsed.elapsed() > 10000) break;
    }
    // A very short process may already have exited successfully by this point.
    if (!started) {
        result.error = process.errorString();
        if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(); }
        result.cancelled = cancel && cancel->load();
        return result;
    }
    auto drain = [&]() {
        QByteArray out = process.readAllStandardOutput();
        QByteArray err = process.readAllStandardError();
        result.output += out;
        result.diagnostics += err;
        // Keep diagnostics bounded even for long jobs with repeated warnings.
        if (result.diagnostics.size() > 256 * 1024) result.diagnostics = result.diagnostics.right(256 * 1024);
        if (onData) onData(out, err);
    };
    while (process.state() != QProcess::NotRunning) {
        if (cancel && cancel->load()) {
            result.cancelled = true;
            process.kill();
            process.waitForFinished();
            break;
        }
        if (timeoutMs > 0 && elapsed.elapsed() > timeoutMs) {
            result.error = "Process timed out: " + program;
            process.kill();
            process.waitForFinished();
            break;
        }
        process.waitForFinished(100);
        drain();
        if (onData) result.output.clear(); // Progress is consumed by the caller.
    }
    drain();
    result.ok = !result.cancelled && result.error.isEmpty()
        && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    if (!result.ok && result.error.isEmpty() && !result.cancelled)
        result.error = QString("%1 failed (exit %2): %3").arg(QFileInfo(program).fileName())
            .arg(process.exitCode()).arg(QString::fromUtf8(result.diagnostics.right(4000)));
    return result;
}

QString findDependency(const QString &name)
{
    QString path = QStandardPaths::findExecutable(name);
    if (!path.isEmpty()) return path;
    QString filename = name;
#ifdef Q_OS_WIN
    filename += ".exe";
#endif
    QString local = QDir(QCoreApplication::applicationDirPath()).filePath(filename);
    if (QFileInfo(local).isFile() && QFileInfo(local).isExecutable()) return local;
#ifdef Q_OS_MAC
    local = QDir(QCoreApplication::applicationDirPath()).filePath("../Resources/" + filename);
    if (QFileInfo(local).isFile() && QFileInfo(local).isExecutable()) return local;
#endif
    return {};
}

QString computeFastHash(const QString &filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const qint64 size = file.size(), chunkSize = 1024 * 1024;
    hash.addData(QByteArray::number(size));
    for (qint64 offset : {qint64(0), qMax(qint64(0), size / 2 - chunkSize / 2), qMax(qint64(0), size - chunkSize)}) {
        if (!file.seek(offset)) return {};
        const QByteArray chunk = file.read(qMin(chunkSize, size - offset));
        if (file.error() != QFileDevice::NoError) return {};
        hash.addData(chunk);
    }
    return "v2:" + QString::fromLatin1(hash.result().toHex());
}

static double fraction(const QString &text, QChar separator, double fallback)
{
    const QStringList parts = text.split(separator);
    if (parts.size() == 2 && parts[1].toDouble() > 0 && parts[0].toDouble() > 0)
        return parts[0].toDouble() / parts[1].toDouble();
    return fallback;
}

VideoMetadata parseMetadata(const QJsonObject &json)
{
    VideoMetadata meta;
    meta.json = json;
    meta.duration = json.value("format").toObject().value("duration").toString().toDouble();
    bool selectedDefault = false;
    for (const auto &value : json.value("streams").toArray()) {
        const QJsonObject s = value.toObject();
        const QJsonObject disposition = s.value("disposition").toObject();
        MediaStream stream{s.value("index").toInt(-1), s.value("codec_type").toString(),
            s.value("codec_name").toString(), disposition.value("attached_pic").toInt() != 0};
        meta.streams.append(stream);
        if (stream.type == "audio") {
            if (!meta.hasAudio) meta.acodec = stream.codec;
            meta.hasAudio = true;
        }
        const bool isDefault = disposition.value("default").toInt() != 0;
        if (stream.type != "video" || stream.attachedPicture
            || (meta.videoIndex >= 0 && (selectedDefault || !isDefault))) continue;
        selectedDefault = isDefault;
        meta.videoIndex = stream.index;
        meta.vcodec = stream.codec;
        meta.width = s.value("width").toInt();
        meta.height = s.value("height").toInt();
        meta.fps = fraction(s.value("avg_frame_rate").toString(), '/', 0);
        meta.fieldOrder = s.value("field_order").toString();
        meta.displayAspectRatio = s.value("display_aspect_ratio").toString();
        meta.sampleAspectRatio = s.value("sample_aspect_ratio").toString();
        meta.colorTransfer = s.value("color_transfer").toString();
        meta.colorPrimaries = s.value("color_primaries").toString();
        meta.colorSpace = s.value("color_space").toString();
        meta.colorRange = s.value("color_range").toString();
        meta.hdr = meta.colorTransfer == "smpte2084" || meta.colorTransfer == "arib-std-b67";
        meta.rotation = 0;
        meta.dolbyVision = false;
        for (const auto &side : s.value("side_data_list").toArray()) {
            const QJsonObject data = side.toObject();
            if (data.contains("rotation")) meta.rotation = data.value("rotation").toDouble();
            if (data.value("side_data_type").toString().contains("DOVI")) meta.dolbyVision = true;
        }
    }
    meta.valid = meta.videoIndex >= 0 && meta.width > 0 && meta.height > 0
        && std::isfinite(meta.duration) && meta.duration > 0 && std::isfinite(meta.fps);
    if (!meta.valid) meta.error = "No usable video stream or duration was found.";
    return meta;
}

VideoMetadata probeMetadata(const QString &filepath, const QString &ffprobeBin, const std::atomic_bool *cancel)
{
    auto result = runProcess(ffprobeBin, {"-v", "error", "-show_format", "-show_streams",
        "-show_chapters", "-of", "json", filepath}, cancel);
    if (!result.ok) {
        VideoMetadata meta;
        meta.error = result.cancelled ? "Cancelled" : result.error;
        return meta;
    }
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(result.output, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        VideoMetadata meta;
        meta.error = "Invalid FFprobe response: " + error.errorString();
        return meta;
    }
    return parseMetadata(doc.object());
}

VideoGeometry videoGeometry(const VideoMetadata &meta, bool downscale)
{
    VideoGeometry result{meta.width, meta.height, false};
    double sar = fraction(meta.sampleAspectRatio, ':', 1.0);
    if (qAbs(sar - 1.0) < 0.005 && meta.width > 0)
        sar = fraction(meta.displayAspectRatio, ':', double(meta.width) / meta.height) * meta.height / meta.width;
    const int rotation = ((qRound(meta.rotation) % 360) + 360) % 360;
    if (rotation == 90 || rotation == 270) {
        qSwap(result.width, result.height);
        sar = 1.0 / sar;
    }
    if (qAbs(sar - 1.0) > 0.005) {
        result.width = qMax(2, qRound(result.width * sar) / 2 * 2);
        result.scale = true;
    }
    if (downscale) {
        const bool portrait = result.height > result.width;
        const double factor = qMin(double(portrait ? 1080 : 1920) / result.width,
            double(portrait ? 1920 : 1080) / result.height);
        if (factor < 1.0) {
            result.width = qMax(2, int(result.width * factor) / 2 * 2);
            result.height = qMax(2, int(result.height * factor) / 2 * 2);
            result.scale = true;
        }
    }
    return result;
}

bool isCompliant(const QString &filepath, const VideoMetadata &meta, const QVariantMap &settings)
{
    Q_UNUSED(settings);
    if (!meta.valid || QFileInfo(filepath).suffix().compare("mkv", Qt::CaseInsensitive) != 0) return false;
    // Preserve the original codec-only scan rule. Ordinary settings never
    // turn an already-compliant MKV into another encode request.
    bool hevc = false, aac = false;
    for (const auto &stream : meta.streams) {
        hevc |= stream.codec == "hevc";
        aac |= stream.codec == "aac";
    }
    return hevc && aac;
}

QString settingsSignature(const QVariantMap &settings)
{
    QJsonObject stable{{"version", 2}, {"crf", settings.value("crf", 28).toInt()},
        {"preset", settings.value("preset", "medium").toString()},
        {"downscale", settings.value("downscale", true).toBool()},
        {"debob", settings.value("debob", true).toBool()}};
    if (VideoEncoder::isNvenc(settings)) {
        stable.remove("crf");
        stable.remove("preset");
        stable.insert("encoder", "hevc_nvenc");
        stable.insert("nvenc_profile_version", 1);
    }
    if (settings.value("avs_enabled", false).toBool()) {
        stable.insert("avisynth_version", 1);
        stable.insert("avisynth_loader", "LWLibavVideoSource");
        stable.insert("avisynth_filters", settings.value("avs_filters").toString());
    }
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(stable).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}
