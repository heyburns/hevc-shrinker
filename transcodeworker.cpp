#include "transcodeworker.h"
#include "databasemanager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>

// Helper function to safely move files across different disk volumes or filesystems.
// Under Linux, rename() fails if target is on a different mount point (e.g. SSD to HDD share).
// This function falls back to copy-then-delete if standard rename fails.
static bool safeMove(const QString &src, const QString &dest) {
    if (QFile::rename(src, dest)) return true;
    if (QFile::copy(src, dest)) {
        return QFile::remove(src);
    }
    return false;
}

// Searches for executable dependencies (like ffmpeg/ffprobe) on the host system.
// Checks the system PATH first, then falls back to check the directory where this application runs.
QString findDependency(const QString &name) {
    QString path = QStandardPaths::findExecutable(name);
    if (!path.isEmpty()) return path;

    QString appDir = QCoreApplication::applicationDirPath();
    QString localPath = appDir + "/" + name;
#ifdef Q_OS_WIN
    localPath += ".exe"; // Append suffix on Windows
#endif
    if (QFile::exists(localPath)) return localPath;

#ifdef Q_OS_MAC
    // On macOS, check inside the app bundle's Resources directory as well
    QString macResourcesPath = appDir + "/../Resources/" + name;
    if (QFile::exists(macResourcesPath)) return macResourcesPath;
#endif

    return ""; // Not found
}

// Generates a quick cryptographic hash from the first 4MB of the video file.
// Used as a unique signature to record and verify sizing history in the database.
QString computeFastHash(const QString &filepath) {
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) return "";
    QCryptographicHash hash(QCryptographicHash::Sha1);
    QByteArray chunk = file.read(4 * 1024 * 1024); // Read first 4MB
    hash.addData(chunk);
    return QString(hash.result().toHex());
}

// Probes a video file using FFprobe to check if it is already compliant (HEVC/H.265 video + AAC audio).
// Non-compliant files require transcoding.
bool probeFileCompliance(const QString &filepath, const QString &ffprobeBin) {
    QFileInfo fi(filepath);
    // Standard library format mandates Matroska (.mkv) container
    if (fi.suffix().toLower() != "mkv") return false;

    QProcess proc;
    // Launch FFprobe requesting json output representing codecs
    proc.start(ffprobeBin, {
        "-v", "error",
        "-show_entries", "stream=codec_name",
        "-of", "json",
        filepath
    });
    if (proc.waitForFinished()) {
        QByteArray out = proc.readAllStandardOutput();
        QJsonDocument doc = QJsonDocument::fromJson(out);
        if (!doc.isNull() && doc.isObject()) {
            QJsonObject obj = doc.object();
            QJsonArray streams = obj["streams"].toArray();
            bool hasHevc = false;
            bool hasAac = false;
            // Iterate through audio/video streams to inspect their codecs
            for (const QJsonValue &val : streams) {
                QString codec = val.toObject()["codec_name"].toString().toLower();
                if (codec == "hevc") hasHevc = true;
                if (codec == "aac") hasAac = true;
            }
            // Fully compliant if container is .mkv and contains both HEVC and AAC streams
            return (hasHevc && hasAac);
        }
    }
    return false;
}

// Checks if the host's FFmpeg binary has the high-quality Fraunhofer AAC encoder (libfdk_aac) compiled in.
static bool detectFdkAac(const QString &ffmpegBin) {
    QProcess proc;
    proc.start(ffmpegBin, {"-encoders"});
    if (proc.waitForFinished()) {
        QString out = QString::fromUtf8(proc.readAllStandardOutput());
        return out.contains("libfdk_aac"); // True if fdk is present
    }
    return false;
}

// Parses JSON information returned by FFprobe to extract duration, codecs, sizes, and frames.
VideoMetadata probeMetadata(const QString &filepath, const QString &ffprobeBin) {
    VideoMetadata meta;
    QProcess proc;
    proc.start(ffprobeBin, {
        "-v", "error",
        "-show_entries", "format=duration:stream=codec_name,codec_type,width,height,avg_frame_rate,field_order,display_aspect_ratio",
        "-of", "json",
        filepath
    });
    if (proc.waitForFinished()) {
        QJsonDocument doc = QJsonDocument::fromJson(proc.readAllStandardOutput());
        QJsonObject root = doc.object();
        
        if (root.contains("format")) {
            meta.duration = root["format"].toObject()["duration"].toString().toDouble();
        }
        
        if (root.contains("streams")) {
            QJsonArray streams = root["streams"].toArray();
            for (const QJsonValue &val : streams) {
                QJsonObject s = val.toObject();
                QString type = s["codec_type"].toString().toLower();
                if (type == "video") {
                    meta.vcodec = s["codec_name"].toString().toLower();
                    meta.width = s["width"].toInt();
                    meta.height = s["height"].toInt();
                    meta.fieldOrder = s["field_order"].toString().toLower();
                    meta.displayAspectRatio = s["display_aspect_ratio"].toString();
                    
                    // Framerates are returned as fractions (e.g. "30000/1001"). Parse them.
                    QString fpsStr = s["avg_frame_rate"].toString();
                    if (fpsStr.contains("/")) {
                        QStringList parts = fpsStr.split("/");
                        if (parts.size() == 2) {
                            double num = parts[0].toDouble();
                            double den = parts[1].toDouble();
                            if (den != 0.0) {
                                meta.fps = num / den;
                            }
                        }
                    }
                } else if (type == "audio") {
                    // Extract the first audio stream codec
                    if (!meta.hasAudio) {
                        meta.acodec = s["codec_name"].toString().toLower();
                        meta.hasAudio = true;
                    }
                }
            }
        }
    }
    return meta;
}

// Analytically evaluates if a video contains interlaced frames.
// Runs FFmpeg's 'idet' filter for 360 frames (about 12–15 seconds) to determine if
// interlacing characteristics exist.
static bool probeInterlaced(const QString &filepath, const QString &ffmpegBin) {
    QProcess proc;
    proc.start(ffmpegBin, {
        "-filter_threads", "4",
        "-i", filepath,
        "-filter:v", "idet", // Interlace detection filter
        "-frames:v", "360", // Probe first 360 frames
        "-an", // No audio decoding
        "-f", "null", // Null muxer (discard output)
        "-"
    });
    if (proc.waitForFinished()) {
        QString err = QString::fromUtf8(proc.readAllStandardError());
        // Parse the Multi-frame detection summary line
        QRegularExpression re("Multi frame detection:\\s*TFF:\\s*(\\d+)\\s*BFF:\\s*(\\d+)\\s*Progressive:\\s*(\\d+)");
        QRegularExpressionMatch match = re.match(err);
        if (match.hasMatch()) {
            int tff = match.captured(1).toInt(); // Top Field First frames
            int bff = match.captured(2).toInt(); // Bottom Field First frames
            int prog = match.captured(3).toInt(); // Progressive frames
            int interlaced = tff + bff;
            // Heuristic: If interlaced frames are more than double the progressive frames,
            // the video is classified as interlaced.
            if (interlaced > prog * 2) {
                return true;
            }
        }
    }
    return false;
}

// Constructor: Initializes members and thread control variables.
TranscodeWorker::TranscodeWorker(const QStringList &fileQueue, const QString &rootDir, const QString &dbPath, const QVariantMap &settings, QObject *parent)
    : QThread(parent)
    , m_fileQueue(fileQueue)
    , m_rootDir(rootDir)
    , m_dbPath(dbPath)
    , m_settings(settings)
    , m_isRunning(false)
    , m_livePreviewEnabled(false)
    , m_activeProcess(nullptr)
    , m_hasFdk(false)
{
    m_livePreviewEnabled = settings.value("live_preview", false).toBool();
}

// Destructor: Safely stops execution and cleans up process handles.
TranscodeWorker::~TranscodeWorker()
{
    stop();
}

// Thread-safe request to stop processing files in the queue.
void TranscodeWorker::stop()
{
    m_isRunning = false;
}

// Dynamic control of live progress preview generation.
void TranscodeWorker::setLivePreviewEnabled(bool enabled)
{
    m_livePreviewEnabled = enabled;
}

// Core execution method running inside the background thread.
void TranscodeWorker::run()
{
    m_isRunning = true;
    QString ffmpegBin = findDependency("ffmpeg");
    QString ffprobeBin = findDependency("ffprobe");

    // Abort if dependencies are not found on the host system
    if (ffmpegBin.isEmpty() || ffprobeBin.isEmpty()) {
        emit logSignal("[ERROR] ffmpeg or ffprobe dependency is missing. Cannot process.");
        emit finishedSignal();
        return;
    }

    // Set up trash and error directories inside the active workspace
    QString trashDir = QDir(m_rootDir).filePath(".Trash");
    QString errorDir = QDir(m_rootDir).filePath(".Errors");

    // Detect if Fraunhofer AAC is supported by this FFmpeg binary
    m_hasFdk = detectFdkAac(ffmpegBin);
    emit logSignal(QString("[NOTICE] Audio encoder: %1").arg(m_hasFdk ? "libfdk_aac (preferred)" : "aac (native fallback)"));

    // Loop through files in the queue
    for (const QString &filepath : m_fileQueue) {
        if (!m_isRunning) break; // User requested abort

        try {
            processFile(filepath, ffmpegBin, ffprobeBin, trashDir, errorDir);
        } catch (const std::exception &e) {
            emit logSignal(QString("[ERROR] Exception processing %1: %2").arg(QFileInfo(filepath).fileName(), e.what()));
            moveToErrors(filepath, errorDir);
            emit statusSignal(filepath, "Error", "Critical exception");
        }
    }

    emit finishedSignal(); // Signal parent thread that worker is done
}

// Main execution method that controls the transcode pipeline for a single file.
bool TranscodeWorker::processFile(const QString &filepath, const QString &ffmpegBin, const QString &ffprobeBin, const QString &trashDir, const QString &errorDir)
{
    QFileInfo fileInfo(filepath);
    QString baseName = fileInfo.fileName();
    QString baseNoExt = fileInfo.completeBaseName();
    QString fileDir = fileInfo.absolutePath();

    emit logSignal(QString("\n[START] Processing file: %1").arg(baseName));
    emit statusSignal(filepath, "Processing", "Preparing...");

    // 1. Calculate the file signature (Fast Hash) to detect if we processed it before
    QString fileHash = computeFastHash(filepath);
    if (fileHash.isEmpty()) {
        emit logSignal("[ERROR] Failed to compute file hash. Skipping.");
        emit statusSignal(filepath, "Error", "Hash failed");
        return false;
    }

    // 2. Query file properties (codecs, frame dimensions, durations) using FFprobe
    VideoMetadata meta = probeMetadata(filepath, ffprobeBin);
    if (meta.duration <= 0.0) {
        emit logSignal("[ERROR] Failed to read video metadata. Skipping.");
        emit statusSignal(filepath, "Error", "Metadata failed");
        return false;
    }
    emit logSignal(QString("Metadata - Video: %1 (%2x%3, %4 fps), Audio: %5, Duration: %6s")
                   .arg(meta.vcodec,
                        QString::number(meta.width),
                        QString::number(meta.height),
                        QString::number(meta.fps, 'f', 2),
                        meta.hasAudio ? meta.acodec : "none",
                        QString::number(meta.duration, 'f', 1)));

    // 3. Scan the video for interlaced line characteristics using FFmpeg's 'idet' filter
    bool isInterlaced = probeInterlaced(filepath, ffmpegBin);
    emit logSignal(QString("Scan results: %1").arg(isInterlaced ? "Interlaced frames detected" : "Progressive scan detected"));

    // Compare scan results with container metadata. If they disagree, log a warning and trust our scan analysis.
    bool metadataInterlaced = (meta.fieldOrder == "tt" || meta.fieldOrder == "bb" || meta.fieldOrder == "tb" || meta.fieldOrder == "bt");
    if (metadataInterlaced == isInterlaced) {
        emit logSignal(QString("Interlace check: Metadata and scan analysis agree (Interlaced: %1).")
                       .arg(isInterlaced ? "yes" : "no"));
    } else {
        if (isInterlaced) {
            emit logSignal("[NOTICE] Metadata says progressive/unknown, but scan analysis detected interlaced lines. OVERRIDING metadata to deinterlace.");
        } else {
            emit logSignal("[NOTICE] Metadata says interlaced, but scan analysis confirmed progressive frames. OVERRIDING metadata to skip deinterlacing.");
        }
    }

    // 4. Decide target transcode rules based on compliance logic
    bool isAlreadyHevc = (meta.vcodec == "hevc");
    bool isAlreadyAac = (meta.acodec == "aac" || !meta.hasAudio);
    
    // If interlaced, a de-bobbing deinterlacer doubles the frame rate
    double effectiveFps = isInterlaced ? (meta.fps * 2.0) : meta.fps;
    // We only downsample frame rates (bobbed) if they exceed 50 FPS and the user has enabled it
    bool bobbed = (effectiveFps >= 50.0 && m_settings["debob"].toBool());
    
    // Check if the video dimensions exceed 1080p (1920x1080) and need downscaling
    bool isPortrait = (meta.height > meta.width);
    bool needsDownscale = false;
    if (m_settings["downscale"].toBool()) {
        if (isPortrait) {
            needsDownscale = (meta.width > 1080 || meta.height > 1920);
        } else {
            needsDownscale = (meta.height > 1080 || meta.width > 1920);
        }
    }

    // Check if file is in an obsolete format (e.g. WMV, AVI) which should be fully re-encoded to prevent container stream copy bugs.
    QString suffix = fileInfo.suffix().toLower();
    bool isObsoleteFormat = (suffix == "wmv" || suffix == "flv" || suffix == "avi" || suffix == "asf" || suffix == "f4v" || suffix == "divx");

    // Codec target decisions (copy streams if they are already compliant, transcode otherwise)
    QString wantVideoCodec = (isAlreadyHevc && !needsDownscale && !bobbed && !isInterlaced && !isObsoleteFormat) ? "copy" : "libx265";
    QString wantAudioCodec = (isAlreadyAac && !isObsoleteFormat) ? "copy" : "encode";

    // Create absolute filepath path for the temporary transcoding output file
    QString tmpOut = QDir(fileDir).filePath(baseNoExt + ".tmp_out.mkv");
    
    // 5. Build the command line arguments for the FFmpeg process execution
    QStringList cmdArgs;
    int threads = m_settings.value("threads", 0).toInt(); // Fetch thread limits calculated in main window
    if (threads > 0) {
        // Enforce dynamic CPU thread partitions to prevent thread over-provisioning and cache thrashing
        cmdArgs << "-y" << "-threads" << QString::number(threads) << "-filter_threads" << QString::number(qMax(1, threads / 2)) << "-i" << filepath;
    } else {
        cmdArgs << "-y" << "-filter_threads" << "4" << "-i" << filepath;
    }

    // Set up Video arguments
    if (wantVideoCodec == "copy") {
        cmdArgs << "-c:v" << "copy"; // Stream copy video (instantaneous, no quality loss)
    } else {
        // Build x265 parameters: enforce Main 10 profile (10-bit H.265), disable Sample Adaptive Offset (SAO) for detail retention,
        // and set thread pools to match our partitioned thread count.
        QString x265Params = "profile=main10:no-sao=1:selective-sao=0:aq-mode=1:pmode=1:pme=1";
        if (threads > 0) {
            x265Params += QString(":pools=%1").arg(threads);
        }

        // We copy video by default for extra streams (e.g., cover art attachments) but transcode
        // the main video stream (v:0) to H.265 (libx265)
        cmdArgs << "-c:v" << "copy"
                << "-c:v:0" << "libx265"
                << "-preset" << m_settings["preset"].toString() // ultrafast, medium, veryslow, etc.
                << "-crf" << QString::number(m_settings["crf"].toInt()) // Constant Rate Factor (quality scale)
                << "-x265-params" << x265Params;

        // Build video filter pipelines (-filter:v:0)
        QStringList videoFilters;
        if (isInterlaced) {
            // Apply double-framerate de-bob deinterlacer filter
            videoFilters << "bwdif=mode=send_field:parity=-1:deint=all";
        }
        if (needsDownscale) {
            // Scale keeping aspect ratio using high-quality Lanczos scaling algorithm
            if (isPortrait) {
                if (static_cast<double>(meta.height) / meta.width > 1920.0 / 1080.0) {
                    videoFilters << "scale=-2:1920:flags=lanczos";
                } else {
                    videoFilters << "scale=1080:-2:flags=lanczos";
                }
            } else {
                if (static_cast<double>(meta.width) / meta.height > 1920.0 / 1080.0) {
                    videoFilters << "scale=1920:-2:flags=lanczos";
                } else {
                    videoFilters << "scale=-2:1080:flags=lanczos";
                }
            }
        }
        if (bobbed) {
            // Downsample high frame rates back to half (typically 60fps to 30fps)
            double targetFps = effectiveFps / 2.0;
            videoFilters << QString("fps=fps=%1:round=down").arg(QString::number(targetFps, 'f', 6));
        }
        // Force output color format to high-depth 10-bit YUV 4:2:0
        videoFilters << "format=yuv420p10le";
        
        cmdArgs << "-filter:v:0" << videoFilters.join(",");
        emit logSignal(QString("Video filters applied (to stream 0:v:0): %1").arg(videoFilters.join(",")));
    }

    // Set up Audio arguments
    // Set up Audio arguments
    if (wantAudioCodec == "copy") {
        cmdArgs << "-c:a" << "copy"; // Stream copy audio
    } else {
        if (m_hasFdk) {
            // Use high-performance Fraunhofer FDK encoder in Variable Bit Rate Mode 4
            cmdArgs << "-c:a" << "libfdk_aac" << "-vbr" << "4";
        } else {
            // Use native AAC encoder in Variable Bit Rate mode (quality scale 1.5)
            cmdArgs << "-c:a" << "aac" << "-q:a" << "1.5";
        }
    }

    // Map all streams (subtitles, audio channels, attachments)
    cmdArgs << "-c:s" << "copy" // Stream copy subtitles
            << "-map" << "0:v"   // Map all video tracks
            << "-map" << "0:a?"  // Map all audio tracks (optional)
            << "-map" << "0:s?"  // Map all subtitle tracks (optional)
            << "-map" << "0:t?"; // Map all attachment/font tracks (optional)
    
    // Set file metadata title to matches original filename without suffix
    cmdArgs << "-metadata" << "title=" + baseNoExt;
    
    // Maintain aspect ratio configurations
    if (!meta.displayAspectRatio.isEmpty() && meta.displayAspectRatio != "0:1") {
        cmdArgs << "-aspect" << meta.displayAspectRatio;
    }
    cmdArgs << tmpOut; // Output filepath

    // Execute transcode process
    emit logSignal(QString("Executing: %1 %2").arg(ffmpegBin, cmdArgs.join(" ")));
    emit logSignal(QString("Encoding to: %1").arg(tmpOut));
    emit statusSignal(filepath, "Processing", "Transcoding...");

    // 6. Launch the FFmpeg subprocess
    bool success = runFfmpegProcess(QStringList() << ffmpegBin << cmdArgs, meta.duration, filepath);

    if (success && QFile::exists(tmpOut)) {
        // Successful transcode completed
        qint64 oldSize = fileInfo.size();
        qint64 newSize = QFileInfo(tmpOut).size();
        
        // Bloat Protection check: If H.265 compression made the file LARGER, we discard the transcode!
        // (Unless it was in an obsolete container, which we must always transcode to satisfy standards)
        if (wantVideoCodec == "libx265" && newSize >= oldSize && !isObsoleteFormat) {
            emit logSignal(QString("[NOTICE] Transcoded file (%1 MB) is larger than or equal to original (%2 MB). Discarding bloated transcode.")
                           .arg(QString::number(static_cast<double>(newSize)/(1024.0*1024.0), 'f', 1),
                                QString::number(static_cast<double>(oldSize)/(1024.0*1024.0), 'f', 1)));
            QFile::remove(tmpOut); // Remove the transcode file
            
            QString finalOut = QDir(fileDir).filePath(baseNoExt + ".mkv");
            
            if (fileInfo.suffix().toLower() == "mkv") {
                // If the original file was already in an MKV container, keep the original file in place
                {
                    DatabaseManager db(m_dbPath);
                    db.recordProcessedFile(filepath, oldSize, oldSize, fileHash);
                }
                emit statusSignal(filepath, "Skipped", "Original kept (transcode grew)");
                emit fileDoneSignal(filepath, "Skipped", oldSize, oldSize);
                return true;
            } else {
                // If original is NOT in an MKV container, we remux (stream copy) it to MKV
                emit logSignal(QString("Remuxing original %1 directly to MKV container (stream copy)...").arg(baseName));
                
                QProcess remuxProc;
                remuxProc.start(ffmpegBin, {
                    "-y", "-i", filepath,
                    "-c", "copy", "-map", "0",
                    finalOut
                });
                
                if (remuxProc.waitForFinished() && remuxProc.exitCode() == 0 && QFile::exists(finalOut)) {
                    // Trash the original file
                    bool trashed = moveToTrash(filepath, trashDir);
                    if (!trashed) {
                        emit logSignal(QString("Attempting direct deletion of original file: %1").arg(filepath));
                        trashed = QFile::remove(filepath); // Fallback delete
                    }

                    // Duplicate prevention safeguard: If both trashing and deletion fail, abort!
                    if (!trashed) {
                        emit logSignal(QString("[ERROR] Failed to trash or delete original file: %1. Cleaning up remuxed output to prevent duplicates.").arg(filepath));
                        if (QFile::exists(finalOut)) {
                            QFile::remove(finalOut);
                        }
                        moveToErrors(filepath, errorDir);
                        emit statusSignal(filepath, "Error", "Original delete failed");
                        emit fileDoneSignal(filepath, "Failed", oldSize, 0);
                        return false;
                    }

                    {
                        DatabaseManager db(m_dbPath);
                        db.recordProcessedFile(finalOut, oldSize, QFileInfo(finalOut).size(), fileHash);
                    }
                    emit statusSignal(filepath, "Completed", "Remuxed original (transcode grew)");
                    emit fileDoneSignal(filepath, "Completed", oldSize, QFileInfo(finalOut).size());
                    return true;
                } else {
                    emit logSignal("[ERROR] Failed to remux original file to MKV.");
                    if (QFile::exists(finalOut)) QFile::remove(finalOut);
                    moveToErrors(filepath, errorDir);
                    emit statusSignal(filepath, "Error", "Remux failed");
                    emit fileDoneSignal(filepath, "Failed", oldSize, 0);
                    return false;
                }
            }
        }

        // Calculate space reduction percentage
        double ratio = (static_cast<double>(oldSize - newSize) / oldSize) * 100.0;
        
        emit logSignal(QString("[SUCCESS] Compressed %1 (Saved %2 MB, %3% space reduction)")
                       .arg(baseName,
                            QString::number(static_cast<double>(oldSize - newSize) / (1024.0 * 1024.0), 'f', 1),
                            QString::number(ratio, 'f', 1)));

        // 7. Move original file to trash
        bool trashed = moveToTrash(filepath, trashDir);
        if (!trashed) {
            emit logSignal(QString("Attempting direct deletion of original file: %1").arg(filepath));
            trashed = QFile::remove(filepath); // Fallback delete
        }

        // Duplicate prevention safeguard: If original cannot be deleted, clean up output and abort!
        if (!trashed) {
            emit logSignal(QString("[ERROR] Failed to trash or delete original file: %1. Cleaning up temporary output to prevent duplicates.").arg(filepath));
            if (QFile::exists(tmpOut)) {
                QFile::remove(tmpOut);
            }
            emit statusSignal(filepath, "Error", "Original delete failed");
            emit fileDoneSignal(filepath, "Failed", oldSize, 0);
            return false;
        }

        // 8. Place the finished MKV file in its final directory location
        QString finalOut = QDir(fileDir).filePath(baseNoExt + ".mkv");
        
        // Remove collision before rename
        if (QFile::exists(finalOut)) {
            QFile::remove(finalOut);
        }
        if (!safeMove(QDir::toNativeSeparators(tmpOut), QDir::toNativeSeparators(finalOut))) {
            emit logSignal(QString("[ERROR] Failed to move temporary output to final location: %1").arg(finalOut));
            moveToErrors(filepath, errorDir);
            emit statusSignal(filepath, "Error", "File move failed");
            emit fileDoneSignal(filepath, "Failed", oldSize, 0);
            return false;
        }

        // 9. Record sizing statistics history in the global database
        {
            DatabaseManager db(m_dbPath);
            db.recordProcessedFile(finalOut, oldSize, newSize, fileHash);
        }
        emit fileDoneSignal(filepath, "Completed", oldSize, newSize);
        return true;
    } else {
        // Clean up temporary output file
        if (QFile::exists(tmpOut)) {
            QFile::remove(tmpOut);
        }

        if (!m_isRunning) {
            emit logSignal(QString("Transcoding cancelled by user: %1").arg(baseName));
            emit statusSignal(filepath, "Pending", "Cancelled");
            emit fileDoneSignal(filepath, "Cancelled", fileInfo.size(), 0);
            return false;
        }

        // True FFmpeg execution failure
        emit logSignal(QString("[ERROR] Transcoding failed for %1").arg(baseName));
        moveToErrors(filepath, errorDir);
        emit statusSignal(filepath, "Error", "FFmpeg failure");
        emit fileDoneSignal(filepath, "Failed", fileInfo.size(), 0);
        return false;
    }
}

// Executes the FFmpeg command line process in the background.
// Parses stdout/stderr logs in real-time to compute progress percentages, ETA, speeds,
// and outputs live preview signals.
bool TranscodeWorker::runFfmpegProcess(const QStringList &cmd, double duration, const QString &filepath)
{
    m_activeProcess = new QProcess();
    m_activeProcess->setProgram(cmd[0]); // First item is the binary name ("ffmpeg")
    m_activeProcess->setArguments(cmd.mid(1)); // Remaining items are parameters
    
    // Merge standard output and standard error pipelines into a single stream.
    // Under Linux, FFmpeg writes progress info to stderr. Merging channels allows
    // real-time reading from a single descriptor.
    m_activeProcess->setProcessChannelMode(QProcess::MergedChannels);

    m_activeProcess->start();
    if (!m_activeProcess->waitForStarted()) {
        delete m_activeProcess;
        m_activeProcess = nullptr;
        return false;
    }

    QByteArray buffer;
    double lastFps = 0.0;
    double lastSpeed = 0.0;
    QString lastEta = "N/A";
    double lastPreviewTime = -999.0;

    // Continue loop while process is running or data remains in buffer
    while (m_activeProcess->state() == QProcess::Running || m_activeProcess->bytesAvailable() > 0) {
        // Wait up to 100ms for incoming data
        if (m_activeProcess->waitForReadyRead(100) || m_activeProcess->bytesAvailable() > 0) {
            buffer.append(m_activeProcess->readAll());
            
            // Read lines from buffer (split by \r or \n)
            int index;
            while (true) {
                int idxN = buffer.indexOf('\n');
                int idxR = buffer.indexOf('\r');
                if (idxN != -1 && idxR != -1) {
                    index = qMin(idxN, idxR);
                } else if (idxN != -1) {
                    index = idxN;
                } else {
                    index = idxR;
                }
                if (index == -1) break; // Incomplete line, wait for more data

                QByteArray lineBytes = buffer.left(index);
                buffer.remove(0, index + 1);
                
                QString line = QString::fromUtf8(lineBytes).trimmed();
                if (line.isEmpty()) continue;

                // Log filters: identify progress lines and compiler headers
                bool isProgress = line.contains("frame=") || line.contains("size=") || line.contains("time=");
                bool isHeader = line.startsWith("ffmpeg version") || line.startsWith("built with") || 
                                line.startsWith("configuration:") || line.startsWith("libav");
                                
                if (isProgress) {
                    emit logSignal(QString("[DEBUG PROGRESS] Line: \"%1\"").arg(line));
                }
                if (!isProgress && !isHeader) {
                    emit logSignal(line); // Emit detailed milestone logs
                }

                // 7. Parse progress variables from the FFmpeg ticker line
                if (isProgress && duration > 0.0) {
                    // Extract time elapsed (matches formats like time=00:01:23.45)
                    QRegularExpression timeRe("time=(\\d+):(\\d+):(\\d+(?:\\.\\d+)?)");
                    QRegularExpressionMatch timeMatch = timeRe.match(line);
                    if (timeMatch.hasMatch()) {
                        int h = timeMatch.captured(1).toInt();
                        int m = timeMatch.captured(2).toInt();
                        double s = timeMatch.captured(3).toDouble();
                        double secs = h * 3600.0 + m * 60.0 + s;
                        
                        // If live previews are enabled, trigger frame thumbnail extraction
                        // every 3 seconds of video timeline duration
                        if (m_livePreviewEnabled) {
                            if (secs >= lastPreviewTime + 3.0) {
                                emit previewFrameSignal(filepath, secs);
                                lastPreviewTime = secs;
                            }
                        }
                        
                        // Calculate percentage completion
                        int pct = static_cast<int>((secs / duration) * 100.0);

                        // Extract frames per second (fps)
                        QRegularExpression fpsRe("fps=\\s*(\\d+(\\.\\d+)?)");
                        QRegularExpressionMatch fpsMatch = fpsRe.match(line);
                        if (fpsMatch.hasMatch()) {
                            lastFps = fpsMatch.captured(1).toDouble();
                        }

                        // Extract speed factor (e.g. speed=2.3x)
                        QRegularExpression speedRe("speed=\\s*(\\d+(\\.\\d+)?)x");
                        QRegularExpressionMatch speedMatch = speedRe.match(line);
                        if (speedMatch.hasMatch()) {
                            lastSpeed = speedMatch.captured(1).toDouble();
                        }

                        // Calculate Estimated Time of Arrival (ETA)
                        if (lastSpeed > 0.0) {
                            double remainingSecs = qMax(0.0, duration - secs);
                            int etaVal = static_cast<int>(remainingSecs / lastSpeed);
                            if (etaVal >= 3600) {
                                int eh = etaVal / 3600;
                                int em = (etaVal % 3600) / 60;
                                int es = etaVal % 60;
                                lastEta = QString("%1:%2:%3")
                                            .arg(eh, 2, 10, QChar('0'))
                                            .arg(em, 2, 10, QChar('0'))
                                            .arg(es, 2, 10, QChar('0'));
                            } else {
                                int em = etaVal / 60;
                                int es = etaVal % 60;
                                lastEta = QString("%1:%2")
                                            .arg(em, 2, 10, QChar('0'))
                                            .arg(es, 2, 10, QChar('0'));
                            }
                        }

                        // Calculate current temporary file size on disk and project final size
                        qint64 outSizeBytes = 0;
                        try {
                            outSizeBytes = QFileInfo(cmd.last()).size();
                        } catch (...) {}
                        double outSizeMb = static_cast<double>(outSizeBytes) / (1024.0 * 1024.0);
                        double projectedSizeMb = 0.0;
                        if (pct >= 3) {
                            projectedSizeMb = (outSizeMb / pct) * 100.0;
                        }

                        // Send progress update signal to UI cards
                        emit progressSignal(filepath, qMin(pct, 99), lastFps, lastSpeed, lastEta, outSizeMb, projectedSizeMb);
                    }
                }
            }
        }

        // Handle queue cancellations
        if (!m_isRunning) {
            m_activeProcess->kill(); // Send terminate signal
            m_activeProcess->waitForFinished();
            break;
        }
    }

    int returnCode = m_activeProcess->exitCode();
    delete m_activeProcess;
    m_activeProcess = nullptr;

    return returnCode == 0;
}

// Moves original files to the workspace's trash folder (.Trash), preserving subdirectories.
bool TranscodeWorker::moveToTrash(const QString &filepath, const QString &trashDir)
{
    try {
        QFileInfo fi(filepath);
        QString fileDir = fi.absolutePath();
        QString relDir = QDir(m_rootDir).relativeFilePath(fileDir);
        
        // Re-construct the relative directory hierarchy inside the Trash folder
        QString targetTrashDir = trashDir;
        if (!relDir.isEmpty() && relDir != "." && relDir != "..") {
            targetTrashDir = QDir(trashDir).filePath(relDir);
        }

        // Convert path separators to native format to support Windows UNC network shares
        QDir().mkpath(QDir::toNativeSeparators(targetTrashDir));
        QString baseName = fi.fileName();
        QString dest = QDir(targetTrashDir).filePath(baseName);
        
        // If a file with the same name already exists in trash, append the file's modification timestamp to prevent collision
        if (QFile::exists(dest)) {
            QString baseNoExt = fi.completeBaseName();
            QString ext = fi.suffix();
            qint64 mtime = fi.lastModified().toSecsSinceEpoch();
            dest = QDir(targetTrashDir).filePath(QString("%1_%2.%3").arg(baseNoExt, QString::number(mtime), ext));
        }

        QString nativeSrc = QDir::toNativeSeparators(filepath);
        QString nativeDest = QDir::toNativeSeparators(dest);

        if (safeMove(nativeSrc, nativeDest)) {
            emit logSignal(QString("Original file moved to Trash: %1").arg(QDir(trashDir).relativeFilePath(dest)));
            return true;
        } else {
            emit logSignal(QString("[WARN] Failed to move %1 to Trash").arg(baseName));
        }
    } catch (...) {
        emit logSignal(QString("[WARN] Failed to move %1 to Trash").arg(QFileInfo(filepath).fileName()));
    }
    return false;
}

// Moves failed source files to the errors folder (.Errors), preserving folder subdirectories.
void TranscodeWorker::moveToErrors(const QString &filepath, const QString &errorDir)
{
    try {
        QFileInfo fi(filepath);
        QString fileDir = fi.absolutePath();
        QString relDir = QDir(m_rootDir).relativeFilePath(fileDir);
        
        // Re-construct relative directory hierarchy inside the Errors folder
        QString targetErrorDir = errorDir;
        if (!relDir.isEmpty() && relDir != "." && relDir != "..") {
            targetErrorDir = QDir(errorDir).filePath(relDir);
        }

        // Convert path separators to native format
        QDir().mkpath(QDir::toNativeSeparators(targetErrorDir));
        QString baseName = fi.fileName();
        QString dest = QDir(targetErrorDir).filePath(baseName);
        
        // Append modification timestamp if file name collision occurs
        if (QFile::exists(dest)) {
            QString baseNoExt = fi.completeBaseName();
            QString ext = fi.suffix();
            qint64 mtime = fi.lastModified().toSecsSinceEpoch();
            dest = QDir(targetErrorDir).filePath(QString("%1_%2.%3").arg(baseNoExt, QString::number(mtime), ext));
        }

        QString nativeSrc = QDir::toNativeSeparators(filepath);
        QString nativeDest = QDir::toNativeSeparators(dest);

        if (safeMove(nativeSrc, nativeDest)) {
            emit logSignal(QString("Failed file moved to .Errors: %1").arg(QDir(errorDir).relativeFilePath(dest)));
        } else {
            emit logSignal(QString("[WARN] Failed to move %1 to Errors").arg(baseName));
        }
    } catch (...) {
        emit logSignal(QString("[WARN] Failed to move %1 to Errors").arg(QFileInfo(filepath).fileName()));
    }
}
