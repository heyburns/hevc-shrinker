#include "mainwindow.h"
#include "avisynthdialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSplitter>
#include <QIcon>
#include <QFileDialog>
#include <QMessageBox>
#include <QHeaderView>
#include "filetransaction.h"
#include <QFileInfo>
#include <QStatusBar>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QScrollArea>
#include <QMenu>
#include <QMenuBar>
#include <QInputDialog>
#include <QActionGroup>
#include <QDesktopServices>
#include <QUrl>
#include <QSignalBlocker>
#include <cmath>

namespace {
constexpr auto appVersion = "2.1";
}

// -------------------------------------------------------------
// TranscodeMonitorCard Implementation
// -------------------------------------------------------------

// Constructor: Initializes the card's widgets and layouts for a specific file.
// Constructor: Initializes the card's widgets and layouts for a specific file.
TranscodeMonitorCard::TranscodeMonitorCard(const QString &filepath, QWidget *parent)
    : QFrame(parent)
    , m_filepath(filepath)
    , m_startTime(0)
{
    setObjectName("TranscodeMonitorCard");
    // Apply styling: thin borders, rounded corners, and clear fonts for top level QFrame container only
    setStyleSheet(
        "QFrame#TranscodeMonitorCard { border: 1px solid palette(mid); border-radius: 4px; background-color: palette(window); }"
    );
    setFrameShape(QFrame::StyledPanel);
    setFixedHeight(88); // Collapse height to be extremely compact
    setMinimumWidth(280);

    QVBoxLayout *mainCardLayout = new QVBoxLayout(this);
    mainCardLayout->setContentsMargins(6, 6, 6, 6);
    mainCardLayout->setSpacing(2);

    // Row 0: File Name Display
    QHBoxLayout *row0 = new QHBoxLayout();
    row0->addWidget(new QLabel("File:"));
    m_lblFile = new QLabel(QFileInfo(filepath).fileName());
    m_lblFile->setStyleSheet("font-weight: bold;");
    row0->addWidget(m_lblFile, 1);
    mainCardLayout->addLayout(row0);

    // Row 1: Speed and FPS
    QHBoxLayout *row1 = new QHBoxLayout();
    row1->addWidget(new QLabel("Speed/FPS:"));
    m_lblPerf = new QLabel("N/A");
    m_lblPerf->setStyleSheet("font-weight: bold;");
    row1->addWidget(m_lblPerf, 1);
    mainCardLayout->addLayout(row1);

    // Row 2: Progress Bar Widget
    m_progressBar = new QProgressBar();
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setAlignment(Qt::AlignCenter);
    m_progressBar->setFixedHeight(14);
    m_progressBar->setStyleSheet(
        "QProgressBar { border: 1px solid palette(mid); border-radius: 3px; text-align: center; } QProgressBar::chunk { background-color: palette(highlight); }"
    );
    mainCardLayout->addWidget(m_progressBar);

    // Row 3: Stats Details (Time & Size compact grid)
    QHBoxLayout *row3 = new QHBoxLayout();
    m_lblTime = new QLabel("Time: N/A");
    m_lblTime->setStyleSheet("font-size: 10px;");
    m_lblSize = new QLabel("Size: N/A");
    m_lblSize->setStyleSheet("font-size: 10px;");
    row3->addWidget(m_lblTime, 1);
    row3->addWidget(m_lblSize, 1);
    mainCardLayout->addLayout(row3);
}

// Destructor
TranscodeMonitorCard::~TranscodeMonitorCard()
{
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

    const QString fpsText = std::isfinite(fps) && fps >= 0 ? QString::number(fps, 'f', 0) + " FPS" : "FPS N/A";
    const QString speedText = std::isfinite(speed) && speed >= 0 ? QString::number(speed, 'f', 2) + "x" : "Speed N/A";
    m_lblPerf->setText(fpsText + " | " + speedText);
    m_lblTime->setText(QString("Elapsed: %1 | Remaining: %2").arg(elapsedStr, etaStr));

    // Display sizing details. Hide projection until at least 3% is processed to avoid math skew.
    if (percentage >= 3) {
        m_lblSize->setText(
            QString("Size: %1 MB / ~%2 MB (Orig: %3 MB)")
                .arg(QString::number(outSizeMb, 'f', 1),
                     QString::number(projectedSizeMb, 'f', 1),
                     QString::number(static_cast<double>(QFileInfo(m_filepath).size()) / (1024.0 * 1024.0), 'f', 1))
        );
    } else {
        m_lblSize->setText(
            QString("Size: %1 MB / Calculating...")
                .arg(QString::number(outSizeMb, 'f', 1))
        );
    }

    m_progressBar->setRange(0, percentage < 0 ? 0 : 100);
    if (percentage >= 0) m_progressBar->setValue(percentage);
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
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(0);
    }
}

// Constructor: Initializes the main window interface components.
MainWindow::MainWindow(QWidget *parent, const QString &dbPath)
    : QMainWindow(parent)
    , m_databasePath(dbPath)
    , m_viewfinderProcess(nullptr)
    , m_crf(28)
    , m_preset("medium")
    , m_downscale(true)
    , m_debob(true)
    , m_livePreviewEnabled(false)
    , m_concurrentLimit(1)
    , m_dbManager(nullptr)
    , m_isQueueRunning(false)
{
    initUi(); // Build the window layouts and components
    checkDependencies(); // Verify if ffmpeg/ffprobe exist

    // Set Window Icon from compiled Qt resources
    setWindowIcon(QIcon(":/app_icon.png"));
}

// Destructor: Safely stops and joins any active transcode threads on exit.
MainWindow::~MainWindow()
{
    if (m_scanWorker) { m_scanWorker->stop(); m_scanWorker->wait(); }
    for (auto *worker : m_workers) worker->stop();
    for (TranscodeWorker *worker : m_workers) {
        worker->stop(); // Request threads to abort
        worker->wait(); // Join thread execution
        delete worker;  // Clean up memory
    }
    m_workers.clear();

    if (m_viewfinderProcess && m_viewfinderProcess->state() != QProcess::NotRunning) {
        m_viewfinderProcess->kill();
        m_viewfinderProcess->waitForFinished();
    }
    delete m_viewfinderProcess;
    delete m_dbManager; // Release global database object handle
}

// Builds the graphical layout, sidebar configurations, grid tables, and status meters.
// Builds the graphical layout, sidebar configurations, grid tables, and status meters.
void MainWindow::initUi()
{
    setWindowTitle(QString("HEVC Video Shrinker v%1").arg(QString::fromLatin1(appVersion)));
    resize(1050, 620); // Widened default window size to fit columns without clipping text

    // 1. Create the Menu Bar
    m_menuBar = new QMenuBar(this);
    setMenuBar(m_menuBar);

    // File Menu
    m_menuFile = m_menuBar->addMenu("File");
    m_actOpen = m_menuFile->addAction("Open Workspace Folder...");
    m_actOpen->setShortcut(QKeySequence::Open);
    connect(m_actOpen, &QAction::triggered, this, &MainWindow::selectDirectory);

    m_actScan = m_menuFile->addAction("Scan Directory");
    m_actScan->setShortcut(QKeySequence(Qt::Key_F5));
    connect(m_actScan, &QAction::triggered, this, &MainWindow::onScanSelected);

    m_menuFile->addSeparator();
    m_actExit = m_menuFile->addAction("Exit");
    m_actExit->setMenuRole(QAction::QuitRole);
    connect(m_actExit, &QAction::triggered, this, &MainWindow::onExitSelected);

    // View Menu
    m_menuView = m_menuBar->addMenu("View");
    m_actShowPreview = m_menuView->addAction("Show Live Preview");
    m_actShowPreview->setCheckable(true);
    m_actShowPreview->setChecked(m_livePreviewEnabled);
    connect(m_actShowPreview, &QAction::toggled, this, &MainWindow::onTogglePreviewSelected);

    // Options Menu
    m_menuOptions = m_menuBar->addMenu("Options");
    
    m_actCrf = m_menuOptions->addAction("Transcode Quality (CRF)...");
    connect(m_actCrf, &QAction::triggered, this, &MainWindow::onCrfSelected);

    // Preset Speed Submenu
    m_menuPreset = m_menuOptions->addMenu("Speed Preset");
    m_presetGroup = new QActionGroup(this);
    QStringList presets = {"ultrafast", "superfast", "veryfast", "fast", "medium", "slow", "slower", "veryslow"};
    for (const QString &p : presets) {
        QAction *actP = m_menuPreset->addAction(p);
        actP->setCheckable(true);
        if (p == m_preset) actP->setChecked(true);
        m_presetGroup->addAction(actP);
    }
    connect(m_presetGroup, &QActionGroup::triggered, this, &MainWindow::onPresetSelected);

    m_actConcurrency = m_menuOptions->addAction("Max Concurrent Encodes...");
    connect(m_actConcurrency, &QAction::triggered, this, &MainWindow::onConcurrencySelected);

    m_menuOptions->addSeparator();

    m_actDownscale = m_menuOptions->addAction("Downscale 4K/UHD to 1080p");
    m_actDownscale->setCheckable(true);
    m_actDownscale->setChecked(m_downscale);
    connect(m_actDownscale, &QAction::toggled, this, &MainWindow::onDownscaleToggled);

    m_actDebob = m_menuOptions->addAction("High frame-rate de-bob (bwdif)");
    m_actDebob->setCheckable(true);
    m_actDebob->setChecked(m_debob);
    connect(m_actDebob, &QAction::toggled, this, &MainWindow::onDebobToggled);

    m_menuOptions->addSeparator();

    m_actResetConfig = m_menuOptions->addAction("Restore settings to defaults");
    connect(m_actResetConfig, &QAction::triggered, this, &MainWindow::onResetConfigSelected);

    auto *advanced = m_menuBar->addMenu("Advanced");
    m_actNvenc = advanced->addAction("Use NVIDIA hardware encoding");
    m_actNvenc->setCheckable(true);
    m_actNvenc->setChecked(m_nvenc);
    m_actNvenc->setToolTip("Faster encoding with some loss of detail. Uses fixed settings and requires a compatible NVIDIA GPU and driver.");
    connect(m_actNvenc, &QAction::toggled, this, &MainWindow::onNvencToggled);
    advanced->addSeparator();
    m_actAvisynth = advanced->addAction("AviSynth cleanup…");
    connect(m_actAvisynth, &QAction::triggered, this, &MainWindow::configureAvisynth);
    m_actClearQueue = m_menuFile->addAction("Clear Queue");
    connect(m_actClearQueue, &QAction::triggered, this, &MainWindow::clearQueue);

    // Help Menu
    m_menuHelp = m_menuBar->addMenu("Help");
    QAction *actGuide = m_menuHelp->addAction("Usage Guide");
    connect(actGuide, &QAction::triggered, this, &MainWindow::onUsageGuideSelected);
    
    QAction *actAbout = m_menuHelp->addAction("About HEVC Video Shrinker");
    actAbout->setMenuRole(QAction::AboutRole);
    connect(actAbout, &QAction::triggered, this, &MainWindow::onAboutSelected);

    // 2. Central Widget layout
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    QHBoxLayout *mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(10);

    // Left Panel: Splitter dividing the video queue table (Top) and active monitors (Bottom)
    QSplitter *rightSplitter = new QSplitter(Qt::Vertical);
    rightSplitter->setHandleWidth(4);

    // Queue Table Group Box
    QGroupBox *tableGroup = new QGroupBox("Videos scan queue");
    QVBoxLayout *tableLayout = new QVBoxLayout(tableGroup);
    tableLayout->setContentsMargins(5, 10, 5, 5);

    m_tableQueue = new QTableWidget(0, 7);
    m_tableQueue->setHorizontalHeaderLabels({
        "Filename", "Status", "Original Size", "Compressed Size", "Progress", "Path", "AviSynth"
    });
    m_tableQueue->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tableQueue->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tableQueue->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tableQueue, &QTableWidget::customContextMenuRequested, this, &MainWindow::showTableContextMenu);
    // Connect table row selection change signal
    connect(m_tableQueue, &QTableWidget::itemSelectionChanged, this, &MainWindow::onQueueTableSelectionChanged);

    QHeaderView *header = m_tableQueue->horizontalHeader();
    for (int col = 0; col < m_tableQueue->columnCount(); ++col) {
        header->setSectionResizeMode(col, QHeaderView::Interactive);
    }
    header->setStretchLastSection(true);

    m_tableQueue->setColumnWidth(0, 140);
    m_tableQueue->setColumnWidth(1, 80);
    m_tableQueue->setColumnWidth(2, 95);
    m_tableQueue->setColumnWidth(3, 115);
    m_tableQueue->setColumnWidth(4, 160);
    m_tableQueue->setColumnWidth(5, 100);
    m_tableQueue->setColumnWidth(6, 120);

    tableLayout->addWidget(m_tableQueue);
    rightSplitter->addWidget(tableGroup);

    // Active Monitor dashboard area (Compact vertical stack inside scroll area)
    QWidget *bottomDashboard = new QWidget();
    QVBoxLayout *bottomLayout = new QVBoxLayout(bottomDashboard);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(5);

    QScrollArea *scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_monitorsContainer = new QWidget();
    m_monitorsLayout = new QVBoxLayout(m_monitorsContainer);
    m_monitorsLayout->setContentsMargins(0, 0, 0, 0);
    m_monitorsLayout->setSpacing(6);
    m_monitorsLayout->addStretch(); // Push progress cards to top initially

    scrollArea->setWidget(m_monitorsContainer);
    bottomLayout->addWidget(scrollArea, 1);

    rightSplitter->addWidget(bottomDashboard);
    rightSplitter->setSizes({420, 200});

    // Right Sidebar Layout
    QWidget *leftContainer = new QWidget();
    leftContainer->setFixedWidth(310);
    QVBoxLayout *leftLayout = new QVBoxLayout(leftContainer);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(10);

    // Settings Configuration summary label
    m_lblConfigSummary = new QLabel();
    m_lblConfigSummary->setWordWrap(true);
    m_lblConfigSummary->setStyleSheet("font-weight: bold; font-size: 11px; padding: 4px; border: 1px solid palette(mid); border-radius: 4px; background-color: palette(alternate-base);");
    updateConfigSummaryText();
    leftLayout->addWidget(m_lblConfigSummary);
    m_lblAvisynthSummary = new QLabel("AviSynth: OFF for this batch");
    m_lblAvisynthSummary->setWordWrap(true);
    leftLayout->addWidget(m_lblAvisynthSummary);

    // Live Viewfinder monitor frame
    m_viewfinderGroup = new QGroupBox("Live Viewfinder");
    QVBoxLayout *viewfinderLayout = new QVBoxLayout(m_viewfinderGroup);
    viewfinderLayout->setContentsMargins(10, 10, 10, 10);
    viewfinderLayout->setSpacing(6);

    m_lblViewfinder = new QLabel("Standby / Select active encode");
    m_lblViewfinder->setAlignment(Qt::AlignCenter);
    m_lblViewfinder->setFixedSize(240, 240);
    m_lblViewfinder->setStyleSheet("background-color: black; border: 1px solid palette(mid); border-radius: 4px; color: gray; font-weight: bold;");
    viewfinderLayout->addWidget(m_lblViewfinder, 0, Qt::AlignCenter);

    m_lblViewfinderStatus = new QLabel("Now Viewing: Standby");
    m_lblViewfinderStatus->setStyleSheet("font-weight: bold; font-size: 11px;");
    viewfinderLayout->addWidget(m_lblViewfinderStatus);

    m_lblViewfinderTime = new QLabel("Current Time: N/A");
    m_lblViewfinderTime->setStyleSheet("font-size: 11px;");
    viewfinderLayout->addWidget(m_lblViewfinderTime);

    leftLayout->addWidget(m_viewfinderGroup);
    m_viewfinderGroup->setVisible(m_livePreviewEnabled);

    // Initialize Viewfinder Process
    m_viewfinderProcess = new QProcess(this);
    connect(m_viewfinderProcess, &QProcess::readyReadStandardOutput, this, &MainWindow::onViewfinderDataAvailable);
    connect(m_viewfinderProcess, &QProcess::finished, this, &MainWindow::onViewfinderProcessFinished);

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

    // Space Savings Scoreboard
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

    // Action Execution Buttons
    QVBoxLayout *actionsLayout = new QVBoxLayout();
    actionsLayout->setSpacing(6);

    m_btnScan = new QPushButton("Scan Directory");
    m_btnScan->setEnabled(false);
    m_btnScan->setFixedHeight(28);
    connect(m_btnScan, &QPushButton::clicked, this, &MainWindow::onScanSelected);
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
    leftLayout->addStretch();

    mainLayout->addWidget(rightSplitter, 1);
    mainLayout->addWidget(leftContainer, 0);
}

void MainWindow::updateConfigSummaryText()
{
    QString summary = "<b>Current Settings:</b><br/>Codec: H.265 (HEVC)<br/>";
    summary += m_nvenc ? QString("Encoder: NVIDIA NVENC<br/>Quality: Fixed<br/>")
        : QString("Encoder: CPU x265<br/>Quality: CRF %1<br/>CPU Speed: %2<br/>").arg(m_crf).arg(m_preset);
    summary += QString("De-bob: %1 | Downscale: %2<br/>Concurrency: %3 encodes in parallel")
        .arg(m_debob ? "ON" : "OFF", m_downscale ? "ON" : "OFF", QString::number(m_concurrentLimit));
    if (m_lblConfigSummary) {
        m_lblConfigSummary->setText(summary);
    }
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
    if (m_scanWorker || !m_workers.isEmpty() || m_isQueueRunning) return;
    // Open directories dialog
    QString dirSelected = QFileDialog::getExistingDirectory(this, "Select Workspace Folder", m_rootDir);
    if (dirSelected.isEmpty()) return;

    m_rootDir = QDir(dirSelected).absolutePath();

    // Release old database managers
    delete m_dbManager;
    m_dbManager = nullptr;

    try {
        QFileInfo dirInfo(m_rootDir);
        if (dirInfo.exists() && dirInfo.isDir()) {
            m_dbManager = new DatabaseManager(m_databasePath);
            if (!m_dbManager->isReady()) throw std::runtime_error(m_dbManager->lastError().toStdString());
            logMessage(QString("Selected workspace: %1").arg(m_rootDir));
            
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
        m_btnScan->setEnabled(false);
        m_btnResetDb->setEnabled(false);
        m_btnResetScoreboard->setEnabled(false);
    }

    clearAvisynthAssignments();
    // Reset GUI queue state
    m_tableQueue->setRowCount(0);
    m_scannedFiles.clear();
    m_activeTranscodeQueue.clear();
    m_btnStart->setEnabled(false);
    updateSavingsDashboard();
}

// Scanning owns its database/processes on a worker thread; no nested GUI event loops.
void MainWindow::scanDirectory()
{
    if (m_rootDir.isEmpty() || m_scanWorker || m_isQueueRunning || !m_workers.isEmpty()) return;
    clearAvisynthAssignments();
    m_actNvenc->setEnabled(false);
    m_actAvisynth->setEnabled(false);
    m_actClearQueue->setEnabled(false);
    m_btnStart->setEnabled(false);
    m_btnScan->setEnabled(false);
    m_actOpen->setEnabled(false);
    m_actScan->setEnabled(false);
    m_menuOptions->setEnabled(false);
    m_btnResetDb->setEnabled(false);
    m_btnResetScoreboard->setEnabled(false);
    m_btnStop->setText("Cancel Scan");
    m_btnStop->setEnabled(true);
    m_scannedFiles.clear();
    m_activeTranscodeQueue.clear();
    m_tableQueue->setRowCount(0);
    const QVariantMap settings{{"crf", m_crf}, {"preset", m_preset}, {"nvenc", m_nvenc},
        {"downscale", m_downscale}, {"debob", m_debob}};
    m_scanWorker = new ScanWorker(m_rootDir, m_dbManager ? m_dbManager->path() : m_databasePath, settings, this);
    connect(m_scanWorker, &ScanWorker::logSignal, this, &MainWindow::logMessage);
    connect(m_scanWorker, &QThread::finished, this, &MainWindow::scanFinished);
    statusBar()->showMessage("Scanning workspace...");
    m_scanWorker->start();
}

void MainWindow::scanFinished()
{
    auto *worker = m_scanWorker;
    if (!worker) return;
    worker->wait();
    if (!worker->wasCancelled() && worker->error.isEmpty()) m_scannedFiles = worker->files;
    const QString message = worker->wasCancelled() ? "Scan cancelled."
        : worker->error.isEmpty() ? QString("Scan complete: %1 videos.").arg(m_scannedFiles.size()) : worker->error;
    worker->deleteLater();
    m_scanWorker = nullptr;
    m_tableQueue->setUpdatesEnabled(false);
    m_tableQueue->setRowCount(m_scannedFiles.size());
    for (int row = 0; row < m_scannedFiles.size(); ++row) {
        const auto &file = m_scannedFiles[row];
        const bool previous = file.history.found && file.status != "Pending";
        const double original = previous ? file.history.originalSize : file.size;
        const double compressedSize = previous ? file.history.compressedSize : file.size;
        const QString compressed = file.status == "Completed" || file.status == "Skipped"
            ? QString("%1 MB").arg(compressedSize / (1024.0 * 1024.0), 0, 'f', 1) : "N/A";
        const QStringList cells{file.filename, file.status, QString("%1 MB").arg(original / (1024.0 * 1024.0), 0, 'f', 1),
            compressed, file.detail, file.relPath, "Off"};
        for (int col = 0; col < cells.size(); ++col) {
            auto *item = new QTableWidgetItem(cells[col]);
            if (file.status != "Pending") {
                item->setBackground(file.status == "Completed" ? Qt::darkGreen : file.status == "Skipped" ? Qt::darkYellow : Qt::darkRed);
                item->setForeground(Qt::white);
            }
            m_tableQueue->setItem(row, col, item);
        }
    }
    m_tableQueue->setUpdatesEnabled(true);
    m_actOpen->setEnabled(true);
    m_actScan->setEnabled(true);
    m_menuOptions->setEnabled(true);
    m_actNvenc->setEnabled(true);
    m_actAvisynth->setEnabled(true);
    m_actClearQueue->setEnabled(true);
    m_btnScan->setEnabled(true);
    m_btnResetDb->setEnabled(true);
    m_btnResetScoreboard->setEnabled(true);
    m_btnStop->setText("Abort Job");
    m_btnStop->setEnabled(false);
    updateStartButtonState();
    updateSavingsDashboard();
    logMessage(message);
    statusBar()->showMessage(message);
}

void MainWindow::refreshScanForSettings()
{
    if (!m_rootDir.isEmpty() && !m_scanWorker && !m_isQueueRunning && m_workers.isEmpty()) scanDirectory();
}

// Triggered by clicking "Start Queue". Spawns initial concurrent threads.
void MainWindow::startProcessing()
{
    if (m_activeTranscodeQueue.isEmpty() || m_rootDir.isEmpty() || m_scanWorker || !m_workers.isEmpty() || m_isQueueRunning) return;

    m_actNvenc->setEnabled(false);
    m_actAvisynth->setEnabled(false);
    m_actClearQueue->setEnabled(false);
    updateAvisynthSummary();
    // Lock configuration UI menus during queue execution (keep parent menus enabled, lock individual items)
    m_actOpen->setEnabled(false);
    m_actScan->setEnabled(false);
    m_actExit->setEnabled(false);
    m_actCrf->setEnabled(false);
    m_menuPreset->setEnabled(false);
    m_actDownscale->setEnabled(false);
    m_actDebob->setEnabled(false);
    m_actResetConfig->setEnabled(false);
    m_btnScan->setEnabled(false);
    m_btnStart->setEnabled(false);
    m_btnResetDb->setEnabled(false);
    m_btnResetScoreboard->setEnabled(false);
    m_btnStop->setEnabled(true);
    m_isQueueRunning = true;
    m_stopping = false;

    m_pendingQueue = m_activeTranscodeQueue;
    m_latestFileTimestamps.clear();

    int filteredCount = 0;
    for (const auto &path : m_pendingQueue) if (m_filterAssignments.contains(path)) ++filteredCount;
    logMessage(QString("Starting %1 files; AviSynth enabled for %2.").arg(m_pendingQueue.count()).arg(filteredCount));
    statusBar()->showMessage(QString("Starting queue: %1 files, %2 with AviSynth cleanup.").arg(m_pendingQueue.count()).arg(filteredCount));

    // Clear bottom progress card panel layout
    if (m_monitorsLayout->count() > 0) {
        QLayoutItem *item = m_monitorsLayout->takeAt(0);
        if (item) {
            delete item;
        }
    }

    // Spawn initial worker slots up to concurrent setting limit
    int maxConcurrent = m_concurrentLimit;
    for (int i = 0; i < maxConcurrent; ++i) {
        startNextQueueJob();
    }
}

// Instantiates a new TranscodeWorker thread, connects signals, and starts execution.
void MainWindow::startNextQueueJob()
{
    if (!m_isQueueRunning || m_pendingQueue.isEmpty() || m_workers.size() >= m_concurrentLimit) return;

    QString filepath = m_pendingQueue.takeFirst();
    logMessage(QString("Dispatching transcode job: %1").arg(QFileInfo(filepath).fileName()));

    QVariantMap settings = currentEncodeSettings();
    settings["live_preview"] = m_livePreviewEnabled && m_viewfinderFocusedFile == filepath;
    if (m_filterAssignments.contains(filepath)) {
        const auto assignment = m_filterAssignments.value(filepath);
        settings["avs_enabled"] = true;
        settings["avs_name"] = assignment.profile.name;
        settings["avs_filters"] = assignment.profile.filters;
        settings["avs_source_size"] = assignment.sourceSize;
        settings["avs_source_modified"] = assignment.sourceModified;
        for (const QString &tool : {QString("ffmpeg"), QString("ffprobe")}) {
            const QString override = m_avisynthTools.value("avs_" + tool).toString();
            if (!override.isEmpty()) settings[tool] = override;
        }
    }

    QString dbPath = m_dbManager ? m_dbManager->path() : m_databasePath;
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
    m_monitorsLayout->addWidget(card);
    m_activeCards.insert(filepath, card);

    // Add stretch back at the end
    m_monitorsLayout->addStretch();

    // Connect Worker Thread signals (redirect worker metrics to MainWindow slots)
    connect(worker, &TranscodeWorker::logSignal, this, &MainWindow::logMessage);
    connect(worker, &TranscodeWorker::progressSignal, this, &MainWindow::updateProgress);
    connect(worker, &TranscodeWorker::statusSignal, this, &MainWindow::updateStatus);
    connect(worker, &TranscodeWorker::fileDoneSignal, this, &MainWindow::fileDone);
    connect(worker, &QThread::finished, this, &MainWindow::onWorkerFinished);
    connect(worker, &TranscodeWorker::previewFrameSignal, this, &MainWindow::onWorkerPreviewFrameRequested);
    connect(worker, &TranscodeWorker::encodedPreviewSignal, this, &MainWindow::onEncodedPreviewReady);

    m_workers.append(worker); // Keep track of the active worker thread handle
    worker->start(); // Launch background execution loop
}

// Callback slot when a background worker thread completes its execution.
void MainWindow::onWorkerFinished()
{
    TranscodeWorker *worker = qobject_cast<TranscodeWorker*>(sender());
    if (!worker) return;

    worker->wait(); // Join thread-local cleanup before releasing its owner.
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
        if (m_workers.count() < m_concurrentLimit) {
            startNextQueueJob();
        }
    } else if (m_pendingQueue.isEmpty() && m_workers.isEmpty()) {
        // Stop execution if both pending queue and active threads are exhausted
        processingFinished();
    }
}

// Receives preview extraction requests from worker threads and routes them to corresponding cards
void MainWindow::onWorkerPreviewFrameRequested(const QString &filepath, double secs)
{
    m_latestFileTimestamps[filepath] = secs;

    if (m_livePreviewEnabled && m_viewfinderFocusedFile == filepath) {
        updateViewfinderFrame(filepath, secs);
    }
}

void MainWindow::onEncodedPreviewReady(const QString &filepath, const QByteArray &png, double secs)
{
    if (!m_livePreviewEnabled || filepath != m_viewfinderFocusedFile || !m_isQueueRunning) return;
    QPixmap frame;
    if (!frame.loadFromData(png, "PNG")) return;
    m_lblViewfinder->setPixmap(frame.scaled(m_lblViewfinder->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_lblViewfinderTime->setText(QString("Encoded preview: %1:%2:%3")
        .arg(int(secs) / 3600, 2, 10, QChar('0')).arg(int(secs) / 60 % 60, 2, 10, QChar('0'))
        .arg(int(secs) % 60, 2, 10, QChar('0')));
}

// Cancellation is asynchronous. Keep result signals connected so active rows
// reach Cancelled (or Completed if their file commit already started).
void MainWindow::stopProcessing()
{
    if (m_scanWorker) {
        m_scanWorker->stop();
        m_btnStop->setEnabled(false);
        return;
    }
    if (m_workers.isEmpty()) return;
    m_stopping = true;
    m_isQueueRunning = false;
    m_pendingQueue.clear();
    m_btnStop->setEnabled(false);
    for (auto *worker : m_workers) worker->stop();
    if (m_viewfinderProcess->state() != QProcess::NotRunning) m_viewfinderProcess->kill();
    statusBar()->showMessage("Stopping active jobs...");
}

// Restores user settings back to initial baseline presets
void MainWindow::resetSettings()
{
    clearAvisynthAssignments();
    m_crf = 28;
    m_preset = "medium";
    m_downscale = true;
    m_debob = true;
    m_livePreviewEnabled = false;
    m_concurrentLimit = 1;
    m_actNvenc->setChecked(false);
    m_nvenc = false;
    m_actCrf->setEnabled(true);
    m_menuPreset->setEnabled(true);

    // Update menu action states
    m_actShowPreview->setChecked(false);
    m_actDownscale->setChecked(true);
    m_actDebob->setChecked(true);
    m_viewfinderGroup->setVisible(false);

    // Update preset check state
    for (QAction *action : m_presetGroup->actions()) {
        if (action->text() == "medium") {
            action->setChecked(true);
            break;
        }
    }

    updateConfigSummaryText();
    logMessage("Configuration settings reset to defaults.");
    refreshScanForSettings();
}

// Writes print statements to standard output.
void MainWindow::logMessage(const QString &message)
{
    qDebug().noquote() << message;
    if (message.startsWith("[Failed]") || message.contains("History could not be saved"))
        statusBar()->showMessage(message);
}

// Updates percentages and sizing projections in the queue grid row and routes to monitor cards.
void MainWindow::updateProgress(const QString &filepath, int percentage, double fps, double speed, const QString &etaStr, double outSizeMb, double projectedSizeMb)
{
    int idx = findRowByFilepath(filepath);
    if (idx != -1) {
        // Format table progress cell details (e.g. "48% (1.5x | ETA 03:12)")
        QString statsStr = (etaStr != "N/A") ? QString(" (%1x | ETA %2)").arg(QString::number(speed, 'f', 1), etaStr) : "";
        const QString progressText = percentage < 0 ? "Encoding…" + statsStr
            : QString("%1%%2").arg(QString::number(percentage), statsStr);
        m_tableQueue->setItem(idx, 4, new QTableWidgetItem(progressText));

        // Update the "Compressed Size" column dynamically while transcoding is running.
        // Once 3% or more is processed, we display "Current Size / ~Estimated Size" (formatted to 1 decimal place).
        if (percentage >= 3) {
            m_tableQueue->setItem(idx, 3, new QTableWidgetItem(QString("%1 MB / ~%2 MB")
                .arg(QString::number(outSizeMb, 'f', 1), QString::number(projectedSizeMb, 'f', 1))));
        } else {
            m_tableQueue->setItem(idx, 3, new QTableWidgetItem(QString("%1 MB")
                .arg(QString::number(outSizeMb, 'f', 1))));
        }
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
            for (int col = 0; col < m_tableQueue->columnCount(); ++col) {
                QTableWidgetItem *item = m_tableQueue->item(idx, col);
                if (item) {
                    item->setBackground(Qt::darkBlue);
                    item->setForeground(Qt::white);
                }
            }
            // Automatically select this row if nothing is currently selected or focused in the viewfinder
            if (m_livePreviewEnabled && m_viewfinderFocusedFile.isEmpty()) {
                autoSelectActiveRow();
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
    Q_UNUSED(oldSize);
    int idx = findRowByFilepath(filepath);
    if (idx != -1) {
        m_tableQueue->setItem(idx, 1, new QTableWidgetItem(status));
        
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

        for (int col = 0; col < m_tableQueue->columnCount(); ++col) {
            QTableWidgetItem *item = m_tableQueue->item(idx, col);
            if (item) {
                item->setBackground(color);
                item->setForeground(Qt::white);
            }
        }

        updateSavingsDashboard();

        // If the finished file was the one focused in the viewfinder, shift focus to another active transcode
        if (m_livePreviewEnabled && m_viewfinderFocusedFile == filepath) {
            autoSelectActiveRow();
        }
    }
}

// Cleans up panels and unlocks sidebar input controls when all queued transcoding files are completed.
void MainWindow::processingFinished()
{
    logMessage(m_stopping ? "Queue stopped." : "All transcode queue tasks finished.");
    m_isQueueRunning = false;
    m_stopping = false;
    m_actNvenc->setEnabled(true);
    m_actAvisynth->setEnabled(true);
    m_actClearQueue->setEnabled(true);
    m_actConcurrency->setEnabled(true);
    // Enable menu actions again (keep parent menus enabled, unlock individual items)
    m_actOpen->setEnabled(true);
    m_actScan->setEnabled(true);
    m_actExit->setEnabled(true);
    m_actCrf->setEnabled(!m_nvenc);
    m_menuPreset->setEnabled(!m_nvenc);
    m_actDownscale->setEnabled(true);
    m_actDebob->setEnabled(true);
    m_actResetConfig->setEnabled(true);
    m_btnScan->setEnabled(true); // Unlock scan
    m_btnResetDb->setEnabled(true); // Re-enable database reset
    m_btnResetScoreboard->setEnabled(true); // Re-enable scoreboard reset
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
    m_btnStart->setEnabled(!m_scanWorker && !m_isQueueRunning && m_workers.isEmpty() && !m_activeTranscodeQueue.isEmpty());
    updateAvisynthSummary();
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
    QTableWidgetItem *statusItem = m_tableQueue->item(row, 1);
    QTableWidgetItem *pathItem = m_tableQueue->item(row, 5);
    if (!pathItem) return;

    QString status = statusItem ? statusItem->text() : "";
    QString relPath = pathItem->text();
    QString absPath = QDir(m_rootDir).filePath(relPath);

    // If transcoding has completed, the original file is moved to .Trash
    // and the processed .mkv output sits in its place.
    QString playPath = absPath;
    if (status == "Completed") {
        QFileInfo fi(absPath);
        QString mkvPath = fi.absolutePath() + "/" + fi.completeBaseName() + ".mkv";
        if (QFile::exists(mkvPath)) {
            playPath = mkvPath;
        }
    }

    QMenu menu(this);
    QAction *playAction = menu.addAction("Play Video");
    QAction *avsAction = menu.addAction("AviSynth cleanup…");
    avsAction->setEnabled(!m_scanWorker && !m_isQueueRunning && m_workers.isEmpty());

    // Display context menu at mouse cursor screen coordinates
    QAction *selected = menu.exec(m_tableQueue->viewport()->mapToGlobal(pos));
    if (selected == avsAction) { configureAvisynth(); return; }
    if (selected == playAction) {
        // Opens the video asynchronously using the host OS's default media player
        QDesktopServices::openUrl(QUrl::fromLocalFile(playPath));
    }
}

// -------------------------------------------------------------
// Menu Slot Implementations
// -------------------------------------------------------------

void MainWindow::onScanSelected()
{
    scanDirectory();
}

// Called when the user clicks "Exit" in the File menu.
// This slot triggers the main window to close, shutting down the app.
void MainWindow::onExitSelected()
{
    close(); // Close the application window
}

// Called when the user toggles "Show Live Preview" in the View menu.
// Enables or disables visual thumbnail seeking to conserve computer resources.
void MainWindow::onTogglePreviewSelected(bool checked)
{
    m_livePreviewEnabled = checked; // Store the toggle state
    m_viewfinderGroup->setVisible(checked); // Show or hide the viewfinder widget box in the sidebar

    // If the user turned off live previews, reset the viewfinder layout back to its idle standby state
    if (!checked) {
        m_viewfinderFocusedFile = ""; // Forget whichever file was being inspected
        m_lblViewfinderStatus->setText("Now Viewing: Standby");
        m_lblViewfinderTime->setText("Current Time: N/A");
        m_lblViewfinder->clear(); // Clear out the last rendered picture frame
        m_lblViewfinder->setText("Standby / Select active encode");
        
        // Terminate any ongoing background ffmpeg process that was extracting a preview frame
        if (m_viewfinderProcess && m_viewfinderProcess->state() != QProcess::NotRunning) {
            m_viewfinderProcess->kill();
            m_viewfinderProcess->waitForFinished();
        }
        m_viewfinderBuffer.clear(); // Clear the image data buffer
    } else {
        // If turned on, check if the user already selected a row. If not, auto-select the first active transcode row.
        if (m_tableQueue->selectedItems().isEmpty()) {
            autoSelectActiveRow();
        } else {
            onQueueTableSelectionChanged();
        }
    }
    updateWorkerPreviewSelection();
}

void MainWindow::updateWorkerPreviewSelection()
{
    for (auto *worker : m_workers)
        worker->setLivePreviewEnabled(m_livePreviewEnabled && worker->property("filepath").toString() == m_viewfinderFocusedFile);
}

// Called when the user clicks "Transcode Quality (CRF)..." in the Options menu.
// Displays a popup dialog prompting the user to type or slide to a quality number.
void MainWindow::onCrfSelected()
{
    bool ok;
    // Open a native input window with a spinbox (0-51 range, default is current m_crf)
    int val = QInputDialog::getInt(this, "Set CRF Quality",
                                   "CRF value (0-51, higher = smaller size):",
                                   m_crf, 0, 51, 1, &ok);
    if (ok) {
        m_crf = val; // Store the new quality setting
        updateConfigSummaryText(); // Update the sidebar summary text label
        logMessage(QString("Transcode CRF quality set to: %1").arg(m_crf));
        refreshScanForSettings();
    }
}

// Called when the user selects a preset speed option from the "Speed Preset" submenu.
// Triggers whenever any speed preset menu option changes.
void MainWindow::onPresetSelected(QAction *action)
{
    if (action) {
        m_preset = action->text(); // Extract the speed text (e.g., "fast", "slow") from the menu item
        updateConfigSummaryText(); // Redraw the sidebar config text label
        logMessage(QString("CPU speed preset set to: %1").arg(m_preset));
        refreshScanForSettings();
    }
}

// Called when the user clicks "Max Concurrent Encodes..." in the Options menu.
// Spawns a numeric input pop-up to limit parallel transcode processes.
void MainWindow::onConcurrencySelected()
{
    bool ok;
    // Prompt the user for an integer number of parallel jobs (range 1-16)
    int val = QInputDialog::getInt(this, "Set Concurrency",
                                   "Max concurrent transcode processes:",
                                   m_concurrentLimit, 1, 16, 1, &ok);
    if (ok) setConcurrencyLimit(val);
}

void MainWindow::setConcurrencyLimit(int limit)
{
    m_concurrentLimit = qBound(1, limit, 16);
    updateConfigSummaryText();
    logMessage(QString("Max concurrent slots set to: %1").arg(m_concurrentLimit));
    // Raising the limit fills available slots immediately. Lowering it lets
    // existing workers finish; the completion handler observes the new limit.
    for (int slot = m_workers.size(); slot < m_concurrentLimit; ++slot)
        startNextQueueJob();
}

// Called when the user toggles "Downscale 4K/UHD to 1080p" in the Options menu.
void MainWindow::onDownscaleToggled(bool checked)
{
    m_downscale = checked; // Store checked status
    updateConfigSummaryText(); // Redraw the sidebar config text label
    logMessage(QString("Downscale 4K to 1080p set to: %1").arg(m_downscale ? "ON" : "OFF"));
    refreshScanForSettings();
}

// Called when the user toggles "High frame-rate de-bob (bwdif)" in the Options menu.
void MainWindow::onDebobToggled(bool checked)
{
    m_debob = checked; // Store checked status
    updateConfigSummaryText(); // Redraw the sidebar config text label
    logMessage(QString("High frame-rate de-bob set to: %1").arg(m_debob ? "ON" : "OFF"));
    refreshScanForSettings();
}

void MainWindow::onNvencToggled(bool checked)
{
    if (m_scanWorker || m_isQueueRunning || !m_workers.isEmpty()) {
        const QSignalBlocker blocker(m_actNvenc);
        m_actNvenc->setChecked(m_nvenc);
        return;
    }
    m_nvenc = checked;
    m_actCrf->setEnabled(!checked);
    m_menuPreset->setEnabled(!checked);
    updateConfigSummaryText();
    logMessage(checked ? "Video encoder: NVIDIA NVENC (fixed settings)." : "Video encoder: CPU x265.");
    // Backend selection does not change eligibility or invalidate batch filters.
}

// Called when the user clicks "Restore settings to defaults" in the Options menu.
void MainWindow::onResetConfigSelected()
{
    resetSettings(); // Restore configurations to initial baselines
}

// Called when the user clicks "Usage Guide" in the Help menu.
// Shows a dialog explaining standard operational sequences.
void MainWindow::onUsageGuideSelected()
{
    QMessageBox msgBox(this);
    msgBox.setWindowTitle("HEVC Video Shrinker - Usage Guide");
    msgBox.setTextFormat(Qt::RichText);
    msgBox.setIcon(QMessageBox::Information);
    msgBox.setText("<h3>Quick Start Guide</h3>"
        "<p><b>1. Select Workspace:</b> Go to File &gt; Open Workspace Folder to choose a directory containing your video files.</p>"
        "<p><b>2. Scan Workspace:</b> Click the 'Scan Directory' button (or File &gt; Scan Directory) to index all video files. The status list will color-code files that are already completed (green) or already compliant (yellow).</p>"
        "<p><b>3. Adjust Configurations:</b> Open the 'Options' menu to set target CRF quality, CPU speed preset, UHD downscaling, deinterlace bobbing, or max concurrent slots.</p>"
        "<p><b>Optional hardware encoding:</b> Advanced &gt; Use NVIDIA hardware encoding enables faster encoding with fixed settings. It requires a compatible NVIDIA GPU, driver, and FFmpeg. CPU quality and preset controls apply when hardware encoding is off.</p>"
        "<p><b>4. Start Queue:</b> Click 'Start Queue' in the sidebar to begin shrinking videos. Active progress indicators will stack vertically.</p>"
        "<p><b>5. Live Previews:</b> Enable View &gt; Show Live Preview. Select any actively-transcoding row in the table to display its live frame monitor in the sidebar viewfinder.</p>");
    msgBox.exec();
}

// Called when the user clicks "About HEVC Video Shrinker" in the Help menu.
// Shows information about application libraries, authors, and system dependencies.
void MainWindow::onAboutSelected()
{
    QString ffmpeg = findDependency("ffmpeg"); // Search host path for FFmpeg binary
    QString ffprobe = findDependency("ffprobe"); // Search host path for FFprobe binary
    
    QMessageBox msgBox(this);
    msgBox.setWindowTitle("About HEVC Video Shrinker");
    msgBox.setTextFormat(Qt::RichText);
    msgBox.setIcon(QMessageBox::Information);
    msgBox.setText(QString("<h3>HEVC Video Shrinker v%1</h3>"
                           "<p>A video compressor interface written in C++ and Qt6.</p>"
                           "<p><b>Dependencies Status:</b><br/>"
                           "• FFmpeg: %2<br/>"
                           "• FFprobe: %3</p>"
                           "<p>Released under the open-source GPLv3 license.</p>")
                   .arg(QString::fromLatin1(appVersion),
                        ffmpeg.isEmpty() ? QString("<font color='red'>Missing</font>") : QString("<font color='green'>Detected</font>"),
                        ffprobe.isEmpty() ? QString("<font color='red'>Missing</font>") : QString("<font color='green'>Detected</font>")));
    msgBox.exec();
}

// -------------------------------------------------------------
// Live Viewfinder Logic Implementations
// -------------------------------------------------------------

// Triggered when FFmpeg outputs picture data chunks to its standard output stream.
// Appends received image bytes to a buffer memory chunk.
void MainWindow::onViewfinderDataAvailable()
{
    if (m_viewfinderProcess) {
        m_viewfinderBuffer.append(m_viewfinderProcess->readAllStandardOutput());
    }
}

// Callback slot triggered when the viewfinder's FFmpeg subprocess finishes seeking.
// Parses the PNG data buffer and updates the pixel screen inside the sidebar.
void MainWindow::onViewfinderProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    // Make sure process exited normally without crash errors
    if (exitStatus == QProcess::NormalExit && exitCode == 0 && m_livePreviewEnabled
        && m_viewfinderProcess->property("previewFile").toString() == m_viewfinderFocusedFile) {
        if (m_viewfinderProcess) {
            // Read any leftover trailing bytes from stdout
            m_viewfinderBuffer.append(m_viewfinderProcess->readAllStandardOutput());
        }
        if (!m_viewfinderBuffer.isEmpty()) {
            QPixmap pixmap;
            // Load the image binary buffer as a PNG
            if (pixmap.loadFromData(m_viewfinderBuffer, "PNG")) {
                // Resize pixmap to fit the label viewport while maintaining aspect ratios (prevents stretching)
                QPixmap scaledPixmap = pixmap.scaled(m_lblViewfinder->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                m_lblViewfinder->setPixmap(scaledPixmap); // Render the picture
            }
        }
    }
    m_viewfinderBuffer.clear(); // Empty buffer for the next frame capture
}

// Slot triggered when the user highlights a different video row in the main queue list.
// Determines if the newly selected row is actively encoding, focusing the viewfinder if it is.
void MainWindow::onQueueTableSelectionChanged()
{
    // Fetch currently highlighted rows in the spreadsheet list
    QList<QTableWidgetItem*> selectedItems = m_tableQueue->selectedItems();
    if (selectedItems.isEmpty()) {
        m_viewfinderFocusedFile = ""; // Deselect
        m_lblViewfinderStatus->setText("Now Viewing: Standby");
        m_lblViewfinderTime->setText("Current Time: N/A");
        m_lblViewfinder->clear();
        m_lblViewfinder->setText("Standby / Select active encode");
        updateWorkerPreviewSelection();
        return;
    }

    // Identify which row was selected and fetch its filename and status text
    int row = selectedItems.first()->row();
    QTableWidgetItem *pathItem = m_tableQueue->item(row, 5); // Index 5 holds the relative file path
    QTableWidgetItem *statusItem = m_tableQueue->item(row, 1); // Index 1 holds the status text
    if (!pathItem || !statusItem) return;

    QString filepath = QDir(m_rootDir).filePath(pathItem->text()); // Build the absolute filepath
    QString status = statusItem->text();

    // If the highlighted video file is actively processing, point the viewfinder focus at it
    if (status == "Processing" && m_activeCards.contains(filepath)) {
        if (m_viewfinderFocusedFile != filepath) m_lblViewfinder->clear();
        m_viewfinderFocusedFile = filepath;
        m_lblViewfinderStatus->setText(QString("Now Viewing: %1").arg(QFileInfo(filepath).fileName()));
        
        // If we have a cached timestamp for this transcode job, request its preview frame immediately
        if (m_latestFileTimestamps.contains(filepath)) {
            updateViewfinderFrame(filepath, m_latestFileTimestamps[filepath]);
        } else {
            m_lblViewfinderTime->setText("Current Time: Seek starting...");
            updateViewfinderFrame(filepath, 0.0); // Start preview extraction from beginning
        }
    } else {
        // If the highlighted row is not actively processing, place viewfinder on standby
        m_viewfinderFocusedFile = "";
        m_lblViewfinderStatus->setText("Now Viewing: Standby");
        m_lblViewfinderTime->setText("Current Time: N/A");
        m_lblViewfinder->clear();
        m_lblViewfinder->setText("Standby / Select active encode");
    }
    updateWorkerPreviewSelection();
}

// Dispatches a seek execution command running FFmpeg asynchronously to pull one frame.
// Lays out the letterbox/pillarbox frame dynamically to match wide/tall aspect ratios.
void MainWindow::updateViewfinderFrame(const QString &filepath, double secs)
{
    if (!m_livePreviewEnabled) return; // Skip if live previews are disabled
    if (m_filterAssignments.contains(filepath)) {
        if (m_lblViewfinder->pixmap().isNull())
            m_lblViewfinder->setText("Waiting for encoded frames…");
        return;
    }

    // Ignore request if the viewfinder's FFmpeg process is currently busy seeking the last frame
    if (m_viewfinderProcess && m_viewfinderProcess->state() != QProcess::NotRunning) {
        return;
    }

    QString ffmpeg = findDependency("ffmpeg");
    if (ffmpeg.isEmpty()) return; // Abort if FFmpeg dependency is missing

    int videoIndex = -1;
    for (const auto &file : m_scannedFiles) {
        if (file.absolutePath == filepath) { videoIndex = file.metadata.videoIndex; break; }
    }
    if (videoIndex < 0) return;

    m_viewfinderBuffer.clear(); // Empty the previous image buffer

    // Format seconds value into HH:MM:SS.ms format required by FFmpeg seek parameter
    int h = static_cast<int>(secs / 3600);
    int m = static_cast<int>((secs - h * 3600) / 60);
    int s = static_cast<int>(secs - h * 3600 - m * 60);
    int ms = static_cast<int>((secs - h * 3600 - m * 60 - s) * 1000);
    QString timeStr = QString("%1:%2:%3.%4")
                      .arg(h, 2, 10, QChar('0'))
                      .arg(m, 2, 10, QChar('0'))
                      .arg(s, 2, 10, QChar('0'))
                      .arg(ms, 3, 10, QChar('0'));

    // Update time status label text
    m_lblViewfinderTime->setText(QString("Current Time: %1:%2:%3")
                                 .arg(h, 2, 10, QChar('0'))
                                 .arg(m, 2, 10, QChar('0'))
                                 .arg(s, 2, 10, QChar('0')));

    // Dispatch FFmpeg subprocess to extract exactly 1 frame at the target time
    // Normalizes to square pixels using iw*sar:ih, scales picture width to 240px, and pipes binary PNG output to stdout.
    m_viewfinderProcess->setProperty("previewFile", filepath);
    m_viewfinderProcess->start(ffmpeg, {
        "-y",
        "-ss", timeStr,
        "-i", filepath,
        "-map", QString("0:%1").arg(videoIndex),
        "-an", "-sn", "-dn",
        "-vf", "scale=iw*sar:ih,scale=240:-1",
        "-frames:v", "1",
        "-f", "image2pipe",
        "-vcodec", "png",
        "-"
    });
}

// Searches for the first active (processing) row in the grid table.
// If found, selects that row to focus the viewfinder preview on it.
void MainWindow::autoSelectActiveRow()
{
    // If live progress previews are disabled, we do not need to manage selection focus
    if (!m_livePreviewEnabled) return;

    // Check if the current focused file is still active/processing.
    // If it is, keep focusing on it to avoid jumping selection.
    if (!m_viewfinderFocusedFile.isEmpty()) {
        int focusedIdx = findRowByFilepath(m_viewfinderFocusedFile);
        if (focusedIdx != -1) {
            QTableWidgetItem *statusItem = m_tableQueue->item(focusedIdx, 1);
            if (statusItem && statusItem->text() == "Processing") {
                return; // Keep focus on the active transcode
            }
        }
    }

    // Traverse the spreadsheet grid to find the first active encoding task
    for (int row = 0; row < m_tableQueue->rowCount(); ++row) {
        QTableWidgetItem *statusItem = m_tableQueue->item(row, 1);
        if (statusItem && statusItem->text() == "Processing") {
            m_tableQueue->selectRow(row); // Select the active row
            onQueueTableSelectionChanged(); // Also focus a row already selected before Start.
            return;
        }
    }

    // If no active transcode is found, clear selection (viewfinder returns to standby)
    m_tableQueue->clearSelection();
}

QVariantMap MainWindow::currentEncodeSettings() const
{
    return {{"crf", m_crf}, {"preset", m_preset}, {"nvenc", m_nvenc}, {"downscale", m_downscale}, {"debob", m_debob},
        {"live_preview", m_livePreviewEnabled}, {"threads", qMax(1, QThread::idealThreadCount() / qMax(1, m_concurrentLimit))}};
}

void MainWindow::updateAvisynthSummary()
{
    if (!m_lblAvisynthSummary) return;
    int count = 0;
    for (const auto &path : m_activeTranscodeQueue) if (m_filterAssignments.contains(path)) ++count;
    m_lblAvisynthSummary->setText(count == 0 ? "AviSynth: OFF for queued files"
        : QString("AviSynth: ON for %1 queued file(s)\nCurrent batch only").arg(count));
    m_lblAvisynthSummary->setStyleSheet(count == 0 ? QString() : "font-weight: bold; color: #d88400;");
}

void MainWindow::assignAvisynth(const QStringList &paths, const AvisynthProfile &profile, bool remove)
{
    if (m_scanWorker || m_isQueueRunning || !m_workers.isEmpty()) return;
    for (const auto &path : paths) {
        const int row = findRowByFilepath(path);
        if (row < 0) continue;
        if (remove) {
            if (!m_filterAssignments.contains(path)) continue;
            const auto previous = m_filterAssignments.take(path);
            // A completed output must not be fed back through the old source assignment.
            if (m_tableQueue->item(row, 1)->text() != "Completed") {
                updateStatus(path, previous.previousStatus, previous.previousDetail);
            }
            m_tableQueue->setItem(row, 6, new QTableWidgetItem("Off"));
        } else {
            QFileInfo file(path);
            if (!file.isFile() || file.isSymLink()) {
                logMessage("Rescan before assigning a profile to a completed or changed source: " + path); continue;
            }
            FilterAssignment assignment;
            if (m_filterAssignments.contains(path)) assignment = m_filterAssignments.value(path);
            else {
                assignment.previousStatus = m_tableQueue->item(row, 1)->text();
                assignment.previousDetail = m_tableQueue->item(row, 4)->text();
            }
            assignment.profile = profile;
            assignment.sourceSize = file.size();
            assignment.sourceModified = file.lastModified().toMSecsSinceEpoch();
            m_filterAssignments.insert(path, assignment);
            updateStatus(path, "Pending", "AviSynth cleanup: " + profile.name);
            m_tableQueue->setItem(row, 6, new QTableWidgetItem(profile.name));
        }
    }
    updateStartButtonState();
}

void MainWindow::clearAvisynthAssignments()
{
    assignAvisynth(m_filterAssignments.keys(), {}, true);
    m_filterAssignments.clear();
    m_avisynthTools.clear();
    updateAvisynthSummary();
}

void MainWindow::clearQueue()
{
    if (m_scanWorker || m_isQueueRunning || !m_workers.isEmpty()) return;
    clearAvisynthAssignments();
    m_tableQueue->setRowCount(0);
    m_scannedFiles.clear(); m_activeTranscodeQueue.clear(); m_pendingQueue.clear();
    updateStartButtonState();
    statusBar()->showMessage("Queue cleared. AviSynth filtering is off.");
}

void MainWindow::configureAvisynth()
{
    if (m_scanWorker || m_isQueueRunning || !m_workers.isEmpty() || m_tableQueue->rowCount() == 0) return;
    QStringList selected;
    for (const auto &index : m_tableQueue->selectionModel()->selectedRows())
        selected.append(QDir(m_rootDir).filePath(m_tableQueue->item(index.row(), 5)->text()));
    int current = m_tableQueue->currentRow();
    if (current < 0) current = 0;
    const QString source = QDir(m_rootDir).filePath(m_tableQueue->item(current, 5)->text());
    if (selected.isEmpty()) selected.append(source);
    auto settings = currentEncodeSettings();
    for (auto it = m_avisynthTools.cbegin(); it != m_avisynthTools.cend(); ++it) settings.insert(it.key(), it.value());
    AvisynthDialog dialog(source, m_filterAssignments.value(source).profile, selected.size(), m_activeTranscodeQueue.size(), settings, this);
    if (dialog.exec() != QDialog::Accepted) return;
    QStringList targets;
    if (dialog.scope() == AvisynthDialog::ThisFile) targets.append(source);
    else if (dialog.scope() == AvisynthDialog::SelectedFiles) targets = selected;
    else targets = dialog.removeProfile() ? m_filterAssignments.keys() : m_activeTranscodeQueue;
    m_avisynthTools = dialog.toolSettings();
    assignAvisynth(targets, dialog.profile(), dialog.removeProfile());
}
