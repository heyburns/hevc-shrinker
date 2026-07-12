#include "databasemanager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QFileInfo>
#include <QDir>
#include <QThread>
#include <QDebug>
#include <QStandardPaths>

// Helper function to resolve the absolute path to the global database file.
// The database is stored inside the user's system profile folder (e.g., ~/.local/share/hevc_shrinker/ on Linux
// or AppData/Local/hevc_shrinker/ on Windows). This keeps target video directories 100% clean.
static QString getGlobalDbPath() {
    // Get the standard OS directory path reserved for application-specific data files
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    // Create the directory hierarchy if it doesn't already exist on disk
    QDir().mkpath(dataDir);
    // Return the absolute path to the SQLite database file
    return QDir(dataDir).filePath("hevc_shrinker.db");
}

// Constructor: Initializes the connection variables and triggers table initialization.
DatabaseManager::DatabaseManager(const QString &dbPath)
{
    Q_UNUSED(dbPath); // Suppress compiler warnings for unused parameter
    m_dbPath = getGlobalDbPath(); // Locate the global database storage file
    
    // Generate a unique database connection identifier per-thread (e.g., "conn_140239012").
    // SQLite requires separate connection handles per thread to prevent cross-thread corruption/race conditions.
    m_connectionName = QString("conn_%1").arg(QString::number((quintptr)QThread::currentThreadId()));
    
    init(); // Run table setup queries immediately
}

// Destructor: Safely shuts down the connection.
DatabaseManager::~DatabaseManager()
{
    closeDb();
}

// Closes and unregisters the SQLite connection from Qt's connection pool.
void DatabaseManager::closeDb()
{
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            // Retrieve the thread-local database connection handle
            QSqlDatabase d = QSqlDatabase::database(m_connectionName);
            if (d.isOpen()) {
                d.close(); // Close the SQL file handle
            }
        }
        // Remove the connection name from Qt's global connection registry
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

// Connects to the database and returns a connection handle.
// Reuses the connection if it's already open to avoid opening/closing thrashing.
QSqlDatabase DatabaseManager::db()
{
    // If a connection with this thread's name already exists in the registry, return it
    if (QSqlDatabase::contains(m_connectionName)) {
        QSqlDatabase existingDb = QSqlDatabase::database(m_connectionName);
        if (!existingDb.isOpen()) {
            existingDb.open();
        }
        return existingDb;
    }
    
    // Otherwise, register a new SQLite connection handle
    QSqlDatabase newDb = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    
    // Format the database connection as a standard SQLite URI with locks disabled (`nolock=1`).
    // Replacing backslashes with forward slashes is required by the SQLite URI parser.
    // Setting `nolock=1` bypasses SQLite's OS file-locking syscalls (lockf/LockFileEx),
    // which prevents "database is locked" errors when the database runs on network CIFS/SMB mounts.
    QString uriPath = m_dbPath;
    uriPath.replace("\\", "/");
    QString uriDbPath = QString("file:%1?nolock=1").arg(uriPath);
    newDb.setDatabaseName(uriDbPath);
    
    if (!newDb.open()) {
        qWarning() << "Failed to open database:" << newDb.lastError().text();
    }
    return newDb;
}

// Creates the required database tables if they do not exist.
bool DatabaseManager::init()
{
    bool ok = false;
    {
        QSqlDatabase d = db(); // Open/fetch thread connection
        if (d.isOpen()) {
            QSqlQuery query(d);
            
            // Table 1: 'processed_files' - stores sizing history and hashes for shrunk videos.
            // This represents the scoreboard datastore.
            ok = query.exec(
                "CREATE TABLE IF NOT EXISTS processed_files ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                "filepath TEXT UNIQUE, " // Absolute path to the finished video file
                "original_size INTEGER, " // File size before compression (bytes)
                "compressed_size INTEGER, " // File size after compression (bytes)
                "hash TEXT, " // 10MB chunk content signature
                "processed_at DATETIME DEFAULT CURRENT_TIMESTAMP"
                ")"
            );
            if (!ok) {
                qWarning() << "Failed to create processed_files table:" << query.lastError().text();
            }
            
            // Table 2: 'scan_cache' - caches compliance scans (skip/process outcomes) per file.
            // This matches the size + modification time of files to determine if they need a re-scan.
            bool okCache = query.exec(
                "CREATE TABLE IF NOT EXISTS scan_cache ("
                "filepath TEXT PRIMARY KEY, " // Absolute path to the scanned video file
                "file_size INTEGER, " // File size in bytes (used to detect file modifications)
                "last_modified INTEGER, " // UNIX timestamp of modification date
                "is_compliant INTEGER" // Cache result: 1 = HEVC+AAC (skip), 0 = needs transcode
                ")"
            );
            if (!okCache) {
                qWarning() << "Failed to create scan_cache table:" << query.lastError().text();
            }
            
            ok = ok && okCache; // Success if both tables are initialized
        }
    }
    closeDb();
    return ok;
}

// Retrieves processed file statistics from the database (original/compressed sizes, file hash).
// If a legacy record is found with empty size columns, this method automatically backfills
// the size from the physical file on disk to maintain database consistency.
ProcessedFileInfo DatabaseManager::getProcessedFileInfo(const QString &filepath)
{
    ProcessedFileInfo info;
    
    {
        QSqlDatabase d = db();
        if (d.isOpen()) {
            // Normalize path separators to forward slashes to ensure cross-platform compatibility
            QString absPath = QFileInfo(filepath).absoluteFilePath();
            absPath.replace("\\", "/");

            QSqlQuery query(d);
            // Search database by absolute file path
            query.prepare(
                "SELECT original_size, compressed_size, hash, filepath FROM processed_files "
                "WHERE filepath = :filepath"
            );
            query.bindValue(":filepath", absPath);
            
            if (query.exec() && query.next()) {
                info.found = true; // Record exists
                
                QVariant origVal = query.value(0);
                QVariant compVal = query.value(1);
                info.hash = query.value(2).toString();
                QString matchedPath = query.value(3).toString();
                
                // Auto-repair/backfill legacy records if sizes are null (from older versions of the app)
                if (origVal.isNull() || compVal.isNull()) {
                    QFileInfo fileInfo(filepath);
                    qint64 currentSize = fileInfo.exists() ? fileInfo.size() : 0;
                    info.originalSize = currentSize;
                    info.compressedSize = currentSize;
                    
                    // Update the record with backfilled size details to fix the record permanently
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
    return info;
}

// Records sizing history and hash to the database after a successful transcode.
bool DatabaseManager::recordProcessedFile(const QString &filepath, qint64 originalSize, qint64 compressedSize, const QString &hash)
{
    bool ok = false;
    {
        QSqlDatabase d = db();
        if (d.isOpen()) {
            // Normalize absolute path separators to forward slashes
            QString absPath = QFileInfo(filepath).absoluteFilePath();
            absPath.replace("\\", "/");

            QSqlQuery query(d);
            // Insert the record, or replace it if a record for this path already exists
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
    return ok;
}

// Queries the database to retrieve scoreboard statistics (space savings)
// scoped specifically to video files residing under the active workspace folder.
bool DatabaseManager::getSpaceSavings(const QString &rootDir, double &totalOriginalMb, double &totalCompressedMb, double &totalSavedMb, double &savingsPct)
{
    // Initialize default values to 0
    totalOriginalMb = 0.0;
    totalCompressedMb = 0.0;
    totalSavedMb = 0.0;
    savingsPct = 0.0;

    bool success = false;
    {
        QSqlDatabase d = db();
        if (d.isOpen()) {
            // Get the absolute root workspace path and normalize it
            QString prefix = QDir(rootDir).absolutePath();
            prefix.replace("\\", "/");
            if (!prefix.endsWith("/")) {
                prefix += "/"; // Append trailing slash to match subdirectories cleanly
            }

            QSqlQuery query(d);
            // Query aggregates (sums) of original and compressed sizes.
            // Filter by prefix matches using the LIKE operator to ensure we only sum
            // files belonging to the active scanned folder directory.
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
                    // Convert sizes from bytes to Megabytes (MB)
                    totalOriginalMb = static_cast<double>(sumOrig) / (1024.0 * 1024.0);
                    totalCompressedMb = static_cast<double>(sumComp) / (1024.0 * 1024.0);
                    // Sizing math
                    totalSavedMb = totalOriginalMb - totalCompressedMb;
                    savingsPct = (totalSavedMb / totalOriginalMb) * 100.0;
                }
                success = true;
            }
        }
    }
    return success;
}

// Retrieves compliance status from cache.
// Checks if the file size and modification time match, ensuring cache validation.
int DatabaseManager::getCachedCompliance(const QString &filepath, qint64 fileSize, qint64 lastModified)
{
    int result = -1; // Default to -1 (cache miss / unknown)
    {
        QSqlDatabase d = db();
        if (d.isOpen()) {
            // Normalize path separators
            QString absPath = QFileInfo(filepath).absoluteFilePath();
            absPath.replace("\\", "/");

            QSqlQuery query(d);
            // Fetch cached status only if filepath, size, and mod-time all match
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
                result = query.value(0).toInt(); // Cache hit! Returns 1 (skip) or 0 (needs transcode)
            }
        }
    }
    return result;
}

// Writes scan outcomes to compliance cache.
void DatabaseManager::setCachedCompliance(const QString &filepath, qint64 fileSize, qint64 lastModified, int isCompliant)
{
    {
        QSqlDatabase d = db();
        if (d.isOpen()) {
            // Normalize path separators
            QString absPath = QFileInfo(filepath).absoluteFilePath();
            absPath.replace("\\", "/");

            QSqlQuery query(d);
            // Cache the result. Use REPLACE to update existing entries for the same path.
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
}

// Scans for legacy processed_files.db files inside target workspace directories.
// Reads their records, converts relative paths to absolute paths, merges them
// into the global profile database, and renames the legacy database file to avoid re-migration.
void DatabaseManager::migrateLocalDatabase(const QString &localDbPath, const QString &rootDir)
{
    // Do nothing if the old database file doesn't exist inside the scanned folder
    if (!QFile::exists(localDbPath)) return;

    qDebug() << "Migrating local database:" << localDbPath;
    
    // Open a temporary connection to the local database file
    QString localConnName = QString("local_conn_%1").arg(QString::number((quintptr)QThread::currentThreadId()));
    QSqlDatabase localDb = QSqlDatabase::addDatabase("QSQLITE", localConnName);
    localDb.setDatabaseName(localDbPath);
    
    if (localDb.open()) {
        QSqlQuery query(localDb);
        // Query all processed file rows from the local database
        if (query.exec("SELECT filepath, original_size, compressed_size, hash, processed_at FROM processed_files")) {
            QSqlDatabase mainDb = db(); // Fetch global database connection
            if (mainDb.isOpen()) {
                QSqlQuery insertQuery(mainDb);
                while (query.next()) {
                    QString oldPath = query.value(0).toString();
                    qint64 origSize = query.value(1).toLongLong();
                    qint64 compSize = query.value(2).toLongLong();
                    QString hash = query.value(3).toString();
                    QString processedAt = query.value(4).toString();
                    
                    // Resolve paths to absolute paths.
                    // If the path in the old database was saved as relative (e.g. "movies/file.mkv"),
                    // prepend the workspace root directory to obtain the absolute path on the host.
                    QString absPath = oldPath;
                    if (QDir::isRelativePath(oldPath)) {
                        absPath = QDir(rootDir).absoluteFilePath(oldPath);
                    }
                    absPath = QDir::cleanPath(absPath);
                    absPath.replace("\\", "/");
                    
                    // Insert the record into the global database. Ignore duplicates.
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
        localDb.close(); // Close local file handle
    }
    QSqlDatabase::removeDatabase(localConnName); // Unregister temporary connection
    
    // Rename the local database to processed_files.db.migrated.
    // This creates a backup and prevents the program from attempting migration next time.
    QString migratedPath = localDbPath + ".migrated";
    if (QFile::exists(migratedPath)) {
        QFile::remove(migratedPath);
    }
    QFile::rename(localDbPath, migratedPath);
}

// Clears cached compliance entries specifically under the active workspace folder.
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
            // Delete all entries whose path matches the folder prefix
            query.prepare("DELETE FROM scan_cache WHERE filepath LIKE :prefix || '%' OR filepath = :exact");
            query.bindValue(":prefix", prefix);
            query.bindValue(":exact", QDir(rootDir).absolutePath().replace("\\", "/"));
            ok = query.exec();
        }
    }
    return ok;
}

// Clears sizing statistics (scoreboard history) entries specifically under the active workspace folder.
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
            // Delete sizing history entries matching this folder
            query.prepare("DELETE FROM processed_files WHERE filepath LIKE :prefix || '%' OR filepath = :exact");
            query.bindValue(":prefix", prefix);
            query.bindValue(":exact", QDir(rootDir).absolutePath().replace("\\", "/"));
            ok = query.exec();
        }
    }
    return ok;
}
