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

// Helper: safe cross-volume file move
static bool safeMove(const QString &src, const QString &dest) {
    if (QFile::rename(src, dest)) return true;
    if (QFile::copy(src, dest)) {
        return QFile::remove(src);
    }
    return false;
}

QString findDependency(const QString &name) {
    QString path = QStandardPaths::findExecutable(name);
    if (!path.isEmpty()) return path;

    QString localPath = QCoreApplication::applicationDirPath() + "/" + name;
#ifdef Q_OS_WIN
    localPath += ".exe";
#endif
    if (QFile::exists(localPath)) return localPath;
    return "";
}

QString computeFastHash(const QString &filepath) {
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) return "";
    QCryptographicHash hash(QCryptographicHash::Sha1);
    QByteArray chunk = file.read(4 * 1024 * 1024); // 4MB
    hash.addData(chunk);
    return QString(hash.result().toHex());
}

bool probeFileCompliance(const QString &filepath, const QString &ffprobeBin) {
    QFileInfo fi(filepath);
    if (fi.suffix().toLower() != "mkv") return false;

    QProcess proc;
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
            for (const QJsonValue &val : streams) {
                QString codec = val.toObject()["codec_name"].toString().toLower();
                if (codec == "hevc") hasHevc = true;
                if (codec == "aac") hasAac = true;
            }
            return (hasHevc && hasAac);
        }
    }
    return false;
}

static bool detectFdkAac(const QString &ffmpegBin) {
    QProcess proc;
    proc.start(ffmpegBin, {"-encoders"});
    if (proc.waitForFinished()) {
        QString out = QString::fromUtf8(proc.readAllStandardOutput());
        return out.contains("libfdk_aac");
    }
    return false;
}

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

static bool probeInterlaced(const QString &filepath, const QString &ffmpegBin) {
    QProcess proc;
    proc.start(ffmpegBin, {
        "-filter_threads", "4",
        "-i", filepath,
        "-filter:v", "idet",
        "-frames:v", "360",
        "-an",
        "-f", "null",
        "-"
    });
    if (proc.waitForFinished()) {
        QString err = QString::fromUtf8(proc.readAllStandardError());
        QRegularExpression re("Multi frame detection:\\s*TFF:\\s*(\\d+)\\s*BFF:\\s*(\\d+)\\s*Progressive:\\s*(\\d+)");
        QRegularExpressionMatch match = re.match(err);
        if (match.hasMatch()) {
            int tff = match.captured(1).toInt();
            int bff = match.captured(2).toInt();
            int prog = match.captured(3).toInt();
            int interlaced = tff + bff;
            if (interlaced > prog * 2) {
                return true;
            }
        }
    }
    return false;
}

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

TranscodeWorker::~TranscodeWorker()
{
    stop();
}

void TranscodeWorker::stop()
{
    m_isRunning = false;
}

void TranscodeWorker::setLivePreviewEnabled(bool enabled)
{
    m_livePreviewEnabled = enabled;
}

void TranscodeWorker::run()
{
    m_isRunning = true;
    QString ffmpegBin = findDependency("ffmpeg");
    QString ffprobeBin = findDependency("ffprobe");

    if (ffmpegBin.isEmpty() || ffprobeBin.isEmpty()) {
        emit logSignal("[ERROR] ffmpeg or ffprobe dependency is missing. Cannot process.");
        emit finishedSignal();
        return;
    }

    QString trashDir = QDir(m_rootDir).filePath(".Trash");
    QString errorDir = QDir(m_rootDir).filePath(".Errors");

    // Detect FDK-AAC support
    m_hasFdk = detectFdkAac(ffmpegBin);
    emit logSignal(QString("[NOTICE] Audio encoder: %1").arg(m_hasFdk ? "libfdk_aac (preferred)" : "aac (native fallback)"));

    for (const QString &filepath : m_fileQueue) {
        if (!m_isRunning) break;

        try {
            processFile(filepath, ffmpegBin, ffprobeBin, trashDir, errorDir);
        } catch (const std::exception &e) {
            emit logSignal(QString("[ERROR] Exception processing %1: %2").arg(QFileInfo(filepath).fileName(), e.what()));
            moveToErrors(filepath, errorDir);
            emit statusSignal(filepath, "Error", "Critical exception");
        }
    }

    emit finishedSignal();
}

bool TranscodeWorker::processFile(const QString &filepath, const QString &ffmpegBin, const QString &ffprobeBin, const QString &trashDir, const QString &errorDir)
{
    QFileInfo fileInfo(filepath);
    QString baseName = fileInfo.fileName();
    QString baseNoExt = fileInfo.completeBaseName();
    QString fileDir = fileInfo.absolutePath();

    emit logSignal(QString("\n[START] Processing file: %1").arg(baseName));
    emit statusSignal(filepath, "Processing", "Preparing...");

    // Compute Fast Hash
    QString fileHash = computeFastHash(filepath);
    if (fileHash.isEmpty()) {
        emit logSignal("[ERROR] Failed to compute file hash. Skipping.");
        emit statusSignal(filepath, "Error", "Hash failed");
        return false;
    }

    // Probe file metadata using unified JSON ffprobe check
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

    // Auto Deinterlace analysis (scan data governs decision)
    bool isInterlaced = probeInterlaced(filepath, ffmpegBin);
    emit logSignal(QString("Scan results: %1").arg(isInterlaced ? "Interlaced frames detected" : "Progressive scan detected"));

    // Compare scan results with container metadata field_order
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

    // Determine target parameters
    bool isAlreadyHevc = (meta.vcodec == "hevc");
    bool isAlreadyAac = (meta.acodec == "aac" || !meta.hasAudio);
    double effectiveFps = isInterlaced ? (meta.fps * 2.0) : meta.fps;
    bool bobbed = (effectiveFps >= 50.0 && m_settings["debob"].toBool());
    bool isPortrait = (meta.height > meta.width);
    bool needsDownscale = false;
    if (m_settings["downscale"].toBool()) {
        if (isPortrait) {
            needsDownscale = (meta.width > 1080 || meta.height > 1920);
        } else {
            needsDownscale = (meta.height > 1080 || meta.width > 1920);
        }
    }

    QString suffix = fileInfo.suffix().toLower();
    bool isObsoleteFormat = (suffix == "wmv" || suffix == "flv" || suffix == "avi" || suffix == "asf" || suffix == "f4v" || suffix == "divx");

    QString wantVideoCodec = (isAlreadyHevc && !needsDownscale && !bobbed && !isInterlaced && !isObsoleteFormat) ? "copy" : "libx265";
    QString wantAudioCodec = (isAlreadyAac && !isObsoleteFormat) ? "copy" : "encode";

    // Prepare transcode commands
    QString tmpOut = QDir(fileDir).filePath(baseNoExt + ".tmp_out.mkv");
    
    // Build arguments
    QStringList cmdArgs;
    cmdArgs << "-y" << "-filter_threads" << "4" << "-i" << filepath;

    // Video compression and filter setup
    if (wantVideoCodec == "copy") {
        cmdArgs << "-c:v" << "copy";
    } else {
        // Set the default video codec for all video streams to copy (protects cover art pictures)
        // and override the first video stream (v:0) to transcode to libx265.
        cmdArgs << "-c:v" << "copy"
                << "-c:v:0" << "libx265"
                << "-preset" << m_settings["preset"].toString()
                << "-crf" << QString::number(m_settings["crf"].toInt())
                << "-x265-params" << "profile=main10:no-sao=1:selective-sao=0:pmode=1:pme=1";

        // Build video filters for encoding (specifically apply only to v:0)
        QStringList videoFilters;
        if (isInterlaced) {
            videoFilters << "bwdif=mode=send_field:parity=-1:deint=all";
        }
        if (needsDownscale) {
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
            double targetFps = effectiveFps / 2.0;
            videoFilters << QString("fps=fps=%1:round=down").arg(QString::number(targetFps, 'f', 6));
        }
        videoFilters << "format=yuv420p10le";
        
        cmdArgs << "-filter:v:0" << videoFilters.join(",");
        emit logSignal(QString("Video filters applied (to stream 0:v:0): %1").arg(videoFilters.join(",")));
    }

    // Audio compression setup
    if (wantAudioCodec == "copy") {
        cmdArgs << "-c:a" << "copy";
    } else {
        if (m_hasFdk) {
            cmdArgs << "-c:a" << "libfdk_aac" << "-vbr" << "4";
        } else {
            cmdArgs << "-c:a" << "aac" << "-q:a" << "1.5";
        }
    }

    // Map all video streams (main video + cover art images)
    cmdArgs << "-c:s" << "copy"
            << "-map" << "0:v"
            << "-map" << "0:a?"
            << "-map" << "0:s?"
            << "-map" << "0:t?";
    cmdArgs << "-metadata" << "title=" + baseNoExt;
    if (!meta.displayAspectRatio.isEmpty() && meta.displayAspectRatio != "0:1") {
        cmdArgs << "-aspect" << meta.displayAspectRatio;
    }
    cmdArgs << tmpOut;

    // Execute transcode process
    emit logSignal(QString("Executing: %1 %2").arg(ffmpegBin, cmdArgs.join(" ")));
    emit logSignal(QString("Encoding to: %1").arg(tmpOut));
    emit statusSignal(filepath, "Processing", "Transcoding...");

    bool success = runFfmpegProcess(QStringList() << ffmpegBin << cmdArgs, meta.duration, filepath);

    if (success && QFile::exists(tmpOut)) {
        // Successful transcode
        qint64 oldSize = fileInfo.size();
        qint64 newSize = QFileInfo(tmpOut).size();
        
        // If we re-encoded the video and the file grew, discard the transcode to prevent bloat!
        if (wantVideoCodec == "libx265" && newSize >= oldSize) {
            emit logSignal(QString("[NOTICE] Transcoded file (%1 MB) is larger than or equal to original (%2 MB). Discarding bloated transcode.")
                           .arg(QString::number(static_cast<double>(newSize)/(1024.0*1024.0), 'f', 1),
                                QString::number(static_cast<double>(oldSize)/(1024.0*1024.0), 'f', 1)));
            QFile::remove(tmpOut);
            
            QString finalOut = QDir(fileDir).filePath(baseNoExt + ".mkv");
            
            if (fileInfo.suffix().toLower() == "mkv") {
                // If original is already MKV, keep it in place
                {
                    DatabaseManager db(m_dbPath);
                    db.recordProcessedFile(filepath, oldSize, oldSize, fileHash);
                }
                emit statusSignal(filepath, "Skipped", "Original kept (transcode grew)");
                emit fileDoneSignal(filepath, "Skipped", oldSize, oldSize);
                return true;
            } else {
                // If original is not MKV, remux it to MKV container
                emit logSignal(QString("Remuxing original %1 directly to MKV container (stream copy)...").arg(baseName));
                
                QProcess remuxProc;
                remuxProc.start(ffmpegBin, {
                    "-y", "-i", filepath,
                    "-c", "copy", "-map", "0",
                    finalOut
                });
                
                if (remuxProc.waitForFinished() && remuxProc.exitCode() == 0 && QFile::exists(finalOut)) {
                    bool trashed = moveToTrash(filepath, trashDir);
                    if (!trashed) {
                        emit logSignal(QString("Attempting direct deletion of original file: %1").arg(filepath));
                        trashed = QFile::remove(filepath);
                    }

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

        double ratio = (static_cast<double>(oldSize - newSize) / oldSize) * 100.0;
        
        emit logSignal(QString("[SUCCESS] Compressed %1 (Saved %2 MB, %3% space reduction)")
                       .arg(baseName,
                            QString::number(static_cast<double>(oldSize - newSize) / (1024.0 * 1024.0), 'f', 1),
                            QString::number(ratio, 'f', 1)));

        // Move original to trash, rename temporary output to final
        bool trashed = moveToTrash(filepath, trashDir);
        if (!trashed) {
            emit logSignal(QString("Attempting direct deletion of original file: %1").arg(filepath));
            trashed = QFile::remove(filepath);
        }

        if (!trashed) {
            emit logSignal(QString("[ERROR] Failed to trash or delete original file: %1. Cleaning up temporary output to prevent duplicates.").arg(filepath));
            if (QFile::exists(tmpOut)) {
                QFile::remove(tmpOut);
            }
            emit statusSignal(filepath, "Error", "Original delete failed");
            emit fileDoneSignal(filepath, "Failed", oldSize, 0);
            return false;
        }

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

        // Record in database
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

        // True FFmpeg failure
        emit logSignal(QString("[ERROR] Transcoding failed for %1").arg(baseName));
        moveToErrors(filepath, errorDir);
        emit statusSignal(filepath, "Error", "FFmpeg failure");
        emit fileDoneSignal(filepath, "Failed", fileInfo.size(), 0);
        return false;
    }
}

bool TranscodeWorker::runFfmpegProcess(const QStringList &cmd, double duration, const QString &filepath)
{
    m_activeProcess = new QProcess();
    m_activeProcess->setProgram(cmd[0]);
    m_activeProcess->setArguments(cmd.mid(1));
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

    while (m_activeProcess->state() == QProcess::Running || m_activeProcess->bytesAvailable() > 0) {
        if (m_activeProcess->waitForReadyRead(100) || m_activeProcess->bytesAvailable() > 0) {
            buffer.append(m_activeProcess->readAll());
            
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
                if (index == -1) break;

                QByteArray lineBytes = buffer.left(index);
                buffer.remove(0, index + 1);
                
                QString line = QString::fromUtf8(lineBytes).trimmed();
                if (line.isEmpty()) continue;

                // Log filters: block FFmpeg verbosity & frame status ticker
                bool isProgress = line.contains("frame=") || line.contains("size=") || line.contains("time=");
                bool isHeader = line.startsWith("ffmpeg version") || line.startsWith("built with") || 
                                line.startsWith("configuration:") || line.startsWith("libav");
                                
                if (isProgress) {
                    emit logSignal(QString("[DEBUG PROGRESS] Line: \"%1\"").arg(line));
                }
                if (!isProgress && !isHeader) {
                    emit logSignal(line);
                }

                // Parse progressive values
                if (isProgress && duration > 0.0) {
                    // Extract time (handles optional decimal seconds)
                    QRegularExpression timeRe("time=(\\d+):(\\d+):(\\d+(?:\\.\\d+)?)");
                    QRegularExpressionMatch timeMatch = timeRe.match(line);
                    if (timeMatch.hasMatch()) {
                        int h = timeMatch.captured(1).toInt();
                        int m = timeMatch.captured(2).toInt();
                        double s = timeMatch.captured(3).toDouble();
                        double secs = h * 3600.0 + m * 60.0 + s;
                        if (m_livePreviewEnabled) {
                            if (secs >= lastPreviewTime + 3.0) {
                                emit previewFrameSignal(filepath, secs);
                                lastPreviewTime = secs;
                            }
                        }
                        int pct = static_cast<int>((secs / duration) * 100.0);

                        // Extract fps
                        QRegularExpression fpsRe("fps=\\s*(\\d+(\\.\\d+)?)");
                        QRegularExpressionMatch fpsMatch = fpsRe.match(line);
                        if (fpsMatch.hasMatch()) {
                            lastFps = fpsMatch.captured(1).toDouble();
                        }

                        // Extract speed
                        QRegularExpression speedRe("speed=\\s*(\\d+(\\.\\d+)?)x");
                        QRegularExpressionMatch speedMatch = speedRe.match(line);
                        if (speedMatch.hasMatch()) {
                            lastSpeed = speedMatch.captured(1).toDouble();
                        }

                        // Calculate ETA
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

                        // Get current and projected size on disk
                        qint64 outSizeBytes = 0;
                        try {
                            outSizeBytes = QFileInfo(cmd.last()).size();
                        } catch (...) {}
                        double outSizeMb = static_cast<double>(outSizeBytes) / (1024.0 * 1024.0);
                        double projectedSizeMb = 0.0;
                        if (pct >= 3) {
                            projectedSizeMb = (outSizeMb / pct) * 100.0;
                        }

                        emit progressSignal(filepath, qMin(pct, 99), lastFps, lastSpeed, lastEta, outSizeMb, projectedSizeMb);
                    }
                }
            }
        }

        if (!m_isRunning) {
            m_activeProcess->kill();
            m_activeProcess->waitForFinished();
            break;
        }
    }

    int returnCode = m_activeProcess->exitCode();
    delete m_activeProcess;
    m_activeProcess = nullptr;

    return returnCode == 0;
}

bool TranscodeWorker::moveToTrash(const QString &filepath, const QString &trashDir)
{
    try {
        QFileInfo fi(filepath);
        QString fileDir = fi.absolutePath();
        QString relDir = QDir(m_rootDir).relativeFilePath(fileDir);
        
        // Target folder inside trashDir preserving folder structure
        QString targetTrashDir = trashDir;
        if (!relDir.isEmpty() && relDir != "." && relDir != "..") {
            targetTrashDir = QDir(trashDir).filePath(relDir);
        }

        // Convert target directory to native separators to support UNC network shares on Windows
        QDir().mkpath(QDir::toNativeSeparators(targetTrashDir));
        QString baseName = fi.fileName();
        QString dest = QDir(targetTrashDir).filePath(baseName);
        
        if (QFile::exists(dest)) {
            QString baseNoExt = fi.completeBaseName();
            QString ext = fi.suffix();
            qint64 mtime = fi.lastModified().toSecsSinceEpoch();
            dest = QDir(targetTrashDir).filePath(QString("%1_%2.%3").arg(baseNoExt, QString::number(mtime), ext));
        }

        // Convert source and destination to native separators before moving
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

void TranscodeWorker::moveToErrors(const QString &filepath, const QString &errorDir)
{
    try {
        QFileInfo fi(filepath);
        QString fileDir = fi.absolutePath();
        QString relDir = QDir(m_rootDir).relativeFilePath(fileDir);
        
        // Target folder inside errorDir preserving folder structure
        QString targetErrorDir = errorDir;
        if (!relDir.isEmpty() && relDir != "." && relDir != "..") {
            targetErrorDir = QDir(errorDir).filePath(relDir);
        }

        // Convert target directory to native separators to support UNC network shares on Windows
        QDir().mkpath(QDir::toNativeSeparators(targetErrorDir));
        QString baseName = fi.fileName();
        QString dest = QDir(targetErrorDir).filePath(baseName);
        
        if (QFile::exists(dest)) {
            QString baseNoExt = fi.completeBaseName();
            QString ext = fi.suffix();
            qint64 mtime = fi.lastModified().toSecsSinceEpoch();
            dest = QDir(targetErrorDir).filePath(QString("%1_%2.%3").arg(baseNoExt, QString::number(mtime), ext));
        }

        // Convert source and destination to native separators before moving
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
