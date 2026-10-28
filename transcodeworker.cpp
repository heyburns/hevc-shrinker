#include "transcodeworker.h"
#include "databasemanager.h"
#include "filetransaction.h"
#include "avisynthpipeline.h"
#include "videoencoder.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>
#include <cmath>
#include <limits>

namespace {
enum class Interlace { Unknown, Progressive, Interlaced };

Interlace probeInterlaced(const QString &file, const QString &ffmpeg, const VideoMetadata &meta, const std::atomic_bool *cancel)
{
    // Sample long videos beyond opening credits without decoding more frames.
    const QList<double> positions = meta.duration >= 60.0 ? QList<double>{0.0, meta.duration / 3.0, meta.duration * 2.0 / 3.0}
                                                        : QList<double>{0.0};
    bool allProgressive = true;
    static const QRegularExpression pattern("Multi frame detection:\\s*TFF:\\s*(\\d+)\\s*BFF:\\s*(\\d+)\\s*Progressive:\\s*(\\d+)");
    for (double position : positions) {
        auto result = runProcess(ffmpeg, {"-nostdin", "-hide_banner", "-filter_threads", "4", "-ss", QString::number(position, 'f', 3), "-i", file,
            "-map", QString("0:%1").arg(meta.videoIndex), "-vf", "idet", "-frames:v", QString::number(360 / positions.size()),
            "-an", "-f", "null", "-"}, cancel);
        if (!result.ok) return Interlace::Unknown;
        auto matches = pattern.globalMatch(QString::fromUtf8(result.diagnostics));
        QRegularExpressionMatch match;
        while (matches.hasNext()) match = matches.next();
        if (!match.hasMatch()) return Interlace::Unknown;
        const int interlaced = match.captured(1).toInt() + match.captured(2).toInt();
        const int progressive = match.captured(3).toInt();
        if (interlaced + progressive < 20) { allProgressive = false; continue; }
        if (interlaced > progressive * 2) return Interlace::Interlaced;
        if (progressive <= interlaced * 2) allProgressive = false;
    }
    return allProgressive ? Interlace::Progressive : Interlace::Unknown;
}

int streamCount(const VideoMetadata &meta, const QString &type)
{
    int count = 0;
    for (const auto &stream : meta.streams) if (stream.type == type) ++count;
    return count;
}

QString validateOutput(const VideoMetadata &source, const VideoMetadata &output,
    bool videoEncoded, const VideoGeometry &geometry, bool fallback)
{
    if (!output.valid) return "Output probe failed: " + output.error;
    const double tolerance = source.fps > 0 ? qBound(0.25, 3.0 / source.fps, 2.0) : 0.5;
    if (qAbs(source.duration - output.duration) > tolerance)
        return "Output duration does not match the source; original retained.";
    for (const QString &type : {QString("video"), QString("audio"), QString("subtitle"), QString("attachment")})
        if (streamCount(source, type) != streamCount(output, type))
            return "Output " + type + " stream count does not match; original retained.";
    if (videoEncoded) {
        if (output.vcodec != "hevc" || output.width != geometry.width || output.height != geometry.height)
            return "Output codec or dimensions do not match the planned video.";
        if (qAbs(output.rotation) > 0.01) return "Unexpected output rotation; original retained.";
        if (geometry.scale && output.sampleAspectRatio != "1:1")
            return "Unexpected output pixel aspect ratio; original retained.";
    }
    else if (output.vcodec != source.vcodec || output.width != source.width || output.height != source.height)
        return "Copied video does not match the source; original retained.";
    if (!fallback) {
        for (const auto &stream : output.streams)
            if (stream.type == "audio" && stream.codec != "aac") return "Output audio conversion was incomplete.";
    }
    return {};
}
}

TranscodeWorker::TranscodeWorker(const QStringList &files, const QString &root, const QString &dbPath,
    const QVariantMap &settings, QObject *parent)
    : QThread(parent), m_fileQueue(files), m_rootDir(root), m_dbPath(dbPath), m_settings(settings)
{
    m_livePreviewEnabled = settings.value("live_preview", false).toBool();
}

TranscodeWorker::~TranscodeWorker()
{
    stop();
    wait();
}

void TranscodeWorker::stop() { m_cancelled = true; }
void TranscodeWorker::setLivePreviewEnabled(bool enabled) { m_livePreviewEnabled = enabled; }

void TranscodeWorker::finishFile(const QString &file, const QString &status, const QString &details,
    qint64 oldSize, qint64 newSize)
{
    emit logSignal(QString("[%1] %2: %3").arg(status, QFileInfo(file).fileName(), details));
    emit statusSignal(file, status, details);
    emit fileDoneSignal(file, status, oldSize, newSize);
}

void TranscodeWorker::run()
{
    const QString ffmpeg = m_settings.value("ffmpeg", findDependency("ffmpeg")).toString();
    const QString ffprobe = m_settings.value("ffprobe", findDependency("ffprobe")).toString();
    for (const auto &file : m_fileQueue) {
        if (m_cancelled) {
            finishFile(file, "Cancelled", "Cancelled; original retained.", QFileInfo(file).size(), 0);
            continue;
        }
        if (ffmpeg.isEmpty() || ffprobe.isEmpty()) {
            finishFile(file, "Failed", "FFmpeg or FFprobe is missing; original retained.", QFileInfo(file).size(), 0);
            continue;
        }
        try { processFile(file, ffmpeg, ffprobe); }
        catch (const std::exception &error) {
            finishFile(file, "Failed", QString::fromUtf8(error.what()), QFileInfo(file).size(), 0);
        }
    }
}

bool TranscodeWorker::processFile(const QString &file, const QString &ffmpeg, const QString &ffprobe)
{
    const QFileInfo source(file);
    const qint64 oldSize = source.size();
    const qint64 oldModified = source.lastModified().toMSecsSinceEpoch();
    auto fail = [&](const QString &error) {
        finishFile(file, m_cancelled ? "Cancelled" : "Failed",
            m_cancelled ? "Cancelled; original retained." : error, oldSize, 0);
        return false;
    };
    emit statusSignal(file, "Processing", "Analyzing...");
    FileTransaction transaction(file, m_rootDir);
    QString error;
    if (!transaction.prepare(error)) return fail(error);
    DatabaseManager db(m_dbPath);
    if (!db.isReady()) return fail("Cannot open processing history: " + db.lastError());
    const bool useAvisynth = m_settings.value("avs_enabled", false).toBool();
    if (useAvisynth && (oldSize != m_settings.value("avs_source_size", oldSize).toLongLong()
        || oldModified != m_settings.value("avs_source_modified", oldModified).toLongLong()))
        return fail("Source changed since this AviSynth profile was assigned. Rescan and assign it again.");
    const QString sourceHash = computeFastHash(file);
    if (sourceHash.isEmpty()) return fail("Cannot read source; original retained.");
    const VideoMetadata meta = probeMetadata(file, ffprobe, &m_cancelled);
    if (!meta.valid) return fail(meta.error);
    if (m_cancelled) return fail({});

    const int threads = qMax(1, m_settings.value("threads", 1).toInt());
    std::unique_ptr<AvisynthPipeline> avisynth;
    if (useAvisynth) {
        avisynth = std::make_unique<AvisynthPipeline>();
        emit statusSignal(file, "Processing", "AviSynth: checking timeline, indexing and validating filters...");
        if (!avisynth->prepare(file, meta, {m_settings.value("avs_name").toString(), m_settings.value("avs_filters").toString()},
            ffmpeg, ffprobe, threads, &m_cancelled, error, m_settings.value("avs_cache_root").toString())) return fail(error);
        if (!avisynth->timingDescription().isEmpty()) emit logSignal(avisynth->timingDescription());
    }
    const Interlace detection = useAvisynth ? Interlace::Progressive : probeInterlaced(file, ffmpeg, meta, &m_cancelled);
    if (m_cancelled) return fail({});
    const bool flaggedInterlaced = QStringList{"tt", "bb", "tb", "bt"}.contains(meta.fieldOrder);
    // An inconclusive probe cannot establish that interlaced metadata is wrong.
    const bool interlaced = detection == Interlace::Interlaced
        || (detection == Interlace::Unknown && flaggedInterlaced);
    if (detection == Interlace::Unknown) emit logSignal("Interlace analysis inconclusive; retaining the metadata decision.");
    const double effectiveFps = meta.fps * (interlaced ? 2.0 : 1.0);
    const bool halveFps = !useAvisynth && effectiveFps >= 50.0 && m_settings.value("debob", true).toBool();
    const auto geometry = useAvisynth ? VideoGeometry{avisynth->filtered.width, avisynth->filtered.height, false}
        : videoGeometry(meta, m_settings.value("downscale", true).toBool());
    const bool obsolete = QStringList{"wmv", "flv", "avi", "asf", "f4v", "divx"}.contains(source.suffix().toLower());
    bool videoEncoded = useAvisynth || meta.vcodec != "hevc" || geometry.scale || interlaced || halveFps
        || qAbs(meta.rotation) > 0.01 || obsolete;
    // Do not silently alter HDR/Dolby Vision while no explicit, verified HDR
    // conversion policy exists. Stream-copy remains available when appropriate.
    if (videoEncoded && (meta.hdr || meta.dolbyVision))
        return fail("HDR/Dolby Vision re-encoding is not supported safely; original retained.");

    QStringList maps{"-map", useAvisynth ? QString("1:v:0") : QString("0:%1").arg(meta.videoIndex)};
    QStringList subtitleOptions;
    int subtitleIndex = 0;
    for (const auto &stream : meta.streams) {
        if (stream.index == meta.videoIndex) continue;
        if (!QStringList{"video", "audio", "subtitle", "attachment"}.contains(stream.type)) continue;
        maps << "-map" << QString("0:%1").arg(stream.index);
        if (stream.type == "subtitle") {
            if (stream.codec == "mov_text") subtitleOptions << QString("-c:s:%1").arg(subtitleIndex) << "srt";
            else if (!QStringList{"subrip", "ass", "ssa", "webvtt", "dvd_subtitle", "hdmv_pgs_subtitle", "dvb_subtitle"}.contains(stream.codec))
                return fail("Unsupported Matroska subtitle codec: " + stream.codec + "; original retained.");
            ++subtitleIndex;
        }
    }
    QStringList input{"-hide_banner", "-nostdin", "-y", "-threads", QString::number(threads),
        "-filter_threads", QString::number(qMax(1, threads / 2)), "-i", file};
    if (useAvisynth) input << "-xerror" << "-f" << "avisynth" << "-i" << avisynth->scriptPath() << "-copyts";
    QStringList args = input;
    args << maps << "-map_metadata" << "0" << "-map_chapters" << "0" << "-c" << "copy" << subtitleOptions;
    if (videoEncoded) {
        error = VideoEncoder::validate(ffmpeg, m_settings, &m_cancelled);
        if (!error.isEmpty()) return fail(error);
        args << VideoEncoder::arguments(m_settings);
        QStringList filters;
        if (interlaced) filters << "bwdif=mode=send_field:parity=-1:deint=all";
        if (geometry.scale) filters << QString("scale=%1:%2:flags=lanczos,setsar=1").arg(geometry.width).arg(geometry.height);
        if (halveFps) filters << QString("fps=fps=%1:round=down").arg(effectiveFps / 2.0, 0, 'f', 8);
        filters << (useAvisynth ? avisynth->videoFilter(true) : "format=" + VideoEncoder::pixelFormat(m_settings));
        if (useAvisynth && VideoEncoder::isNvenc(m_settings)) filters << "format=" + VideoEncoder::pixelFormat(m_settings);
        if (useAvisynth) {
            // Keep sub-frame source offsets and every validated frame intact.
            args << "-enc_time_base:v:0" << "1:1000000" << "-fps_mode:v:0" << "passthrough"
                 << avisynth->colorArguments();
        }
        args << "-filter:v:0" << filters.join(',');
    }
    int audioIndex = 0;
    for (const auto &stream : meta.streams) {
        if (stream.type != "audio") continue;
        if (stream.codec != "aac" || (!useAvisynth && obsolete)) {
            const bool fdk = VideoEncoder::capabilities(ffmpeg, &m_cancelled).output.contains("libfdk_aac");
            args << QString("-c:a:%1").arg(audioIndex) << (fdk ? "libfdk_aac" : "aac");
            if (fdk) args << QString("-vbr:a:%1").arg(audioIndex) << "4";
            else args << QString("-q:a:%1").arg(audioIndex) << "1.5";
        }
        ++audioIndex;
    }
    args << "-metadata" << "title=" + source.completeBaseName();
    args << "-progress" << "pipe:1" << "-nostats" << transaction.temporaryPath();
    emit statusSignal(file, "Processing", "Transcoding...");
    const double outputFps = useAvisynth ? avisynth->filtered.fps : effectiveFps / (halveFps ? 2.0 : 1.0);
    auto result = encode(ffmpeg, args, meta.duration, file, transaction.temporaryPath(), outputFps);
    if (useAvisynth && !avisynth->scriptError().isEmpty()) return fail(avisynth->scriptError());
    if (!result.ok || m_cancelled) return fail(result.error);
    if (useAvisynth) {
        error = avisynth->validateEncodedOutput(transaction.temporaryPath(), ffprobe, &m_cancelled);
        if (!error.isEmpty()) return fail(error);
    }
    auto output = probeMetadata(transaction.temporaryPath(), ffprobe, &m_cancelled);
    error = validateOutput(meta, output, videoEncoded, geometry, false);
    if (!error.isEmpty() || m_cancelled) return fail(error);
    if (useAvisynth && (qAbs(output.fps - avisynth->filtered.fps) > 0.0001
        || output.sampleAspectRatio != avisynth->filtered.sampleAspectRatio))
        return fail("Filtered output frame rate or pixel aspect ratio differs from the validated script.");

    auto record = [&](const QString &path, qint64 size, const QString &status, QString details) {
        if (!db.recordProcessedFile(path, oldSize, size, computeFastHash(path), settingsSignature(m_settings), status))
            details += " History could not be saved: " + db.lastError();
        finishFile(file, status, details, oldSize, size);
        return true;
    };
    auto sourceUnchanged = [&]() {
        const QFileInfo current(file);
        return current.size() == oldSize && current.lastModified().toMSecsSinceEpoch() == oldModified
            && computeFastHash(file) == sourceHash;
    };
    auto retainOriginal = [&](const QString &details) {
        if (!sourceUnchanged()) return fail("Source changed during processing; original retained.");
        if (m_cancelled) return fail({});
        return record(file, oldSize, "Skipped", details);
    };
    qint64 newSize = QFileInfo(transaction.temporaryPath()).size();
    if (useAvisynth && newSize >= oldSize)
        return retainOriginal("Filtered output was larger or equal in size; cleanup was not applied. Original retained.");
    // Restore the original ordinary-encode policy: obsolete containers always
    // convert, audio-only conversions finish, and a bloated video conversion
    // keeps an MKV original or remuxes another container to MKV.
    if (!useAvisynth && videoEncoded && !obsolete && newSize >= oldSize) {
        if (source.suffix().compare("mkv", Qt::CaseInsensitive) == 0)
            return retainOriginal("Original kept because conversion did not shrink it.");
        QStringList remux = input;
        remux << maps << "-map_metadata" << "0" << "-map_chapters" << "0" << "-c" << "copy" << subtitleOptions
            << "-progress" << "pipe:1" << "-nostats" << transaction.temporaryPath();
        emit statusSignal(file, "Processing", "Remuxing original after size check...");
        result = encode(ffmpeg, remux, meta.duration, file, transaction.temporaryPath(), meta.fps);
        if (!result.ok || m_cancelled) return fail(result.error);
        output = probeMetadata(transaction.temporaryPath(), ffprobe, &m_cancelled);
        error = validateOutput(meta, output, false, geometry, true);
        if (!error.isEmpty() || m_cancelled) return fail(error);
        newSize = QFileInfo(transaction.temporaryPath()).size();
    }
    // A file changed by another program while encoding must not be replaced.
    if (!sourceUnchanged()) return fail("Source changed during processing; original retained.");
    if (m_cancelled) return fail({});
    // Commit is deliberately noninterruptible: finish or restore the original.
    if (!transaction.commit(error)) {
        finishFile(file, "Failed", error, oldSize, 0);
        return false;
    }
    return record(transaction.finalPath(), newSize, "Completed", "Finished; original preserved in .Trash.");
}

ProcessResult TranscodeWorker::encode(const QString &ffmpeg, const QStringList &args,
    double duration, const QString &file, const QString &output, double outputFps)
{
    emit logSignal("Executing: " + ffmpeg + " " + args.join(' '));
    QByteArray buffer;
    double seconds = 0, fps = -1, speed = -1;
    bool haveSeconds = false, estimated = false;
    qint64 frames = -1, reportFrames = -1;
    double timeUs = -1, timeMs = -1, timeText = -1, reportFps = -1, reportSpeed = -1;
    const bool validDuration = std::isfinite(duration) && duration > 0;
    const bool validRate = std::isfinite(outputFps) && outputFps > 0;
    auto number = [](const QByteArray &text) {
        bool ok = false;
        const double value = text.toDouble(&ok);
        return ok && std::isfinite(value) && value >= 0 ? value : -1.0;
    };
    auto clockTime = [&](const QByteArray &text) {
        const auto parts = text.split(':');
        if (parts.size() != 3 || text.startsWith('-')) return -1.0;
        bool hoursOk = false, minutesOk = false;
        const qint64 hours = parts[0].toLongLong(&hoursOk);
        const int minutes = parts[1].toInt(&minutesOk);
        const double fraction = number(parts[2]);
        if (!hoursOk || !minutesOk || hours < 0 || minutes < 0 || minutes >= 60 || fraction < 0 || fraction >= 60)
            return -1.0;
        return double(hours) * 3600.0 + minutes * 60.0 + fraction;
    };
    QElapsedTimer elapsed;
    elapsed.start();
    QElapsedTimer preview;
    preview.start();
    bool previewSent = false;
    return runProcess(ffmpeg, args, &m_cancelled, 0, [&](const QByteArray &out, const QByteArray &err) {
        if (!err.isEmpty()) emit logSignal(QString::fromUtf8(err).trimmed());
        buffer += out;
        int end;
        while ((end = buffer.indexOf('\n')) >= 0) {
            const QByteArray line = buffer.left(end).trimmed();
            buffer.remove(0, end + 1);
            const int separator = line.indexOf('=');
            if (separator < 0) continue;
            const QByteArray key = line.left(separator), value = line.mid(separator + 1);
            if (key == "out_time_us") timeUs = number(value) / 1000000.0;
            // Despite the legacy name, FFmpeg's out_time_ms is also in microseconds.
            else if (key == "out_time_ms") timeMs = number(value) / 1000000.0;
            else if (key == "out_time") timeText = clockTime(value);
            else if (key == "frame") {
                bool ok = false;
                const qint64 count = value.toLongLong(&ok);
                if (ok && count >= 0) reportFrames = count;
            }
            else if (key == "fps") reportFps = number(value);
            else if (key == "speed") reportSpeed = number(value.endsWith('x') ? value.chopped(1) : value);
            else if (key == "progress") {
                const bool advancingFrames = reportFrames >= 0 && reportFrames > frames;
                frames = qMax(frames, reportFrames);
                double current = timeUs >= 0 ? timeUs : timeMs >= 0 ? timeMs : timeText;
                if (current == 0 && frames > 0) current = -1;
                bool updatedTime = current >= 0;
                if (updatedTime) {
                    seconds = current;
                    haveSeconds = true;
                    estimated = false;
                } else if (advancingFrames && validRate) {
                    // A cover image can leave FFmpeg's aggregate mux time N/A
                    // throughout an encode. Its primary video frame count still advances.
                    const double frameTime = double(frames) / outputFps;
                    if (std::isfinite(frameTime)) {
                        seconds = qMax(haveSeconds ? seconds : 0.0, frameTime);
                        haveSeconds = updatedTime = estimated = true;
                    }
                }
                if (reportFps >= 0) fps = reportFps;
                if (!haveSeconds) speed = -1;
                else if (reportSpeed > 0) speed = reportSpeed;
                else if (updatedTime && elapsed.elapsed() > 0) {
                    const double measured = seconds / (elapsed.elapsed() / 1000.0);
                    if (std::isfinite(measured) && measured > 0) speed = measured;
                }
                const double fraction = validDuration && haveSeconds
                    ? seconds >= duration ? 1.0 : seconds / duration : -1.0;
                const int percentage = fraction >= 0 ? qMin(99, int(fraction * 100)) : -1;
                const double size = QFileInfo(output).size() / (1024.0 * 1024.0);
                QString eta = "N/A";
                const double remaining = validDuration && haveSeconds && speed > 0
                    ? qMax(0.0, duration - seconds) / speed : -1.0;
                if (std::isfinite(remaining) && remaining >= 0 && remaining <= std::numeric_limits<int>::max()) {
                    const int whole = int(remaining);
                    eta = (estimated ? QString("~") : QString()) + QString("%1:%2:%3")
                        .arg(whole / 3600, 2, 10, QChar('0')).arg(whole / 60 % 60, 2, 10, QChar('0')).arg(whole % 60, 2, 10, QChar('0'));
                }
                emit progressSignal(file, percentage, fps, speed, eta, size, fraction >= 0.03 ? size / fraction : 0);
                timeUs = timeMs = timeText = reportFps = reportSpeed = -1;
                reportFrames = -1;
                if (m_livePreviewEnabled && (preview.elapsed() >= 3000 || (!previewSent && value == "end"))) {
                    if (m_settings.value("avs_enabled", false).toBool()) {
                        // Read already-filtered output, never launch another AviSynth graph.
                        // Stay behind the muxer's buffered clusters. Own the short-lived
                        // reader here so it closes before validation/rename on Windows.
                        const double position = qMax(0.0, seconds - 5.0);
                        const auto frame = runProcess(ffmpeg, {"-v", "error", "-nostdin", "-threads", "1",
                            "-filter_threads", "1", "-seek_timestamp", "1", "-ss", QString::number(position, 'f', 6),
                            "-i", output, "-map", "0:v:0", "-an", "-sn", "-dn", "-vf", "scale=iw*sar:ih,scale=240:-1,setsar=1",
                            "-frames:v", "1", "-threads:v", "1", "-f", "image2pipe", "-c:v", "png", "-"}, &m_cancelled, 2000);
                        if (frame.ok && !frame.output.isEmpty() && m_livePreviewEnabled) {
                            emit encodedPreviewSignal(file, frame.output, position);
                            previewSent = true;
                        }
                    } else {
                        emit previewFrameSignal(file, seconds);
                        previewSent = true;
                    }
                    preview.restart();
                }
            }
        }
    });
}
