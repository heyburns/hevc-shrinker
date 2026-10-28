#ifndef AVISYNTHDIALOG_H
#define AVISYNTHDIALOG_H

#include "avisynthpipeline.h"
#include <QDialog>
#include <QThread>
#include <QTemporaryDir>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QLabel;
class QDoubleSpinBox;
class QCloseEvent;

class AvisynthCheckWorker : public QThread {
    Q_OBJECT
public:
    enum Mode { Installation, Validate, Compare, Sample };
    AvisynthCheckWorker(Mode mode, const QString &source, const AvisynthProfile &profile,
        const QVariantMap &settings, double position, const QString &samplePath, QObject *parent);
    void stop() { m_cancel = true; }
signals:
    void report(const QString &message);
    void comparison(const QByteArray &original, const QByteArray &filtered);
    void sampleReady(const QString &path);
protected:
    void run() override;
private:
    Mode m_mode;
    QString m_source, m_samplePath;
    AvisynthProfile m_profile;
    QVariantMap m_settings;
    double m_position;
    std::atomic_bool m_cancel{false};
};

class AvisynthDialog : public QDialog {
    Q_OBJECT
public:
    enum Scope { ThisFile, SelectedFiles, EntireQueue };
    AvisynthDialog(const QString &source, const AvisynthProfile &initial, int selectedCount,
        int queueCount, const QVariantMap &settings, QWidget *parent = nullptr, const QString &profileStore = {});
    ~AvisynthDialog() override;
    AvisynthProfile profile() const;
    Scope scope() const;
    bool removeProfile() const { return m_remove; }
    QVariantMap toolSettings() const;
protected:
    void reject() override;
private:
    friend class RegressionTests;
    void beginCheck(AvisynthCheckWorker::Mode mode);
    void setBusy(bool busy);
    void saveProfile();
    void loadProfiles();
    QString m_source, m_profileStore;
    QVariantMap m_settings;
    QTemporaryDir m_samples;
    AvisynthCheckWorker *m_worker = nullptr;
    QComboBox *m_scope, *m_presets;
    QLineEdit *m_name, *m_ffmpeg, *m_ffprobe;
    QPlainTextEdit *m_filters, *m_diagnostics;
    QDoubleSpinBox *m_position;
    QLabel *m_original, *m_filtered;
    QPushButton *m_apply, *m_removeButton, *m_cancel, *m_openSample;
    QList<QWidget *> m_editControls;
    QString m_samplePath;
    bool m_remove = false;
};

#endif
