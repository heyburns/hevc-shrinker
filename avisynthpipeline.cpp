#include "avisynthpipeline.h"
#include "filetransaction.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace {
bool writeScript(const QString &path, const QString &script, QString &error)
{
    QFile file(path);
    // AviSynth's parser expects UTF-8 without a BOM on Linux. Current
    // L-SMASH Works accepts the same UTF-8 paths on Windows.
    const QByteArray bytes = script.toUtf8();
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) return true;
    error = "Cannot write AviSynth job script: " + file.errorString();
    return false;
}

VideoMetadata probeScript(const QString &path, const QString &ffprobe, const std::atomic_bool *cancel)
{
    // Opening a source can build a full index. The ordinary probe timeout does
    // not apply; this remains cancellable throughout indexing and filtering.
    auto result = runProcess(ffprobe, {"-v", "error", "-f", "avisynth", "-i", path,
        "-show_format", "-show_streams", "-of", "json"}, cancel, 0);
    if (!result.ok) { VideoMetadata meta; meta.error = result.cancelled ? "Cancelled" : result.error; return meta; }
    return parseMetadata(QJsonDocument::fromJson(result.output).object());
}

QString dependencyMessage(const QString &detail)
{
    return "AviSynth could not open this job. Install AviSynth+ and L-SMASH Works for the selected FFmpeg architecture, "
        "and put LSMASHSource and your filter plugins in AviSynth's autoload directory.\n\n" + detail;
}

QJsonObject videoStream(const VideoMetadata &metadata)
{
    for (const auto &value : metadata.json.value("streams").toArray()) {
        const auto stream = value.toObject();
        if (stream.value("index").toInt(-1) == metadata.videoIndex) return stream;
    }
    return {};
}

QPair<int, int> frameRate(const QString &text)
{
    const auto parts = text.split('/');
    if (parts.size() != 2) return {0, 1};
    bool numeratorOk = false, denominatorOk = false;
    qint64 numerator = parts[0].toLongLong(&numeratorOk), denominator = parts[1].toLongLong(&denominatorOk);
    if (!numeratorOk || !denominatorOk || numerator <= 0 || denominator <= 0) return {0, 1};
    const auto divisor = std::gcd(numerator, denominator);
    numerator /= divisor; denominator /= divisor;
    if (numerator > std::numeric_limits<int>::max() || denominator > std::numeric_limits<int>::max()) return {0, 1};
    return {int(numerator), int(denominator)};
}
}

QString AvisynthPipeline::quote(const QString &value)
{
    // AviSynth strings do not use C-style escapes. Concatenate special bytes
    // explicitly so quotes/newlines in a Linux filename cannot become script code.
    QString result = "\"";
    for (const QChar character : value) {
        const ushort c = character.unicode();
        if (c == '"' || c < 32) result += "\" + Chr(" + QString::number(c) + ") + \"";
        else result += character;
    }
    return result + '"';
}

QString AvisynthPipeline::scriptPath() const { return m_temporary.filePath("filtered.avs"); }
QString AvisynthPipeline::originalScriptPath() const { return m_temporary.filePath("original.avs"); }
QString AvisynthPipeline::videoFilter(bool restoreSourceTiming) const
{
    QString filter = "setsar=" + m_sar + ",format=yuv420p10le";
    // AviSynth starts at zero. Restore the source offset for final muxing with
    // its original audio/subtitles; previews and samples stay relative to zero.
    // An input -itsoffset would round this offset to AviSynth's frame time base.
    if (restoreSourceTiming)
        filter += ",settb=AVTB,setpts=PTS+(" + QString::number(m_videoStart, 'f', 6) + ")/TB";
    return filter;
}
QString AvisynthPipeline::scriptError() const
{
    QFile file(m_temporary.filePath("script-error.txt"));
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.read(64 * 1024)).trimmed() : QString();
}

QString AvisynthPipeline::validateEncodedOutput(const QString &output, const QString &ffprobe, const std::atomic_bool *cancel) const
{
    const QString scriptFailure = scriptError();
    if (!scriptFailure.isEmpty()) return scriptFailure;
    // Check encoded video length separately from container duration: a longer
    // copied audio track can otherwise hide prematurely truncated video.
    const auto result = runProcess(ffprobe, {"-v", "error", "-select_streams", "v:0", "-count_packets",
        "-show_entries", "stream=nb_read_packets", "-of", "json", output}, cancel, 0);
    if (!result.ok) return result.cancelled ? "Cancelled" : result.error;
    const auto streams = QJsonDocument::fromJson(result.output).object().value("streams").toArray();
    if (streams.isEmpty() || streams.first().toObject().value("nb_read_packets").toString().toLongLong() != m_frames)
        return "Filtered output frame count does not match the validated source; original retained.";
    return {};
}

QString AvisynthPipeline::testInstallation(const QString &ffmpeg, const QString &ffprobe, const std::atomic_bool *cancel)
{
    if (ffmpeg.isEmpty() || ffprobe.isEmpty()) return "FFmpeg and FFprobe must be available.";
    QTemporaryDir temporary;
    QString error;
    const QString script = temporary.filePath("installation.avs");
    if (!writeScript(script, "Assert(FunctionExists(\"LWLibavVideoSource\"), \"L-SMASH Works is missing from AviSynth autoload plugins\")\n"
        "BlankClip(length=2, width=64, height=64, fps=24, pixel_type=\"YUV420P10\")\n", error)) return error;
    const auto meta = probeScript(script, ffprobe, cancel);
    if (!meta.valid) return dependencyMessage(meta.error);
    const auto result = runProcess(ffmpeg, {"-v", "error", "-nostdin", "-f", "avisynth", "-i", script,
        "-frames:v", "2", "-an", "-f", "null", "-"}, cancel, 60000);
    return result.ok ? QString() : dependencyMessage(result.error);
}

QString AvisynthPipeline::validateTimeline(const QList<double> &timestamps, double fps)
{
    if (timestamps.size() < 2 || !std::isfinite(fps) || fps <= 0 || !std::isfinite(timestamps.first()))
        return "Cannot establish a constant frame rate for this source.";
    const double tolerance = qMax(0.0011, 0.02 / fps); // Includes millisecond Matroska rounding.
    const double start = timestamps.first();
    for (qsizetype i = 0; i < timestamps.size(); ++i) {
        if (!std::isfinite(timestamps[i]) || std::abs((timestamps[i] - start) - double(i) / fps) > tolerance)
            return "Video frame timing varies.";
    }
    return {};
}

bool AvisynthPipeline::prepare(const QString &source, const VideoMetadata &meta, const AvisynthProfile &profile,
    const QString &ffmpeg, const QString &ffprobe, int threads, const std::atomic_bool *cancel, QString &error,
    const QString &cacheRoot)
{
    m_timingDescription.clear();
    if (!m_temporary.isValid()) { error = "Cannot create AviSynth job directory."; return false; }
    if (!meta.valid) { error = meta.error; return false; }
    if (meta.hdr || meta.dolbyVision) { error = "AviSynth cleanup currently supports SDR sources only."; return false; }
    if (profile.filters.trimmed().isEmpty()) { error = "Enter a filter chain before applying AviSynth."; return false; }
    if (!std::isfinite(meta.fps) || meta.fps <= 0) { error = "The source frame rate is unknown."; return false; }
    error = testInstallation(ffmpeg, ffprobe, cancel);
    if (!error.isEmpty()) return false;

    struct PacketTime { double pts = 0, duration = 0; };
    QList<PacketTime> packets;
    QByteArray buffer;
    bool invalidTimestamp = false;
    auto consume = [&]() {
        int end;
        while ((end = buffer.indexOf('\n')) >= 0) {
            const QByteArray line = buffer.left(end).trimmed(); buffer.remove(0, end + 1);
            if (line.isEmpty()) continue;
            PacketTime packet;
            bool timestampOk = false;
            for (const auto &field : line.split('|')) {
                const auto separator = field.indexOf('=');
                const auto key = field.left(separator), value = field.mid(separator + 1);
                if (key == "pts_time") packet.pts = value.toDouble(&timestampOk);
                else if (key == "duration_time") {
                    bool ok = false;
                    const double duration = value.toDouble(&ok);
                    if (ok && std::isfinite(duration) && duration > 0) packet.duration = duration;
                }
            }
            if (!timestampOk || !std::isfinite(packet.pts)) invalidTimestamp = true;
            else packets.append(packet);
        }
    };
    // Read packet timestamps without decoding the entire source. Sorting handles
    // B-frame reordering. This check is only performed for explicitly filtered jobs.
    const auto timing = runProcess(ffprobe, {"-v", "error", "-select_streams", QString::number(meta.videoIndex),
        "-show_entries", "packet=pts_time,duration_time", "-of", "compact=p=0:nk=0", source}, cancel, 0,
        [&](const QByteArray &out, const QByteArray &) { buffer += out; consume(); });
    if (!buffer.isEmpty()) { buffer += '\n'; consume(); }
    if (!timing.ok) { error = timing.cancelled ? "Cancelled" : timing.error; return false; }
    if (invalidTimestamp) { error = "The source has missing or invalid video timestamps."; return false; }
    if (packets.size() < 2) { error = "Cannot establish a video timeline for this source."; return false; }
    std::sort(packets.begin(), packets.end(), [](const PacketTime &a, const PacketTime &b) { return a.pts < b.pts; });
    QList<double> timestamps;
    timestamps.reserve(packets.size());
    for (const auto &packet : packets) {
        if (!timestamps.isEmpty() && packet.pts <= timestamps.last()) {
            error = "The source has duplicate video timestamps; its playback timeline is ambiguous."; return false;
        }
        timestamps.append(packet.pts);
    }
    const bool normalizeTiming = !validateTimeline(timestamps, meta.fps).isEmpty();
    m_frames = packets.size();
    m_videoStart = packets.first().pts;
    const auto sourceStream = videoStream(meta);
    auto rate = frameRate(sourceStream.value("avg_frame_rate").toString());
    if (rate.first == 0 && meta.fps < double(std::numeric_limits<qint64>::max()) / 1000000.0)
        rate = frameRate(QString::number(qRound64(meta.fps * 1000000.0)) + "/1000000");
    if (rate.first == 0) { error = "Cannot establish a supported source frame rate."; return false; }
    if (normalizeTiming) {
        // Use the nominal cadence when it is plausible, rather than an average
        // slightly reduced by occasional missing frames. L-SMASH uses original
        // timestamps to repeat/drop frames, preserving elapsed playback time.
        const auto nominal = frameRate(sourceStream.value("r_frame_rate").toString());
        const double nominalFps = double(nominal.first) / nominal.second;
        if (nominal.first > 0 && nominalFps >= meta.fps * 0.9999
            && nominalFps <= meta.fps * 2.0 && nominalFps <= 120.0) rate = nominal;
        m_timingDescription = QString("AviSynth: variable source timing normalized to %1/%2 fps while preserving playback duration.")
            .arg(rate.first).arg(rate.second);
    }
    const double expectedFps = normalizeTiming ? double(rate.first) / rate.second : meta.fps;
    double sourceVideoDuration = packets.last().pts - m_videoStart;
    if (packets.last().duration > 0) sourceVideoDuration += packets.last().duration;
    else {
        bool durationOk = false;
        const double declaredDuration = sourceStream.value("duration").toString().toDouble(&durationOk);
        if (durationOk && std::isfinite(declaredDuration) && declaredDuration > sourceVideoDuration)
            sourceVideoDuration = declaredDuration;
        else sourceVideoDuration += packets.last().pts - packets[packets.size() - 2].pts;
    }

    QString cache = cacheRoot;
    if (cache.isEmpty()) cache = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath("avisynth-indexes");
    if (!QDir().mkpath(cache)) { error = "Cannot create AviSynth index cache."; return false; }
    const QFileInfo input(source);
    const QString fingerprint = computeFastHash(source);
    if (fingerprint.isEmpty()) { error = "Cannot read AviSynth source."; return false; }
    const QByteArray identity = (normalizedPath(source) + '\n' + fingerprint + '\n'
        + QString::number(input.lastModified().toMSecsSinceEpoch()) + '\n' + QString::number(meta.videoIndex)
        + '\n' + ffmpeg + QString::number(QFileInfo(ffmpeg).lastModified().toMSecsSinceEpoch())).toUtf8();
    const QString key = QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
    const QString index = QDir(cache).absoluteFilePath(key + ".lwi");
    m_cacheLock = std::make_unique<QLockFile>(index + ".lock");
    m_cacheLock->setStaleLockTime(0);
    if (!m_cacheLock->tryLock(0)) { error = "Another AviSynth job is using this source index. Retry after it finishes."; return false; }
    QString header = "Assert(FunctionExists(\"LWLibavVideoSource\"), \"L-SMASH Works is missing from AviSynth autoload plugins\")\n"
        "SetMemoryMax(512)\n"
        "last = LWLibavVideoSource(" + quote(input.absoluteFilePath()) + ", stream_index=" + QString::number(meta.videoIndex)
        + ", threads=" + QString::number(qBound(1, threads, 4)) + ", cache=true, cachefile=" + quote(index)
        + (normalizeTiming ? QString(", fpsnum=%1, fpsden=%2").arg(rate.first).arg(rate.second) : QString()) + ")\n";
    // L-SMASH supplies coded pixels; normalize the display orientation once.
    const int rotation = ((qRound(meta.rotation) % 360) + 360) % 360;
    if (qAbs(meta.rotation - qRound(meta.rotation)) > 0.01 || rotation % 90 != 0) {
        error = "AviSynth cleanup supports only right-angle display rotations."; return false;
    }
    if (rotation == 90) header += "TurnLeft()\n";
    if (rotation == 270) header += "TurnRight()\n";
    if (rotation == 180) header += "Turn180()\n";
    // Normalize anamorphic source pixels before the user's crop/resize chain.
    const auto geometry = videoGeometry(meta, false);
    if (geometry.scale) header += QString("Spline36Resize(%1, %2)\n").arg(geometry.width).arg(geometry.height);
    header += "propSet(\"_SARNum\", 1)\npropSet(\"_SARDen\", 1)\n";
    if (!writeScript(originalScriptPath(), header + "return last\n", error)) return false;
    if (normalizeTiming) {
        const auto normalized = probeScript(originalScriptPath(), ffprobe, cancel);
        if (!normalized.valid) { error = normalized.error; return false; }
        m_frames = videoStream(normalized).value("nb_frames").toString().toLongLong();
        if (m_frames <= 0 || qAbs(normalized.fps - expectedFps) > 0.0001
            || qAbs(normalized.duration - sourceVideoDuration) > 1.0 / expectedFps + 0.0011) {
            error = "L-SMASH Works could not normalize source timing without changing playback duration; original retained.";
            return false;
        }
    }
    // The function isolates the filter body: even a user's 'return' cannot
    // bypass the timeline assertions and output conversion in our wrapper.
    const QString script = "function ShrinkerFilters(clip source) {\nlast = source\n" + profile.filters
        + "\nreturn last\n}\n" + header + "source = last\nlast = ShrinkerFilters(source)\n"
        "Assert(IsClip(last), \"The filter chain must return a video clip\")\n"
        "Assert(FrameCount(last) == FrameCount(source), \"Filters must preserve frame count\")\n"
        "Assert(FrameRateNumerator(last) == FrameRateNumerator(source) && FrameRateDenominator(last) == FrameRateDenominator(source), \"Filters must preserve frame rate\")\n"
        "Assert(Width(last) % 2 == 0 && Height(last) % 2 == 0, \"Output dimensions must be even for Main10\")\n"
        "return last.KillAudio().ConvertBits(10).ConvertToYUV420()\n";
    const QString body = m_temporary.filePath("filter-chain.avs");
    if (!writeScript(body, script, error)) return false;
    // Some source plugins replace FFmpeg's log callback and suppress errors
    // during Import. Capture the actual AviSynth message before rethrowing it.
    const QString wrapper = "try {\nresult = Import(" + quote(body) + ")\n} catch (error_message) {\n"
        "global hevc_script_error = error_message\n"
        "report = WriteFileStart(BlankClip(length=1, width=16, height=16), "
        + quote(m_temporary.filePath("script-error.txt")) + ", \"hevc_script_error\", append=false)\n"
        "Assert(false, error_message)\n}\nreturn result\n";
    if (!writeScript(scriptPath(), wrapper, error)) return false;
    filtered = probeScript(scriptPath(), ffprobe, cancel);
    error = scriptError();
    if (!error.isEmpty()) return false;
    if (!filtered.valid) { error = filtered.error; return false; }
    const double videoDuration = double(m_frames) / expectedFps;
    if (qAbs(filtered.fps - expectedFps) > 0.0001
        || videoStream(filtered).value("nb_frames").toString().toLongLong() != m_frames
        || qAbs(filtered.duration - videoDuration) > qMax(0.002, 0.1 / expectedFps)
        || (normalizeTiming && qAbs(filtered.duration - sourceVideoDuration) > 1.0 / expectedFps + 0.0011)) {
        error = "The source reader or filter chain changed video timing. Original retained."; return false;
    }
    const QString sar = filtered.sampleAspectRatio;
    if (!sar.isEmpty() && sar != "N/A" && sar != "0:1") m_sar = QString(sar).replace(':', '/');
    // Some FFmpeg AviSynth demuxers omit SAR even when frame properties exist.
    // The wrapper normalizes the source; the final FFmpeg filter enforces it.
    filtered.sampleAspectRatio = QString(m_sar).replace('/', ':');
    const QList<QPair<QString, QPair<QString, QString>>> tags{
        {"-color_primaries:v:0", {filtered.colorPrimaries, meta.colorPrimaries}},
        {"-color_trc:v:0", {filtered.colorTransfer, meta.colorTransfer}},
        {"-colorspace:v:0", {filtered.colorSpace, meta.colorSpace}},
        {"-color_range:v:0", {filtered.colorRange, meta.colorRange}}};
    for (const auto &tag : tags) {
        QString value = tag.second.first;
        if (value.isEmpty() || value == "unknown") value = tag.second.second;
        if (!value.isEmpty() && value != "unknown") m_colorArguments << tag.first << value;
    }
    // Decode representative frames; stream headers alone cannot catch lazy filter errors.
    for (double position : {0.0, filtered.duration / 2.0, qMax(0.0, filtered.duration - 1.0 / filtered.fps)}) {
        const auto decode = runProcess(ffmpeg, {"-v", "error", "-nostdin", "-ss", QString::number(position, 'f', 8),
            "-f", "avisynth", "-i", scriptPath(), "-frames:v", "1", "-an", "-f", "null", "-"}, cancel, 0);
        if (!decode.ok) { error = decode.cancelled ? "Cancelled" : decode.error; return false; }
    }
    return true;
}
