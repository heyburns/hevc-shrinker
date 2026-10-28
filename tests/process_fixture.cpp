#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    const QString trace = qEnvironmentVariable("HEVC_TEST_COMMANDS");
    if (!trace.isEmpty()) {
        QFile output(trace);
        if (output.open(QIODevice::WriteOnly | QIODevice::Append))
            output.write(QJsonDocument(QJsonArray::fromStringList(args.mid(1))).toJson(QJsonDocument::Compact) + '\n');
    }
    if (args.contains("--sleep")) {
        QThread::msleep(args.value(2).toULong());
        return 0;
    }
    if (args.contains("--progress-script")) {
        QFile script(args.value(args.indexOf("--progress-script") + 1));
        QFile output(args.last());
        if (!script.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) return 20;
        const auto chunks = QJsonDocument::fromJson(script.readAll()).array();
        for (const auto &value : chunks) {
            const auto chunk = value.toObject();
            if (chunk.contains("size") && !output.resize(chunk.value("size").toInt())) return 21;
            output.flush();
            const auto text = chunk.value("text").toString().toUtf8();
            if (std::fwrite(text.constData(), 1, size_t(text.size()), stdout) != size_t(text.size())) return 22;
            std::fflush(stdout);
            QThread::msleep(chunk.value("sleep_ms").toInt(120));
        }
        return 0;
    }
    if (args.contains("-encoders")) {
        if (!qEnvironmentVariableIsSet("HEVC_TEST_HIDE_X265")) std::puts(" V..... libx265");
        if (qEnvironmentVariableIsSet("HEVC_TEST_NVENC_SUPPORT")) std::puts(" V..... hevc_nvenc");
        std::puts(" A..... aac");
        return 0;
    }
    if (args.contains("lavfi") && args.contains("hevc_nvenc")) {
        if (qEnvironmentVariableIsSet("HEVC_TEST_NVENC_PROBE_FAIL")) {
            std::fprintf(stderr, "Cannot load NVIDIA driver; NVENC initialization failed\n");
            return 19;
        }
        return 0;
    }
    if (args.contains("idet")) {
        std::fprintf(stderr, "Multi frame detection: TFF: 0 BFF: 0 Progressive: 360\n");
        return 0;
    }
    const QString fixture = qEnvironmentVariable("HEVC_TEST_ENCODED_FILE");
    const QString gates = qEnvironmentVariable("HEVC_TEST_ENCODE_GATES");
    if (!gates.isEmpty() && args.contains("-progress")) {
        const QString input = args.value(args.indexOf("-i") + 1);
        const QString gate = QDir(gates).filePath(QFileInfo(input).fileName());
        while (!QFileInfo::exists(gate)) QThread::msleep(20);
    }
    const int delay = qEnvironmentVariableIntValue("HEVC_TEST_ENCODE_DELAY_MS");
    if (delay > 0) QThread::msleep(delay);
    if (fixture.isEmpty()) return 7;
    // Emulate a successful encoder producing an independently prepared file.
    QFile::remove(args.last());
    return QFile::copy(fixture, args.last()) ? 0 : 8;
}
