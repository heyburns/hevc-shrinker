#include "mainwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSplitter>
#include <QIcon>
#include <QFileDialog>
#include <QMessageBox>
#include <QHeaderView>
#include <QDirIterator>
#include <QFileInfo>
#include <QScrollBar>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QScrollArea>
#include <QMenu>
#include <QDesktopServices>
#include <QUrl>

// -------------------------------------------------------------
// TranscodeMonitorCard Implementation
// -------------------------------------------------------------

// Constructor: Initializes the card's widgets and layouts for a specific file.
TranscodeMonitorCard::TranscodeMonitorCard(const QString &filepath, QWidget *parent)
    : QGroupBox("Active Transcode Monitor", parent)
    , m_filepath(filepath)
    , m_startTime(0)
{
    // Apply styling: thin borders, rounded corners, and clear fonts
    setStyleSheet(
        "QGroupBox { font-weight: bold; border: 1px solid palette(mid); border-radius: 4px; margin-top: 6px; padding-top: 10px; background-color: palette(window); }"
    );
    setFixedWidth(460); // Card width with preview enabled
    setFixedHeight(170); // Fixed height to fit progress details cleanly

    // Horizontal layout splits card into: [Left: Preview Box] | [Right: Details Panel]
    QHBoxLayout *mainCardLayout = new QHBoxLayout(this);
    mainCardLayout->setContentsMargins(8, 8, 8, 8);
    mainCardLayout->setSpacing(8);

    // Left Side: Square Preview Label
    m_lblPreview = new QLabel("Preview Off");
    m_lblPreview->setAlignment(Qt::AlignCenter);
    m_lblPreview->setFixedSize(140, 140);
    m_lblPreview->setStyleSheet("background-color: black; border: 1px solid palette(mid); border-radius: 4px; color: gray; font-weight: bold;");
    mainCardLayout->addWidget(m_lblPreview);

    // Right Side: Info and Progress Widget
    QWidget *infoWidget = new QWidget();
    QVBoxLayout *infoLayout = new QVBoxLayout(infoWidget);
    infoLayout->setContentsMargins(0, 0, 0, 0);
    infoLayout->setSpacing(4);

    // Row 0: File Name Display
    QHBoxLayout *row0 = new QHBoxLayout();
    row0->addWidget(new QLabel("File:"));
    m_lblFile = new QLabel(QFileInfo(filepath).fileName());
    m_lblFile->setStyleSheet("font-weight: bold;");
    row0->addWidget(m_lblFile, 1);
    infoLayout->addLayout(row0);

    // Row 1: Speed and Frames Per Second
    QHBoxLayout *row1 = new QHBoxLayout();
    row1->addWidget(new QLabel("Speed/FPS:"));
    m_lblPerf = new QLabel("N/A");
    m_lblPerf->setStyleSheet("font-weight: bold;");
    row1->addWidget(m_lblPerf, 1);
    infoLayout->addLayout(row1);

    // Row 2: Timing Statistics (Elapsed and Remaining)
    QHBoxLayout *row2 = new QHBoxLayout();
    row2->addWidget(new QLabel("Time:"));
    m_lblTime = new QLabel("N/A");
    m_lblTime->setStyleSheet("font-weight: bold;");
    row2->addWidget(m_lblTime, 1);
    infoLayout->addLayout(row2);

    // Row 3: Progress Bar Widget
    m_progressBar = new QProgressBar();
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setAlignment(Qt::AlignCenter);
    m_progressBar->setFixedHeight(18);
    m_progressBar->setStyleSheet(
        "QProgressBar { border: 1px solid palette(mid); border-radius: 3px; text-align: center; } QProgressBar::chunk { background-color: palette(highlight); }"
    );
    infoLayout->addWidget(m_progressBar);

    // Row 4: Sizing Statistics (Current size / projected size / original size)
    QHBoxLayout *row4 = new QHBoxLayout();
    row4->addWidget(new QLabel("Size:"));
    m_lblSize = new QLabel("N/A");
    m_lblSize->setStyleSheet("font-weight: bold;");
    row4->addWidget(m_lblSize, 1);
    infoLayout->addLayout(row4);

    mainCardLayout->addWidget(infoWidget, 1);

    // Initialize the background image extraction Process
    m_previewProcess = new QProcess(this);
    connect(m_previewProcess, &QProcess::readyReadStandardOutput, this, &TranscodeMonitorCard::onPreviewDataAvailable);
    connect(m_previewProcess, &QProcess::finished, this, &TranscodeMonitorCard::onPreviewProcessFinished);
}

// Destructor: Safely terminates any running preview generation process
TranscodeMonitorCard::~TranscodeMonitorCard()
{
    if (m_previewProcess && m_previewProcess->state() != QProcess::NotRunning) {
        m_previewProcess->kill();
        m_previewProcess->waitForFinished();
    }
}

// Updates statistics text boxes and progress bars on the active card.
void TranscodeMonitorCard::updateProgress(int percentage, double fps, double speed, const QString &etaStr, double outSizeMb, double projectedSizeMb)
{
    // Record start time timestamp when first update occurs
    if (m_startTime == 0) {
        m_startTime = QDateTime::currentSecsSinceEpoch();
    }
    qint64 elapsedSec = QDateTime::currentSecsSinceEpoch() - m_startTime;
    QString elapsedStr = QString("%1:%2")
                            .arg(elapsedSec / 60, 2, 10, QChar('0'))
                            .arg(elapsedSec % 60, 2, 10, QChar('0'));

    m_lblPerf->setText(QString("%1 FPS | %2x").arg(QString::number(fps, 'f', 0), QString::number(speed, 'f', 2)));
    m_lblTime->setText(QString("Elapsed: %1 | Remaining: %2").arg(elapsedStr, etaStr));

    // Display sizing details. Hide projection until at least 3% is processed to avoid math skew.
    if (percentage >= 3) {
        m_lblSize->setText(
            QString("%1 MB / ~%2 MB (Orig: %3 MB)")
                .arg(QString::number(outSizeMb, 'f', 1),
                     QString::number(projectedSizeMb, 'f', 1),
                     QString::number(static_cast<double>(QFileInfo(m_filepath).size()) / (1024.0 * 1024.0), 'f', 1))
        );
    } else {
        m_lblSize->setText(
            QString("%1 MB / Calculating... (Orig: %2 MB)")
                .arg(QString::number(outSizeMb, 'f', 1),
                     QString::number(static_cast<double>(QFileInfo(m_filepath).size()) / (1024.0 * 1024.0), 'f', 1))
        );
    }

    m_progressBar->setValue(percentage);
}

// Sets initial label configurations when worker status updates occur
void TranscodeMonitorCard::updateStatus(const QString &status, const QString &details)
{
    Q_UNUSED(details);
    if (status == "Processing") {
        m_startTime = QDateTime::currentSecsSinceEpoch();
        m_lblPerf->setText("N/A");
        m_lblTime->setText("N/A");
        m_lblSize->setText("N/A");
        m_progressBar->setValue(0);
        m_lblPreview->clear();
        m_lblPreview->setText("Preview Off");
    }
}

// Asynchronously seek extracts a frame thumbnail at a specific second using FFmpeg image piping
void TranscodeMonitorCard::requestFramePreview(double secs)
{
    // Skip if a seek process is already running on this card
    if (m_previewProcess->state() != QProcess::NotRunning) {
        return;
    }

    QString ffmpeg = findDependency("ffmpeg");
    if (ffmpeg.isEmpty()) return;

    m_previewBuffer.clear();

    // Convert seconds to HH:MM:SS.ms string required by FFmpeg seek parameter
    int h = static_cast<int>(secs / 3600);
    int m = static_cast<int>((secs - h * 3600) / 60);
    int s = static_cast<int>(secs - h * 3600 - m * 60);
    int ms = static_cast<int>((secs - h * 3600 - m * 60 - s) * 1000);
    QString timeStr = QString("%1:%2:%3.%4")
                      .arg(h, 2, 10, QChar('0'))
                      .arg(m, 2, 10, QChar('0'))
                      .arg(s, 2, 10, QChar('0'))
                      .arg(ms, 3, 10, QChar('0'));

    // Launch seeking process. Pipes binary PNG bytes into stdout.
    m_previewProcess->start(ffmpeg, {
        "-y",
        "-ss", timeStr, // Fast seek before input
        "-i", m_filepath,
        "-vf", "scale=140:-1", // Downscale width to 140px, keep aspect height
        "-frames:v", "1", // Extract 1 frame
        "-f", "image2pipe", // Output as image stream
        "-vcodec", "png", // Encode as PNG image
        "-"
    });
}

// Appends buffered byte chunks received from FFmpeg stdout pipe
void TranscodeMonitorCard::onPreviewDataAvailable()
{
    m_previewBuffer.append(m_previewProcess->readAllStandardOutput());
}

// Renders the extracted image to the preview box upon process completion
void TranscodeMonitorCard::onPreviewProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        m_previewBuffer.append(m_previewProcess->readAllStandardOutput());
        if (!m_previewBuffer.isEmpty()) {
            QPixmap pixmap;
            if (pixmap.loadFromData(m_previewBuffer, "PNG")) {
                QPixmap scaledPixmap = pixmap.scaled(m_lblPreview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                m_lblPreview->setPixmap(scaledPixmap);
            }
        }
    }
    m_previewBuffer.clear();
}

// Toggles preview box visibility on the card.
// If visible is false, hides the preview frame and collapses the card's width to 310px.
void TranscodeMonitorCard::setPreviewVisible(bool visible)
{
    m_lblPreview->setVisible(visible); // Hide or show the square preview container
    setFixedWidth(visible ? 460 : 310); // Shrink card width if preview is disabled
    
    // If turning preview off, kill any active thumbnail extraction process to free CPU cycles
    if (!visible) {
        m_lblPreview->clear();
        m_lblPreview->setText("Preview Off");
        if (m_previewProcess && m_previewProcess->state() != QProcess::NotRunning) {
            m_previewProcess->kill();
            m_previewProcess->waitForFinished();
        }
        m_previewBuffer.clear();
    }
}

// Constructor: Initializes the main window interface components.
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_dbManager(nullptr)
    , m_isQueueRunning(false)
{
    initUi(); // Build the window layouts and components
    checkDependencies(); // Verify if ffmpeg/ffprobe exist

    // Set Window Icon from deployed application directory path if it exists
    QString iconPath = QCoreApplication::applicationDirPath() + "/app_icon.png";
    if (QFile::exists(iconPath)) {
        setWindowIcon(QIcon(iconPath));
    }
}

// Destructor: Safely stops and joins any active transcode threads on exit.
MainWindow::~MainWindow()
{
    for (TranscodeWorker *worker : m_workers) {
        worker->stop(); // Request threads to abort
        worker->wait(); // Join thread execution
    }
    m_workers.clear();
    delete m_dbManager; // Release global database object handle
}

// Builds the graphical layout, sidebar configurations, grid tables, and status meters.
void MainWindow::initUi()
{
    setWindowTitle("HEVC Video Shrinker v1.0");
    resize(1050, 620); // Widened default window size to fit columns without clipping text

    // Create the central container widget
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    QHBoxLayout *mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(10);

    // Left Panel: Splitter dividing the video queue table (Top) and active monitors (Bottom)
    QSplitter *rightSplitter = new QSplitter(Qt::Vertical);
    rightSplitter->setHandleWidth(4);

    // 1. Queue Table Group Box
    QGroupBox *tableGroup = new QGroupBox("Videos scan queue");
    QVBoxLayout *tableLayout = new QVBoxLayout(tableGroup);
    tableLayout->setContentsMargins(5, 10, 5, 5);

    // Initialize 6-column Table Grid
    m_tableQueue = new QTableWidget(0, 6);
    m_tableQueue->setHorizontalHeaderLabels({
        "Filename", "Status", "Original Size", "Compressed Size", "Progress", "Path"
    });
    m_tableQueue->setSelectionBehavior(QAbstractItemView::SelectRows); // Select entire row
    m_tableQueue->setEditTriggers(QAbstractItemView::NoEditTriggers); // Read-only cells
    m_tableQueue->setContextMenuPolicy(Qt::CustomContextMenu); // Custom context menu trigger
    connect(m_tableQueue, &QTableWidget::customContextMenuRequested, this, &MainWindow::showTableContextMenu);

    // Configure headers to be interactive resizable columns
    QHeaderView *header = m_tableQueue->horizontalHeader();
    for (int col = 0; col < 6; ++col) {
        header->setSectionResizeMode(col, QHeaderView::Interactive);
    }
    header->setStretchLastSection(true); // Let Path column stretch to fill remainder space

    // Set fixed widths for sizing columns to prevent text clipping
    m_tableQueue->setColumnWidth(0, 140); // Filename
    m_tableQueue->setColumnWidth(1, 80);  // Status
    m_tableQueue->setColumnWidth(2, 95);  // Original Size
    m_tableQueue->setColumnWidth(3, 115); // Compressed Size
    m_tableQueue->setColumnWidth(4, 160); // Progress details
    m_tableQueue->setColumnWidth(5, 100); // Path (relative directory)

    tableLayout->addWidget(m_tableQueue);
    rightSplitter->addWidget(tableGroup);

    // 2. Active Monitor dashboard area
    QWidget *bottomDashboard = new QWidget();
    QHBoxLayout *bottomLayout = new QHBoxLayout(bottomDashboard);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(10);

    // Scrollable area for dynamically spawned active transcode cards
    QScrollArea *scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // Disable vertical scrolling
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded); // Enable horizontal scrolling

    m_monitorsContainer = new QWidget();
    m_monitorsLayout = new QHBoxLayout(m_monitorsContainer);
    m_monitorsLayout->setContentsMargins(0, 0, 0, 0);
    m_monitorsLayout->setSpacing(10);
    m_monitorsLayout->addStretch(); // Push progress cards to the left initially

    scrollArea->setWidget(m_monitorsContainer);
    bottomLayout->addWidget(scrollArea, 1);

    rightSplitter->addWidget(bottomDashboard);
    rightSplitter->setSizes({420, 200}); // Setup split proportions

    // Right Panel: Control Config Sidebar
    QWidget *leftContainer = new QWidget();
    leftContainer->setFixedWidth(310);
    QVBoxLayout *leftLayout = new QVBoxLayout(leftContainer);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(10);

    // Sidebar settings container
    QGroupBox *ctrlGroup = new QGroupBox("Shrinker configuration");
    QVBoxLayout *ctrlLayout = new QVBoxLayout(ctrlGroup);
    ctrlLayout->setContentsMargins(10, 15, 10, 10);
    ctrlLayout->setSpacing(10);

    // Workspace Input Box
    ctrlLayout->addWidget(new QLabel("Workspace Folder:"));
    QHBoxLayout *dirInputLayout = new QHBoxLayout();
    m_dirInput = new QLineEdit();
    m_dirInput->setReadOnly(true);
    m_dirInput->setPlaceholderText("Select video folder...");
    m_btnBrowse = new QPushButton("Browse");
    connect(m_btnBrowse, &QPushButton::clicked, this, &MainWindow::selectDirectory);
    dirInputLayout->addWidget(m_dirInput);
    dirInputLayout->addWidget(m_btnBrowse);
    ctrlLayout->addLayout(dirInputLayout);

    // Quality Spinner (CRF)
    ctrlLayout->addWidget(new QLabel("CRF Quality (higher = smaller file):"));
    m_spinCrf = new QSpinBox();
    m_spinCrf->setRange(0, 51);
    m_spinCrf->setValue(28); // Standard H.265 default crf is 28
    ctrlLayout->addWidget(m_spinCrf);

    // Speed Preset Selector
    ctrlLayout->addWidget(new QLabel("FFmpeg CPU Speed Preset:"));
    m_comboPreset = new QComboBox();
    m_comboPreset->addItems({
        "ultrafast", "superfast", "veryfast", "fast", "medium", "slow", "slower", "veryslow"
    });
    m_comboPreset->setCurrentText("medium");
    ctrlLayout->addWidget(m_comboPreset);

    // UHD/4K Downscale Checkbox
    m_chkDownscale = new QCheckBox("Downscale 4K/UHD to 1080p");
    m_chkDownscale->setChecked(true);
    ctrlLayout->addWidget(m_chkDownscale);

    // High Frame Rate De-bob Checkbox
    m_chkDebob = new QCheckBox("High frame-rate de-bob (bwdif)");
    m_chkDebob->setChecked(true);
    ctrlLayout->addWidget(m_chkDebob);

    // Live Progress Preview Checkbox (Privacy Toggle)
    m_chkPreview = new QCheckBox("Enable Live Progress Preview");
    m_chkPreview->setChecked(false); // Off by default for privacy
    ctrlLayout->addWidget(m_chkPreview);

    // Max Concurrent Encodes Selector
    ctrlLayout->addWidget(new QLabel("Max Concurrent Encodes:"));
    m_spinConcurrent = new QSpinBox();
    m_spinConcurrent->setRange(1, 16);
    m_spinConcurrent->setValue(1);
    ctrlLayout->addWidget(m_spinConcurrent);
    connect(m_spinConcurrent, &QSpinBox::valueChanged, this, &MainWindow::onConcurrentChanged);

    // Reset Configuration Defaults Button
    m_btnReset = new QPushButton("Restore settings back to defaults");
    connect(m_btnReset, &QPushButton::clicked, this, &MainWindow::resetSettings);
    ctrlLayout->addWidget(m_btnReset);

    leftLayout->addWidget(ctrlGroup);

    // Status Panel (FFmpeg & FFprobe tool checks)
    QGroupBox *statusGroup = new QGroupBox("Tools & binaries dependencies status");
    QGridLayout *statusLayout = new QGridLayout(statusGroup);
    statusLayout->setContentsMargins(10, 12, 10, 10);
    statusLayout->setSpacing(8);

    statusLayout->addWidget(new QLabel("FFmpeg binary:"), 0, 0);
    m_lblFfmpegStatus = new QLabel("Searching...");
    m_lblFfmpegStatus->setStyleSheet("font-weight: bold; color: yellow;");
    statusLayout->addWidget(m_lblFfmpegStatus, 0, 1);

    statusLayout->addWidget(new QLabel("FFprobe binary:"), 1, 0);
    m_lblFfprobeStatus = new QLabel("Searching...");
    m_lblFfprobeStatus->setStyleSheet("font-weight: bold; color: yellow;");
    statusLayout->addWidget(m_lblFfprobeStatus, 1, 1);

    leftLayout->addWidget(statusGroup);

    // Savings dashboard scorecard layout
    QGroupBox *savingsGroup = new QGroupBox("Space Savings Scoreboard");
    QGridLayout *savingsLayout = new QGridLayout(savingsGroup);
    savingsLayout->setContentsMargins(10, 12, 10, 10);
    savingsLayout->setSpacing(8);

    savingsLayout->addWidget(new QLabel("Original size:"), 0, 0);
    m_lblDashboardOrig = new QLabel("0.0 MB");
    m_lblDashboardOrig->setStyleSheet("font-weight: bold;");
    savingsLayout->addWidget(m_lblDashboardOrig, 0, 1);

    savingsLayout->addWidget(new QLabel("Compressed size:"), 1, 0);
    m_lblDashboardComp = new QLabel("0.0 MB");
    m_lblDashboardComp->setStyleSheet("font-weight: bold;");
    savingsLayout->addWidget(m_lblDashboardComp, 1, 1);

    savingsLayout->addWidget(new QLabel("Saved space:"), 2, 0);
    m_lblDashboardSaved = new QLabel("0.0 MB");
    m_lblDashboardSaved->setStyleSheet("font-weight: bold; color: green;");
    savingsLayout->addWidget(m_lblDashboardSaved, 2, 1);

    savingsLayout->addWidget(new QLabel("Savings ratio:"), 3, 0);
    m_lblDashboardPct = new QLabel("0.0%");
    savingsLayout->addWidget(m_lblDashboardPct, 3, 1);

    // Database Reset Buttons (Workspace-scoped)
    m_btnResetDb = new QPushButton("Reset Database");
    m_btnResetDb->setEnabled(false);
    connect(m_btnResetDb, &QPushButton::clicked, this, &MainWindow::onResetDbClicked);

    m_btnResetScoreboard = new QPushButton("Reset Scoreboard");
    m_btnResetScoreboard->setEnabled(false);
    connect(m_btnResetScoreboard, &QPushButton::clicked, this, &MainWindow::onResetScoreboardClicked);

    QHBoxLayout *resetButtonsLayout = new QHBoxLayout();
    resetButtonsLayout->addWidget(m_btnResetDb);
    resetButtonsLayout->addWidget(m_btnResetScoreboard);
    savingsLayout->addLayout(resetButtonsLayout, 4, 0, 1, 2);

    leftLayout->addWidget(savingsGroup);

    // Action Execution Buttons (Scan, Start, and Abort)
    QVBoxLayout *actionsLayout = new QVBoxLayout();
    actionsLayout->setSpacing(6);

    m_btnScan = new QPushButton("Scan Directory");
    m_btnScan->setEnabled(false);
    m_btnScan->setFixedHeight(28);
    connect(m_btnScan, &QPushButton::clicked, this, &MainWindow::scanDirectory);
    actionsLayout->addWidget(m_btnScan);

    QHBoxLayout *queueActionsLayout = new QHBoxLayout();
    queueActionsLayout->setSpacing(6);

    m_btnStart = new QPushButton("Start Queue");
    m_btnStart->setEnabled(false);
    m_btnStart->setFixedHeight(28);
    m_btnStart->setStyleSheet("font-weight: bold; background-color: darkgreen; color: white;");
    connect(m_btnStart, &QPushButton::clicked, this, &MainWindow::startProcessing);

    m_btnStop = new QPushButton("Abort Job");
    m_btnStop->setEnabled(false);
    m_btnStop->setFixedHeight(28);
    m_btnStop->setStyleSheet("font-weight: bold; background-color: darkred; color: white;");
    connect(m_btnStop, &QPushButton::clicked, this, &MainWindow::stopProcessing);

    queueActionsLayout->addWidget(m_btnStart);
    queueActionsLayout->addWidget(m_btnStop);
    actionsLayout->addLayout(queueActionsLayout);

    leftLayout->addLayout(actionsLayout);
    leftLayout->addStretch(); // Spacers push elements up

    // Add Splitters to core Layout
    mainLayout->addWidget(rightSplitter, 1);
    mainLayout->addWidget(leftContainer, 0);
}

// Verifies if dependencies are present on the host environment
void MainWindow::checkDependencies()
{
    QString ffmpeg = findDependency("ffmpeg");
    QString ffprobe = findDependency("ffprobe");

    if (!ffmpeg.isEmpty()) {
        m_lblFfmpegStatus->setText("DETECTED");
        m_lblFfmpegStatus->setStyleSheet("font-weight: bold; color: green;");
    } else {
        m_lblFfmpegStatus->setText("MISSING");
        m_lblFfmpegStatus->setStyleSheet("font-weight: bold; color: red;");
    }

    if (!ffprobe.isEmpty()) {
        m_lblFfprobeStatus->setText("DETECTED");
        m_lblFfprobeStatus->setStyleSheet("font-weight: bold; color: green;");
    } else {
        m_lblFfprobeStatus->setText("MISSING");
        m_lblFfprobeStatus->setStyleSheet("font-weight: bold; color: red;");
    }

    if (ffmpeg.isEmpty() || ffprobe.isEmpty()) {
        logMessage("[ERROR] Critical system tools missing. Please make sure ffmpeg/ffprobe are installed and present in your system environment PATH.");
    }
}

// Slot triggered when clicking "Browse" button. Sets up the active target directory path.
void MainWindow::selectDirectory()
{
    // Open directories dialog
    QString dirSelected = QFileDialog::getExistingDirectory(this, "Select Workspace Folder", m_rootDir);
    if (dirSelected.isEmpty()) return;

    m_rootDir = QDir(dirSelected).absolutePath();
    m_dirInput->setText(m_rootDir);

    // Release old database managers
    delete m_dbManager;
    m_dbManager = nullptr;

    try {
        QFileInfo dirInfo(m_rootDir);
        if (dirInfo.exists() && dirInfo.isDir()) {
            m_dbManager = new DatabaseManager();
            logMessage(QString("Selected workspace: %1").arg(m_rootDir));
            
            // Check if directory contains a legacy database file processed_files.db.
            // If it does, automatically import its records into the user's global profile datastore.
            QString localDbPath = QDir(m_rootDir).filePath("processed_files.db");
            if (QFile::exists(localDbPath)) {
                logMessage("Local database detected in workspace. Migrating records to global database...");
                m_dbManager->migrateLocalDatabase(localDbPath, m_rootDir);
                logMessage("Migration complete. Local database has been renamed to processed_files.db.migrated.");
            }
            
            // Enable workspace-scoped controls
            m_btnScan->setEnabled(true);
            m_btnResetDb->setEnabled(true);
            m_btnResetScoreboard->setEnabled(true);
        } else {
            throw std::runtime_error("Directory does not exist or has no write permissions.");
        }
    } catch (const std::exception &e) {
        logMessage(QString("[ERROR] Failed to access workspace folder: %1").arg(e.what()));
        QMessageBox::critical(this, "Workspace Error",
            QString("Could not select the folder.\n\nError: %1\n\nEnsure you have permissions to write in this folder.")
            .arg(e.what())
        );
        m_rootDir = "";
        m_dirInput->setText("");
        m_btnScan->setEnabled(false);
        m_btnResetDb->setEnabled(false);
        m_btnResetScoreboard->setEnabled(false);
    }

    // Reset GUI queue state
    m_tableQueue->setRowCount(0);
    m_scannedFiles.clear();
    m_activeTranscodeQueue.clear();
    m_btnStart->setEnabled(false);
}

// Recursive helper function scanning local subfolders for video extensions.
// Ignores .Trash and .Errors folders automatically.
static void scanDirRecursive(const QString &dirPath, const QString &rootDir, const QStringList &videoExtensions, QList<ScannedFile> &scannedFiles)
{
    // Keeps GUI events processing during folder traversal
    QCoreApplication::processEvents();

    // Ignore Trash and Errors subfolders
    if (dirPath.contains("/.Trash") || dirPath.contains("/.Errors") ||
        dirPath.contains("\\.Trash") || dirPath.contains("\\.Errors")) {
        return;
    }

    QDir dir(dirPath);
    QFileInfoList list = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &fi : list) {
        if (fi.isDir()) {
            scanDirRecursive(fi.absoluteFilePath(), rootDir, videoExtensions, scannedFiles);
        } else {
            if (videoExtensions.contains(fi.suffix().toLower())) {
                ScannedFile sf;
                sf.absolutePath = fi.absoluteFilePath();
                sf.filename = fi.fileName();
                sf.relPath = QDir(rootDir).relativeFilePath(sf.absolutePath);
                sf.size = fi.size();
                sf.lastModified = fi.lastModified().toSecsSinceEpoch();
                scannedFiles.append(sf);
            }
        }
    }
}

void MainWindow::scanDirectory()
{
    if (m_rootDir.isEmpty()) return;

    logMessage(QString("Scanning workspace directory: %1").arg(m_rootDir));
    m_scannedFiles.clear();
    m_activeTranscodeQueue.clear();
    m_tableQueue->setRowCount(0); // Wipe table cells

    QString ffprobeBin = findDependency("ffprobe");

    // Supported Video Extensions (Case-Insensitive)
    static const QStringList videoExtensions = {
        "mp4", "mkv", "wmv", "avi", "mov", "flv", "mpeg", "mpg", "vid", "m4v", "asf", "f4v", "divx"
    };

    // 1. Traverse directory recursively to find video files
    scanDirRecursive(m_rootDir, m_rootDir, videoExtensions, m_scannedFiles);

    logMessage(QString("Found %1 video files.").arg(m_scannedFiles.count()));

    // OPTIMIZATION: Disable table redraw painting temporarily.
    // Inserting hundreds of rows into a QTableWidget causes massive rendering lag
    // if Qt attempts to re-calculate layouts and repaint after every cell insertion.
    m_tableQueue->setUpdatesEnabled(false);
    m_tableQueue->setRowCount(m_scannedFiles.count());
    
    int cacheHits = 0;
    int cacheMisses = 0;

    // 2. Iterate through each scanned file to determine status (Completed, Compliant, or Pending)
    for (int idx = 0; idx < m_scannedFiles.count(); ++idx) {
        const ScannedFile &sf = m_scannedFiles[idx];
        QString filepath = sf.absolutePath;
        QString filename = sf.filename;
        QString relPath = sf.relPath;
        double sizeMb = static_cast<double>(sf.size) / (1024.0 * 1024.0);

        bool isProcessed = false;
        bool isCompliant = false;

        QString origSizeDisplay = QString("%1 MB").arg(QString::number(sizeMb, 'f', 1));
        QString compSizeDisplay = "N/A";
        QString status = "Pending";
        QString detail = "Waiting...";

        // Query Database: check if this file has already been shrunk in the past
        if (m_dbManager) {
            ProcessedFileInfo info = m_dbManager->getProcessedFileInfo(filepath);
            if (info.found) {
                isProcessed = true;
                origSizeDisplay = QString("%1 MB").arg(QString::number(static_cast<double>(info.originalSize) / (1024.0 * 1024.0), 'f', 1));
                compSizeDisplay = QString("%1 MB").arg(QString::number(static_cast<double>(info.compressedSize) / (1024.0 * 1024.0), 'f', 1));
            }
        }

        // Query Compliance: if not already processed, check if it's already H.265+AAC (compliant)
        if (!isProcessed && !ffprobeBin.isEmpty()) {
            qint64 fileSize = sf.size;
            qint64 lastModified = sf.lastModified;
            int cached = -1;
            
            // Check scan_cache table to bypass slow shell-probe calls
            if (m_dbManager) {
                cached = m_dbManager->getCachedCompliance(filepath, fileSize, lastModified);
            }

            if (cached != -1) {
                isCompliant = (cached == 1);
                cacheHits++; // Saved CPU cycles!
            } else {
                cacheMisses++; // Probe file using FFprobe
                isCompliant = probeFileCompliance(filepath, ffprobeBin);
                // Save outcome to scan cache
                if (m_dbManager) {
                    m_dbManager->setCachedCompliance(filepath, fileSize, lastModified, isCompliant ? 1 : 0);
                }
            }
            if (isCompliant) {
                compSizeDisplay = origSizeDisplay; // Compliant files won't change size
            }
        }

        // Map status text and build queue list
        if (isProcessed) {
            status = "Completed";
            detail = "Already processed (in DB)";
        } else if (isCompliant) {
            status = "Skipped";
            detail = "Already compliant (HEVC+AAC)";
        } else {
            status = "Pending";
            detail = "Waiting...";
            m_activeTranscodeQueue.append(filepath); // Add to transcode queue
        }

        // Populate Table Grid cells
        m_tableQueue->setItem(idx, 0, new QTableWidgetItem(filename));
        m_tableQueue->setItem(idx, 1, new QTableWidgetItem(status));
        m_tableQueue->setItem(idx, 2, new QTableWidgetItem(origSizeDisplay));
        m_tableQueue->setItem(idx, 3, new QTableWidgetItem(compSizeDisplay));
        m_tableQueue->setItem(idx, 4, new QTableWidgetItem(detail));
        m_tableQueue->setItem(idx, 5, new QTableWidgetItem(relPath));

        // Color-code grid rows dynamically
        if (isProcessed) {
            for (int col = 0; col < 6; ++col) {
                QTableWidgetItem *item = m_tableQueue->item(idx, col);
                if (item) {
                    item->setBackground(Qt::darkGreen);
                    item->setForeground(Qt::white);
                }
            }
        } else if (isCompliant) {
            for (int col = 0; col < 6; ++col) {
                QTableWidgetItem *item = m_tableQueue->item(idx, col);
                if (item) {
                    item->setBackground(Qt::darkYellow);
                    item->setForeground(Qt::white);
                }
            }
        }
    }
    m_tableQueue->setUpdatesEnabled(true); // Re-enable grid repainting updates

    updateSavingsDashboard(); // Recalculate scoreboard stats
    logMessage(QString("Scan complete. Cache Hits: %1, Cache Misses: %2. %3 files queued for processing.")
        .arg(cacheHits).arg(cacheMisses).arg(m_activeTranscodeQueue.count()));

    m_btnStart->setEnabled(m_activeTranscodeQueue.count() > 0);
}

// Triggered by clicking "Start Queue". Spawns initial concurrent threads.
void MainWindow::startProcessing()
{
    if (m_activeTranscodeQueue.isEmpty() || m_rootDir.isEmpty()) return;

    // Lock configuration UI widgets during queue execution
    m_btnBrowse->setEnabled(false);
    m_btnScan->setEnabled(false);
    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);
    m_isQueueRunning = true;

    m_pendingQueue = m_activeTranscodeQueue;

    logMessage(QString("Starting transcode queue with %1 files...").arg(m_pendingQueue.count()));

    // Clear bottom progress card panel layout
    if (m_monitorsLayout->count() > 0) {
        QLayoutItem *item = m_monitorsLayout->takeAt(0);
        if (item) {
            delete item;
        }
    }

    // Spawn initial worker slots up to concurrent setting limit
    int maxConcurrent = m_spinConcurrent->value();
    for (int i = 0; i < maxConcurrent; ++i) {
        startNextQueueJob();
    }
}

// Instantiates a new TranscodeWorker thread, connects signals, and starts execution.
void MainWindow::startNextQueueJob()
{
    if (!m_isQueueRunning || m_pendingQueue.isEmpty()) return;

    // Take the next filepath from the pending queue
    QString filepath = m_pendingQueue.takeFirst();
    logMessage(QString("Dispatching transcode job: %1").arg(QFileInfo(filepath).fileName()));

    // Populate transcode parameters mapping
    QVariantMap settings;
    settings["crf"] = m_spinCrf->value();
    settings["preset"] = m_comboPreset->currentText();
    settings["downscale"] = m_chkDownscale->isChecked();
    settings["debob"] = m_chkDebob->isChecked();
    settings["live_preview"] = m_chkPreview->isChecked();

    // Recalculate CPU threads split dynamically.
    // Splits cores evenly between concurrent encodes to avoid context switching.
    int totalCores = QThread::idealThreadCount();
    int maxConcurrent = m_spinConcurrent->value();
    int threadsPerJob = qMax(1, totalCores / maxConcurrent);
    settings["threads"] = threadsPerJob;

    QString dbPath = QDir(m_rootDir).filePath("processed_files.db");
    TranscodeWorker *worker = new TranscodeWorker(QStringList{filepath}, m_rootDir, dbPath, settings, this);
    worker->setProperty("filepath", filepath);

    // Remove last stretch spacer if it exists
    if (m_monitorsLayout->count() > 0) {
        QLayoutItem *lastItem = m_monitorsLayout->itemAt(m_monitorsLayout->count() - 1);
        if (lastItem && lastItem->spacerItem()) {
            m_monitorsLayout->removeItem(lastItem);
            delete lastItem;
        }
    }

    // Add progress monitor card
    TranscodeMonitorCard *card = new TranscodeMonitorCard(filepath, m_monitorsContainer);
    card->setPreviewVisible(m_chkPreview->isChecked());
    m_monitorsLayout->addWidget(card);
    m_activeCards.insert(filepath, card);

    // Add stretch back at the end
    m_monitorsLayout->addStretch();

    // Connect Worker Thread signals (redirect worker metrics to MainWindow slots)
    connect(worker, &TranscodeWorker::logSignal, this, &MainWindow::logMessage);
    connect(worker, &TranscodeWorker::progressSignal, this, &MainWindow::updateProgress);
    connect(worker, &TranscodeWorker::statusSignal, this, &MainWindow::updateStatus);
    connect(worker, &TranscodeWorker::fileDoneSignal, this, &MainWindow::fileDone);
    connect(worker, &TranscodeWorker::finishedSignal, this, &MainWindow::onWorkerFinished);
    connect(worker, &TranscodeWorker::previewFrameSignal, this, &MainWindow::onWorkerPreviewFrameRequested);

    m_workers.append(worker); // Keep track of the active worker thread handle
    worker->start(); // Launch background execution loop
}

// Callback slot when a background worker thread completes its execution.
void MainWindow::onWorkerFinished()
{
    TranscodeWorker *worker = qobject_cast<TranscodeWorker*>(sender());
    if (!worker) return;

    m_workers.removeOne(worker); // Remove thread from active list
    worker->deleteLater(); // Queue thread memory deletion

    // Remove the associated monitor card from layout
    QString filepath = worker->property("filepath").toString();
    if (m_activeCards.contains(filepath)) {
        TranscodeMonitorCard *card = m_activeCards.take(filepath);
        m_monitorsLayout->removeWidget(card);
        delete card;
    }

    // Schedule next file in queue if slots are available
    if (m_isQueueRunning && !m_pendingQueue.isEmpty()) {
        if (m_workers.count() < m_spinConcurrent->value()) {
            startNextQueueJob();
        }
    } else if (m_pendingQueue.isEmpty() && m_workers.isEmpty()) {
        // Stop execution if both pending queue and active threads are exhausted
        processingFinished();
    }
}

// Triggered when concurrent encodes spinbox values change during queue execution.
void MainWindow::onConcurrentChanged(int val)
{
    if (!m_isQueueRunning) return;

    int activeCount = m_workers.count();
    // Scale up: if slots increased, start new transcode threads immediately
    if (activeCount < val) {
        int spawnCount = val - activeCount;
        for (int i = 0; i < spawnCount; ++i) {
            startNextQueueJob();
        }
    }
    // Scale down: let running workers complete naturally. onWorkerFinished() blocks new spawns.
}

// Receives preview extraction requests from worker threads and routes them to corresponding cards
void MainWindow::onWorkerPreviewFrameRequested(const QString &filepath, double secs)
{
    if (m_activeCards.contains(filepath)) {
        m_activeCards[filepath]->requestFramePreview(secs);
    }
}

// Slot triggered when clicking "Abort Job". Cancels and joins all active encodes.
void MainWindow::stopProcessing()
{
    logMessage("[ABORT] Stopping all active transcode tasks...");
    m_btnStop->setEnabled(false);
    m_isQueueRunning = false;
    m_pendingQueue.clear(); // Wipe the pending execution queue

    // Tell all running worker threads to abort their background loops
    for (TranscodeWorker *worker : m_workers) {
        worker->stop();
    }

    // Wait for all workers to shut down and clean up thread memory handles
    for (TranscodeWorker *worker : m_workers) {
        worker->wait();
        worker->deleteLater();
    }
    m_workers.clear();

    // Delete all active GUI monitor cards
    QList<QString> keys = m_activeCards.keys();
    for (const QString &key : keys) {
        TranscodeMonitorCard *card = m_activeCards.take(key);
        m_monitorsLayout->removeWidget(card);
        delete card;
    }
    m_activeCards.clear();

    // Re-insert layout stretches
    m_monitorsLayout->addStretch();

    processingFinished(); // Unlock GUI control panels
}

// Restores user settings back to initial baseline presets
void MainWindow::resetSettings()
{
    m_spinCrf->setValue(28);
    m_comboPreset->setCurrentText("medium");
    m_chkDownscale->setChecked(true);
    m_chkDebob->setChecked(true);
    m_chkPreview->setChecked(false);
    logMessage("Configuration panel reset to default configurations.");
}

// Writes print statements to standard output.
void MainWindow::logMessage(const QString &message)
{
    qDebug().noquote() << message;
}

// Updates percentages and sizing projections in the queue grid row and routes to monitor cards.
void MainWindow::updateProgress(const QString &filepath, int percentage, double fps, double speed, const QString &etaStr, double outSizeMb, double projectedSizeMb)
{
    int idx = findRowByFilepath(filepath);
    if (idx != -1) {
        // Format table progress cell details
        QString statsStr = (etaStr != "N/A") ? QString(" (%1x | ETA %2)").arg(QString::number(speed, 'f', 1), etaStr) : "";
        m_tableQueue->setItem(idx, 4, new QTableWidgetItem(QString("%1%%2").arg(QString::number(percentage), statsStr)));
    }

    // Route message data to corresponding active card
    if (m_activeCards.contains(filepath)) {
        m_activeCards[filepath]->updateProgress(percentage, fps, speed, etaStr, outSizeMb, projectedSizeMb);
    }
}

// Updates file status labels in the queue grid row (highlighting active rows blue).
void MainWindow::updateStatus(const QString &filepath, const QString &status, const QString &details)
{
    int idx = findRowByFilepath(filepath);
    if (idx != -1) {
        m_tableQueue->setItem(idx, 1, new QTableWidgetItem(status));
        m_tableQueue->setItem(idx, 4, new QTableWidgetItem(details));

        // Highlight actively processing file rows blue
        if (status == "Processing") {
            for (int col = 0; col < 6; ++col) {
                QTableWidgetItem *item = m_tableQueue->item(idx, col);
                if (item) {
                    item->setBackground(Qt::darkBlue);
                    item->setForeground(Qt::white);
                }
            }
        }
    }

    // Route details to card widget
    if (m_activeCards.contains(filepath)) {
        m_activeCards[filepath]->updateStatus(status, details);
    }
}

// Called when a worker finishes transcoding. Handles database recordings and grid table color codes.
void MainWindow::fileDone(const QString &filepath, const QString &status, qint64 oldSize, qint64 newSize)
{
    int idx = findRowByFilepath(filepath);
    if (idx != -1) {
        m_tableQueue->setItem(idx, 1, new QTableWidgetItem(status));
        m_tableQueue->setItem(idx, 4, new QTableWidgetItem("Finished"));
        
        double newSizeMb = static_cast<double>(newSize) / (1024.0 * 1024.0);
        m_tableQueue->setItem(idx, 3, new QTableWidgetItem(QString("%1 MB").arg(QString::number(newSizeMb, 'f', 1))));

        // Color-code completed rows: Green for Completed, Yellow for Skipped, Red for Failures
        QColor color;
        if (status == "Completed") {
            color = Qt::darkGreen;
        } else if (status == "Skipped") {
            color = Qt::darkYellow;
        } else if (status == "Failed" || status == "Error") {
            color = Qt::darkRed;
        } else {
            color = Qt::darkGray;
        }

        for (int col = 0; col < 6; ++col) {
            QTableWidgetItem *item = m_tableQueue->item(idx, col);
            if (item) {
                item->setBackground(color);
                item->setForeground(Qt::white);
            }
        }

        updateSavingsDashboard();
    }
}

// Cleans up panels and unlocks sidebar input controls when all queued transcoding files are completed.
void MainWindow::processingFinished()
{
    logMessage("\nAll transcode queue tasks finished.");
    m_btnBrowse->setEnabled(true); // Unlock folder browse
    m_btnScan->setEnabled(true); // Unlock scan
    updateStartButtonState(); // Enable start button if pending items remain
    m_btnStop->setEnabled(false); // Lock abort
    m_isQueueRunning = false;
}

// Re-evaluates row items inside the queue table and updates the enabled state of the Start Queue button.
void MainWindow::updateStartButtonState()
{
    m_activeTranscodeQueue.clear();
    for (int row = 0; row < m_tableQueue->rowCount(); ++row) {
        QTableWidgetItem *statusItem = m_tableQueue->item(row, 1);
        QTableWidgetItem *pathItem = m_tableQueue->item(row, 5);
        if (statusItem && pathItem) {
            QString status = statusItem->text();
            // Re-queue items that are pending or were cancelled mid-transcode
            if (status == "Pending" || status == "Cancelled") {
                QString filepath = QDir(m_rootDir).filePath(pathItem->text());
                m_activeTranscodeQueue.append(filepath);
            }
        }
    }
    // Enable start button only if files remain to be processed
    m_btnStart->setEnabled(m_activeTranscodeQueue.count() > 0);
}

// Queries workspace statistics from database and updates GUI scoreboard labels.
void MainWindow::updateSavingsDashboard()
{
    if (m_dbManager) {
        double orig = 0.0, comp = 0.0, saved = 0.0, pct = 0.0;
        // Fetch sizing sums from SQLite database scoped to root folder
        if (m_dbManager->getSpaceSavings(m_rootDir, orig, comp, saved, pct)) {
            m_lblDashboardOrig->setText(QString("%1 MB").arg(QString::number(orig, 'f', 1)));
            m_lblDashboardComp->setText(QString("%1 MB").arg(QString::number(comp, 'f', 1)));
            m_lblDashboardSaved->setText(QString("%1 MB").arg(QString::number(saved, 'f', 1)));
            m_lblDashboardPct->setText(QString("%1%").arg(QString::number(pct, 'f', 1)));
        }
    }
}

// Searches the queue table row matching an absolute filepath.
int MainWindow::findRowByFilepath(const QString &filepath)
{
    for (int row = 0; row < m_tableQueue->rowCount(); ++row) {
        QTableWidgetItem *pathItem = m_tableQueue->item(row, 5);
        if (pathItem) {
            QString itemPath = QDir(m_rootDir).filePath(pathItem->text());
            if (QDir(itemPath).absolutePath() == QDir(filepath).absolutePath()) {
                return row;
            }
        }
    }
    return -1; // Not found
}

// Propagates preview checkbox state shifts dynamically to active threads and UI cards.
void MainWindow::togglePreview(bool checked)
{
    for (TranscodeWorker *worker : m_workers) {
        worker->setLivePreviewEnabled(checked);
    }
    for (TranscodeMonitorCard *card : m_activeCards.values()) {
        card->setPreviewVisible(checked);
    }
}

// Slot triggered when clicking "Reset Database". Clears compliance cache.
void MainWindow::onResetDbClicked()
{
    if (m_rootDir.isEmpty() || !m_dbManager) return;

    QMessageBox::StandardButton reply = QMessageBox::question(
        this, 
        "Reset Database Cache", 
        "Are you sure you want to clear the metadata cache for this workspace?\n\nThis will force a full file-probing scan the next time you scan this directory.",
        QMessageBox::Yes | QMessageBox::No
    );

    if (reply == QMessageBox::Yes) {
        // Clear scan_cache entries scoped to root directory
        if (m_dbManager->clearScanCache(m_rootDir)) {
            logMessage("Scan metadata cache cleared for this workspace.");
            scanDirectory(); // Refresh the table queue
        } else {
            logMessage("[ERROR] Failed to clear metadata cache.");
        }
    }
}

// Slot triggered when clicking "Reset Scoreboard". Clears space sizing history.
void MainWindow::onResetScoreboardClicked()
{
    if (m_rootDir.isEmpty() || !m_dbManager) return;

    QMessageBox::StandardButton reply = QMessageBox::question(
        this, 
        "Reset Scoreboard", 
        "Are you sure you want to clear the space savings history for this workspace?\n\nThis will reset the statistics to 0 and allow previously completed files to be transcoded again.",
        QMessageBox::Yes | QMessageBox::No
    );

    if (reply == QMessageBox::Yes) {
        // Clear processed_files entries scoped to root directory
        if (m_dbManager->clearProcessedFiles(m_rootDir)) {
            logMessage("Scoreboard transcode history cleared for this workspace.");
            updateSavingsDashboard(); // Reset dashboard labels to 0
            scanDirectory(); // Re-scan folder to mark files as pending
        } else {
            logMessage("[ERROR] Failed to clear transcode history.");
        }
    }
}

// Triggered on customContextMenuRequested on the queue grid table.
// Displays right-click popup menu with Play option.
void MainWindow::showTableContextMenu(const QPoint &pos)
{
    QTableWidgetItem *item = m_tableQueue->itemAt(pos);
    if (!item) return;

    int row = item->row();
    QTableWidgetItem *pathItem = m_tableQueue->item(row, 5);
    if (!pathItem) return;

    QString relPath = pathItem->text();
    QString absPath = QDir(m_rootDir).filePath(relPath);

    QMenu menu(this);
    QAction *playAction = menu.addAction("Play Video");

    // Display context menu at mouse cursor screen coordinates
    QAction *selected = menu.exec(m_tableQueue->viewport()->mapToGlobal(pos));
    if (selected == playAction) {
        // Opens the video asynchronously using the host OS's default media player
        QDesktopServices::openUrl(QUrl::fromLocalFile(absPath));
    }
}
