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
#include "databasemanager.h"
#include "transcodeworker.h"

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
    void onFramePreviewRequested(const QString &filepath, double secs);
    void onPreviewProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onPreviewDataAvailable();
    void togglePreview(bool checked);

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

    QLabel *m_lblFfmpegStatus;
    QLabel *m_lblFfprobeStatus;

    QLabel *m_lblDashboardOrig;
    QLabel *m_lblDashboardComp;
    QLabel *m_lblDashboardSaved;
    QLabel *m_lblDashboardPct;

    QTableWidget *m_tableQueue;

    // Collapsible Monitor Card widgets
    QGroupBox *m_statusCard;
    QLabel *m_lblStatusFile;
    QLabel *m_lblStatusPerf;
    QLabel *m_lblStatusTime;
    QLabel *m_lblStatusSize;
    QProgressBar *m_statusProgressBar;
    QGroupBox *m_previewGroup;
    QLabel *m_lblPreview;
    QProcess *m_previewProcess;
    QByteArray m_previewBuffer;

    // Logic members
    QString m_rootDir;
    DatabaseManager *m_dbManager;
    TranscodeWorker *m_worker;
    QStringList m_scannedFiles;
    QStringList m_activeTranscodeQueue;
    qint64 m_transcodeStartTime;

    void initUi();
    void checkDependencies();
    void updateSavingsDashboard();
    int findRowByFilepath(const QString &filepath);
    void updateStartButtonState();
};

#endif // MAINWINDOW_H
