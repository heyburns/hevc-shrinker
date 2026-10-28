#include "databasemanager.h"
#include "filetransaction.h"
#include "mediautils.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

namespace {
QString globalDbPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("hevc_shrinker.db");
}

QString pathCollation()
{
#ifdef Q_OS_WIN
    return " COLLATE NOCASE";
#else
    return " COLLATE BINARY";
#endif
}

QString workspaceCondition()
{
    // Literal prefix comparison: %, _ and backslashes are ordinary path characters.
    return "(substr(filepath, 1, length(:prefix)) = :prefix" + pathCollation()
        + " OR filepath = :exact" + pathCollation() + ")";
}

void bindWorkspace(QSqlQuery &query, const QString &root)
{
    QString exact = normalizedPath(root);
    query.bindValue(":exact", exact);
    if (!exact.endsWith('/')) exact += '/';
    query.bindValue(":prefix", exact);
}
}

DatabaseManager::DatabaseManager(const QString &dbPath)
    : m_dbPath(dbPath.isEmpty() ? globalDbPath() : QFileInfo(dbPath).absoluteFilePath()),
      m_connectionName("hevc_" + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    init();
}

DatabaseManager::~DatabaseManager()
{
    if (QSqlDatabase::contains(m_connectionName)) {
        { auto connection = QSqlDatabase::database(m_connectionName, false); connection.close(); }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

QSqlDatabase DatabaseManager::db()
{
    if (QSqlDatabase::contains(m_connectionName)) return QSqlDatabase::database(m_connectionName, false);
    auto connection = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    QDir().mkpath(QFileInfo(m_dbPath).absolutePath());
    connection.setDatabaseName(m_dbPath);
    connection.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000");
    if (!connection.open()) m_error = connection.lastError().text();
    return connection;
}

bool DatabaseManager::init()
{
    auto connection = db();
    QSqlQuery query(connection);
    m_ready = false;
    if (!connection.isOpen() || !query.exec("BEGIN IMMEDIATE")) {
        if (connection.isOpen()) m_error = query.lastError().text();
        return false;
    }
    auto execute = [&](const QString &sql) {
        if (query.exec(sql)) return true;
        m_error = query.lastError().text();
        connection.rollback();
        return false;
    };
    if (!execute("CREATE TABLE IF NOT EXISTS processed_files (id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "filepath TEXT UNIQUE, original_size INTEGER, compressed_size INTEGER, hash TEXT, "
        "processed_at DATETIME DEFAULT CURRENT_TIMESTAMP)")) return false;
    QSet<QString> columns;
    if (!execute("PRAGMA table_info(processed_files)")) return false;
    while (query.next()) columns.insert(query.value(1).toString());
    query.finish();
    for (const QString &definition : {QString("output_modified INTEGER"), QString("settings_key TEXT"), QString("outcome TEXT")}) {
        if (!columns.contains(definition.section(' ', 0, 0))
            && !execute("ALTER TABLE processed_files ADD COLUMN " + definition)) return false;
    }
    // Separate metadata cache from legacy codec-only decisions; old decisions
    // cannot represent current geometry, stream selection or user settings.
    if (!execute("CREATE TABLE IF NOT EXISTS metadata_cache (filepath TEXT PRIMARY KEY, file_size INTEGER, "
        "last_modified INTEGER, metadata BLOB)")) return false;
    if (!connection.commit()) {
        m_error = connection.lastError().text();
        connection.rollback();
        return false;
    }
    m_ready = true;
    m_error.clear();
    return true;
}

ProcessedFileInfo DatabaseManager::getProcessedFileInfo(const QString &filepath)
{
    ProcessedFileInfo info;
    QSqlQuery query(db());
    query.prepare("SELECT original_size, compressed_size, hash, output_modified, settings_key, outcome "
        "FROM processed_files WHERE filepath = :path" + pathCollation());
    query.bindValue(":path", normalizedPath(filepath));
    if (!query.exec()) { m_error = query.lastError().text(); return info; }
    if (!query.next()) return info;
    info.found = true;
    info.originalSize = query.value(0).toLongLong();
    info.compressedSize = query.value(1).toLongLong();
    info.hash = query.value(2).toString();
    info.settingsKey = query.value(4).toString();
    info.outcome = query.value(5).toString();
    const QFileInfo file(filepath);
    // Legacy hashes describe the input, not the output: never trust them as an identity check.
    info.current = file.isFile() && !file.isSymLink() && info.hash.startsWith("v2:")
        && file.size() == info.compressedSize && !query.value(3).isNull()
        && file.lastModified().toMSecsSinceEpoch() == query.value(3).toLongLong()
        && computeFastHash(filepath) == info.hash;
    return info;
}

bool DatabaseManager::recordProcessedFile(const QString &filepath, qint64 originalSize, qint64 compressedSize,
    const QString &hash, const QString &settingsKey, const QString &outcome)
{
    if (!m_ready || hash.isEmpty()) { m_error = "Cannot record an unverified output."; return false; }
    auto connection = db();
    if (!connection.transaction()) { m_error = connection.lastError().text(); return false; }
    QSqlQuery query(connection);
    // Replace legacy Windows entries with differently cased paths atomically.
    query.prepare("DELETE FROM processed_files WHERE filepath = :path" + pathCollation());
    query.bindValue(":path", normalizedPath(filepath));
    if (!query.exec()) { m_error = query.lastError().text(); connection.rollback(); return false; }
    query.prepare("INSERT OR REPLACE INTO processed_files "
        "(filepath, original_size, compressed_size, hash, output_modified, settings_key, outcome, processed_at) "
        "VALUES (:path, :orig, :comp, :hash, :modified, :settings, :outcome, CURRENT_TIMESTAMP)");
    query.bindValue(":path", normalizedPath(filepath));
    query.bindValue(":orig", originalSize);
    query.bindValue(":comp", compressedSize);
    query.bindValue(":hash", hash);
    query.bindValue(":modified", QFileInfo(filepath).lastModified().toMSecsSinceEpoch());
    query.bindValue(":settings", settingsKey);
    query.bindValue(":outcome", outcome);
    if (!query.exec()) { m_error = query.lastError().text(); connection.rollback(); return false; }
    if (connection.commit()) return true;
    m_error = connection.lastError().text();
    connection.rollback();
    return false;
}

QByteArray DatabaseManager::cachedMetadata(const QString &filepath, qint64 size, qint64 modified)
{
    QSqlQuery query(db());
    query.prepare("SELECT metadata FROM metadata_cache WHERE filepath = :path" + pathCollation()
        + " AND file_size = :size AND last_modified = :modified");
    query.bindValue(":path", normalizedPath(filepath));
    query.bindValue(":size", size);
    query.bindValue(":modified", modified);
    if (!query.exec()) { m_error = query.lastError().text(); return {}; }
    return query.next() ? query.value(0).toByteArray() : QByteArray();
}

bool DatabaseManager::cacheMetadata(const QString &filepath, qint64 size, qint64 modified, const QByteArray &json)
{
    QSqlQuery query(db());
    query.prepare("INSERT OR REPLACE INTO metadata_cache VALUES (:path, :size, :modified, :json)");
    query.bindValue(":path", normalizedPath(filepath));
    query.bindValue(":size", size);
    query.bindValue(":modified", modified);
    query.bindValue(":json", json);
    if (query.exec()) return true;
    m_error = query.lastError().text();
    return false;
}

bool DatabaseManager::getSpaceSavings(const QString &root, double &original, double &compressed, double &saved, double &percent)
{
    original = compressed = saved = percent = 0;
    QSqlQuery query(db());
    query.prepare("SELECT SUM(original_size), SUM(compressed_size) FROM processed_files WHERE " + workspaceCondition());
    bindWorkspace(query, root);
    if (!query.exec() || !query.next()) { m_error = query.lastError().text(); return false; }
    original = query.value(0).toDouble() / (1024 * 1024);
    compressed = query.value(1).toDouble() / (1024 * 1024);
    saved = original - compressed;
    if (original > 0) percent = saved / original * 100;
    return true;
}

bool DatabaseManager::clearTable(const QString &table, const QString &root)
{
    QSqlQuery query(db());
    query.prepare("DELETE FROM " + table + " WHERE " + workspaceCondition());
    bindWorkspace(query, root);
    if (query.exec()) return true;
    m_error = query.lastError().text();
    return false;
}

bool DatabaseManager::clearScanCache(const QString &root) { return clearTable("metadata_cache", root); }
bool DatabaseManager::clearProcessedFiles(const QString &root) { return clearTable("processed_files", root); }

bool DatabaseManager::migrateLocalDatabase(const QString &localPath, const QString &root)
{
    if (!QFileInfo::exists(localPath)) return true;
    const QString name = "legacy_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool imported = false;
    {
        auto legacy = QSqlDatabase::addDatabase("QSQLITE", name);
        legacy.setDatabaseName(localPath);
        legacy.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000");
        if (legacy.open()) {
            QSqlQuery select(legacy);
            if (select.exec("SELECT filepath, original_size, compressed_size, hash, processed_at FROM processed_files")) {
                auto main = db();
                if (main.transaction()) {
                    QSqlQuery insert(main);
                    insert.prepare("INSERT OR IGNORE INTO processed_files "
                        "(filepath, original_size, compressed_size, hash, processed_at) VALUES (?, ?, ?, ?, ?)");
                    imported = true;
                    while (select.next()) {
                        QString path = select.value(0).toString();
                        if (QDir::isRelativePath(path)) path = QDir(root).filePath(path);
                        insert.bindValue(0, normalizedPath(path));
                        for (int i = 1; i < 5; ++i) insert.bindValue(i, select.value(i));
                        if (!insert.exec()) { m_error = insert.lastError().text(); imported = false; break; }
                    }
                    if (select.lastError().isValid()) { m_error = select.lastError().text(); imported = false; }
                    if (imported && !main.commit()) { m_error = main.lastError().text(); imported = false; }
                    if (!imported) main.rollback();
                } else m_error = main.lastError().text();
            } else m_error = select.lastError().text();
        } else m_error = legacy.lastError().text();
        legacy.close();
    }
    QSqlDatabase::removeDatabase(name);
    if (!imported) return false;
    QString backup = localPath + ".migrated";
    if (QFileInfo::exists(backup)) backup += "." + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QFile::rename(localPath, backup)) { m_error = "Imported history, but could not rename the legacy database."; return false; }
    return true;
}
