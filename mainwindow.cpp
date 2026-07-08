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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_dbManager(nullptr)
    , m_worker(nullptr)
    , m_previewProcess(nullptr)
    , m_transcodeStartTime(0)
{
    initUi();
    checkDependencies();

    // Set Window Icon from deployed application directory path
    QString iconPath = QCoreApplication::applicationDirPath() + "/app_icon.png";
    if (QFile::exists(iconPath)) {
        setWindowIcon(QIcon(iconPath));
    }
}

MainWindow::~MainWindow()
{
    if (m_worker) {
        m_worker->stop();
        m_worker->wait();
    }
    if (m_previewProcess) {
        m_previewProcess->kill();
        m_previewProcess->waitForFinished();
        delete m_previewProcess;
    }
    delete m_dbManager;
}

void MainWindow::initUi()
{
    setWindowTitle("HEVC Video Shrinker v1.0");
    resize(1050, 620);

    // Main central widget
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    QHBoxLayout *mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(10);

    // Left Panel: Splitter for Table and Log
    QSplitter *rightSplitter = new QSplitter(Qt::Vertical);
    rightSplitter->setHandleWidth(4);

    // 1. Table Group
    QGroupBox *tableGroup = new QGroupBox("Videos scan queue");
    QVBoxLayout *tableLayout = new QVBoxLayout(tableGroup);
    tableLayout->setContentsMargins(5, 10, 5, 5);

    m_tableQueue = new QTableWidget(0, 6);
    m_tableQueue->setHorizontalHeaderLabels({
        "Filename", "Status", "Original Size", "Compressed Size", "Progress", "Path"
    });
    m_tableQueue->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tableQueue->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // Configure headers to be interactive
    QHeaderView *header = m_tableQueue->horizontalHeader();
    for (int col = 0; col < 6; ++col) {
        header->setSectionResizeMode(col, QHeaderView::Interactive);
    }
    header->setStretchLastSection(true);

    m_tableQueue->setColumnWidth(0, 140); // Filename
    m_tableQueue->setColumnWidth(1, 80);  // Status
    m_tableQueue->setColumnWidth(2, 95);  // Original Size
    m_tableQueue->setColumnWidth(3, 115); // Compressed Size
    m_tableQueue->setColumnWidth(4, 160); // Progress
    m_tableQueue->setColumnWidth(5, 100); // Path (stretches)

    tableLayout->addWidget(m_tableQueue);
    rightSplitter->addWidget(tableGroup);

    // 2. Bottom Dashboard panel
    QWidget *bottomDashboard = new QWidget();
    QHBoxLayout *bottomLayout = new QHBoxLayout(bottomDashboard);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(10);

    // Center Column: Live Progress Preview
    m_previewGroup = new QGroupBox("Live Progress Preview");
    QVBoxLayout *previewLayout = new QVBoxLayout(m_previewGroup);
    previewLayout->setContentsMargins(5, 10, 5, 5);
    m_lblPreview = new QLabel("Preview Off");
    m_lblPreview->setAlignment(Qt::AlignCenter);
    m_lblPreview->setFixedSize(240, 240);
    m_lblPreview->setStyleSheet("background-color: black; border: 1px solid palette(mid); border-radius: 4px; color: gray; font-weight: bold;");
    previewLayout->addWidget(m_lblPreview);
    bottomLayout->addWidget(m_previewGroup, 0); // Keep fixed width
    m_previewGroup->setVisible(false); // Hidden initially

    // Right Column: Active Transcode Monitor Card
    m_statusCard = new QGroupBox("Active Transcode Monitor");
    m_statusCard->setStyleSheet(
        "QGroupBox { font-weight: bold; border: 1px solid palette(mid); border-radius: 4px; margin-top: 6px; padding-top: 10px; background-color: palette(window); }"
    );
    QGridLayout *cardLayout = new QGridLayout(m_statusCard);
    cardLayout->setContentsMargins(10, 10, 10, 10);
    cardLayout->setSpacing(6);

    // Row 0: File Name
    cardLayout->addWidget(new QLabel("Current File:"), 0, 0);
    m_lblStatusFile = new QLabel("None");
    m_lblStatusFile->setStyleSheet("font-weight: bold;");
    cardLayout->addWidget(m_lblStatusFile, 0, 1, 1, 3);

    // Row 1: Perf & Time details
    cardLayout->addWidget(new QLabel("Speed/FPS:"), 1, 0);
    m_lblStatusPerf = new QLabel("N/A");
    m_lblStatusPerf->setStyleSheet("font-weight: bold;");
    cardLayout->addWidget(m_lblStatusPerf, 1, 1);

    QLabel *lblTime = new QLabel("Time:");
    lblTime->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    cardLayout->addWidget(lblTime, 1, 2);
    m_lblStatusTime = new QLabel("N/A");
    m_lblStatusTime->setStyleSheet("font-weight: bold;");
    cardLayout->addWidget(m_lblStatusTime, 1, 3);

    // Row 2: Progress
    cardLayout->addWidget(new QLabel("Progress:"), 2, 0);
    m_statusProgressBar = new QProgressBar();
    m_statusProgressBar->setRange(0, 100);
    m_statusProgressBar->setValue(0);
    m_statusProgressBar->setTextVisible(true);
    m_statusProgressBar->setAlignment(Qt::AlignCenter);
    m_statusProgressBar->setStyleSheet(
        "QProgressBar { border: 1px solid palette(mid); border-radius: 3px; text-align: center; } QProgressBar::chunk { background-color: palette(highlight); }"
    );
    cardLayout->addWidget(m_statusProgressBar, 2, 1, 1, 3);

    // Row 3: Size details
    cardLayout->addWidget(new QLabel("Size Stats:"), 3, 0);
    m_lblStatusSize = new QLabel("N/A");
    m_lblStatusSize->setStyleSheet("font-weight: bold;");
    cardLayout->addWidget(m_lblStatusSize, 3, 1, 1, 3);

    bottomLayout->addWidget(m_statusCard, 2); // Stretch factor 2
    m_statusCard->setVisible(false); // Collapsed initially

    rightSplitter->addWidget(bottomDashboard);
    rightSplitter->setSizes({420, 200});

    // Right Panel: Control Sidebar
    QWidget *leftContainer = new QWidget();
    leftContainer->setFixedWidth(310);
    QVBoxLayout *leftLayout = new QVBoxLayout(leftContainer);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(10);

    // Control Group
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
    m_spinCrf->setValue(28);
    ctrlLayout->addWidget(m_spinCrf);

    // Speed Preset Combobox
    ctrlLayout->addWidget(new QLabel("FFmpeg CPU Speed Preset:"));
    m_comboPreset = new QComboBox();
    m_comboPreset->addItems({
        "ultrafast", "superfast", "veryfast", "fast", "medium", "slow", "slower", "veryslow"
    });
    m_comboPreset->setCurrentText("medium");
    ctrlLayout->addWidget(m_comboPreset);

    // Scaling Filter
    m_chkDownscale = new QCheckBox("Downscale 4K/UHD to 1080p");
    m_chkDownscale->setChecked(true);
    ctrlLayout->addWidget(m_chkDownscale);

    // Deinterlacing Filter (De-bob)
    m_chkDebob = new QCheckBox("High frame-rate de-bob (bwdif)");
    m_chkDebob->setChecked(true);
    ctrlLayout->addWidget(m_chkDebob);

    // Live Progress Preview
    m_chkPreview = new QCheckBox("Enable Live Progress Preview");
    m_chkPreview->setChecked(false);
    ctrlLayout->addWidget(m_chkPreview);
    connect(m_chkPreview, &QCheckBox::toggled, this, &MainWindow::togglePreview);

    // Reset Buttons
    m_btnReset = new QPushButton("Restore settings back to defaults");
    connect(m_btnReset, &QPushButton::clicked, this, &MainWindow::resetSettings);
    ctrlLayout->addWidget(m_btnReset);

    leftLayout->addWidget(ctrlGroup);

    // Status Panel (FFmpeg details)
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

    // Savings dashboard
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

    // Action Execution Buttons (Stacked: Row 1 has Scan; Row 2 has Start & Abort)
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

    leftLayout->addStretch();

    // Assemble Panels
    mainLayout->addWidget(rightSplitter, 1);
    mainLayout->addWidget(leftContainer, 0);
}

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

void MainWindow::selectDirectory()
{
    QString dirSelected = QFileDialog::getExistingDirectory(this, "Select Workspace Folder", m_rootDir);
    if (dirSelected.isEmpty()) return;

    m_rootDir = QDir(dirSelected).absolutePath();
    m_dirInput->setText(m_rootDir);

    delete m_dbManager;
    m_dbManager = nullptr;

    try {
        QFileInfo dirInfo(m_rootDir);
        if (dirInfo.exists() && dirInfo.isDir()) {
            m_dbManager = new DatabaseManager();
            logMessage(QString("Selected workspace: %1").arg(m_rootDir));
            
            QString localDbPath = QDir(m_rootDir).filePath("processed_files.db");
            if (QFile::exists(localDbPath)) {
                logMessage("Local database detected in workspace. Migrating records to global database...");
                m_dbManager->migrateLocalDatabase(localDbPath, m_rootDir);
                logMessage("Migration complete. Local database has been renamed to processed_files.db.migrated.");
            }
            
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

    m_tableQueue->setRowCount(0);
    m_scannedFiles.clear();
    m_activeTranscodeQueue.clear();
    m_btnStart->setEnabled(false);
}

static void scanDirRecursive(const QString &dirPath, const QString &rootDir, const QStringList &videoExtensions, QList<ScannedFile> &scannedFiles)
{
    // Ignore .Trash and .Errors subfolders
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
    m_tableQueue->setRowCount(0);

    QString ffprobeBin = findDependency("ffprobe");

    // Video extensions list (case-insensitive)
    static const QStringList videoExtensions = {
        "mp4", "mkv", "wmv", "avi", "mov", "flv", "mpeg", "mpg", "vid", "m4v", "asf", "f4v", "divx"
    };

    scanDirRecursive(m_rootDir, m_rootDir, videoExtensions, m_scannedFiles);

    logMessage(QString("Found %1 video files.").arg(m_scannedFiles.count()));

    m_tableQueue->setUpdatesEnabled(false); // OPTIMIZATION: Disable QTableWidget updates to prevent massive rendering lag
    m_tableQueue->setRowCount(m_scannedFiles.count());
    
    int cacheHits = 0;
    int cacheMisses = 0;

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

        // Query SQLite Database
        if (m_dbManager) {
            ProcessedFileInfo info = m_dbManager->getProcessedFileInfo(filepath);
            if (info.found) {
                isProcessed = true;
                origSizeDisplay = QString("%1 MB").arg(QString::number(static_cast<double>(info.originalSize) / (1024.0 * 1024.0), 'f', 1));
                compSizeDisplay = QString("%1 MB").arg(QString::number(static_cast<double>(info.compressedSize) / (1024.0 * 1024.0), 'f', 1));
            }
        }

        // Run compliance probe
        if (!isProcessed && !ffprobeBin.isEmpty()) {
            qint64 fileSize = sf.size;
            qint64 lastModified = sf.lastModified;
            int cached = -1;
            if (m_dbManager) {
                cached = m_dbManager->getCachedCompliance(filepath, fileSize, lastModified);
            }

            if (cached != -1) {
                isCompliant = (cached == 1);
                cacheHits++;
            } else {
                cacheMisses++;
                isCompliant = probeFileCompliance(filepath, ffprobeBin);
                if (m_dbManager) {
                    m_dbManager->setCachedCompliance(filepath, fileSize, lastModified, isCompliant ? 1 : 0);
                }
            }
            if (isCompliant) {
                compSizeDisplay = origSizeDisplay; // Same size
            }
        }

        // Set status text
        if (isProcessed) {
            status = "Completed";
            detail = "Already processed (in DB)";
        } else if (isCompliant) {
            status = "Skipped";
            detail = "Already compliant (HEVC+AAC)";
        } else {
            status = "Pending";
            detail = "Waiting...";
            m_activeTranscodeQueue.append(filepath);
        }

        // Set Row Data
        m_tableQueue->setItem(idx, 0, new QTableWidgetItem(filename));
        m_tableQueue->setItem(idx, 1, new QTableWidgetItem(status));
        m_tableQueue->setItem(idx, 2, new QTableWidgetItem(origSizeDisplay));
        m_tableQueue->setItem(idx, 3, new QTableWidgetItem(compSizeDisplay));
        m_tableQueue->setItem(idx, 4, new QTableWidgetItem(detail));
        m_tableQueue->setItem(idx, 5, new QTableWidgetItem(relPath));

        // Style cells
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
    m_tableQueue->setUpdatesEnabled(true); // OPTIMIZATION: Re-enable QTableWidget updates

    updateSavingsDashboard();
    logMessage(QString("Scan complete. Cache Hits: %1, Cache Misses: %2. %3 files queued for processing.")
        .arg(cacheHits).arg(cacheMisses).arg(m_activeTranscodeQueue.count()));

    m_btnStart->setEnabled(m_activeTranscodeQueue.count() > 0);
}

void MainWindow::startProcessing()
{
    if (m_activeTranscodeQueue.isEmpty() || m_rootDir.isEmpty()) return;

    m_btnBrowse->setEnabled(false);
    m_btnScan->setEnabled(false);
    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);

    logMessage("Starting transcode queue...");

    QVariantMap settings;
    settings["crf"] = m_spinCrf->value();
    settings["preset"] = m_comboPreset->currentText();
    settings["downscale"] = m_chkDownscale->isChecked();
    settings["debob"] = m_chkDebob->isChecked();
    settings["live_preview"] = m_chkPreview->isChecked();

    QString dbPath = QDir(m_rootDir).filePath("processed_files.db");
    m_worker = new TranscodeWorker(m_activeTranscodeQueue, m_rootDir, dbPath, settings, this);
    
    // Connect Worker Thread signals
    connect(m_worker, &TranscodeWorker::logSignal, this, &MainWindow::logMessage);
    connect(m_worker, &TranscodeWorker::progressSignal, this, &MainWindow::updateProgress);
    connect(m_worker, &TranscodeWorker::statusSignal, this, &MainWindow::updateStatus);
    connect(m_worker, &TranscodeWorker::fileDoneSignal, this, &MainWindow::fileDone);
    connect(m_worker, &TranscodeWorker::finishedSignal, this, &MainWindow::processingFinished);
    connect(m_worker, &TranscodeWorker::previewFrameSignal, this, &MainWindow::onFramePreviewRequested);
    
    // Wire cleanup
    connect(m_worker, &TranscodeWorker::finished, m_worker, &TranscodeWorker::deleteLater);

    m_worker->start();
}

void MainWindow::stopProcessing()
{
    if (m_worker && m_worker->isRunning()) {
        logMessage("[ABORT] Stopping processing and cleaning up...");
        m_btnStop->setEnabled(false);
        m_statusCard->setVisible(false);
        m_previewGroup->setVisible(false);
        m_lblPreview->clear();
        m_lblPreview->setText("Preview Off");

        if (m_previewProcess && m_previewProcess->state() != QProcess::NotRunning) {
            m_previewProcess->kill();
        }

        m_worker->stop();
    }
}

void MainWindow::resetSettings()
{
    m_spinCrf->setValue(28);
    m_comboPreset->setCurrentText("medium");
    m_chkDownscale->setChecked(true);
    m_chkDebob->setChecked(true);
    m_chkPreview->setChecked(false);
    logMessage("Configuration panel reset to default configurations.");
}

void MainWindow::logMessage(const QString &message)
{
    qDebug().noquote() << message;
}

void MainWindow::updateProgress(const QString &filepath, int percentage, double fps, double speed, const QString &etaStr, double outSizeMb, double projectedSizeMb)
{
    int idx = findRowByFilepath(filepath);
    if (idx != -1) {
        // Table cell updates
        QString statsStr = (etaStr != "N/A") ? QString(" (%1x | ETA %2)").arg(QString::number(speed, 'f', 1), etaStr) : "";
        m_tableQueue->setItem(idx, 4, new QTableWidgetItem(QString("%1%%2").arg(QString::number(percentage), statsStr)));

        // Monitor card updates
        if (m_transcodeStartTime == 0) {
            m_transcodeStartTime = QDateTime::currentSecsSinceEpoch();
        }
        qint64 elapsedSec = QDateTime::currentSecsSinceEpoch() - m_transcodeStartTime;
        QString elapsedStr = QString("%1:%2")
                                .arg(elapsedSec / 60, 2, 10, QChar('0'))
                                .arg(elapsedSec % 60, 2, 10, QChar('0'));

        m_lblStatusFile->setText(QFileInfo(filepath).fileName());
        m_lblStatusPerf->setText(QString("%1 FPS | %2x").arg(QString::number(fps, 'f', 0), QString::number(speed, 'f', 2)));
        m_lblStatusTime->setText(QString("Elapsed: %1 | Remaining: %2").arg(elapsedStr, etaStr));

        if (percentage >= 3) {
            m_lblStatusSize->setText(
                QString("Current: %1 MB | Projected: ~%2 MB (Original: %3 MB)")
                    .arg(QString::number(outSizeMb, 'f', 1),
                         QString::number(projectedSizeMb, 'f', 1),
                         QString::number(static_cast<double>(QFileInfo(filepath).size()) / (1024.0 * 1024.0), 'f', 1))
            );
        } else {
            m_lblStatusSize->setText(
                QString("Current: %1 MB | Projected: Calculating... (Original: %2 MB)")
                    .arg(QString::number(outSizeMb, 'f', 1),
                         QString::number(static_cast<double>(QFileInfo(filepath).size()) / (1024.0 * 1024.0), 'f', 1))
            );
        }

        m_statusProgressBar->setValue(percentage);
    }
}

void MainWindow::updateStatus(const QString &filepath, const QString &status, const QString &details)
{
    int idx = findRowByFilepath(filepath);
    if (idx != -1) {
        m_tableQueue->setItem(idx, 1, new QTableWidgetItem(status));
        m_tableQueue->setItem(idx, 4, new QTableWidgetItem(details));

        if (status == "Processing") {
            m_transcodeStartTime = QDateTime::currentSecsSinceEpoch();

            m_lblStatusFile->setText(QFileInfo(filepath).fileName());
            m_lblStatusPerf->setText("N/A");
            m_lblStatusTime->setText("N/A");
            m_lblStatusSize->setText("N/A");
            m_statusProgressBar->setValue(0);
            m_statusCard->setVisible(true);
            m_previewGroup->setVisible(m_chkPreview->isChecked());
            m_lblPreview->clear();
            m_lblPreview->setText("Preview Off");

            for (int col = 0; col < 6; ++col) {
                QTableWidgetItem *item = m_tableQueue->item(idx, col);
                if (item) {
                    item->setBackground(Qt::darkBlue);
                    item->setForeground(Qt::white);
                }
            }
        }
    }
}

void MainWindow::fileDone(const QString &filepath, const QString &status, qint64 oldSize, qint64 newSize)
{
    int idx = findRowByFilepath(filepath);
    if (idx != -1) {
        m_previewGroup->setVisible(false);
        m_lblPreview->clear();
        m_lblPreview->setText("Preview Off");
        m_tableQueue->setItem(idx, 1, new QTableWidgetItem(status));
        m_tableQueue->setItem(idx, 4, new QTableWidgetItem("Finished"));
        
        double newSizeMb = static_cast<double>(newSize) / (1024.0 * 1024.0);
        m_tableQueue->setItem(idx, 3, new QTableWidgetItem(QString("%1 MB").arg(QString::number(newSizeMb, 'f', 1))));

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

void MainWindow::processingFinished()
{
    logMessage("\nAll queue tasks finished.");
    m_btnBrowse->setEnabled(true);
    m_btnScan->setEnabled(true);
    updateStartButtonState();
    m_btnStop->setEnabled(false);
    m_statusCard->setVisible(false);
    m_worker = nullptr;
}

void MainWindow::updateStartButtonState()
{
    m_activeTranscodeQueue.clear();
    for (int row = 0; row < m_tableQueue->rowCount(); ++row) {
        QTableWidgetItem *statusItem = m_tableQueue->item(row, 1);
        QTableWidgetItem *pathItem = m_tableQueue->item(row, 5);
        if (statusItem && pathItem) {
            QString status = statusItem->text();
            if (status == "Pending" || status == "Cancelled") {
                QString filepath = QDir(m_rootDir).filePath(pathItem->text());
                m_activeTranscodeQueue.append(filepath);
            }
        }
    }
    m_btnStart->setEnabled(m_activeTranscodeQueue.count() > 0);
}

void MainWindow::updateSavingsDashboard()
{
    if (m_dbManager) {
        double orig = 0.0, comp = 0.0, saved = 0.0, pct = 0.0;
        if (m_dbManager->getSpaceSavings(m_rootDir, orig, comp, saved, pct)) {
            m_lblDashboardOrig->setText(QString("%1 MB").arg(QString::number(orig, 'f', 1)));
            m_lblDashboardComp->setText(QString("%1 MB").arg(QString::number(comp, 'f', 1)));
            m_lblDashboardSaved->setText(QString("%1 MB").arg(QString::number(saved, 'f', 1)));
            m_lblDashboardPct->setText(QString("%1%").arg(QString::number(pct, 'f', 1)));
        }
    }
}

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
    return -1;
}

void MainWindow::onFramePreviewRequested(const QString &filepath, double secs)
{
    // If the preview setting is disabled or m_previewGroup is hidden, do nothing
    if (!m_chkPreview->isChecked()) return;

    // Check if the preview process is already running to avoid piling up
    if (m_previewProcess && m_previewProcess->state() != QProcess::NotRunning) {
        return;
    }

    QString ffmpeg = findDependency("ffmpeg");
    if (ffmpeg.isEmpty()) return;

    if (!m_previewProcess) {
        m_previewProcess = new QProcess(this);
        connect(m_previewProcess, &QProcess::readyReadStandardOutput, this, &MainWindow::onPreviewDataAvailable);
        connect(m_previewProcess, &QProcess::finished, this, &MainWindow::onPreviewProcessFinished);
    }

    // Clear old PNG byte buffer
    m_previewBuffer.clear();

    // Format the time as hh:mm:ss.zzz (ensure seconds have two digits padded to prevent FFmpeg parsing failure)
    int h = static_cast<int>(secs / 3600);
    int m = static_cast<int>((secs - h * 3600) / 60);
    int s = static_cast<int>(secs - h * 3600 - m * 60);
    int ms = static_cast<int>((secs - h * 3600 - m * 60 - s) * 1000);
    QString timeStr = QString("%1:%2:%3.%4")
                      .arg(h, 2, 10, QChar('0'))
                      .arg(m, 2, 10, QChar('0'))
                      .arg(s, 2, 10, QChar('0'))
                      .arg(ms, 3, 10, QChar('0'));

    // Run high-speed input seeking and pipe directly to stdout as PNG (buffered asynchronously)
    m_previewProcess->start(ffmpeg, {
        "-y",
        "-ss", timeStr,
        "-i", filepath,
        "-vf", "scale=240:-1",
        "-frames:v", "1",
        "-f", "image2pipe",
        "-vcodec", "png",
        "-"
    });
}

void MainWindow::onPreviewDataAvailable()
{
    if (m_previewProcess) {
        m_previewBuffer.append(m_previewProcess->readAllStandardOutput());
    }
}

void MainWindow::onPreviewProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        // Read any remaining final buffer bytes
        if (m_previewProcess) {
            m_previewBuffer.append(m_previewProcess->readAllStandardOutput());
        }

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

void MainWindow::togglePreview(bool checked)
{
    if (m_worker && m_worker->isRunning()) {
        m_worker->setLivePreviewEnabled(checked);
        m_previewGroup->setVisible(checked);
        if (!checked) {
            m_lblPreview->clear();
            m_lblPreview->setText("Preview Off");
            if (m_previewProcess && m_previewProcess->state() != QProcess::NotRunning) {
                m_previewProcess->kill();
            }
            m_previewBuffer.clear();
        }
    } else {
        m_previewGroup->setVisible(false);
    }
}

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
        if (m_dbManager->clearScanCache(m_rootDir)) {
            logMessage("Scan metadata cache cleared for this workspace.");
            scanDirectory(); // Refresh the table queue
        } else {
            logMessage("[ERROR] Failed to clear metadata cache.");
        }
    }
}

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
        if (m_dbManager->clearProcessedFiles(m_rootDir)) {
            logMessage("Scoreboard transcode history cleared for this workspace.");
            updateSavingsDashboard(); // Reset the labels to 0
            scanDirectory(); // Re-scan the directory to mark files as pending
        } else {
            logMessage("[ERROR] Failed to clear transcode history.");
        }
    }
}
