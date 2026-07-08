#include "databasemanager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QFileInfo>
#include <QDir>
#include <QThread>
#include <QDebug>
#include <QStandardPaths>

static QString getGlobalDbPath() {
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dataDir);
    return QDir(dataDir).filePath("hevc_shrinker.db");
}

DatabaseManager::DatabaseManager(const QString &dbPath)
{
    Q_UNUSED(dbPath);
    m_dbPath = getGlobalDbPath();
    // Generate a unique connection name per thread to ensure thread safety
    m_connectionName = QString("conn_%1").arg(QString::number((quintptr)QThread::currentThreadId()));
    init(); // Auto-initialize tables immediately
}

DatabaseManager::~DatabaseManager()
{
    closeDb();
}

void DatabaseManager::closeDb()
{
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            QSqlDatabase d = QSqlDatabase::database(m_connectionName);
            if (d.isOpen()) {
                d.close();
            }
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

QSqlDatabase DatabaseManager::db()
{
    if (QSqlDatabase::contains(m_connectionName)) {
        QSqlDatabase existingDb = QSqlDatabase::database(m_connectionName);
        if (!existingDb.isOpen()) {
            existingDb.open();
        }
        return existingDb;
    }
    
    QSqlDatabase newDb = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    QString uriPath = m_dbPath;
    uriPath.replace("\\", "/");
    QString uriDbPath = QString("file:%1?nolock=1").arg(uriPath);
    newDb.setDatabaseName(uriDbPath);
    if (!newDb.open()) {
        qWarning() << "Failed to open database:" << newDb.lastError().text();
    }
    return newDb;
}

bool DatabaseManager::init()
{
    bool ok = false;
    {
        QSqlDatabase d = db();
        if (d.isOpen()) {
            QSqlQuery query(d);
            ok = query.exec(
                "CREATE TABLE IF NOT EXISTS processed_files ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                "filepath TEXT UNIQUE, "
                "original_size INTEGER, "
                "compressed_size INTEGER, "
                "hash TEXT, "
                "processed_at DATETIME DEFAULT CURRENT_TIMESTAMP"
                ")"
            );
            if (!ok) {
                qWarning() << "Failed to create processed_files table:" << query.lastError().text();
            }
            
            bool okCache = query.exec(
                "CREATE TABLE IF NOT EXISTS scan_cache ("
                "filepath TEXT PRIMARY KEY, "
                "file_size INTEGER, "
                "last_modified INTEGER, "
                "is_compliant INTEGER"
                ")"
            );
            if (!okCache) {
                qWarning() << "Failed to create scan_cache table:" << query.lastError().text();
            }
            
            ok = ok && okCache;
        }
    }
    closeDb();
    return ok;
}

ProcessedFileInfo DatabaseManager::getProcessedFileInfo(const QString &filepath)
{
    ProcessedFileInfo info;
    
    {
        QSqlDatabase d = db();
        if (d.isOpen()) {
            QString absPath = QFileInfo(filepath).absoluteFilePath();
            absPath.replace("\\", "/");

            QSqlQuery query(d);
            query.prepare(
                "SELECT original_size, compressed_size, hash, filepath FROM processed_files "
                "WHERE filepath = :filepath"
            );
            query.bindValue(":filepath", absPath);
            
            if (query.exec() && query.next()) {
                info.found = true;
                
                QVariant origVal = query.value(0);
                QVariant compVal = query.value(1);
                info.hash = query.value(2).toString();
                QString matchedPath = query.value(3).toString();
                
                // Auto-repair/backfill legacy records if sizes are null
                if (origVal.isNull() || compVal.isNull()) {
                    QFileInfo fileInfo(filepath);
                    qint64 currentSize = fileInfo.exists() ? fileInfo.size() : 0;
                    info.originalSize = currentSize;
                    info.compressedSize = currentSize;
                    
                    // Update the record with backfilled size
                    QSqlQuery updateQuery(d);
                    updateQuery.prepare("UPDATE processed_files SET original_size = :orig, compressed_size = :comp WHERE filepath = :filepath");
                    updateQuery.bindValue(":orig", currentSize);
                    updateQuery.bindValue(":comp", currentSize);
                    updateQuery.bindValue(":filepath", matchedPath);
                    updateQuery.exec();
                } else {
                    info.originalSize = origVal.toLongLong();
                    info.compressedSize = compVal.toLongLong();
                }
            }
        }
    }
    closeDb();
    return info;
}

bool DatabaseManager::recordProcessedFile(const QString &filepath, qint64 originalSize, qint64 compressedSize, const QString &hash)
{
    bool ok = false;
    {
        QSqlDatabase d = db();
        if (d.isOpen()) {
            QString absPath = QFileInfo(filepath).absoluteFilePath();
            absPath.replace("\\", "/");

            QSqlQuery query(d);
            query.prepare(
                "INSERT OR REPLACE INTO processed_files (filepath, original_size, compressed_size, hash, processed_at) "
                "VALUES (:filepath, :orig, :comp, :hash, CURRENT_TIMESTAMP)"
            );
            query.bindValue(":filepath", absPath);
            query.bindValue(":orig", originalSize);
            query.bindValue(":comp", compressedSize);
            query.bindValue(":hash", hash);

            ok = query.exec();
            if (!ok) {
                qWarning() << "Failed to insert record:" << query.lastError().text();
            }
        }
    }
    closeDb();
    return ok;
}


bool DatabaseManager::getSpaceSavings(const QString &rootDir, double &totalOriginalMb, double &totalCompressedMb, double &totalSavedMb, double &savingsPct)
{
    totalOriginalMb = 0.0;
    totalCompressedMb = 0.0;
    totalSavedMb = 0.0;
    savingsPct = 0.0;

    bool success = false;
    {
        QSqlDatabase d = db();
        if (d.isOpen()) {
            QString prefix = QDir(rootDir).absolutePath();
            prefix.replace("\\", "/");
            if (!prefix.endsWith("/")) {
                prefix += "/";
            }

            QSqlQuery query(d);
            query.prepare(
                "SELECT SUM(original_size), SUM(compressed_size) FROM processed_files "
                "WHERE filepath LIKE :prefix || '%' OR filepath = :exact"
            );
            query.bindValue(":prefix", prefix);
            query.bindValue(":exact", QDir(rootDir).absolutePath().replace("\\", "/"));

            if (query.exec() && query.next()) {
                qint64 sumOrig = query.value(0).toLongLong();
                qint64 sumComp = query.value(1).toLongLong();
                
                if (sumOrig > 0) {
                    totalOriginalMb = static_cast<double>(sumOrig) / (1024.0 * 1024.0);
                    totalCompressedMb = static_cast<double>(sumComp) / (1024.0 * 1024.0);
                    totalSavedMb = totalOriginalMb - totalCompressedMb;
                    savingsPct = (totalSavedMb / totalOriginalMb) * 100.0;
                }
                success = true;
            }
        }
    }
    closeDb();
    return success;
}

int DatabaseManager::getCachedCompliance(const QString &filepath, qint64 fileSize, qint64 lastModified)
{
    int result = -1;
    {
        QSqlDatabase d = db();
        if (d.isOpen()) {
            QString absPath = QFileInfo(filepath).absoluteFilePath();
            absPath.replace("\\", "/");

            QSqlQuery query(d);
            query.prepare(
                "SELECT is_compliant FROM scan_cache "
                "WHERE filepath = :filepath "
                "  AND file_size = :file_size "
                "  AND last_modified = :last_modified"
            );
            query.bindValue(":filepath", absPath);
            query.bindValue(":file_size", fileSize);
            query.bindValue(":last_modified", lastModified);

            if (query.exec() && query.next()) {
                result = query.value(0).toInt();
            }
        }
    }
    closeDb();
    return result;
}

void DatabaseManager::setCachedCompliance(const QString &filepath, qint64 fileSize, qint64 lastModified, int isCompliant)
{
    {
        QSqlDatabase d = db();
        if (d.isOpen()) {
            QString absPath = QFileInfo(filepath).absoluteFilePath();
            absPath.replace("\\", "/");

            QSqlQuery query(d);
            query.prepare(
                "INSERT OR REPLACE INTO scan_cache (filepath, file_size, last_modified, is_compliant) "
                "VALUES (:filepath, :file_size, :last_modified, :is_compliant)"
            );
            query.bindValue(":filepath", absPath);
            query.bindValue(":file_size", fileSize);
            query.bindValue(":last_modified", lastModified);
            query.bindValue(":is_compliant", isCompliant);

            if (!query.exec()) {
                qWarning() << "Failed to set cached compliance:" << query.lastError().text();
            }
        }
    }
    closeDb();
}

void DatabaseManager::migrateLocalDatabase(const QString &localDbPath, const QString &rootDir)
{
    if (!QFile::exists(localDbPath)) return;

    qDebug() << "Migrating local database:" << localDbPath;
    
    // Open the local database
    QString localConnName = QString("local_conn_%1").arg(QString::number((quintptr)QThread::currentThreadId()));
    QSqlDatabase localDb = QSqlDatabase::addDatabase("QSQLITE", localConnName);
    localDb.setDatabaseName(localDbPath);
    
    if (localDb.open()) {
        QSqlQuery query(localDb);
        if (query.exec("SELECT filepath, original_size, compressed_size, hash, processed_at FROM processed_files")) {
            QSqlDatabase mainDb = db();
            if (mainDb.isOpen()) {
                QSqlQuery insertQuery(mainDb);
                while (query.next()) {
                    QString oldPath = query.value(0).toString();
                    qint64 origSize = query.value(1).toLongLong();
                    qint64 compSize = query.value(2).toLongLong();
                    QString hash = query.value(3).toString();
                    QString processedAt = query.value(4).toString();
                    
                    // Resolve old path to absolute path
                    QString absPath = oldPath;
                    if (QDir::isRelativePath(oldPath)) {
                        absPath = QDir(rootDir).absoluteFilePath(oldPath);
                    }
                    absPath = QDir::cleanPath(absPath);
                    absPath.replace("\\", "/");
                    
                    insertQuery.prepare(
                        "INSERT OR IGNORE INTO processed_files (filepath, original_size, compressed_size, hash, processed_at) "
                        "VALUES (:filepath, :original_size, :compressed_size, :hash, :processed_at)"
                    );
                    insertQuery.bindValue(":filepath", absPath);
                    insertQuery.bindValue(":original_size", origSize);
                    insertQuery.bindValue(":compressed_size", compSize);
                    insertQuery.bindValue(":hash", hash);
                    insertQuery.bindValue(":processed_at", processedAt);
                    insertQuery.exec();
                }
            }
        }
        localDb.close();
    }
    QSqlDatabase::removeDatabase(localConnName);
    
    // Rename local database to .migrated so we don't migrate it again
    QString migratedPath = localDbPath + ".migrated";
    if (QFile::exists(migratedPath)) {
        QFile::remove(migratedPath);
    }
    QFile::rename(localDbPath, migratedPath);
}

bool DatabaseManager::clearScanCache(const QString &rootDir)
{
    bool ok = false;
    {
        QSqlDatabase d = db();
        if (d.isOpen()) {
            QString prefix = QDir(rootDir).absolutePath();
            prefix.replace("\\", "/");
            if (!prefix.endsWith("/")) {
                prefix += "/";
            }

            QSqlQuery query(d);
            query.prepare("DELETE FROM scan_cache WHERE filepath LIKE :prefix || '%' OR filepath = :exact");
            query.bindValue(":prefix", prefix);
            query.bindValue(":exact", QDir(rootDir).absolutePath().replace("\\", "/"));
            ok = query.exec();
        }
    }
    closeDb();
    return ok;
}

bool DatabaseManager::clearProcessedFiles(const QString &rootDir)
{
    bool ok = false;
    {
        QSqlDatabase d = db();
        if (d.isOpen()) {
            QString prefix = QDir(rootDir).absolutePath();
            prefix.replace("\\", "/");
            if (!prefix.endsWith("/")) {
                prefix += "/";
            }

            QSqlQuery query(d);
            query.prepare("DELETE FROM processed_files WHERE filepath LIKE :prefix || '%' OR filepath = :exact");
            query.bindValue(":prefix", prefix);
            query.bindValue(":exact", QDir(rootDir).absolutePath().replace("\\", "/"));
            ok = query.exec();
        }
    }
    closeDb();
    return ok;
}
