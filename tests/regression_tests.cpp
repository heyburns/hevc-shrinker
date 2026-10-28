#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QInputDialog>
#include <QMenu>
#include <QScopeGuard>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>
#include <vector>
#include "databasemanager.h"
#include "filetransaction.h"
#include "mainwindow.h"
#include "mediautils.h"
#include "scanworker.h"
#include "transcodeworker.h"
#include "avisynthpipeline.h"
#include "avisynthdialog.h"
#include "videoencoder.h"

namespace {
bool put(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
QByteArray read(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
QString fixtureProgram()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
        "process_fixture.exe"
#else
        "process_fixture"
#endif
    );
}
QJsonObject video(int index, const QString &codec, int width, int height, bool defaultStream = false, bool cover = false)
{
    return {{"index", index}, {"codec_type", "video"}, {"codec_name", codec}, {"width", width}, {"height", height},
        {"avg_frame_rate", "24000/1001"}, {"sample_aspect_ratio", "1:1"}, {"field_order", "progressive"},
        {"disposition", QJsonObject{{"default", defaultStream ? 1 : 0}, {"attached_pic", cover ? 1 : 0}}}};
}
QJsonObject metadata(const QJsonArray &streams)
{
    return {{"format", QJsonObject{{"duration", "4.0"}}}, {"streams", streams}};
}
}

class RegressionTests : public QObject {
    Q_OBJECT
private:
    QString m_ffmpeg, m_ffprobe, m_lastWorkerDetails;
    bool ff(const QStringList &args) {
        auto result = runProcess(m_ffmpeg, QStringList{"-hide_banner", "-loglevel", "error", "-nostdin", "-y"} + args, nullptr, 60000);
        if (!result.ok) qWarning().noquote() << result.error;
        return result.ok;
    }
    bool source(const QString &path, int seconds = 4) {
        return ff({"-f", "lavfi", "-i", "testsrc2=size=160x96:rate=24", "-t", QString::number(seconds),
            "-c:v", "libx264", "-threads", "1", "-crf", "0", path});
    }
    QVariantMap settings() const { return {{"crf",28},{"preset","medium"},{"downscale",true},{"debob",true},{"threads",2},
        {"ffmpeg",m_ffmpeg},{"ffprobe",m_ffprobe}}; }
    QList<double> packetTimes(const QString &path, const QString &stream) const {
        QList<double> times;
        const auto result=runProcess(m_ffprobe,{"-v","error","-select_streams",stream,
            "-show_entries","packet=pts_time","-of","json",path});
        if (!result.ok) return times;
        for (const auto &packet : QJsonDocument::fromJson(result.output).object().value("packets").toArray()) {
            bool ok=false; const double time=packet.toObject().value("pts_time").toString().toDouble(&ok);
            if (!ok || !std::isfinite(time)) return {};
            times.append(time);
        }
        std::sort(times.begin(),times.end()); return times;
    }
    QString runWorker(const QString &path, const QString &root, const QString &dbPath, QVariantMap config = {}) {
        if (config.isEmpty()) config = settings();
        TranscodeWorker worker({path}, root, dbPath, config);
        QSignalSpy result(&worker, &TranscodeWorker::fileDoneSignal);
        QSignalSpy statuses(&worker, &TranscodeWorker::statusSignal);
        worker.start();
        if (!worker.wait(60000)) { worker.stop(); worker.wait(); return "Timeout"; }
        QCoreApplication::processEvents();
        m_lastWorkerDetails = statuses.isEmpty() ? QString() : statuses.last().at(2).toString();
        if (!result.isEmpty() && result.at(0).at(1).toString() == "Failed")
            qWarning().noquote() << m_lastWorkerDetails;
        return result.size() == 1 ? result.at(0).at(1).toString() : "Missing result";
    }
    void setupWindow(MainWindow &window, const QString &root, const QString &dbPath) {
        window.m_rootDir = root;
        window.m_dbManager = new DatabaseManager(dbPath);
    }
    struct ProgressCapture {
        ProcessResult result;
        QList<QList<QVariant>> reports;
    };
    ProgressCapture progressFixture(const QJsonArray &chunks, double duration, double outputFps) {
        QTemporaryDir dir;
        const QString script=dir.filePath("progress.json"), output=dir.filePath("growing.mkv");
        if (!put(script,QJsonDocument(chunks).toJson())) {
            ProgressCapture failed; failed.result.error="Cannot write progress fixture"; return failed;
        }
        TranscodeWorker worker({},dir.path(),dir.filePath("history.db"),settings());
        QSignalSpy progress(&worker,&TranscodeWorker::progressSignal);
        const auto result=worker.encode(fixtureProgram(),{"--progress-script",script,output},
            duration,"synthetic.mp4",output,outputFps);
        return {result,progress};
    }
private slots:
    void initTestCase() {
        m_ffmpeg = findDependency("ffmpeg");
        m_ffprobe = findDependency("ffprobe");
        QVERIFY(QFileInfo::exists(fixtureProgram()));
    }
    void processExitTimeoutAndCancellation() {
        QVERIFY(runProcess(fixtureProgram(), {"--sleep","0"}).ok);
        QVERIFY(!runProcess(fixtureProgram(), {"--sleep","5000"}, nullptr, 100).ok);
        std::atomic_bool cancelled{false};
        std::thread stopper([&](){ QThread::msleep(100); cancelled = true; });
        auto result = runProcess(fixtureProgram(), {"--sleep","5000"}, &cancelled, 0);
        stopper.join();
        QVERIFY(result.cancelled);
        QVERIFY(!result.ok);
        cancelled = false;
        QVERIFY(runProcess(fixtureProgram(), {"--sleep","150"}, &cancelled, 0).ok);
        QVERIFY(!runProcess(fixtureProgram(), {"--invalid"}).ok);
    }
    void progressMissingTimesUsesOutputRate_data() {
        QTest::addColumn<double>("outputFps"); QTest::addColumn<int>("expectedPercent");
        QTest::newRow("ordinary") << 25.0 << 20;
        QTest::newRow("doubled-frame-rate") << 50.0 << 10;
        QTest::newRow("halved-frame-rate") << 12.5 << 40;
    }
    void progressMissingTimesUsesOutputRate() {
        QFETCH(double,outputFps); QFETCH(int,expectedPercent);
        // Split a key between writes and finish every report with missing
        // mux timestamps, as FFmpeg does when a copied cover has no DTS.
        const QJsonArray chunks{
            QJsonObject{{"text","frame=25\nfps=100.00\nout_t"},{"size",262144}},
            QJsonObject{{"text","ime_us=N/A\nout_time_ms=N/A\nout_time=N/A\nspeed=N/A\nprogress=continue\n"},{"size",524288}},
            QJsonObject{{"text","frame=50\nfps=100.00\nout_time_us=N/A\nout_time_ms=N/A\nout_time=N/A\nspeed=N/A\nprogress=continue\n"},{"size",1048576}},
            QJsonObject{{"text","frame=500\nfps=100.00\nout_time_us=N/A\nspeed=N/A\nprogress=end\n"},{"size",2097152},{"sleep_ms",0}}};
        const auto capture=progressFixture(chunks,10.0,outputFps);
        QVERIFY2(capture.result.ok,qPrintable(capture.result.error)); QCOMPARE(capture.reports.size(),3);
        const auto mid=capture.reports.at(1);
        QCOMPARE(mid.at(1).toInt(),expectedPercent); QCOMPARE(mid.at(2).toDouble(),100.0);
        QVERIFY(mid.at(3).toDouble()>0); QVERIFY(std::isfinite(mid.at(3).toDouble()));
        QVERIFY(mid.at(4).toString().startsWith('~'));
        const double size=mid.at(5).toDouble(), projected=mid.at(6).toDouble();
        QVERIFY(size>0); QVERIFY(qAbs(projected-size/(expectedPercent/100.0))<0.000001);
        for (const auto &report : capture.reports) QVERIFY(report.at(1).toInt()<100);
        QCOMPARE(capture.reports.last().at(1).toInt(),99); // Validation/commit still owns completion.
    }
    void progressTimestampFormatsAndInvalidValues_data() {
        QTest::addColumn<QString>("fields"); QTest::addColumn<int>("expectedPercent");
        QTest::newRow("microseconds") << QString("out_time_us=4000000\nout_time_ms=N/A\nout_time=N/A\n") << 40;
        QTest::newRow("legacy-ms-is-microseconds") << QString("out_time_us=N/A\nout_time_ms=6000000\nout_time=N/A\n") << 60;
        QTest::newRow("clock-format") << QString("out_time_us=N/A\nout_time_ms=N/A\nout_time=00:00:03.500000\n") << 35;
    }
    void progressTimestampFormatsAndInvalidValues() {
        QFETCH(QString,fields); QFETCH(int,expectedPercent);
        const QJsonArray chunks{
            QJsonObject{{"text","frame=50\nfps=50.00\n"+fields+"speed=2.0x\nprogress=continue\n"},{"size",1048576}},
            QJsonObject{{"text","fps=N/A\nout_time_us=NaN\nout_time_ms=Inf\nout_time=N/A\nspeed=NaNx\nprogress=end\n"},{"size",2097152},{"sleep_ms",0}}};
        const auto capture=progressFixture(chunks,10.0,25.0);
        QVERIFY2(capture.result.ok,qPrintable(capture.result.error)); QCOMPARE(capture.reports.size(),2);
        for (const auto &report : capture.reports) {
            QCOMPARE(report.at(1).toInt(),expectedPercent);
            QCOMPARE(report.at(2).toDouble(),50.0); QCOMPARE(report.at(3).toDouble(),2.0);
            QVERIFY(report.at(4).toString()!="N/A"); QVERIFY(report.at(6).toDouble()>0);
        }
    }
    void progressUnknownTimingIsIndeterminate() {
        const QJsonArray chunks{QJsonObject{{"text","frame=100\nfps=25.00\nout_time_us=-9223372036854775808\nout_time_ms=N/A\nout_time=N/A\nspeed=N/A\nprogress=end\n"},{"size",1048576},{"sleep_ms",0}}};
        for (double rate : {0.0,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
            const auto capture=progressFixture(chunks,10.0,rate);
            QVERIFY2(capture.result.ok,qPrintable(capture.result.error)); QCOMPARE(capture.reports.size(),1);
            const auto report=capture.reports.first();
            QCOMPARE(report.at(1).toInt(),-1); QCOMPARE(report.at(3).toDouble(),-1.0);
            QCOMPARE(report.at(4).toString(),QString("N/A")); QCOMPARE(report.at(6).toDouble(),0.0);
        }
        for (double duration : {0.0,std::numeric_limits<double>::infinity()}) {
            const auto capture=progressFixture(chunks,duration,25.0);
            QVERIFY(capture.result.ok); QCOMPARE(capture.reports.size(),1);
            QCOMPARE(capture.reports.first().at(1).toInt(),-1);
            QCOMPARE(capture.reports.first().at(4).toString(),QString("N/A"));
            QCOMPARE(capture.reports.first().at(6).toDouble(),0.0);
        }
        QTemporaryDir dir;
        const QString input=dir.filePath("clip.mp4");
        TranscodeMonitorCard card(input);
        card.updateProgress(-1,247,-1,"N/A",2,0);
        auto *bar=card.findChild<QProgressBar*>(); QVERIFY(bar);
        QCOMPARE(bar->minimum(),0); QCOMPARE(bar->maximum(),0);
        const auto labels=card.findChildren<QLabel*>();
        QVERIFY(std::any_of(labels.cbegin(),labels.cend(),[](const auto *label) { return label->text().contains("Speed N/A"); }));
        card.updateProgress(25,247,9.88,"~00:01:00",2,8);
        QCOMPARE(bar->minimum(),0); QCOMPARE(bar->maximum(),100); QCOMPARE(bar->value(),25);
        card.updateProgress(-1,247,-1,"N/A",2,0); card.updateStatus("Processing","Analyzing...");
        QCOMPARE(bar->minimum(),0); QCOMPARE(bar->maximum(),100); QCOMPARE(bar->value(),0);
        MainWindow window(nullptr,dir.filePath("history.db")); window.m_rootDir=dir.path();
        window.m_tableQueue->setRowCount(1);
        window.m_tableQueue->setItem(0,5,new QTableWidgetItem("clip.mp4"));
        window.updateProgress(input,-1,247,-1,"N/A",2,0);
        QCOMPARE(window.m_tableQueue->item(0,4)->text(),QString("Encoding…"));
    }
    void embeddedCoverProgressAndPreservation() {
        if(m_ffmpeg.isEmpty()||m_ffprobe.isEmpty()) QSKIP("FFmpeg/FFprobe required");
        QTemporaryDir dir;
        const QString base=dir.filePath("base.mp4"), input=dir.filePath("with-cover.mp4");
        const QString cover=dir.filePath("cover.png"), output=dir.filePath("with-cover.mkv"), dbPath=dir.filePath("history.db");
        QImage image(32,32,QImage::Format_RGB32); image.fill(Qt::red); QVERIFY(image.save(cover,"PNG"));
        QVERIFY(ff({"-f","lavfi","-i","testsrc=size=320x240:rate=25","-t","4","-pix_fmt","yuv420p",
            "-c:v","libx264","-threads","1","-crf","0",base}));
        QVERIFY(ff({"-i",base,"-f","lavfi","-i","sine=frequency=1000","-i",cover,"-t","4",
            "-map","0:v:0","-map","1:a:0","-map","2:v:0","-c:v:0","copy","-c:a","aac",
            "-c:v:1","copy","-disposition:v:1","attached_pic",input}));
        const auto sourceMeta=probeMetadata(input,m_ffprobe); QVERIFY(sourceMeta.valid);
        QVERIFY(std::any_of(sourceMeta.streams.cbegin(),sourceMeta.streams.cend(),[](const auto &stream) {
            return stream.codec=="png" && stream.attachedPicture;
        }));
        const auto original=read(input);
        const auto raw=runProcess(m_ffmpeg,{"-v","error","-nostdin","-y","-i",input,"-map","0","-c","copy",
            "-progress","pipe:1","-nostats",dir.filePath("remux.mkv")});
        QVERIFY2(raw.ok,qPrintable(raw.error)); QVERIFY(raw.output.contains("progress=end"));
        // Some FFmpeg versions report timestamps for this case. The scripted
        // tests always exercise N/A; this test verifies real cover preservation.
        if (raw.output.contains("out_time_us=N/A")) qInfo("Reproduced FFmpeg's missing mux timestamp with an embedded cover.");
        TranscodeWorker worker({input},dir.path(),dbPath,settings());
        QSignalSpy done(&worker,&TranscodeWorker::fileDoneSignal), progress(&worker,&TranscodeWorker::progressSignal);
        QSignalSpy statuses(&worker,&TranscodeWorker::statusSignal);
        worker.start(); QVERIFY(worker.wait(60000)); QCoreApplication::processEvents();
        QCOMPARE(done.size(),1);
        const QString details=statuses.isEmpty()?QString():statuses.last().at(2).toString();
        QVERIFY2(done.first().at(1).toString()=="Completed",qPrintable(details));
        QVERIFY(!progress.isEmpty());
        QVERIFY(std::any_of(progress.cbegin(),progress.cend(),[](const auto &report) {
            return report.at(1).toInt()>0 && report.at(1).toInt()<100 && report.at(3).toDouble()>0
                && report.at(4).toString()!="N/A" && report.at(6).toDouble()>0;
        }));
        const auto encoded=probeMetadata(output,m_ffprobe); QVERIFY(encoded.valid);
        QCOMPARE(encoded.vcodec,QString("hevc")); QCOMPARE(encoded.width,320); QCOMPARE(encoded.height,240);
        QCOMPARE(encoded.acodec,QString("aac")); QCOMPARE(encoded.streams.size(),3);
        const auto coverStream=std::find_if(encoded.streams.cbegin(),encoded.streams.cend(),[](const auto &stream) { return stream.codec=="png"; });
        QVERIFY(coverStream!=encoded.streams.cend());
        const auto copiedCover=runProcess(m_ffmpeg,{"-v","error","-nostdin","-i",output,
            "-map",QString("0:%1").arg(coverStream->index),"-c","copy","-frames:v","1","-f","image2pipe","-"});
        QVERIFY2(copiedCover.ok,qPrintable(copiedCover.error)); QCOMPARE(copiedCover.output,read(cover));
        QCOMPARE(read(dir.filePath(".Trash/with-cover.mp4")),original);
        DatabaseManager db(dbPath); QVERIFY(db.getProcessedFileInfo(output).found);
    }
    void fileCollisionAndLock() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString input = dir.filePath("clip.mp4");
        QVERIFY(put(input, "original"));
        QVERIFY(put(dir.filePath("clip.mkv"), "unrelated"));
        QString error;
        { FileTransaction transaction(input, dir.path()); QVERIFY(!transaction.prepare(error)); }
        QCOMPARE(read(input), QByteArray("original"));
        QCOMPARE(read(dir.filePath("clip.mkv")), QByteArray("unrelated"));
        QVERIFY(QFile::remove(dir.filePath("clip.mkv")));
        FileTransaction first(input, dir.path());
        QVERIFY(first.prepare(error));
        QVERIFY(put(dir.filePath("clip.avi"), "other input"));
        FileTransaction second(dir.filePath("clip.avi"), dir.path());
        QVERIFY(!second.prepare(error));
        QVERIFY(isJobArtifact(QFileInfo(first.temporaryPath()).fileName()));
    }
    void originalPreservedWhenTrashUnavailable() {
        QTemporaryDir dir;
        const QString input = dir.filePath("clip.mp4");
        QVERIFY(put(input, "original"));
        QVERIFY(put(dir.filePath(".Trash"), "blocked"));
        FileTransaction transaction(input, dir.path());
        QString error;
        QVERIFY(transaction.prepare(error));
        QVERIFY(put(transaction.temporaryPath(), "encoded"));
        QVERIFY(!transaction.commit(error));
        QCOMPARE(read(input), QByteArray("original"));
        QVERIFY(!QFileInfo::exists(transaction.finalPath()));
    }
    void commitPreservesBothOriginalsAndCase() {
        QTemporaryDir dir;
        const QString input = dir.filePath("sub/vidéø.MKV");
        QVERIFY(put(input, "original"));
        QVERIFY(put(dir.filePath(".Trash/sub/vidéø.MKV"), "old backup"));
        FileTransaction transaction(input, dir.path());
        QString error;
        QVERIFY(transaction.prepare(error));
        QCOMPARE(transaction.finalPath(), input);
        QVERIFY(put(transaction.temporaryPath(), "encoded"));
        QVERIFY2(transaction.commit(error), qPrintable(error));
        QCOMPARE(read(input), QByteArray("encoded"));
        QCOMPARE(read(transaction.backupPath()), QByteArray("original"));
        QCOMPARE(read(dir.filePath(".Trash/sub/vidéø.MKV")), QByteArray("old backup"));
    }
    void lateCollisionAndEmptyOutputRetainSource() {
        QTemporaryDir dir;
        const QString input = dir.filePath("clip.mp4");
        QVERIFY(put(input, "original"));
        FileTransaction transaction(input, dir.path());
        QString error;
        QVERIFY(transaction.prepare(error));
        QVERIFY(!transaction.commit(error));
        QVERIFY(put(transaction.temporaryPath(), "encoded"));
        QVERIFY(put(transaction.finalPath(), "appeared later"));
        QVERIFY(!transaction.commit(error));
        QCOMPARE(read(input), QByteArray("original"));
        QCOMPARE(read(transaction.finalPath()), QByteArray("appeared later"));
    }
    void databaseOwnershipIdentityAndScope() {
        QTemporaryDir dir;
        const QString a = dir.filePath("Show_A/clip.mkv"), b = dir.filePath("ShowBA/clip.mkv");
        QVERIFY(put(a,"aaaa")); QVERIFY(put(b,"bbbb"));
        DatabaseManager db(dir.filePath("history.db"));
        QVERIFY2(db.isReady(),qPrintable(db.lastError()));
        QVERIFY(db.recordProcessedFile(a,8,4,computeFastHash(a),"settings"));
        QVERIFY(db.recordProcessedFile(b,8,4,computeFastHash(b),"settings"));
        QVERIFY(db.getProcessedFileInfo(a).current);
        { DatabaseManager other(dir.filePath("other.db")); QVERIFY(other.isReady()); QVERIFY(!other.getProcessedFileInfo(a).found); }
        QVERIFY(db.getProcessedFileInfo(a).found);
        const QDateTime oldDate = QFileInfo(a).lastModified();
        QVERIFY(put(a,"zzzz"));
        QFile restoredTime(a); QVERIFY(restoredTime.open(QIODevice::ReadWrite));
        QVERIFY(restoredTime.setFileTime(oldDate, QFileDevice::FileModificationTime)); restoredTime.close();
        QVERIFY(!db.getProcessedFileInfo(a).current);
        QVERIFY(db.clearProcessedFiles(dir.filePath("Show_A")));
        QVERIFY(!db.getProcessedFileInfo(a).found);
        QVERIFY(db.getProcessedFileInfo(b).found);
        const QString pct = dir.filePath("100%/clip.mkv"), otherPct = dir.filePath("100abc/clip.mkv");
        QVERIFY(put(pct,"aaaa")); QVERIFY(put(otherPct,"bbbb"));
        QVERIFY(db.cacheMetadata(pct,4,1,"json")); QVERIFY(db.cacheMetadata(otherPct,4,1,"other"));
        QVERIFY(db.clearScanCache(dir.filePath("100%")));
        QVERIFY(db.cachedMetadata(pct,4,1).isEmpty());
        QCOMPARE(db.cachedMetadata(otherPct,4,1),QByteArray("other"));
#ifndef Q_OS_WIN
        QVERIFY(db.recordProcessedFile(dir.filePath("Case/clip.mkv"),8,4,"legacy"));
        QVERIFY(db.recordProcessedFile(dir.filePath("case/clip.mkv"),8,4,"legacy"));
        QVERIFY(db.clearProcessedFiles(dir.filePath("Case")));
        QVERIFY(db.getProcessedFileInfo(dir.filePath("case/clip.mkv")).found);
#endif
    }
    void databaseWriteLock() {
        QTemporaryDir dir;
        DatabaseManager db(dir.filePath("history.db"));
        QVERIFY(db.isReady());
        QSqlQuery first(db.db());
        QVERIFY(first.exec("BEGIN IMMEDIATE"));
        {
            auto second = QSqlDatabase::addDatabase("QSQLITE", "lock_test");
            second.setDatabaseName(db.path()); second.setConnectOptions("QSQLITE_BUSY_TIMEOUT=50");
            QVERIFY(second.open());
            QSqlQuery competing(second);
            QVERIFY(!competing.exec("BEGIN IMMEDIATE"));
            QVERIFY(first.exec("ROLLBACK"));
            QVERIFY(competing.exec("BEGIN IMMEDIATE"));
            QVERIFY(competing.exec("ROLLBACK"));
        }
        QSqlDatabase::removeDatabase("lock_test");
    }
    void failedMigrationPreservesDatabase() {
        QTemporaryDir dir;
        const QString legacy = dir.filePath("processed_files.db");
        QVERIFY(put(legacy,"not a database"));
        DatabaseManager db(dir.filePath("history.db"));
        QVERIFY(!db.migrateLocalDatabase(legacy,dir.path()));
        QCOMPARE(read(legacy),QByteArray("not a database"));
        QVERIFY(!QFileInfo::exists(legacy+".migrated"));
    }
    void metadataSelectionComplianceAndGeometry() {
        QJsonArray streams{video(0,"mjpeg",40,40,false,true), video(1,"hevc",3840,2160,true),video(2,"h264",160,120),
            QJsonObject{{"index",3},{"codec_type","audio"},{"codec_name","aac"}},
            QJsonObject{{"index",4},{"codec_type","audio"},{"codec_name","ac3"}}};
        auto meta = parseMetadata(metadata(streams));
        QVERIFY(meta.valid); QCOMPARE(meta.videoIndex,1); QCOMPARE(meta.width,3840);
        QVERIFY(isCompliant("clip.mkv",meta,{{"downscale",false}}));
        streams.removeLast(); meta = parseMetadata(metadata(streams));
        QVERIFY(isCompliant("clip.mkv",meta,{{"downscale",false}}));
        QVERIFY(isCompliant("clip.mkv",meta,{{"downscale",true}}));
        streams.removeLast(); meta = parseMetadata(metadata(streams));
        QVERIFY(!isCompliant("clip.mkv",meta,{{"downscale",false}})); // Original rule requires HEVC and AAC.
        auto portrait = videoGeometry(meta,true); QCOMPARE(portrait.width,1920); QCOMPARE(portrait.height,1080);
        meta.rotation=90; portrait=videoGeometry(meta,true);
        QCOMPARE(portrait.width,1080); QCOMPARE(portrait.height,1920);
        QVERIFY(!parseMetadata({}).valid);
    }
    void existingHistoryAlwaysStaysProcessed() {
        QTemporaryDir dir;
        const QString dbPath=dir.filePath("history.db");
        const QString legacy=dir.filePath("legacy.mkv"), filtered=dir.filePath("filtered.mkv"), retained=dir.filePath("retained.mp4");
        QVERIFY(put(legacy,"legacy output")); QVERIFY(put(filtered,"filtered output")); QVERIFY(put(retained,"retained input"));
        DatabaseManager db(dbPath); QVERIFY(db.isReady());
        QSqlQuery insert(db.db());
        insert.prepare("INSERT INTO processed_files (filepath,original_size,compressed_size,hash) VALUES (?,?,?,?)");
        insert.addBindValue(normalizedPath(legacy)); insert.addBindValue(10000000);
        insert.addBindValue(1000000); insert.addBindValue("original-input-sha1"); QVERIFY(insert.exec());
        auto avs=settings(); avs["avs_enabled"]=true; avs["avs_filters"]="Crop(0,8,0,-8)";
        QVERIFY(db.recordProcessedFile(filtered,10000000,QFileInfo(filtered).size(),computeFastHash(filtered),settingsSignature(avs)));
        QVERIFY(db.recordProcessedFile(retained,10000000,QFileInfo(retained).size(),computeFastHash(retained),"old-settings","Skipped"));
        for (int crf : {18,40}) for (bool nvenc : {false,true}) {
            auto config=settings(); config["crf"]=crf; config["preset"]="fast"; config["ffprobe"]=fixtureProgram();
            config["nvenc"]=nvenc;
            ScanWorker scan(dir.path(),dbPath,config); scan.start(); QVERIFY(scan.wait(15000));
            QVERIFY2(scan.error.isEmpty(),qPrintable(scan.error)); QCOMPARE(scan.files.size(),3);
            // These deliberately invalid media files would fail probing. Every
            // history row must be terminal without probing or matching settings.
            for (const auto &file : scan.files) QCOMPARE(file.status,QString("Completed"));
        }
        QCOMPARE(db.getProcessedFileInfo(legacy).hash,QString("original-input-sha1"));
        MainWindow window(nullptr,dbPath); setupWindow(window,dir.path(),dbPath);
        window.scanDirectory(); QTRY_VERIFY_WITH_TIMEOUT(window.m_scanWorker==nullptr,15000);
        QCOMPARE(window.m_tableQueue->rowCount(),3); QVERIFY(window.m_activeTranscodeQueue.isEmpty());
        QVERIFY(!window.m_btnStart->isEnabled());
        for (int row=0; row<3; ++row) {
            QCOMPARE(window.m_tableQueue->item(row,1)->text(),QString("Completed"));
            QCOMPARE(window.m_tableQueue->item(row,2)->text(),QString("9.5 MB"));
            if (window.m_tableQueue->item(row,0)->text()=="legacy.mkv")
                QCOMPARE(window.m_tableQueue->item(row,3)->text(),QString("1.0 MB"));
        }
    }
    void avisynthWholeQueuePreservesProcessedRows() {
        QTemporaryDir dir; const QString dbPath=dir.filePath("history.db");
        MainWindow window(nullptr,dbPath); setupWindow(window,dir.path(),dbPath);
        const QStringList statuses{"Completed","Skipped","Pending","Cancelled","Failed"};
        QStringList paths; window.m_tableQueue->setRowCount(statuses.size());
        for (int row=0; row<statuses.size(); ++row) {
            const QString name=QString("video%1.mkv").arg(row), path=dir.filePath(name);
            QVERIFY(put(path,"original")); paths << path;
            const QStringList cells{name,statuses[row],"1","0","existing status",name,"Off"};
            for (int col=0; col<cells.size(); ++col) window.m_tableQueue->setItem(row,col,new QTableWidgetItem(cells[col]));
        }
        window.updateStartButtonState(); QCOMPARE(window.m_activeTranscodeQueue.size(),2);
        window.m_tableQueue->selectRow(0);
        QTimer::singleShot(0,&window,[] {
            auto *dialog=qobject_cast<AvisynthDialog*>(QApplication::activeModalWidget()); Q_ASSERT(dialog);
            if (!dialog) return;
            const int all=dialog->m_scope->findData(AvisynthDialog::EntireQueue);
            dialog->m_scope->setCurrentIndex(all);
            dialog->m_filters->setPlainText("Crop(0,8,0,-8)"); dialog->m_name->setText("Batch crop"); dialog->accept();
        });
        window.configureAvisynth();
        QCOMPARE(window.m_filterAssignments.size(),2); QCOMPARE(window.m_activeTranscodeQueue.size(),2);
        QVERIFY(window.m_filterAssignments.contains(paths[2])); QVERIFY(window.m_filterAssignments.contains(paths[3]));
        for (int row : {0,1,4}) {
            QCOMPARE(window.m_tableQueue->item(row,1)->text(),statuses[row]);
            QCOMPARE(window.m_tableQueue->item(row,6)->text(),QString("Off"));
        }
    }
    void stopBeforeWorkerStarts() {
        QTemporaryDir dir;
        const QString input=dir.filePath("clip.mp4"); QVERIFY(put(input,"original"));
        TranscodeWorker worker({input},dir.path(),dir.filePath("history.db"),settings());
        QSignalSpy result(&worker,&TranscodeWorker::fileDoneSignal);
        worker.stop(); worker.start(); QVERIFY(worker.wait(5000)); QCoreApplication::processEvents();
        QCOMPARE(result.size(),1); QCOMPARE(result.at(0).at(1).toString(),QString("Cancelled"));
        QCOMPARE(read(input),QByteArray("original"));
    }
    void avisynthTimelineAndSignature() {
        QVERIFY(AvisynthPipeline::validateTimeline({0, 0.04, 0.08}, 25).isEmpty());
        QVERIFY(!AvisynthPipeline::validateTimeline({0, 0.04, 0.1}, 25).isEmpty());
        QVERIFY(AvisynthPipeline::validateTimeline({2, 2.04, 2.08}, 25).isEmpty());
        QVERIFY(AvisynthPipeline::validateTimeline({0.021, 0.061, 0.101}, 25).isEmpty());
        QVERIFY(AvisynthPipeline::validateTimeline({-0.04, 0, 0.04}, 25).isEmpty());
        QVERIFY(!AvisynthPipeline::validateTimeline({2, 2.04, 2.1}, 25).isEmpty());
        QVERIFY(!AvisynthPipeline::validateTimeline({std::numeric_limits<double>::quiet_NaN(), 0.04}, 25).isEmpty());
        QCOMPARE(AvisynthPipeline::quote("a\"b\nc"), QString("\"a\" + Chr(34) + \"b\" + Chr(10) + \"c\""));
        auto plain = settings(), filtered = plain;
        filtered["avs_enabled"] = true; filtered["avs_filters"] = "Crop(0, 8, 0, -8)";
        QVERIFY(settingsSignature(plain) != settingsSignature(filtered));
        const QString first = settingsSignature(filtered);
        filtered["avs_filters"] = "Crop(0, 4, 0, -4)";
        QVERIFY(first != settingsSignature(filtered));
        filtered["avs_enabled"] = false;
        QCOMPARE(settingsSignature(plain), settingsSignature(filtered));
    }
    void nvencFixedConfigurationAndSignature() {
        auto cpu=settings(), gpu=cpu;
        gpu["nvenc"]=true;
        QVERIFY(!VideoEncoder::isNvenc(cpu)); QVERIFY(VideoEncoder::isNvenc(gpu));
        const auto cpuArgs=VideoEncoder::arguments(cpu), gpuArgs=VideoEncoder::arguments(gpu);
        QVERIFY(cpuArgs.contains("libx265")); QVERIFY(!cpuArgs.contains("hevc_nvenc"));
        QVERIFY(gpuArgs.contains("hevc_nvenc")); QVERIFY(!gpuArgs.contains("libx265"));
        QVERIFY(!gpuArgs.contains("-crf:v:0")); QVERIFY(!gpuArgs.contains("-x265-params:v:0"));
        QVERIFY(gpuArgs.contains("35.25")); QCOMPARE(VideoEncoder::pixelFormat(gpu),QString("p010le"));
        const QString cpuSignature=settingsSignature(cpu), gpuSignature=settingsSignature(gpu);
        QCOMPARE(cpuSignature,QString("40e63046673cf882cb302a99a4ca755741eb4a9d6fdd96448c30a55ac9575858")); // Existing CPU signature.
        QVERIFY(cpuSignature!=gpuSignature);
        cpu["nvenc"]=false; QCOMPARE(settingsSignature(cpu),cpuSignature);
        // CPU controls must never affect the fixed hardware mode or its history signature.
        gpu["crf"]=0; gpu["preset"]="placebo"; gpu["threads"]=16;
        QCOMPARE(VideoEncoder::arguments(gpu),gpuArgs); QCOMPARE(settingsSignature(gpu),gpuSignature);
        gpu["avs_enabled"]=true; gpu["avs_filters"]="Crop(0,8,0,-8)";
        QVERIFY(settingsSignature(gpu)!=gpuSignature);
    }
    void nvencToggleDefaultsAndReset() {
        QTemporaryDir dir;
        MainWindow window(nullptr,dir.filePath("history.db"));
        QVERIFY(!window.m_nvenc); QVERIFY(window.m_actNvenc->isCheckable());
        QVERIFY(!window.m_actNvenc->isChecked()); QVERIFY(window.m_actNvenc->isEnabled());
        QVERIFY(window.m_actCrf->isEnabled()); QVERIFY(window.m_menuPreset->isEnabled());
        window.m_actNvenc->trigger();
        QVERIFY(window.m_nvenc); QVERIFY(window.m_actNvenc->isChecked());
        QVERIFY(window.currentEncodeSettings().value("nvenc").toBool());
        QVERIFY(!window.m_actCrf->isEnabled()); QVERIFY(!window.m_menuPreset->isEnabled());
        QVERIFY(window.m_actConcurrency->isEnabled()); QVERIFY(window.m_actDownscale->isEnabled());
        window.m_actNvenc->trigger();
        QVERIFY(!window.m_nvenc); QVERIFY(window.m_actCrf->isEnabled()); QVERIFY(window.m_menuPreset->isEnabled());
        window.m_actNvenc->trigger(); window.resetSettings();
        QVERIFY(!window.m_nvenc); QVERIFY(!window.m_actNvenc->isChecked());
        QVERIFY(!window.currentEncodeSettings().value("nvenc").toBool());
        window.m_isQueueRunning=true;
        window.m_actNvenc->setChecked(true); // A programmatic change must not alter a running batch either.
        QVERIFY(!window.m_nvenc); QVERIFY(!window.m_actNvenc->isChecked());
        window.m_isQueueRunning=false;
        MainWindow reopened(nullptr,dir.filePath("history.db"));
        QVERIFY(!reopened.m_nvenc); QVERIFY(!reopened.m_actNvenc->isChecked());
    }
    void nvencUnavailableRetainsSource() {
        if(m_ffmpeg.isEmpty()||m_ffprobe.isEmpty()) QSKIP("FFmpeg/FFprobe required");
        const auto oldSupport=qgetenv("HEVC_TEST_NVENC_SUPPORT"), oldFailure=qgetenv("HEVC_TEST_NVENC_PROBE_FAIL");
        const auto oldHidden=qgetenv("HEVC_TEST_HIDE_X265"), oldTrace=qgetenv("HEVC_TEST_COMMANDS");
        const auto restore=qScopeGuard([&] {
            for (const auto &item : {qMakePair("HEVC_TEST_NVENC_SUPPORT",oldSupport), qMakePair("HEVC_TEST_NVENC_PROBE_FAIL",oldFailure),
                qMakePair("HEVC_TEST_HIDE_X265",oldHidden), qMakePair("HEVC_TEST_COMMANDS",oldTrace)})
                if (item.second.isNull()) qunsetenv(item.first); else qputenv(item.first,item.second);
        });
        qunsetenv("HEVC_TEST_NVENC_SUPPORT"); qunsetenv("HEVC_TEST_NVENC_PROBE_FAIL"); qputenv("HEVC_TEST_HIDE_X265","1");
        for (bool runtimeFailure : {false,true}) {
            QTemporaryDir dir;
            const QString input=dir.filePath("input.mp4"), dbPath=dir.filePath("history.db");
            const QString mock=dir.filePath(QFileInfo(fixtureProgram()).fileName()), trace=dir.filePath("commands.jsonl");
            QVERIFY(QFile::copy(fixtureProgram(),mock)); QVERIFY(source(input)); const auto original=read(input);
            if (runtimeFailure) { qputenv("HEVC_TEST_NVENC_SUPPORT","1"); qputenv("HEVC_TEST_NVENC_PROBE_FAIL","1"); }
            qputenv("HEVC_TEST_COMMANDS",trace.toUtf8());
            auto config=settings(); config["nvenc"]=true; config["ffmpeg"]=mock;
            QCOMPARE(runWorker(input,dir.path(),dbPath,config),QString("Failed"));
            QVERIFY2(m_lastWorkerDetails.contains("NVENC",Qt::CaseInsensitive),qPrintable(m_lastWorkerDetails));
            QCOMPARE(read(input),original); QVERIFY(!QFileInfo::exists(dir.filePath("input.mkv")));
            QVERIFY(!QFileInfo::exists(dir.filePath(".Trash/input.mp4")));
            DatabaseManager db(dbPath); QVERIFY(!db.getProcessedFileInfo(input).found);
            for (const auto &line : read(trace).split('\n')) if (!line.isEmpty()) {
                const auto args=QJsonDocument::fromJson(line).array().toVariantList();
                QVERIFY(!args.contains(QString("-progress"))); // Neither real work nor a CPU fallback was launched.
            }
        }
    }
    void nvencWorkerDoesNotRequireX265() {
        if(m_ffmpeg.isEmpty()||m_ffprobe.isEmpty()) QSKIP("FFmpeg/FFprobe required");
        QTemporaryDir dir;
        const QString input=dir.filePath("input.mp4"), encoded=dir.filePath("encoded.mkv"), dbPath=dir.filePath("history.db");
        QVERIFY(source(input)); const auto original=read(input);
        QVERIFY(ff({"-i",input,"-c:v","libx265","-x265-params","pools=2:log-level=error","-pix_fmt","yuv420p10le",encoded}));
        const QString mock=dir.filePath(QFileInfo(fixtureProgram()).fileName()), trace=dir.filePath("commands.jsonl");
        QVERIFY(QFile::copy(fixtureProgram(),mock));
        const auto oldSupport=qgetenv("HEVC_TEST_NVENC_SUPPORT"), oldHidden=qgetenv("HEVC_TEST_HIDE_X265"), oldFixture=qgetenv("HEVC_TEST_ENCODED_FILE");
        const auto oldTrace=qgetenv("HEVC_TEST_COMMANDS"), oldFailure=qgetenv("HEVC_TEST_NVENC_PROBE_FAIL");
        const auto restore=qScopeGuard([&] {
            for (const auto &item : {qMakePair("HEVC_TEST_NVENC_SUPPORT",oldSupport),qMakePair("HEVC_TEST_HIDE_X265",oldHidden),
                qMakePair("HEVC_TEST_ENCODED_FILE",oldFixture),qMakePair("HEVC_TEST_COMMANDS",oldTrace),qMakePair("HEVC_TEST_NVENC_PROBE_FAIL",oldFailure)})
                if (item.second.isNull()) qunsetenv(item.first); else qputenv(item.first,item.second);
        });
        qputenv("HEVC_TEST_NVENC_SUPPORT","1"); qputenv("HEVC_TEST_HIDE_X265","1"); qunsetenv("HEVC_TEST_NVENC_PROBE_FAIL");
        qputenv("HEVC_TEST_ENCODED_FILE",encoded.toUtf8()); qputenv("HEVC_TEST_COMMANDS",trace.toUtf8());
        auto config=settings(); config["nvenc"]=true; config["ffmpeg"]=mock; config["crf"]=0; config["preset"]="placebo";
        QCOMPARE(runWorker(input,dir.path(),dbPath,config),QString("Completed"));
        QCOMPARE(read(dir.filePath(".Trash/input.mp4")),original);
        DatabaseManager db(dbPath); const auto history=db.getProcessedFileInfo(dir.filePath("input.mkv"));
        QVERIFY(history.found); QCOMPARE(history.settingsKey,settingsSignature(config));
        bool sawProbe=false,sawEncode=false;
        for (const auto &line : read(trace).split('\n')) if (!line.isEmpty()) {
            const auto args=QJsonDocument::fromJson(line).array().toVariantList();
            if (args.contains(QString("lavfi"))) sawProbe=true;
            if (args.contains(QString("-progress"))) {
                sawEncode=true; QVERIFY(args.contains(QString("hevc_nvenc"))); QVERIFY(args.contains(QString("35.25")));
                QVERIFY(!args.contains(QString("libx265"))); QVERIFY(!args.contains(QString("-crf:v:0")));
                QVERIFY(!args.contains(QString("-x265-params:v:0"))); QVERIFY(args.contains(QString("format=p010le")));
            }
        }
        QVERIFY(sawProbe); QVERIFY(sawEncode);
    }
    void nvencRealWorkerAndAvisynthCrop() {
        if (!qEnvironmentVariableIsSet("HEVC_TEST_NVENC") && !qEnvironmentVariableIsSet("HEVC_TEST_REQUIRE_NVENC"))
            QSKIP("Set HEVC_TEST_NVENC=1 to run real NVIDIA hardware tests");
        if (m_ffmpeg.isEmpty() || m_ffprobe.isEmpty()) QFAIL("FFmpeg/FFprobe required for requested NVENC test");
        auto config=settings(); config["nvenc"]=true;
        const QString availability=VideoEncoder::validate(m_ffmpeg,config);
        if (!availability.isEmpty()) {
            if (qEnvironmentVariableIsSet("HEVC_TEST_REQUIRE_NVENC")) QFAIL(qPrintable(availability));
            QSKIP(qPrintable(availability));
        }
        QTemporaryDir dir;
        const QString base=dir.filePath("base.mp4"), input=dir.filePath("normal.mp4"), dbPath=dir.filePath("history.db");
        // Keep dimensions above NVENC's minimum and avoid testsrc2's synthetic
        // line patterns being classified as interlaced by the real idet pass.
        QVERIFY(ff({"-f","lavfi","-i","testsrc=size=320x240:rate=24","-t","4",
            "-c:v","libx264","-threads","1","-crf","0","-pix_fmt","yuv420p",base}));
        QVERIFY(put(dir.filePath("captions.srt"),"1\n00:00:00,000 --> 00:00:02,000\nKeep these captions\n"));
        QVERIFY(ff({"-i",base,"-f","lavfi","-i","sine=frequency=1000","-i",dir.filePath("captions.srt"),
            "-t","4","-map","0:v","-map","1:a","-map","2:s","-c:v","copy","-c:a","aac","-c:s","mov_text",input}));
        const auto original=read(input);
        QCOMPARE(runWorker(input,dir.path(),dbPath,config),QString("Completed"));
        auto checkOutput=[&](const QString &output,int height) {
            const auto meta=probeMetadata(output,m_ffprobe);
            QVERIFY2(meta.valid,qPrintable(meta.error)); QCOMPARE(meta.vcodec,QString("hevc"));
            QCOMPARE(meta.width,320); QCOMPARE(meta.height,height); QCOMPARE(meta.fps,24.0);
            QCOMPARE(meta.sampleAspectRatio,QString("1:1")); QCOMPARE(meta.streams.size(),3);
            QCOMPARE(meta.acodec,QString("aac")); QCOMPARE(packetTimes(output,"v:0").size(),96);
            const auto stream=meta.json.value("streams").toArray().first().toObject();
            QCOMPARE(stream.value("profile").toString(),QString("Main 10"));
            QCOMPARE(stream.value("pix_fmt").toString(),QString("yuv420p10le"));
            DatabaseManager db(dbPath); const auto history=db.getProcessedFileInfo(output);
            QVERIFY(history.found); QCOMPARE(history.settingsKey,settingsSignature(config));
        };
        checkOutput(dir.filePath("normal.mkv"),240);
        QCOMPARE(read(dir.filePath(".Trash/normal.mp4")),original);
        const QString installation=AvisynthPipeline::testInstallation(m_ffmpeg,m_ffprobe,nullptr);
        if (!installation.isEmpty()) {
            if (qEnvironmentVariableIsSet("HEVC_TEST_REQUIRE_AVISYNTH")) QFAIL(qPrintable(installation));
            QSKIP(qPrintable(installation));
        }
        const QString filtered=dir.filePath("filtered.mp4"); QVERIFY(put(filtered,original));
        config["avs_enabled"]=true; config["avs_filters"]="Crop(0,8,0,-8)"; config["avs_cache_root"]=dir.filePath("cache");
        config["live_preview"]=true;
        TranscodeWorker filteredWorker({filtered},dir.path(),dbPath,config);
        QSignalSpy done(&filteredWorker,&TranscodeWorker::fileDoneSignal);
        QSignalSpy pictures(&filteredWorker,&TranscodeWorker::encodedPreviewSignal);
        filteredWorker.start(); QVERIFY(filteredWorker.wait(60000)); QCoreApplication::processEvents();
        QCOMPARE(done.size(),1); QCOMPARE(done.first().at(1).toString(),QString("Completed"));
        QVERIFY(!pictures.isEmpty());
        for (const auto &picture : pictures) {
            QCOMPARE(picture.first().toString(),filtered);
            QCOMPARE(QImage::fromData(picture.at(1).toByteArray(),"PNG").size(),QSize(240,168));
        }
        checkOutput(dir.filePath("filtered.mkv"),224);
        QCOMPARE(read(dir.filePath(".Trash/filtered.mp4")),original);
        const QString sample=dir.filePath("nvenc-sample.mkv");
        AvisynthCheckWorker sampleWorker(AvisynthCheckWorker::Sample,base,{"Crop","Crop(0,8,0,-8)"},config,0,sample,nullptr);
        QSignalSpy ready(&sampleWorker,&AvisynthCheckWorker::sampleReady), reports(&sampleWorker,&AvisynthCheckWorker::report);
        sampleWorker.start(); QVERIFY(sampleWorker.wait(60000)); QCoreApplication::processEvents();
        QVERIFY2(ready.size()==1,qPrintable(reports.isEmpty()?QString("No sample report"):reports.last().first().toString()));
        QCOMPARE(ready.first().first().toString(),sample);
        const auto sampleMeta=probeMetadata(sample,m_ffprobe);
        QVERIFY2(sampleMeta.valid,qPrintable(sampleMeta.error)); QCOMPARE(sampleMeta.vcodec,QString("hevc"));
        QCOMPARE(sampleMeta.width,320); QCOMPARE(sampleMeta.height,224); QCOMPARE(sampleMeta.fps,24.0);
        QCOMPARE(sampleMeta.streams.size(),1); QVERIFY(!sampleMeta.hasAudio);
        QCOMPARE(packetTimes(sample,"v:0").size(),96);
        const auto sampleStream=sampleMeta.json.value("streams").toArray().first().toObject();
        QCOMPARE(sampleStream.value("profile").toString(),QString("Main 10"));
        QCOMPARE(sampleStream.value("pix_fmt").toString(),QString("yuv420p10le"));
    }
    void avisynthBatchAssignmentsExpire() {
        QTemporaryDir dir;
        const QString first = dir.filePath("one.mkv"), second = dir.filePath("two.mp4");
        QVERIFY(put(first,"original1")); QVERIFY(put(second,"original2"));
        MainWindow window(nullptr, dir.filePath("history.db"));
        setupWindow(window,dir.path(),dir.filePath("history.db"));
        window.m_tableQueue->setRowCount(2);
        for (int row = 0; row < 2; ++row) {
            const QStringList values{row == 0 ? "one.mkv" : "two.mp4", row == 0 ? "Skipped" : "Pending",
                "1", "0", "original status", row == 0 ? "one.mkv" : "two.mp4", "Off"};
            for (int col=0; col<values.size(); ++col) window.m_tableQueue->setItem(row,col,new QTableWidgetItem(values[col]));
        }
        window.assignAvisynth({first,second},{"Crop", "Crop(0, 8, 0, -8)"},false);
        QCOMPARE(window.m_filterAssignments.size(),2);
        QCOMPARE(window.m_activeTranscodeQueue.size(),2); // Even compatible HEVC is queued.
        window.m_crf=21; window.m_preset="fast";
        for (int toggle=0; toggle<2; ++toggle) {
            window.m_actNvenc->trigger();
            QCOMPARE(window.m_crf,21); QCOMPARE(window.m_preset,QString("fast"));
            QCOMPARE(window.m_filterAssignments.size(),2); QCOMPARE(window.m_activeTranscodeQueue.size(),2);
            QCOMPARE(window.m_filterAssignments.value(first).profile.filters,QString("Crop(0, 8, 0, -8)"));
            QCOMPARE(window.m_filterAssignments.value(second).profile.filters,QString("Crop(0, 8, 0, -8)"));
        }
        window.assignAvisynth({first},{"Other", "Blur(0.1)"},false);
        QCOMPARE(window.m_filterAssignments.value(second).profile.name,QString("Crop"));
        window.assignAvisynth({first},{},true);
        QCOMPARE(window.m_tableQueue->item(0,1)->text(),QString("Skipped"));
        QCOMPARE(window.m_filterAssignments.size(),1);
        window.m_avisynthTools["avs_ffmpeg"]="batch tool";
        window.clearQueue();
        QVERIFY(window.m_filterAssignments.isEmpty()); QVERIFY(window.m_avisynthTools.isEmpty());
        QCOMPARE(window.m_tableQueue->rowCount(),0);
        MainWindow reopened(nullptr,dir.filePath("history.db"));
        QVERIFY(reopened.m_filterAssignments.isEmpty()); QVERIFY(reopened.m_avisynthTools.isEmpty());
    }
    void avisynthRealCropAndRejections() {
        if(m_ffmpeg.isEmpty()||m_ffprobe.isEmpty()) QSKIP("FFmpeg/FFprobe required");
        const QString installation = AvisynthPipeline::testInstallation(m_ffmpeg,m_ffprobe,nullptr);
        if (!installation.isEmpty()) {
            if (qEnvironmentVariableIsSet("HEVC_TEST_REQUIRE_AVISYNTH")) QFAIL(qPrintable(installation));
            QSKIP(qPrintable(installation));
        }
        QTemporaryDir dir;
        const QString input=dir.filePath("vidéø 日本 crop.mp4"), base=dir.filePath("base.mp4");
        QVERIFY(source(base));
        QVERIFY(put(dir.filePath("caption.srt"),"1\n00:00:01,000 --> 00:00:03,000\nKeep these captions\n"));
        QVERIFY(ff({"-i",base,"-f","lavfi","-i","sine=frequency=1000","-i",dir.filePath("caption.srt"),
            "-t","4","-map","0:v","-map","1:a","-map","2:s","-c:v","copy","-c:a","aac","-c:s","mov_text",input}));
        const QByteArray original=read(input);
        auto config=settings();
        config["avs_enabled"]=true; config["avs_filters"]="Crop(0, 8, 0, -8)";
        config["avs_cache_root"]=dir.filePath("cache");
        QCOMPARE(runWorker(input,dir.path(),dir.filePath("history.db"),config),QString("Completed"));
        const auto output=probeMetadata(dir.filePath("vidéø 日本 crop.mkv"),m_ffprobe);
        QCOMPARE(output.width,160); QCOMPARE(output.height,80); QCOMPARE(output.fps,24.0);
        QCOMPARE(output.streams.size(),3); QCOMPARE(output.acodec,QString("aac"));
        QCOMPARE(output.streams.last().codec,QString("subrip"));
        QCOMPARE(read(dir.filePath(".Trash/vidéø 日本 crop.mp4")),original);
        const QString rejected=dir.filePath("reject.mp4"); QVERIFY(source(rejected));
        const auto bytes=read(rejected);
        config["avs_filters"]="Trim(0, 10)";
        QCOMPARE(runWorker(rejected,dir.path(),dir.filePath("history.db"),config),QString("Failed"));
        QVERIFY(m_lastWorkerDetails.contains("frame count")); QCOMPARE(read(rejected),bytes);
        config["avs_filters"]="ShrinkerMissingPluginFilter()";
        QCOMPARE(runWorker(rejected,dir.path(),dir.filePath("history.db"),config),QString("Failed"));
        QVERIFY(m_lastWorkerDetails.contains("ShrinkerMissingPluginFilter")); QCOMPARE(read(rejected),bytes);
        config["avs_filters"]="Crop(0, 8, 0, -8)"; config["avs_source_size"]=1;
        QCOMPARE(runWorker(rejected,dir.path(),dir.filePath("history.db"),config),QString("Failed"));
        QVERIFY(m_lastWorkerDetails.contains("changed since"));
        config.remove("avs_source_size");
        const QString multiple=dir.filePath("multiple.mkv");
        QVERIFY(ff({"-i",base,"-f","lavfi","-i","testsrc2=size=96x64:rate=24","-t","4",
            "-map","0:v","-map","1:v","-c:v","libx264","-crf","0","-threads","1",
            "-disposition:v:0","0","-disposition:v:1","default",multiple}));
        QCOMPARE(runWorker(multiple,dir.path(),dir.filePath("history.db"),config),QString("Completed"));
        const auto selected=probeMetadata(multiple,m_ffprobe);
        QCOMPARE(selected.width,96); QCOMPARE(selected.height,48); QCOMPARE(selected.streams.size(),2);
    }
    void avisynthPreservesStreamOffsets_data() {
        QTest::addColumn<QString>("rate");
        QTest::addColumn<QString>("videoDelay");
        QTest::addColumn<QString>("audioDelay");
        QTest::newRow("21ms-at-25fps") << "25" << "0.021" << "0";
        QTest::newRow("44ms-at-30fps") << "30" << "0.044" << "0";
        QTest::newRow("fractional-frame-rate") << "24000/1001" << "0.021" << "0";
        QTest::newRow("two-second-video-delay") << "24" << "2" << "0";
        QTest::newRow("video-before-audio") << "25" << "0.021" << "0.2";
    }
    void avisynthPreservesStreamOffsets() {
        QFETCH(QString, rate);
        QFETCH(QString, videoDelay);
        QFETCH(QString, audioDelay);
        if (m_ffmpeg.isEmpty() || m_ffprobe.isEmpty()) QSKIP("FFmpeg/FFprobe required");
        const QString installation = AvisynthPipeline::testInstallation(m_ffmpeg,m_ffprobe,nullptr);
        if (!installation.isEmpty()) {
            if (qEnvironmentVariableIsSet("HEVC_TEST_REQUIRE_AVISYNTH")) QFAIL(qPrintable(installation));
            QSKIP(qPrintable(installation));
        }
        QTemporaryDir dir;
        const QString base = dir.filePath("base.mkv"), input = dir.filePath("offset.mkv");
        // Encode first, then offset copied packets so the fixture's 21/44 ms
        // delay is not itself rounded by an encoder's frame-sized time base.
        QVERIFY(ff({"-f","lavfi","-i","testsrc2=size=160x96:rate=" + rate,"-t","4",
            "-c:v","libx264","-threads","1","-crf","0",base}));
        QVERIFY(put(dir.filePath("caption.srt"),"1\n00:00:01,000 --> 00:00:03,000\nKeep timing\n"));
        QVERIFY(ff({"-itsoffset",videoDelay,"-i",base,"-itsoffset",audioDelay,
            "-f","lavfi","-i","sine=frequency=1000:duration=7","-i",dir.filePath("caption.srt"),
            "-map","0:v","-map","1:a","-map","2:s","-c:v","copy","-c:a","aac","-c:s","copy",input}));
        const auto videoBefore = packetTimes(input,"v:0"), audioBefore = packetTimes(input,"a:0"),
            subtitlesBefore = packetTimes(input,"s:0");
        QVERIFY(videoBefore.size() >= 96); QVERIFY(!audioBefore.isEmpty()); QCOMPARE(subtitlesBefore.size(),1);
        const auto original = read(input);
        auto config = settings();
        config["avs_enabled"] = true; config["avs_filters"] = "Crop(0, 8, 0, -8)";
        config["avs_cache_root"] = dir.filePath("cache");
        QCOMPARE(runWorker(input,dir.path(),dir.filePath("history.db"),config),QString("Completed"));
        const auto videoAfter = packetTimes(input,"v:0"), audioAfter = packetTimes(input,"a:0"),
            subtitlesAfter = packetTimes(input,"s:0");
        QCOMPARE(videoAfter.size(),videoBefore.size());
        QCOMPARE(audioAfter.size(),audioBefore.size()); QCOMPARE(subtitlesAfter.size(),subtitlesBefore.size());
        // A muxer may shift every stream by a common amount. What must survive
        // is the alignment, including fractions of a video frame, all the way
        // through the clip. Audio outlasts video to exercise the count check.
        const double shift = audioAfter.first() - audioBefore.first();
        for (qsizetype i = 0; i < videoBefore.size(); ++i)
            QVERIFY2(std::abs((videoAfter[i] - videoBefore[i]) - shift) <= 0.002,
                qPrintable(QString("Video timing changed at frame %1: before=%2 after=%3 common shift=%4")
                    .arg(i).arg(videoBefore[i],0,'f',6).arg(videoAfter[i],0,'f',6).arg(shift,0,'f',6)));
        for (qsizetype i = 0; i < audioBefore.size(); ++i)
            QVERIFY(std::abs((audioAfter[i] - audioBefore[i]) - shift) <= 0.002);
        QVERIFY(std::abs((subtitlesAfter.first() - subtitlesBefore.first()) - shift) <= 0.002);
        QCOMPARE(probeMetadata(input,m_ffprobe).height,80);
        QCOMPARE(read(dir.filePath(".Trash/offset.mkv")),original);
    }
    void variableTimingWorksWithAndWithoutAvisynth() {
        if (m_ffmpeg.isEmpty() || m_ffprobe.isEmpty()) QSKIP("FFmpeg/FFprobe required");
        const QString installation=AvisynthPipeline::testInstallation(m_ffmpeg,m_ffprobe,nullptr);
        if (!installation.isEmpty()) {
            if (qEnvironmentVariableIsSet("HEVC_TEST_REQUIRE_AVISYNTH")) QFAIL(qPrintable(installation));
            QSKIP(qPrintable(installation));
        }
        QTemporaryDir dir;
        const QString base=dir.filePath("vfr.mkv"), input=dir.filePath("filtered.mkv"), normal=dir.filePath("normal.mkv");
        QVERIFY(ff({"-f","lavfi","-i","testsrc=size=160x96:rate=30","-t","4",
            "-vf","select=not(eq(n\\,30)+eq(n\\,60))","-fps_mode","vfr",
            "-pix_fmt","yuv420p","-c:v","libx264","-crf","0","-threads","1",base}));
        QVERIFY(put(dir.filePath("caption.srt"),"1\n00:00:01,000 --> 00:00:03,000\nKeep timing\n"));
        QVERIFY(ff({"-itsoffset","0.044","-i",base,"-f","lavfi","-i","sine=frequency=1000:duration=6",
            "-i",dir.filePath("caption.srt"),"-map","0:v","-map","1:a","-map","2:s",
            "-c:v","copy","-c:a","aac","-c:s","copy",input}));
        QVERIFY(QFile::copy(input,normal));
        const auto beforeVideo=packetTimes(input,"v:0"), beforeAudio=packetTimes(input,"a:0"), beforeSubs=packetTimes(input,"s:0");
        QCOMPARE(beforeVideo.size(),118); QVERIFY(!beforeAudio.isEmpty()); QCOMPARE(beforeSubs.size(),1);
        auto config=settings(); config["avs_enabled"]=true; config["avs_filters"]="Crop(0,8,0,-8)";
        config["avs_cache_root"]=dir.filePath("cache");
        QCOMPARE(runWorker(input,dir.path(),dir.filePath("history.db"),config),QString("Completed"));
        const auto afterVideo=packetTimes(input,"v:0"), afterAudio=packetTimes(input,"a:0"), afterSubs=packetTimes(input,"s:0");
        QCOMPARE(afterVideo.size(),120); QCOMPARE(afterAudio.size(),beforeAudio.size()); QCOMPARE(afterSubs.size(),1);
        const double shift=afterAudio.first()-beforeAudio.first();
        QVERIFY(std::abs(afterVideo.first()-beforeVideo.first()-shift)<=0.002);
        QVERIFY(std::abs(afterVideo.last()-beforeVideo.last()-shift)<=0.002);
        for (qsizetype i=1; i<afterVideo.size(); ++i)
            QVERIFY(std::abs((afterVideo[i]-afterVideo[0])-double(i)/30.0)<=0.0011);
        for (qsizetype i=0; i<afterAudio.size(); ++i) QVERIFY(std::abs(afterAudio[i]-beforeAudio[i]-shift)<=0.002);
        QVERIFY(std::abs(afterSubs.first()-beforeSubs.first()-shift)<=0.002);
        QCOMPARE(probeMetadata(input,m_ffprobe).height,80);
        QCOMPARE(runWorker(normal,dir.path(),dir.filePath("history.db")),QString("Completed"));
        // This progressive fixture retains its variable cadence on the
        // ordinary path; only the AviSynth reader needs CFR normalization.
        const auto normalVideo=packetTimes(normal,"v:0"); QCOMPARE(normalVideo.size(),beforeVideo.size());
        const double normalShift=normalVideo.first()-beforeVideo.first();
        for (qsizetype i=0; i<normalVideo.size(); ++i) QVERIFY(std::abs(normalVideo[i]-beforeVideo[i]-normalShift)<=0.002);
        ScanWorker scan(dir.path(),dir.filePath("history.db"),settings()); scan.start(); QVERIFY(scan.wait(15000));
        for (const auto &file : scan.files)
            if (file.absolutePath==input || file.absolutePath==normal) QCOMPARE(file.status,QString("Completed"));
    }
    void avisynthSavedProfileStartsInactive() {
        QTemporaryDir dir;
        const QString store=dir.filePath("profiles.ini");
        {
            AvisynthDialog dialog("example.mp4",{},1,10,settings(),nullptr,store);
            QVERIFY(dialog.profile().filters.isEmpty());
            dialog.m_name->setText("Batch crop"); dialog.m_filters->setPlainText("Crop(0, 8, 0, -8)");
            dialog.saveProfile();
        }
        AvisynthDialog reopened("example.mp4",{},1,10,settings(),nullptr,store);
        QCOMPARE(reopened.m_presets->count(),2);
        QCOMPARE(reopened.m_presets->currentIndex(),0);
        QVERIFY(reopened.profile().filters.isEmpty());
        QCOMPARE(reopened.scope(),AvisynthDialog::ThisFile);
        const QString screenshot=qEnvironmentVariable("HEVC_TEST_SCREENSHOT_DIR");
        if (!screenshot.isEmpty()) {
            QDir().mkpath(screenshot);
            reopened.show(); QTest::qWait(100);
            QVERIFY(reopened.grab().save(QDir(screenshot).filePath("avisynth-dialog.png")));
        }
    }
    void avisynthPreviewSampleAndNoGrowth() {
        if(m_ffmpeg.isEmpty()||m_ffprobe.isEmpty()) QSKIP("FFmpeg/FFprobe required");
        const QString installation = AvisynthPipeline::testInstallation(m_ffmpeg,m_ffprobe,nullptr);
        if (!installation.isEmpty()) {
            if (qEnvironmentVariableIsSet("HEVC_TEST_REQUIRE_AVISYNTH")) QFAIL(qPrintable(installation));
            QSKIP(qPrintable(installation));
        }
        QTemporaryDir dir;
        const QString input=dir.filePath("source.mp4"), sample=dir.filePath("sample.mkv");
        QVERIFY(source(input));
        auto config=settings(); config["avs_cache_root"]=dir.filePath("cache");
        AvisynthCheckWorker compare(AvisynthCheckWorker::Compare,input,{"Crop","Crop(0, 8, 0, -8)"},config,1,sample,nullptr);
        QSignalSpy pictures(&compare,&AvisynthCheckWorker::comparison), messages(&compare,&AvisynthCheckWorker::report);
        compare.start(); QVERIFY(compare.wait(60000)); QCoreApplication::processEvents();
        QVERIFY2(pictures.size()==1,qPrintable(messages.isEmpty()?QString("No report"):messages.last().at(0).toString()));
        QVERIFY(!QImage::fromData(pictures.at(0).at(0).toByteArray()).isNull());
        QVERIFY(!QImage::fromData(pictures.at(0).at(1).toByteArray()).isNull());
        AvisynthCheckWorker encode(AvisynthCheckWorker::Sample,input,{"Crop","Crop(0, 8, 0, -8)"},config,1,sample,nullptr);
        QSignalSpy samples(&encode,&AvisynthCheckWorker::sampleReady);
        encode.start(); QVERIFY(encode.wait(60000)); QCoreApplication::processEvents();
        QCOMPARE(samples.size(),1); QCOMPARE(probeMetadata(sample,m_ffprobe).height,80);
        const QString small=dir.filePath("tiny.mp4");
        QVERIFY(ff({"-i",input,"-c:v","libx265","-crf","45","-x265-params","pools=2:log-level=error",small}));
        const auto original=read(small);
        config["avs_enabled"]=true; config["avs_filters"]="Crop(0, 8, 0, -8)"; config["crf"]=0;
        QCOMPARE(runWorker(small,dir.path(),dir.filePath("history.db"),config),QString("Skipped"));
        QVERIFY(m_lastWorkerDetails.contains("cleanup was not applied"));
        QCOMPARE(read(small),original); QVERIFY(!QFileInfo::exists(dir.filePath("tiny.mkv")));
        QVERIFY(!QFileInfo::exists(dir.filePath(".Trash")));
    }
    void realEncodeRotationAudioAndSubtitles() {
        if(m_ffmpeg.isEmpty()||m_ffprobe.isEmpty()) QSKIP("FFmpeg/FFprobe are required for media integration tests");
        QTemporaryDir dir;
        QVERIFY(source(dir.filePath("base.mp4")));
        QVERIFY(put(dir.filePath("captions.srt"),"1\n00:00:00,000 --> 00:00:02,000\nHello\n"));
        const QString input=dir.filePath("input.mp4");
        QVERIFY(ff({"-i",dir.filePath("base.mp4"),"-f","lavfi","-i","sine=frequency=1000", "-f","lavfi","-i","sine=frequency=500",
            "-i",dir.filePath("captions.srt"),"-t","4","-map","0:v","-map","1:a","-map","2:a","-map","3:s",
            "-c:v","copy","-c:a:0","aac","-c:a:1","ac3","-c:s","mov_text",input}));
        const auto original=read(input);
        QCOMPARE(runWorker(input,dir.path(),dir.filePath("history.db")),QString("Completed"));
        QCOMPARE(read(dir.filePath(".Trash/input.mp4")),original);
        auto output=probeMetadata(dir.filePath("input.mkv"),m_ffprobe); QVERIFY(output.valid);
        int audioCount=0,subtitleCount=0;
        for(const auto &stream:output.streams) {
            if(stream.type=="audio") { QCOMPARE(stream.codec,QString("aac")); ++audioCount; }
            if(stream.type=="subtitle") { QCOMPARE(stream.codec,QString("subrip")); ++subtitleCount; }
        }
        QCOMPARE(audioCount,2); QCOMPARE(subtitleCount,1);
        const QString rotated=dir.filePath("rotated.mp4");
        QVERIFY(ff({"-display_rotation:v:0","90","-i",dir.filePath("base.mp4"),"-c","copy",rotated}));
        QCOMPARE(runWorker(rotated,dir.path(),dir.filePath("history.db")),QString("Completed"));
        output=probeMetadata(dir.filePath("rotated.mkv"),m_ffprobe);
        QCOMPARE(output.width,96); QCOMPARE(output.height,160); QCOMPARE(output.sampleAspectRatio,QString("1:1"));
    }
    void validationRejectsTruncatedOutput() {
        if(m_ffmpeg.isEmpty()||m_ffprobe.isEmpty()) QSKIP("FFmpeg/FFprobe are required for media integration tests");
        QTemporaryDir dir;
        const QString input=dir.filePath("source.mp4"),encoded=dir.filePath("short.mkv");
        QVERIFY(source(input,5)); const auto original=read(input);
        QVERIFY(ff({"-i",input,"-t","0.5","-c:v","libx265","-x265-params","pools=2:log-level=error","-pix_fmt","yuv420p10le",encoded}));
        qputenv("HEVC_TEST_ENCODED_FILE",encoded.toUtf8());
        auto config=settings();config["ffmpeg"]=fixtureProgram();
        QCOMPARE(runWorker(input,dir.path(),dir.filePath("history.db"),config),QString("Failed"));
        qunsetenv("HEVC_TEST_ENCODED_FILE");
        QVERIFY(m_lastWorkerDetails.contains("duration"));
        QCOMPARE(read(input),original); QVERIFY(!QFileInfo::exists(dir.filePath("source.mkv")));
    }
    void audioConversionsKeepOriginalCompletionPolicy() {
        if(m_ffmpeg.isEmpty()||m_ffprobe.isEmpty()) QSKIP("FFmpeg/FFprobe are required for media integration tests");
        QTemporaryDir dir;
        const QString base=dir.filePath("base.mkv"), encoded=dir.filePath("encoded.mkv");
        QVERIFY(ff({"-f","lavfi","-i","testsrc2=size=160x96:rate=24","-f","lavfi","-i","sine=frequency=1000",
            "-t","4","-c:v","libx265","-x265-params","pools=2:log-level=error","-pix_fmt","yuv420p10le","-c:a","ac3",base}));
        QVERIFY(ff({"-i",base,"-c:v","copy","-c:a","aac",encoded}));
        auto config=settings(); config["ffmpeg"]=fixtureProgram();
        for (const QString &extension : {QString("mp4"),QString("mkv")}) {
            config["nvenc"]=(extension=="mkv"); // Audio-only work must not require an available GPU.
            const QString input=dir.filePath("input."+extension);
            QVERIFY(ff({"-i",base,"-c","copy",input}));
            const QByteArray original=read(input);
            // Valid output with trailing padding simulates an audio conversion
            // larger than its input, without relying on encoder size heuristics.
            QFile padded(encoded); QVERIFY(padded.open(QIODevice::ReadWrite));
            QVERIFY(padded.resize(qMax(padded.size(),qint64(original.size()))+8192)); padded.close();
            qputenv("HEVC_TEST_ENCODED_FILE",encoded.toUtf8());
            const QString status=runWorker(input,dir.path(),dir.filePath("history.db"),config);
            qunsetenv("HEVC_TEST_ENCODED_FILE");
            QCOMPARE(status,QString("Completed"));
            QCOMPARE(read(dir.filePath(".Trash/input."+extension)),original);
            QCOMPARE(probeMetadata(dir.filePath("input.mkv"),m_ffprobe).acodec,QString("aac"));
            DatabaseManager db(dir.filePath("history.db"));
            const auto history=db.getProcessedFileInfo(dir.filePath("input.mkv"));
            QVERIFY(history.current); QCOMPARE(history.outcome,QString("Completed"));
            QVERIFY(QFile::remove(dir.filePath("input.mkv")));
        }
    }
    void livePreviewReadsGrowingOutput() {
        if (m_ffmpeg.isEmpty() || m_ffprobe.isEmpty()) QSKIP("FFmpeg/FFprobe required");
        QTemporaryDir dir;
        const QString input=dir.filePath("source.mp4"), output=dir.filePath("growing.mkv");
        QVERIFY(source(input,8));
        auto config=settings(); config["avs_enabled"]=true; config["live_preview"]=true;
        TranscodeWorker worker({},dir.path(),dir.filePath("history.db"),config);
        int progress=0; bool previewDuringEncode=false;
        QSignalSpy pictures(&worker,&TranscodeWorker::encodedPreviewSignal);
        connect(&worker,&TranscodeWorker::progressSignal,this,[&](const QString &,int percentage) { progress=percentage; });
        connect(&worker,&TranscodeWorker::encodedPreviewSignal,this,[&] { if (progress < 90) previewDuringEncode=true; });
        // Pace a real encoder so previews must decode an incomplete Matroska
        // file. Flush small clusters to make this short fixture readable early.
        const auto result=worker.encode(m_ffmpeg,{"-v","error","-nostdin","-y","-re","-i",input,
            "-an","-vf","crop=160:80:0:8","-c:v","libx265","-preset","ultrafast",
            "-x265-params","pools=2:log-level=error","-pix_fmt","yuv420p10le",
            "-flush_packets","1","-cluster_time_limit","1000","-progress","pipe:1",output},8,input,output);
        QVERIFY2(result.ok,qPrintable(result.error)); QVERIFY(previewDuringEncode);
        for (const auto &picture : pictures)
            QCOMPARE(QImage::fromData(picture.at(1).toByteArray(),"PNG").size(),QSize(240,120));
        QVERIFY(QFile::rename(output,dir.filePath("finished.mkv"))); // Preview reader released the output.
    }
    void avisynthLivePreviewUsesFilteredOutput() {
        if (m_ffmpeg.isEmpty() || m_ffprobe.isEmpty()) QSKIP("FFmpeg/FFprobe required");
        const QString installation=AvisynthPipeline::testInstallation(m_ffmpeg,m_ffprobe,nullptr);
        if (!installation.isEmpty()) {
            if (qEnvironmentVariableIsSet("HEVC_TEST_REQUIRE_AVISYNTH")) QFAIL(qPrintable(installation));
            QSKIP(qPrintable(installation));
        }
        QTemporaryDir dir;
        const QString input=dir.filePath("preview.mp4"); QVERIFY(source(input));
        auto config=settings(); config["avs_enabled"]=true; config["avs_filters"]="Crop(0, 8, 0, -8)";
        config["avs_cache_root"]=dir.filePath("cache"); config["live_preview"]=true;
        MainWindow window(nullptr,dir.filePath("history.db"));
        window.m_livePreviewEnabled=true; window.m_isQueueRunning=true; window.m_viewfinderFocusedFile=input;
        TranscodeWorker worker({input},dir.path(),dir.filePath("history.db"),config);
        QSignalSpy pictures(&worker,&TranscodeWorker::encodedPreviewSignal), done(&worker,&TranscodeWorker::fileDoneSignal);
        connect(&worker,&TranscodeWorker::encodedPreviewSignal,&window,&MainWindow::onEncodedPreviewReady);
        worker.start(); QVERIFY(worker.wait(60000)); QCoreApplication::processEvents();
        QCOMPARE(done.size(),1); QCOMPARE(done.first().at(1).toString(),QString("Completed"));
        QVERIFY(!pictures.isEmpty()); QCOMPARE(pictures.first().at(0).toString(),input);
        const auto png=pictures.first().at(1).toByteArray();
        const auto image=QImage::fromData(png,"PNG");
        QCOMPARE(image.size(),QSize(240,120)); // Cropped output; the source would be 240x144.
        QVERIFY(!window.m_lblViewfinder->pixmap().isNull());
        window.m_lblViewfinder->clear();
        window.onEncodedPreviewReady("different.mp4",png,0);
        QVERIFY(window.m_lblViewfinder->pixmap().isNull());
        window.m_livePreviewEnabled=false; window.onEncodedPreviewReady(input,png,0);
        QVERIFY(window.m_lblViewfinder->pixmap().isNull());
        window.m_livePreviewEnabled=true; window.m_isQueueRunning=false; window.onEncodedPreviewReady(input,png,0);
        QVERIFY(window.m_lblViewfinder->pixmap().isNull());
    }
    void liveConcurrencyChanges() {
        if (m_ffmpeg.isEmpty() || m_ffprobe.isEmpty()) QSKIP("FFmpeg/FFprobe required");
        QTemporaryDir dir;
        const QString base=dir.filePath("base.mp4"), encoded=dir.filePath("encoded.mkv"), gates=dir.filePath("gates");
        QVERIFY(source(base));
        QVERIFY(ff({"-i",base,"-c:v","libx265","-x265-params","pools=2:log-level=error",
            "-pix_fmt","yuv420p10le",encoded}));
        const QString bin=dir.filePath("bin"); QVERIFY(QDir().mkpath(bin));
        const QString mock=QDir(bin).filePath(QFileInfo(m_ffmpeg).fileName());
        QVERIFY(QFile::copy(fixtureProgram(),mock));
        const auto oldPath=qgetenv("PATH"), oldFixture=qgetenv("HEVC_TEST_ENCODED_FILE"), oldGates=qgetenv("HEVC_TEST_ENCODE_GATES");
        const auto restore=qScopeGuard([&] {
            qputenv("PATH",oldPath);
            if (oldFixture.isNull()) qunsetenv("HEVC_TEST_ENCODED_FILE"); else qputenv("HEVC_TEST_ENCODED_FILE",oldFixture);
            if (oldGates.isNull()) qunsetenv("HEVC_TEST_ENCODE_GATES"); else qputenv("HEVC_TEST_ENCODE_GATES",oldGates);
        });
        // The fixture holds each encoder until its gate opens; use the same
        // process launch path on Windows and Linux, with Qt DLLs on PATH.
        qputenv("PATH",bin.toUtf8()+QDir::listSeparator().toLatin1()+QCoreApplication::applicationDirPath().toUtf8()
            +QDir::listSeparator().toLatin1()+oldPath);
        qputenv("HEVC_TEST_ENCODED_FILE",encoded.toUtf8()); qputenv("HEVC_TEST_ENCODE_GATES",gates.toUtf8());
        QCOMPARE(findDependency("ffmpeg"),mock);
        MainWindow window(nullptr,dir.filePath("history.db")); setupWindow(window,dir.path(),dir.filePath("history.db"));
        window.m_tableQueue->setRowCount(4);
        QStringList paths;
        for (int row=0; row<4; ++row) {
            const QString name=QString("input%1.mp4").arg(row), path=dir.filePath(name);
            QVERIFY(QFile::copy(base,path)); paths << path;
            const QStringList cells{name,"Pending","1","0","",name,"Off"};
            for (int col=0; col<cells.size(); ++col) window.m_tableQueue->setItem(row,col,new QTableWidgetItem(cells[col]));
        }
        window.m_activeTranscodeQueue=paths;
        window.startProcessing();
        QVERIFY(window.m_menuOptions->isEnabled()); QVERIFY(window.m_actConcurrency->isEnabled());
        QVERIFY(!window.m_actNvenc->isEnabled());
        QCOMPARE(window.m_workers.size(),1);
        auto chooseLimit=[&](int limit) {
            QTimer::singleShot(0,&window,[limit] {
                auto *dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
                Q_ASSERT(dialog);
                if (dialog) { dialog->setIntValue(limit); dialog->accept(); }
            });
            window.m_actConcurrency->trigger();
        };
        chooseLimit(3);
        QCOMPARE(window.m_workers.size(),3); QCOMPARE(window.m_pendingQueue.size(),1);
        const auto active=window.m_workers;
        chooseLimit(1);
        QCOMPARE(window.m_workers,active); // Lowering does not cancel or replace active jobs.
        window.startNextQueueJob(); QCOMPARE(window.m_workers.size(),3); // Enforce at dispatch too.
        QVERIFY(put(QDir(gates).filePath("input0.mp4"),"go"));
        QTRY_COMPARE_WITH_TIMEOUT(window.m_workers.size(),2,10000); QCOMPARE(window.m_pendingQueue.size(),1);
        QVERIFY(put(QDir(gates).filePath("input1.mp4"),"go"));
        QTRY_COMPARE_WITH_TIMEOUT(window.m_workers.size(),1,10000); QCOMPARE(window.m_pendingQueue.size(),1);
        QVERIFY(put(QDir(gates).filePath("input2.mp4"),"go"));
        QTRY_VERIFY_WITH_TIMEOUT(window.m_pendingQueue.isEmpty(),10000);
        QCOMPARE(window.m_workers.size(),1); QCOMPARE(window.m_workers.first()->property("filepath").toString(),paths.last());
        for (int row=0; row<3; ++row) QCOMPARE(window.m_tableQueue->item(row,1)->text(),QString("Completed"));
        window.stopProcessing(); window.setConcurrencyLimit(4);
        QTRY_VERIFY_WITH_TIMEOUT(window.m_workers.isEmpty(),10000);
        QVERIFY(!window.m_isQueueRunning); QCOMPARE(window.m_tableQueue->item(3,1)->text(),QString("Cancelled"));
        QVERIFY(window.m_actNvenc->isEnabled());
    }
    void parallelAvisynthJobs() {
        if (m_ffmpeg.isEmpty() || m_ffprobe.isEmpty()) QSKIP("FFmpeg/FFprobe required");
        const QString installation=AvisynthPipeline::testInstallation(m_ffmpeg,m_ffprobe,nullptr);
        if (!installation.isEmpty()) {
            if (qEnvironmentVariableIsSet("HEVC_TEST_REQUIRE_AVISYNTH")) QFAIL(qPrintable(installation));
            QSKIP(qPrintable(installation));
        }
        QTemporaryDir dir;
        const QString base=dir.filePath("base.mp4"), dbPath=dir.filePath("history.db"); QVERIFY(source(base));
        MainWindow window(nullptr,dbPath); setupWindow(window,dir.path(),dbPath);
        window.m_tableQueue->setRowCount(3);
        QStringList paths;
        for (int row=0; row<3; ++row) {
            const QString name=QString("filtered%1.mp4").arg(row), path=dir.filePath(name);
            QVERIFY(QFile::copy(base,path)); paths << path;
            const QStringList cells{name,"Pending","1","0","",name,"Off"};
            for (int col=0; col<cells.size(); ++col) window.m_tableQueue->setItem(row,col,new QTableWidgetItem(cells[col]));
        }
        window.assignAvisynth(paths,{"Crop","Crop(0, 8, 0, -8)"},false);
        window.startProcessing(); QCOMPARE(window.m_workers.size(),1);
        window.setConcurrencyLimit(3); QCOMPARE(window.m_workers.size(),3); QVERIFY(window.m_pendingQueue.isEmpty());
        for (auto *worker : window.m_workers) QVERIFY(worker->m_settings.value("avs_enabled").toBool());
        window.setConcurrencyLimit(1); QCOMPARE(window.m_workers.size(),3);
        window.stopProcessing();
        QTRY_VERIFY_WITH_TIMEOUT(window.m_workers.isEmpty(),15000);

        // Run actual filtered jobs concurrently with isolated index files and
        // a shared history database, then verify all output crops and backups.
        auto config=settings(); config["avs_enabled"]=true; config["avs_filters"]="Crop(0, 8, 0, -8)";
        config["avs_cache_root"]=dir.filePath("cache");
        std::vector<std::unique_ptr<TranscodeWorker>> workers;
        std::vector<std::unique_ptr<QSignalSpy>> results;
        for (const auto &path : paths) {
            workers.push_back(std::make_unique<TranscodeWorker>(QStringList{path},dir.path(),dbPath,config));
            results.push_back(std::make_unique<QSignalSpy>(workers.back().get(),&TranscodeWorker::fileDoneSignal));
            workers.back()->start();
        }
        for (auto &worker : workers) QVERIFY(worker->wait(60000));
        QCoreApplication::processEvents();
        for (int i=0; i<paths.size(); ++i) {
            QCOMPARE(results[i]->size(),1); QCOMPARE(results[i]->first().at(1).toString(),QString("Completed"));
            QCOMPARE(probeMetadata(outputPathFor(paths[i]),m_ffprobe).height,80);
            QCOMPARE(read(dir.filePath(".Trash/"+QFileInfo(paths[i]).fileName())),read(base));
        }
    }
    void scanAndGuiCancellationResume() {
        if(m_ffmpeg.isEmpty()||m_ffprobe.isEmpty()) QSKIP("FFmpeg/FFprobe are required for media integration tests");
        QTemporaryDir dir;
        const QString input=dir.filePath("input.mp4"), dbPath=dir.filePath("history.db");
        QVERIFY(source(input,20));
        QVERIFY(put(dir.filePath("old.tmp_out.mkv"),"partial"));
        QVERIFY(put(dir.filePath(".hevc-shrinker-abcd.mkv"),"partial"));
#ifndef Q_OS_WIN
        QVERIFY(QFile::link(dir.path(),dir.filePath("loop")));
#endif
        MainWindow window(nullptr,dbPath);setupWindow(window,dir.path(),dbPath);
        window.scanDirectory(); auto *scan=window.m_scanWorker; QVERIFY(scan);
        QVERIFY(!window.m_actNvenc->isEnabled());
        window.scanDirectory(); QCOMPARE(window.m_scanWorker,scan); // Reentry is rejected.
        QTRY_VERIFY_WITH_TIMEOUT(window.m_scanWorker==nullptr,15000);
        QVERIFY(window.m_actNvenc->isEnabled());
        QCOMPARE(window.m_tableQueue->rowCount(),1);
        window.assignAvisynth({input},{"Batch crop","Crop(0, 8, 0, -8)"},false);
        window.startProcessing(); window.stopProcessing();
        QTRY_VERIFY_WITH_TIMEOUT(window.m_workers.isEmpty(),15000);
        QCOMPARE(window.m_tableQueue->item(0,1)->text(),QString("Cancelled"));
        QCOMPARE(window.m_activeTranscodeQueue,QStringList{input});
        QVERIFY(window.m_btnStart->isEnabled());
        QVERIFY(QFileInfo::exists(input));
        QCOMPARE(window.m_filterAssignments.size(),1); // Stop/restart is the same batch.
        // Both conflicting inputs are rejected before dispatch.
        QVERIFY(put(dir.filePath("input.mkv"),read(input)));
        window.scanDirectory();QTRY_VERIFY_WITH_TIMEOUT(window.m_scanWorker==nullptr,15000);
        QVERIFY(window.m_filterAssignments.isEmpty()); // Replacing the queue resets activation.
        QVERIFY(window.m_activeTranscodeQueue.isEmpty());
    }
};
QTEST_MAIN(RegressionTests)
#include "regression_tests.moc"
