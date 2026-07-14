#ifndef TRANSCODEWORKER_H
#define TRANSCODEWORKER_H

#include <QThread> // Qt class representing execution threads
#include <QStringList> // Qt class representing a list of strings
#include <QVariantMap> // Qt class representing a dictionary of settings
#include <QProcess> // Qt class representing external processes (runs FFmpeg/FFprobe)
#include <atomic> // Standard C++ atomic types for thread-safe state flags

// Class representing a worker thread that handles the CPU-intensive transcoding pipeline.
// Subclassing QThread allows the GUI thread to remain responsive while video processing runs in the background.
class TranscodeWorker : public QThread {
    Q_OBJECT
public:
    // Constructor. Sets up target file lists, path environments, presets, and settings.
    TranscodeWorker(const QStringList &fileQueue, const QString &rootDir, const QString &dbPath, const QVariantMap &settings, QObject *parent = nullptr);
    
    // Destructor. Stops any active background processes and joins the thread.
    ~TranscodeWorker();

    // Requests the worker thread to stop processing. Terminating active transcode processes.
    void stop();
    
    // Dynamically enables/disables the live thumbnail preview generation loop.
    void setLivePreviewEnabled(bool enabled);

signals:
    // Signal sent to MainWindow to print detailed messages to the log text terminal.
    void logSignal(const QString &message);
    
    // Signal sent to update progress monitor stats (percentages, speeds, ETAs, and sizes).
    void progressSignal(const QString &filepath, int percentage, double fps, double speed, const QString &etaStr, double outSizeMb, double projectedSizeMb);
    
    // Signal sent to update the current action/status text (e.g. "Analyzing...", "Encoding...").
    void statusSignal(const QString &filepath, const QString &status, const QString &details);
    
    // Signal sent when a video finishes transcoding, updating database records and file tables.
    void fileDoneSignal(const QString &filepath, const QString &status, qint64 oldSize, qint64 newSize);
    
    // Signal sent when this worker thread finishes executing and exits.
    void finishedSignal();
    
    // Signal sent to trigger a frame extraction request for the video thumbnail view.
    void previewFrameSignal(const QString &filepath, double secs);

protected:
    // The core execution loop of the worker thread. Runs in the background.
    void run() override;

private:
    QStringList m_fileQueue;                // List of absolute filepaths queued for transcode.
    QString m_rootDir;                      // Path to the active scanned folder root workspace.
    QString m_dbPath;                       // Path to the global SQLite database folder.
    QVariantMap m_settings;                 // Presets and settings (CRF, preset speed, etc.).
    std::atomic<bool> m_isRunning;          // Atomic flag governing the background loop execution.
    std::atomic<bool> m_livePreviewEnabled; // Atomic flag governing whether preview request signals are emitted.
    QProcess *m_activeProcess;              // Pointer to the currently executing FFmpeg process.
    bool m_hasFdk;                          // Flag indicating if the host's FFmpeg binary supports libfdk_aac.

    // Core method processing a single video file (probing metadata, running FFmpeg, verifying results, and trashing originals).
    bool processFile(const QString &filepath, const QString &ffmpegBin, const QString &ffprobeBin, const QString &trashDir, const QString &errorDir);
    
    // Invokes the FFmpeg transcode command and parses stdout/stderr logs in real-time to compute progress percentages.
    bool runFfmpegProcess(const QStringList &cmd, double duration, const QString &filepath);
    
    // Moves original files to the trash folder. Returns false if moving fails.
    bool moveToTrash(const QString &filepath, const QString &trashDir);
    
    // Moves files to the errors folder if the transcoding attempt fails.
    void moveToErrors(const QString &filepath, const QString &errorDir);
};

// Structure holding probed container metadata, video codecs, audio streams, frame dimensions, and timings.
struct VideoMetadata {
    QString vcodec;                 // Video codec name (e.g. "h264", "hevc")
    QString acodec;                 // Audio codec name (e.g. "aac", "ac3")
    int width = 0;                  // Video frame width in pixels
    int height = 0;                 // Video frame height in pixels
    double duration = 0.0;          // Total video duration in seconds
    double fps = 0.0;               // Frame rate (Frames Per Second)
    QString fieldOrder;             // Interlacing status (e.g. "progressive", "tt", "bb")
    QString displayAspectRatio;     // Aspect ratio (e.g. "16:9", "4:3")
    bool hasAudio = false;          // True if an audio stream is present in the file
};

// Locates utility executables (FFmpeg, FFprobe) in the system PATH or local folders.
QString findDependency(const QString &name);

// Generates a quick unique file signature (hash) by reading 10MB chunks of data.
QString computeFastHash(const QString &filepath);

// Evaluates a video file's stream codecs using FFprobe. Returns true if file is already compliant (H.265+AAC).
bool probeFileCompliance(const QString &filepath, const QString &ffprobeBin);

// Probes a file's container metadata (codecs, frame sizes, frame-rates) using FFprobe and parses JSON output.
VideoMetadata probeMetadata(const QString &filepath, const QString &ffprobeBin);

#endif // TRANSCODEWORKER_H
