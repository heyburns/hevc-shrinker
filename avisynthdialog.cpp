#include "avisynthdialog.h"
#include "videoencoder.h"
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <QDir>

namespace {
QString profileStore()
{
    const QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(path);
    return QDir(path).filePath("avisynth-profiles.ini");
}
}

AvisynthCheckWorker::AvisynthCheckWorker(Mode mode, const QString &source, const AvisynthProfile &profile,
    const QVariantMap &settings, double position, const QString &samplePath, QObject *parent)
    : QThread(parent), m_mode(mode), m_source(source), m_samplePath(samplePath), m_profile(profile),
      m_settings(settings), m_position(position)
{}

void AvisynthCheckWorker::run()
{
    const QString ffmpeg = m_settings.value("ffmpeg", findDependency("ffmpeg")).toString();
    const QString ffprobe = m_settings.value("ffprobe", findDependency("ffprobe")).toString();
    emit report("Using FFmpeg: " + ffmpeg + "\nUsing FFprobe: " + ffprobe);
    if (m_mode == Installation) {
        const QString error = AvisynthPipeline::testInstallation(ffmpeg, ffprobe, &m_cancel);
        emit report(m_cancel ? "Cancelled." : error.isEmpty() ? "AviSynth+ and L-SMASH Works are available to both tools." : error);
        return;
    }
    const auto meta = probeMetadata(m_source, ffprobe, &m_cancel);
    if (!meta.valid) { emit report(m_cancel ? "Cancelled." : meta.error); return; }
    emit report("Checking source timing, building/reusing its index, and validating filters. Indexing large files can take a while; Cancel remains available.");
    AvisynthPipeline pipeline;
    QString error;
    if (!pipeline.prepare(m_source, meta, m_profile, ffmpeg, ffprobe, m_settings.value("threads", 2).toInt(), &m_cancel, error,
        m_settings.value("avs_cache_root").toString())) {
        emit report(m_cancel ? "Cancelled." : error); return;
    }
    if (!pipeline.timingDescription().isEmpty()) emit report(pipeline.timingDescription());
    emit report(QString("Validated: %1 × %2, %3 fps, %4 seconds. Audio and subtitles will come from the original.")
        .arg(pipeline.filtered.width).arg(pipeline.filtered.height).arg(pipeline.filtered.fps, 0, 'f', 5)
        .arg(pipeline.filtered.duration, 0, 'f', 2));
    const double position = qBound(0.0, m_position, qMax(0.0, pipeline.filtered.duration - 1.0 / pipeline.filtered.fps));
    if (m_mode == Compare) {
        QByteArray pictures[2];
        for (int i = 0; i < 2; ++i) {
            // Seek the complete graph; do not trim away temporal filter context.
            auto result = runProcess(ffmpeg, {"-v", "error", "-nostdin", "-ss", QString::number(position, 'f', 8),
                "-f", "avisynth", "-i", i == 0 ? pipeline.originalScriptPath() : pipeline.scriptPath(),
                "-frames:v", "1", "-an", "-vf", pipeline.videoFilter() + ",scale=480:360:force_original_aspect_ratio=decrease,format=rgb24",
                "-c:v", "png", "-f", "image2pipe", "-"}, &m_cancel, 0);
            if (!result.ok) { emit report(m_cancel ? "Cancelled." : result.error); return; }
            pictures[i] = result.output;
        }
        emit comparison(pictures[0], pictures[1]);
        emit report(QString("Original and filtered frames at %1 seconds.").arg(position, 0, 'f', 3));
    } else if (m_mode == Sample) {
        error = VideoEncoder::validate(ffmpeg, m_settings, &m_cancel);
        if (!error.isEmpty()) { emit report(m_cancel ? "Cancelled." : error); return; }
        const double duration = qMin(10.0, pipeline.filtered.duration - position);
        const QString sampleFilter = pipeline.videoFilter()
            + (VideoEncoder::isNvenc(m_settings) ? ",format=" + VideoEncoder::pixelFormat(m_settings) : QString());
        QStringList args{"-v", "error", "-nostdin", "-y", "-ss", QString::number(position, 'f', 8),
            "-f", "avisynth", "-i", pipeline.scriptPath(), "-t", QString::number(duration, 'f', 8),
            "-map", "0:v:0", "-an", "-vf", sampleFilter};
        args << VideoEncoder::arguments(m_settings) << pipeline.colorArguments() << "-xerror" << m_samplePath;
        QElapsedTimer timer; timer.start();
        emit report(VideoEncoder::isNvenc(m_settings)
            ? "Encoding a video-only sample with NVIDIA NVENC (fixed P5 / CQ 35.25)..."
            : "Encoding a video-only sample with the current x265 settings...");
        auto result = runProcess(ffmpeg, args, &m_cancel, 0);
        if (!result.ok || !pipeline.scriptError().isEmpty()) {
            QFile::remove(m_samplePath);
            emit report(m_cancel ? "Cancelled." : pipeline.scriptError().isEmpty() ? result.error : pipeline.scriptError()); return;
        }
        emit report(QString("Sample: %1 seconds of video; %2 seconds to encode; %3 MiB. This does not include audio or subtitles.")
            .arg(duration, 0, 'f', 2).arg(timer.elapsed() / 1000.0, 0, 'f', 2)
            .arg(QFileInfo(m_samplePath).size() / 1048576.0, 0, 'f', 2));
        emit sampleReady(m_samplePath);
    }
}

AvisynthDialog::AvisynthDialog(const QString &source, const AvisynthProfile &initial, int selectedCount,
    int queueCount, const QVariantMap &settings, QWidget *parent, const QString &storePath)
    : QDialog(parent), m_source(source), m_profileStore(storePath.isEmpty() ? profileStore() : storePath), m_settings(settings)
{
    setWindowTitle("Advanced AviSynth cleanup");
    resize(920, 780);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel("L-SMASH Works loads each source automatically. Enter only the video filter chain below. "
        "Source orientation and pixel aspect are normalized first. This chain replaces automatic downscaling/deinterlacing.\n"
        "Assignments last only for this batch. Rescanning, clearing the queue, or restarting turns filtering off.");
    intro->setWordWrap(true); layout->addWidget(intro);
    auto *sourceLabel = new QLabel("Preview file: " + QFileInfo(source).fileName());
    sourceLabel->setTextFormat(Qt::PlainText); sourceLabel->setToolTip(source); layout->addWidget(sourceLabel);
    auto *form = new QFormLayout;
    m_scope = new QComboBox;
    m_scope->addItem("This video", ThisFile);
    m_scope->addItem(QString("Selected videos (%1)").arg(selectedCount), SelectedFiles);
    m_scope->addItem(QString("All current queue videos (%1) — replaces individual assignments").arg(queueCount), EntireQueue);
    m_scope->setCurrentIndex(selectedCount > 1 ? 1 : 0);
    form->addRow("Apply / remove for:", m_scope);
    m_presets = new QComboBox; form->addRow("Saved profiles:", m_presets);
    m_name = new QLineEdit(initial.name); m_name->setPlaceholderText("Custom cleanup");
    form->addRow("Profile name:", m_name);
    layout->addLayout(form);
    m_filters = new QPlainTextEdit(initial.filters);
    m_filters->setObjectName("avisynthFilters");
    m_filters->setPlaceholderText("# Example crop (pixels removed from each edge):\n# Crop(0, 120, 0, -120)\n\n# Example light temporal cleanup (built in):\n# TemporalSoften(2, 3, 3, 15, 2)\n\n# Add your installed denoise/deblock filters here.");
    m_filters->setMinimumHeight(130); layout->addWidget(m_filters, 1);
    auto *profileButtons = new QHBoxLayout;
    auto *save = new QPushButton("Save Profile"); auto *import = new QPushButton("Import Filter Chain…");
    auto *exportButton = new QPushButton("Export Filter Chain…");
    profileButtons->addWidget(save); profileButtons->addWidget(import); profileButtons->addWidget(exportButton);
    layout->addLayout(profileButtons);
    loadProfiles();
    connect(m_presets, &QComboBox::activated, this, [this](int index) {
        if (index <= 0) return;
        m_name->setText(m_presets->itemText(index)); m_filters->setPlainText(m_presets->itemData(index).toString());
    });
    connect(save, &QPushButton::clicked, this, &AvisynthDialog::saveProfile);
    connect(import, &QPushButton::clicked, this, [this]() {
        const auto path = QFileDialog::getOpenFileName(this, "Import filter chain", {}, "Filter chains (*.avs *.avsi *.txt);;All files (*)");
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) { m_diagnostics->appendPlainText(file.errorString()); return; }
        m_filters->setPlainText(QString::fromUtf8(file.readAll()).remove(QChar(0xfeff)));
        m_name->setText(QFileInfo(path).completeBaseName());
    });
    connect(exportButton, &QPushButton::clicked, this, [this]() {
        const auto path = QFileDialog::getSaveFileName(this, "Export filter chain", "cleanup.avs", "Filter chains (*.avs)");
        if (path.isEmpty()) return;
        QFile file(path); const auto text = m_filters->toPlainText().toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size()) m_diagnostics->appendPlainText(file.errorString());
    });
    auto *tools = new QGroupBox("Optional FFmpeg tools for this batch (blank uses the normal tools)");
    auto *toolForm = new QFormLayout(tools);
    m_ffmpeg = new QLineEdit(settings.value("avs_ffmpeg").toString());
    m_ffprobe = new QLineEdit(settings.value("avs_ffprobe").toString());
    toolForm->addRow("FFmpeg executable:", m_ffmpeg); toolForm->addRow("FFprobe executable:", m_ffprobe);
    layout->addWidget(tools);
    auto *controls = new QHBoxLayout;
    auto *installation = new QPushButton("Test Installation"); auto *validate = new QPushButton("Validate");
    auto *compare = new QPushButton("Compare Frames"); auto *sample = new QPushButton("Encode 10s Sample");
    m_position = new QDoubleSpinBox; m_position->setRange(0, 864000); m_position->setSuffix(" s"); m_position->setDecimals(3);
    m_cancel = new QPushButton("Cancel Check"); m_cancel->setEnabled(false);
    controls->addWidget(installation); controls->addWidget(validate); controls->addWidget(m_position);
    controls->addWidget(compare); controls->addWidget(sample); controls->addWidget(m_cancel); layout->addLayout(controls);
    connect(installation, &QPushButton::clicked, this, [this]() { beginCheck(AvisynthCheckWorker::Installation); });
    connect(validate, &QPushButton::clicked, this, [this]() { beginCheck(AvisynthCheckWorker::Validate); });
    connect(compare, &QPushButton::clicked, this, [this]() { beginCheck(AvisynthCheckWorker::Compare); });
    connect(sample, &QPushButton::clicked, this, [this]() { beginCheck(AvisynthCheckWorker::Sample); });
    connect(m_cancel, &QPushButton::clicked, this, [this]() { if (m_worker) m_worker->stop(); });
    auto *pictures = new QHBoxLayout;
    m_original = new QLabel("No preview yet"); m_filtered = new QLabel("No preview yet");
    for (auto *label : {m_original, m_filtered}) {
        auto *column = new QVBoxLayout;
        auto *title = new QLabel(label == m_original ? "Original" : "Filtered");
        title->setAlignment(Qt::AlignCenter); column->addWidget(title);
        label->setAlignment(Qt::AlignCenter); label->setMinimumSize(250, 120);
        label->setStyleSheet("background: #181818; color: #ddd; border: 1px solid #555;");
        column->addWidget(label); pictures->addLayout(column);
    }
    layout->addLayout(pictures);
    m_diagnostics = new QPlainTextEdit; m_diagnostics->setReadOnly(true); m_diagnostics->setMaximumHeight(120);
    m_diagnostics->setPlaceholderText("SDR and constant frame rate only. Filters must preserve frame count and frame rate. "
        "All required plugins must already be installed in AviSynth's autoload directory."); layout->addWidget(m_diagnostics);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_openSample = buttons->addButton("Open Sample", QDialogButtonBox::ActionRole); m_openSample->setEnabled(false);
    m_removeButton = buttons->addButton("Remove Profile", QDialogButtonBox::ActionRole);
    m_apply = buttons->addButton("Apply to Batch", QDialogButtonBox::AcceptRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &AvisynthDialog::reject);
    connect(m_apply, &QPushButton::clicked, this, [this]() {
        if (m_filters->toPlainText().trimmed().isEmpty()) { m_diagnostics->appendPlainText("Enter a filter chain first."); return; }
        accept();
    });
    connect(m_removeButton, &QPushButton::clicked, this, [this]() { m_remove = true; accept(); });
    connect(m_openSample, &QPushButton::clicked, this, [this]() { QDesktopServices::openUrl(QUrl::fromLocalFile(m_samplePath)); });
    m_editControls = {m_scope, m_presets, m_name, m_filters, save, import, exportButton, m_ffmpeg, m_ffprobe,
        installation, validate, compare, sample, m_position, m_apply, m_removeButton};
    auto invalidatePreview = [this]() {
        m_original->setText("Preview needs updating"); m_filtered->setText("Preview needs updating");
        m_samplePath.clear(); m_openSample->setEnabled(false);
    };
    connect(m_filters, &QPlainTextEdit::textChanged, this, invalidatePreview);
    connect(m_ffmpeg, &QLineEdit::textChanged, this, invalidatePreview);
    connect(m_ffprobe, &QLineEdit::textChanged, this, invalidatePreview);
}

AvisynthDialog::~AvisynthDialog()
{
    if (m_worker) { m_worker->stop(); m_worker->wait(); }
}

void AvisynthDialog::reject()
{
    if (m_worker) { m_worker->stop(); m_diagnostics->appendPlainText("Stopping check… Close again when it finishes."); return; }
    QDialog::reject();
}

AvisynthProfile AvisynthDialog::profile() const
{
    return {m_name->text().trimmed().isEmpty() ? QString("Custom cleanup") : m_name->text().trimmed(), m_filters->toPlainText()};
}
AvisynthDialog::Scope AvisynthDialog::scope() const { return Scope(m_scope->currentData().toInt()); }
QVariantMap AvisynthDialog::toolSettings() const
{
    return {{"avs_ffmpeg", m_ffmpeg->text().trimmed()}, {"avs_ffprobe", m_ffprobe->text().trimmed()}};
}
void AvisynthDialog::setBusy(bool busy)
{
    for (auto *widget : m_editControls) widget->setEnabled(!busy);
    m_cancel->setEnabled(busy);
    m_openSample->setEnabled(!busy && !m_samplePath.isEmpty() && QFileInfo::exists(m_samplePath));
}
void AvisynthDialog::beginCheck(AvisynthCheckWorker::Mode mode)
{
    if (m_worker) return;
    if (mode == AvisynthCheckWorker::Sample && !m_samples.isValid()) {
        m_diagnostics->appendPlainText("Cannot create a temporary sample directory."); return;
    }
    QVariantMap settings = m_settings;
    settings["ffmpeg"] = m_ffmpeg->text().trimmed().isEmpty() ? findDependency("ffmpeg") : m_ffmpeg->text().trimmed();
    settings["ffprobe"] = m_ffprobe->text().trimmed().isEmpty() ? findDependency("ffprobe") : m_ffprobe->text().trimmed();
    m_diagnostics->clear();
    setBusy(true);
    m_worker = new AvisynthCheckWorker(mode, m_source, profile(), settings, m_position->value(),
        m_samples.filePath("sample.mkv"), this);
    connect(m_worker, &AvisynthCheckWorker::report, m_diagnostics, &QPlainTextEdit::appendPlainText);
    connect(m_worker, &AvisynthCheckWorker::comparison, this, [this](const QByteArray &before, const QByteArray &after) {
        QPixmap original, filtered; original.loadFromData(before, "PNG"); filtered.loadFromData(after, "PNG");
        m_original->setPixmap(original.scaled(400, 260, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        m_filtered->setPixmap(filtered.scaled(400, 260, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    });
    connect(m_worker, &AvisynthCheckWorker::sampleReady, this, [this](const QString &path) {
        m_samplePath = path; m_openSample->setEnabled(true);
    });
    connect(m_worker, &QThread::finished, this, [this]() {
        m_worker->wait(); m_worker->deleteLater(); m_worker = nullptr; setBusy(false);
    });
    m_worker->start();
}
void AvisynthDialog::loadProfiles()
{
    QSettings store(m_profileStore, QSettings::IniFormat);
    const auto profiles = QJsonDocument::fromJson(store.value("profiles").toByteArray()).array();
    m_presets->clear(); m_presets->addItem("Choose a saved profile…");
    for (const auto &value : profiles) {
        const auto profile = value.toObject(); m_presets->addItem(profile.value("name").toString(), profile.value("filters").toString());
    }
    // Deliberately never persist or restore the selected profile or assignments.
}
void AvisynthDialog::saveProfile()
{
    const auto current = profile();
    if (current.filters.trimmed().isEmpty()) { m_diagnostics->appendPlainText("Enter a filter chain first."); return; }
    QSettings store(m_profileStore, QSettings::IniFormat);
    auto profiles = QJsonDocument::fromJson(store.value("profiles").toByteArray()).array();
    for (int i = profiles.size() - 1; i >= 0; --i)
        if (profiles[i].toObject().value("name").toString() == current.name) profiles.removeAt(i);
    profiles.append(QJsonObject{{"name", current.name}, {"filters", current.filters}});
    store.setValue("profiles", QJsonDocument(profiles).toJson(QJsonDocument::Compact)); store.sync();
    if (store.status() != QSettings::NoError) m_diagnostics->appendPlainText("Could not save this profile.");
    else { loadProfiles(); m_diagnostics->appendPlainText("Profile saved. Saving does not enable filtering for any batch."); }
}
