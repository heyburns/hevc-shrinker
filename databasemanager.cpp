#include "databasemanager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QFileInfo>
#include <QDir>
#include <QThread>
#include <QDebug>

DatabaseManager::DatabaseManager(const QString &dbPath)
    : m_dbPath(dbPath)
{
    // Generate a unique connection name per thread to ensure thread safety
    m_connectionName = QString("conn_%1").arg(QString::number((quintptr)QThread::currentThreadId()));
}

DatabaseManager::~DatabaseManager()
{
    // Remove the database connection when this instance is destroyed
    if (QSqlDatabase::contains(m_connectionName)) {
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
    newDb.setDatabaseName(m_dbPath);
    if (!newDb.open()) {
        qWarning() << "Failed to open database:" << newDb.lastError().text();
    }
    return newDb;
}

bool DatabaseManager::init()
{
    QSqlDatabase d = db();
    if (!d.isOpen()) return false;

    QSqlQuery query(d);
    bool ok = query.exec(
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
        qWarning() << "Failed to create table:" << query.lastError().text();
    }
    return ok;
}

ProcessedFileInfo DatabaseManager::getProcessedFileInfo(const QString &filepath)
{
    ProcessedFileInfo info;
    
    // Defer file creation: if file does not exist, return not found immediately
    if (!QFile::exists(m_dbPath)) {
        return info;
    }

    QSqlDatabase d = db();
    if (!d.isOpen()) return info;

    QString absPath = QFileInfo(filepath).absoluteFilePath();
    QFileInfo dbFileInfo(m_dbPath);
    QDir rootDir = dbFileInfo.dir();
    QString relpath = rootDir.relativeFilePath(absPath);
    relpath.replace("\\", "/");

    QSqlQuery query(d);
    query.prepare(
        "SELECT original_size, compressed_size, hash, filepath FROM processed_files "
        "WHERE filepath = :filepath "
        "   OR filepath = :relpath "
        "   OR filepath LIKE '%' || :relpath"
    );
    query.bindValue(":filepath", absPath);
    query.bindValue(":relpath", relpath);
    
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
    return info;
}

bool DatabaseManager::recordProcessedFile(const QString &filepath, qint64 originalSize, qint64 compressedSize, const QString &hash)
{
    // Auto-initialize SQLite schema if database file doesn't exist or is 0 bytes
    QFileInfo dbInfo(m_dbPath);
    if (!dbInfo.exists() || dbInfo.size() == 0) {
        init();
    }

    QSqlDatabase d = db();
    if (!d.isOpen()) return false;

    QString absPath = QFileInfo(filepath).absoluteFilePath();
    QFileInfo dbFileInfo(m_dbPath);
    QDir rootDir = dbFileInfo.dir();
    QString relpath = rootDir.relativeFilePath(absPath);
    relpath.replace("\\", "/");

    QSqlQuery query(d);
    query.prepare(
        "INSERT OR REPLACE INTO processed_files (filepath, original_size, compressed_size, hash, processed_at) "
        "VALUES (:filepath, :orig, :comp, :hash, CURRENT_TIMESTAMP)"
    );
    query.bindValue(":filepath", relpath);
    query.bindValue(":orig", originalSize);
    query.bindValue(":comp", compressedSize);
    query.bindValue(":hash", hash);

    bool ok = query.exec();
    if (!ok) {
        qWarning() << "Failed to insert record:" << query.lastError().text();
    }
    return ok;
}

bool DatabaseManager::getSpaceSavings(double &totalOriginalMb, double &totalCompressedMb, double &totalSavedMb, double &savingsPct)
{
    totalOriginalMb = 0.0;
    totalCompressedMb = 0.0;
    totalSavedMb = 0.0;
    savingsPct = 0.0;

    // Defer file creation: if file does not exist, return defaults immediately
    if (!QFile::exists(m_dbPath)) {
        return false;
    }

    QSqlDatabase d = db();
    if (!d.isOpen()) return false;

    QSqlQuery query(d);
    bool ok = query.exec("SELECT SUM(original_size), SUM(compressed_size) FROM processed_files");
    if (ok && query.next()) {
        qint64 sumOrig = query.value(0).toLongLong();
        qint64 sumComp = query.value(1).toLongLong();
        
        if (sumOrig > 0) {
            totalOriginalMb = static_cast<double>(sumOrig) / (1024.0 * 1024.0);
            totalCompressedMb = static_cast<double>(sumComp) / (1024.0 * 1024.0);
            totalSavedMb = totalOriginalMb - totalCompressedMb;
            savingsPct = (totalSavedMb / totalOriginalMb) * 100.0;
        }
        return true;
    }
    return false;
}
