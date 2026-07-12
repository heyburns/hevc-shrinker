#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow> // Core window container class
#include <QTableWidget> // Grid-like spreadsheet table for queue display
#include <QTextEdit> // Multi-line log terminal widget
#include <QGroupBox> // Framed title container grouping widgets together
#include <QProgressBar> // Graphical progress bar indicator
#include <QLabel> // Plain text display widget
#include <QLineEdit> // Single-line text input field (holds folder path)
#include <QPushButton> // Clickable button trigger
#include <QSpinBox> // Numeric selector input with up/down arrows
#include <QComboBox> // Drop-down menu list selector
#include <QCheckBox> // Toggleable checkbox selector
#include <QVariantMap> // Dictionary holding generic setting values
#include <QStringList> // Resizable list of string elements
#include <QProcess> // Runs background executables (thumbnail seekers)
#include <QHBoxLayout> // Horizontal layout manager aligning widgets left-to-right
#include "databasemanager.h" // Database wrapper header
#include "transcodeworker.h" // Background thread header

// Structure representing a video file scanned inside the target folder
struct ScannedFile {
    QString absolutePath;   // Absolute local path (e.g. "/home/user/videos/movie.mp4")
    QString filename;       // Just the filename (e.g. "movie.mp4")
    QString relPath;        // Path relative to scanned folder (e.g. "subfolder/movie.mp4")
    qint64 size = 0;        // Size in bytes
    qint64 lastModified = 0;// Last modified UNIX timestamp (used to detect changes)
};

// UI Widget representing an active progress display card at the bottom.
// Spawns dynamically for each concurrent transcode thread, hosting its own progress bar,
// statistics panel, and live thumbnail preview picture.
class TranscodeMonitorCard : public QGroupBox {
    Q_OBJECT
public:
    // Constructor. Creates widgets, layouts, and preview timers for the given file.
    TranscodeMonitorCard(const QString &filepath, QWidget *parent = nullptr);
    
    // Destructor. Stops active thumbnail seekers.
    ~TranscodeMonitorCard();

    // Updates text strings and percentages inside the card during transcoding.
    void updateProgress(int percentage, double fps, double speed, const QString &etaStr, double outSizeMb, double projectedSizeMb);
    
    // Updates status messages (e.g., "Analyzing...", "Completed").
    void updateStatus(const QString &status, const QString &details);
    
    // Triggers an asynchronous seeking request to extract a frame thumbnail at a given second.
    void requestFramePreview(double secs);
    
    // Hides/shows the preview thumbnail and resizes the card's width dynamically.
    void setPreviewVisible(bool visible);

private slots:
    // Receives image byte chunks extracted by FFmpeg.
    void onPreviewDataAvailable();
    
    // Callback when thumbnail extraction finishes. Renders pixmap image onto label.
    void onPreviewProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    QString m_filepath;              // Absolute path to the video file monitored by this card
    QLabel *m_lblFile;              // Title label showing filename
    QLabel *m_lblPerf;              // Stats label showing speed and frames-per-second
    QLabel *m_lblTime;              // Stats label showing ETA and elapsed time
    QLabel *m_lblSize;              // Stats label showing current and projected file sizes
    QProgressBar *m_progressBar;    // Progress bar widget
    QLabel *m_lblPreview;           // Square label containing the live video preview frame

    QProcess *m_previewProcess;     // Background process runner extracting thumbnail frames
    QByteArray m_previewBuffer;     // Memory buffer loading image byte chunks
    qint64 m_startTime;             // Millisecond timestamp when transcode began
};

// The primary GUI application class governing layouts, signals, slots, scanning logic,
// dashboard updates, and multi-process thread scheduling.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // Constructor. Instantiates GUI layouts, connects triggers, checks dependencies.
    MainWindow(QWidget *parent = nullptr);
    
    // Destructor. Purges worker processes and cleans connections.
    ~MainWindow();

private slots:
    // Triggered by "Browse" button. Displays directory choice dialog.
    void selectDirectory();
    
    // Triggered by "Scan" button. Iterates folder, updates database compliance cache, builds queue.
    void scanDirectory();
    
    // Triggered by "Start Queue" button. Locks UI controls, dispatches transcode jobs.
    void startProcessing();
    
    // Triggered by "Abort Job" button. Terminates active processes and stops queue dispatching.
    void stopProcessing();
    
    // Restores settings back to default values.
    void resetSettings();
    
    // Purges cache database scan records.
    void onResetDbClicked();
    
    // Purges sizing stats history (resetting scoreboard back to 0).
    void onResetScoreboardClicked();
    
    // Appends message strings to the log terminal text console.
    void logMessage(const QString &message);
    
    // Routes progress status updates from background threads to corresponding monitor cards.
    void updateProgress(const QString &filepath, int percentage, double fps, double speed, const QString &etaStr, double outSizeMb, double projectedSizeMb);
    
    // Routes status text updates to corresponding monitor cards.
    void updateStatus(const QString &filepath, const QString &status, const QString &details);
    
    // Called when a worker finishes transcoding. Handles database recordings and grid table color codes.
    void fileDone(const QString &filepath, const QString &status, qint64 oldSize, qint64 newSize);
    
    // Cleans layouts and unlocks GUI controls when all queue items finish.
    void processingFinished();
    
    // Spawns a background worker thread for the next pending video file in the queue.
    void startNextQueueJob();
    
    // Cleans up worker thread handles and updates active queue slots.
    void onWorkerFinished();
    
    // Dispatches frame seek extraction commands when worker threads request preview updates.
    void onWorkerPreviewFrameRequested(const QString &filepath, double secs);
    
    // Dynamically collapses or expands preview modules inside active cards when checked.
    void togglePreview(bool checked);
    
    // Adjusts worker slots on the fly when concurrent setting values change mid-queue.
    void onConcurrentChanged(int val);
    
    // Displays a right-click context menu containing a "Play Video" option.
    void showTableContextMenu(const QPoint &pos);

private:
    // GUI Controls
    QLineEdit *m_dirInput;           // Folder path entry field
    QPushButton *m_btnBrowse;        // Directory lookup button
    QPushButton *m_btnScan;          // Catalog cataloging button
    QPushButton *m_btnStart;         // Start processing queue button
    QPushButton *m_btnStop;          // Cancellation/abort button
    QPushButton *m_btnReset;         // Reset configuration settings button
    QPushButton *m_btnResetDb;       // Wipe scan database cache button
    QPushButton *m_btnResetScoreboard;// Wipe sizing scoreboard stats button

    QSpinBox *m_spinCrf;             // CRF encoder quality setting (0-51)
    QComboBox *m_comboPreset;        // Speed preset selector (ultrafast-veryslow)
    QCheckBox *m_chkDownscale;       // Downscaling checkbox (UHD to 1080p)
    QCheckBox *m_chkDebob;           // Deinterlacing frame-rate bob checkbox
    QCheckBox *m_chkPreview;         // Enable live thumbnail preview checkbox
    QSpinBox *m_spinConcurrent;      // Max concurrent encodes selector (1-16)

    QLabel *m_lblFfmpegStatus;       // FFmpeg status check indicator
    QLabel *m_lblFfprobeStatus;      // FFprobe status check indicator

    QLabel *m_lblDashboardOrig;      // Statistics: Original size saved text
    QLabel *m_lblDashboardComp;      // Statistics: Compressed size saved text
    QLabel *m_lblDashboardSaved;     // Statistics: Space saved saved text
    QLabel *m_lblDashboardPct;       // Statistics: Savings percentage saved text

    QTableWidget *m_tableQueue;      // Queue grid table displaying files and states

    // Scrollable bottom layout hosting dynamic worker monitor cards
    QWidget *m_monitorsContainer;
    QHBoxLayout *m_monitorsLayout;

    // Execution state and logic containers
    QString m_rootDir;                       // Absolute path to target scanned folder
    DatabaseManager *m_dbManager;            // Database controller object
    QList<TranscodeWorker*> m_workers;       // List of active background worker threads
    QHash<QString, TranscodeMonitorCard*> m_activeCards; // Map linking filepaths to active GUI cards
    QList<ScannedFile> m_scannedFiles;       // List of scanned files cataloged from directory
    QStringList m_activeTranscodeQueue;      // List of files queued for active transcoding
    QStringList m_pendingQueue;              // Thread safe queue of items awaiting dispatch
    bool m_isQueueRunning;                   // Flag indicating if queue processing is active

    // GUI Layout setup
    void initUi();
    
    // Verifies if FFmpeg and FFprobe binaries are available on the host system.
    void checkDependencies();
    
    // Recalculates workspace size savings and updates sidebar dashboard fields.
    void updateSavingsDashboard();
    
    // Searches row indexes mapping absolute filepaths inside the queue table.
    int findRowByFilepath(const QString &filepath);
    
    // Controls enabling state of start queue button based on scan state.
    void updateStartButtonState();
};

#endif // MAINWINDOW_H
