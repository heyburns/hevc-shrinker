#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTableWidget>
#include <QTextEdit>
#include <QGroupBox>
#include <QProgressBar>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QVariantMap>
#include <QStringList>
#include <QProcess>
#include <QHBoxLayout>
#include "databasemanager.h"
#include "transcodeworker.h"

struct ScannedFile {
    QString absolutePath;
    QString filename;
    QString relPath;
    qint64 size = 0;
    qint64 lastModified = 0;
};

class TranscodeMonitorCard : public QGroupBox {
    Q_OBJECT
public:
    TranscodeMonitorCard(const QString &filepath, QWidget *parent = nullptr);
    ~TranscodeMonitorCard();

    void updateProgress(int percentage, double fps, double speed, const QString &etaStr, double outSizeMb, double projectedSizeMb);
    void updateStatus(const QString &status, const QString &details);
    void requestFramePreview(double secs);
    void setPreviewVisible(bool visible);

private slots:
    void onPreviewDataAvailable();
    void onPreviewProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    QString m_filepath;
    QLabel *m_lblFile;
    QLabel *m_lblPerf;
    QLabel *m_lblTime;
    QLabel *m_lblSize;
    QProgressBar *m_progressBar;
    QLabel *m_lblPreview;

    QProcess *m_previewProcess;
    QByteArray m_previewBuffer;
    qint64 m_startTime;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void selectDirectory();
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
    void togglePreview(bool checked);
    void onConcurrentChanged(int val);
    void showTableContextMenu(const QPoint &pos);

private:
    // UI elements
    QLineEdit *m_dirInput;
    QPushButton *m_btnBrowse;
    QPushButton *m_btnScan;
    QPushButton *m_btnStart;
    QPushButton *m_btnStop;
    QPushButton *m_btnReset;
    QPushButton *m_btnResetDb;
    QPushButton *m_btnResetScoreboard;

    QSpinBox *m_spinCrf;
    QComboBox *m_comboPreset;
    QCheckBox *m_chkDownscale;
    QCheckBox *m_chkDebob;
    QCheckBox *m_chkPreview;
    QSpinBox *m_spinConcurrent;

    QLabel *m_lblFfmpegStatus;
    QLabel *m_lblFfprobeStatus;

    QLabel *m_lblDashboardOrig;
    QLabel *m_lblDashboardComp;
    QLabel *m_lblDashboardSaved;
    QLabel *m_lblDashboardPct;

    QTableWidget *m_tableQueue;

    // Scrollable area for active transcode progress cards
    QWidget *m_monitorsContainer;
    QHBoxLayout *m_monitorsLayout;

    // Logic members
    QString m_rootDir;
    DatabaseManager *m_dbManager;
    QList<TranscodeWorker*> m_workers;
    QHash<QString, TranscodeMonitorCard*> m_activeCards;
    QList<ScannedFile> m_scannedFiles;
    QStringList m_activeTranscodeQueue;
    QStringList m_pendingQueue;
    bool m_isQueueRunning;

    void initUi();
    void checkDependencies();
    void updateSavingsDashboard();
    int findRowByFilepath(const QString &filepath);
    void updateStartButtonState();
};

#endif // MAINWINDOW_H
