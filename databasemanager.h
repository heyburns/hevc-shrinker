#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QString> // Qt class for handling Unicode character strings
#include <QSqlDatabase> // Qt class representing our connection to the SQLite database file

// Structure representing a record retrieved from the 'processed_files' database table.
// Holds details of video files that have already been shrunk (transcoded).
struct ProcessedFileInfo {
    bool found = false;         // Set to true if a record matching the file was found in the database.
    qint64 originalSize = 0;    // Size in bytes of the video file before transcoding.
    qint64 compressedSize = 0;  // Size in bytes of the video file after transcoding.
    QString hash;               // A unique text signature (hash) calculated from the file contents.
};

// Class responsible for handling all database operations (creating tables,
// caching compliance scans, recording transcoded file sizes, and fetching stats).
class DatabaseManager {
public:
    // Constructor. Initializes connection variables. Optional path override can be passed.
    DatabaseManager(const QString &dbPath = "");
    
    // Destructor. Cleans up database connection handles.
    ~DatabaseManager();

    // Creates the database file and initializes SQL tables if they do not exist.
    bool init();
    
    // Checks if a video file has already been shrunk in this workspace and returns its sizes.
    ProcessedFileInfo getProcessedFileInfo(const QString &filepath);
    
    // Checks the local cache to see if the file's H.265 compliance status is already known.
    // Returns: -1 if cache miss, 1 if compliant (skip), 0 if non-compliant (requires transcode).
    int getCachedCompliance(const QString &filepath, qint64 fileSize, qint64 lastModified);
    
    // Caches a file's compliance result (HEVC+AAC or not) so we don't have to probe it again.
    void setCachedCompliance(const QString &filepath, qint64 fileSize, qint64 lastModified, int isCompliant);
    
    // Records the file sizing history to the database after a successful transcode completion.
    bool recordProcessedFile(const QString &filepath, qint64 originalSize, qint64 compressedSize, const QString &hash);
    
    // Calculates space savings (Original, Compressed, Saved Megabytes and Percentage)
    // for all shrunk video files whose paths match the active workspace directory.
    bool getSpaceSavings(const QString &rootDir, double &totalOriginalMb, double &totalCompressedMb, double &totalSavedMb, double &savingsPct);

    // Reads records from a legacy relative-path database (if found inside the target folder),
    // imports them as absolute paths into the user's global profile datastore, and renames the old file.
    void migrateLocalDatabase(const QString &localDbPath, const QString &rootDir);
    
    // Deletes all cached compliance scan records associated with the active workspace directory.
    bool clearScanCache(const QString &rootDir);
    
    // Deletes all sizing history scoreboard records associated with the active workspace directory.
    bool clearProcessedFiles(const QString &rootDir);

private:
    QString m_dbPath;            // Absolute path to the global SQLite database file on the local SSD.
    QString m_connectionName;    // A unique connection name generated per-thread to prevent race conditions.
    
    // Establishes or retrieves the thread-local database connection.
    QSqlDatabase db();
    
    // Safely closes the database connection.
    void closeDb();
};

#endif // DATABASEMANAGER_H
