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
#include <QFrame> // Frame container layout helper
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
class TranscodeMonitorCard : public QFrame {
    Q_OBJECT
public:
    // Constructor. Creates widgets and layouts for the given file.
    TranscodeMonitorCard(const QString &filepath, QWidget *parent = nullptr);
    
    // Destructor.
    ~TranscodeMonitorCard();

    // Updates text strings and percentages inside the card during transcoding.
    void updateProgress(int percentage, double fps, double speed, const QString &etaStr, double outSizeMb, double projectedSizeMb);
    
    // Updates status messages (e.g., "Analyzing...", "Completed").
    void updateStatus(const QString &status, const QString &details);

private:
    QString m_filepath;              // Absolute path to the video file monitored by this card
    QLabel *m_lblFile;              // Title label showing filename
    QLabel *m_lblPerf;              // Stats label showing speed and frames-per-second
    QLabel *m_lblTime;              // Stats label showing ETA and elapsed time
    QLabel *m_lblSize;              // Stats label showing current and projected file sizes
    QProgressBar *m_progressBar;    // Progress bar widget
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
    // File menu slots
    void selectDirectory();
    void onScanSelected();
    void onExitSelected();

    // View menu slots
    void onTogglePreviewSelected(bool checked);

    // Options menu slots
    void onCrfSelected();
    void onPresetSelected(QAction *action);
    void onConcurrencySelected();
    void onDownscaleToggled(bool checked);
    void onDebobToggled(bool checked);
    void onResetConfigSelected();
    
    // Help menu slots
    void onUsageGuideSelected();
    void onAboutSelected();

    // General slots
    void scanDirectory();
    void startProcessing();
    void stopProcessing();
    void resetSettings();
    void onResetDbClicked();
    void onResetScoreboardClicked();
    
    void logMessage(const QString &message);
    void updateProgress(const QString &filepath, int percentage, double fps, double speed, const QString &etaStr, double outSizeMb, double projectedSizeMb);
    void updateStatus(const QString &filepath, const QString &status, const QString &details);
    void fileDone(const QString &filepath, const QString &status, qint64 oldSize, qint64 newSize);
    void processingFinished();
    
    void startNextQueueJob();
    void onWorkerFinished();
    void onWorkerPreviewFrameRequested(const QString &filepath, double secs);
    void onConcurrentChanged(int val);
    void showTableContextMenu(const QPoint &pos);

    // Viewfinder slots
    void onViewfinderDataAvailable();
    void onViewfinderProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onQueueTableSelectionChanged();

private:
    // Menu elements
    QMenuBar *m_menuBar;
    QMenu *m_menuFile;
    QMenu *m_menuView;
    QMenu *m_menuOptions;
    QMenu *m_menuHelp;

    QAction *m_actShowPreview;
    QAction *m_actDownscale;
    QAction *m_actDebob;
    QActionGroup *m_presetGroup;

    // File Menu Actions
    QAction *m_actOpen;
    QAction *m_actScan;
    QAction *m_actExit;

    // Options Menu Actions
    QAction *m_actCrf;
    QMenu *m_menuPreset;
    QAction *m_actConcurrency;
    QAction *m_actResetConfig;

    // GUI Controls
    QPushButton *m_btnScan;          // Catalog cataloging button
    QPushButton *m_btnStart;         // Start processing queue button
    QPushButton *m_btnStop;          // Cancellation/abort button
    QPushButton *m_btnResetDb;       // Wipe scan database cache button
    QPushButton *m_btnResetScoreboard;// Wipe sizing scoreboard stats button

    QLabel *m_lblFfmpegStatus;       // FFmpeg status check indicator
    QLabel *m_lblFfprobeStatus;      // FFprobe status check indicator

    // Config details summary label
    QLabel *m_lblConfigSummary;

    // Viewfinder components
    QGroupBox *m_viewfinderGroup;
    QLabel *m_lblViewfinder;
    QLabel *m_lblViewfinderStatus;
    QLabel *m_lblViewfinderTime;

    QProcess *m_viewfinderProcess;
    QByteArray m_viewfinderBuffer;
    QString m_viewfinderFocusedFile;
    QHash<QString, double> m_latestFileTimestamps; // Cache last timestamp of active files for previewing

    QLabel *m_lblDashboardOrig;      // Statistics: Original size saved text
    QLabel *m_lblDashboardComp;      // Statistics: Compressed size saved text
    QLabel *m_lblDashboardSaved;     // Statistics: Space saved saved text
    QLabel *m_lblDashboardPct;       // Statistics: Savings percentage saved text

    QTableWidget *m_tableQueue;      // Queue grid table displaying files and states

    // Scrollable bottom layout hosting dynamic worker monitor cards
    QWidget *m_monitorsContainer;
    QVBoxLayout *m_monitorsLayout;

    // Settings config values in memory
    int m_crf;
    QString m_preset;
    bool m_downscale;
    bool m_debob;
    bool m_livePreviewEnabled;
    int m_concurrentLimit;

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
    void updateConfigSummaryText();
    void updateViewfinderFrame(const QString &filepath, double secs);
    
    // Verifies if FFmpeg and FFprobe binaries are available on the host system.
    void checkDependencies();
    
    // Recalculates workspace size savings and updates sidebar dashboard fields.
    void updateSavingsDashboard();
    
    // Searches row indexes mapping absolute filepaths inside the queue table.
    int findRowByFilepath(const QString &filepath);
    
    // Controls enabling state of start queue button based on scan state.
    void updateStartButtonState();
    
    // Automatically selects the first active (processing) row in the grid table for viewfinder preview.
    void autoSelectActiveRow();
};

#endif // MAINWINDOW_H
