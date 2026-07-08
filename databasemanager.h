#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QString>
#include <QSqlDatabase>

struct ProcessedFileInfo {
    bool found = false;
    qint64 originalSize = 0;
    qint64 compressedSize = 0;
    QString hash;
};

class DatabaseManager {
public:
    DatabaseManager(const QString &dbPath = "");
    ~DatabaseManager();

    bool init();
    
    // Retrieve file info. If missing size details, we backfill them from disk.
    ProcessedFileInfo getProcessedFileInfo(const QString &filepath);
    
    int getCachedCompliance(const QString &filepath, qint64 fileSize, qint64 lastModified);
    void setCachedCompliance(const QString &filepath, qint64 fileSize, qint64 lastModified, int isCompliant);
    
    bool recordProcessedFile(const QString &filepath, qint64 originalSize, qint64 compressedSize, const QString &hash);
    
    // Get workspace-scoped savings
    bool getSpaceSavings(const QString &rootDir, double &totalOriginalMb, double &totalCompressedMb, double &totalSavedMb, double &savingsPct);

    // Migration and reset database methods
    void migrateLocalDatabase(const QString &localDbPath, const QString &rootDir);
    bool clearScanCache(const QString &rootDir);
    bool clearProcessedFiles(const QString &rootDir);

private:
    QString m_dbPath;
    QString m_connectionName;
    
    QSqlDatabase db();
    void closeDb();
};

#endif // DATABASEMANAGER_H
