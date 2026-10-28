#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QSqlDatabase>
#include <QString>

struct ProcessedFileInfo {
    bool found = false;
    bool current = false;
    qint64 originalSize = 0, compressedSize = 0;
    QString hash, settingsKey, outcome;
};

// Each instance owns a connection, and must be used/destroyed on its creating thread.
class DatabaseManager {
public:
    explicit DatabaseManager(const QString &dbPath = {});
    ~DatabaseManager();
    DatabaseManager(const DatabaseManager &) = delete;
    DatabaseManager &operator=(const DatabaseManager &) = delete;

    bool init();
    bool isReady() const { return m_ready; }
    QString path() const { return m_dbPath; }
    QString lastError() const { return m_error; }
    ProcessedFileInfo getProcessedFileInfo(const QString &filepath);
    QByteArray cachedMetadata(const QString &filepath, qint64 size, qint64 modified);
    bool cacheMetadata(const QString &filepath, qint64 size, qint64 modified, const QByteArray &json);
    bool recordProcessedFile(const QString &filepath, qint64 originalSize, qint64 compressedSize,
        const QString &hash, const QString &settingsKey = {}, const QString &outcome = "Completed");
    bool getSpaceSavings(const QString &rootDir, double &original, double &compressed, double &saved, double &percent);
    bool migrateLocalDatabase(const QString &localDbPath, const QString &rootDir);
    bool clearScanCache(const QString &rootDir);
    bool clearProcessedFiles(const QString &rootDir);

private:
    friend class RegressionTests;
    QString m_dbPath, m_connectionName, m_error;
    bool m_ready = false;
    QSqlDatabase db();
    bool clearTable(const QString &table, const QString &rootDir);
};
#endif
