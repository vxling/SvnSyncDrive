#include "core/ConfigStore.h"

#include "core/AppPaths.h"
#include "core/CredCrypto.h"

#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace svnsync {

namespace {
const QString kConnection = QStringLiteral("svnsync_config");
const QString kMasterKeyRow = QStringLiteral("__master__");
QString g_testDbFile;

QString databaseFile()
{
    if (!g_testDbFile.isEmpty())
        return g_testDbFile;
    return AppPaths::dataDir() + QLatin1String("/config.db");
}

void ensureOpen()
{
    if (QSqlDatabase::contains(kConnection)) {
        if (QSqlDatabase::database(kConnection).isOpen())
            return;
        QSqlDatabase::removeDatabase(kConnection);
    }

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), kConnection);
    db.setDatabaseName(databaseFile());
    if (!db.open())
        return;  // Config defaults apply when the store is unavailable.

    QSqlQuery query(db);
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS repos ("
        "name TEXT PRIMARY KEY, "
        "path TEXT NOT NULL, "
        "url TEXT NOT NULL, "
        "username TEXT NOT NULL, "
        "state INTEGER NOT NULL)"));
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS global ("
        "key TEXT PRIMARY KEY, "
        "value TEXT NOT NULL)"));
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS credentials ("
        "name TEXT PRIMARY KEY, "
        "blob TEXT NOT NULL)"));
}

QSqlDatabase database()
{
    ensureOpen();
    return QSqlDatabase::database(kConnection, false);
}

QByteArray masterKey()
{
    QSqlDatabase db = database();
    if (!db.isValid() || !db.isOpen())
        return QByteArray();

    QSqlQuery query(db);
    if (query.exec(QStringLiteral(
            "SELECT blob FROM credentials WHERE name = '__master__'"))
        && query.next()) {
        const QByteArray key =
            QByteArray::fromBase64(query.value(0).toByteArray());
        if (key.size() == 32)
            return key;
    }

    const QByteArray key = CredCrypto::generateKey();
    if (key.isEmpty())
        return QByteArray();
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO credentials(name, blob) VALUES (:name, :blob)"));
    query.bindValue(QStringLiteral(":name"), kMasterKeyRow);
    query.bindValue(QStringLiteral(":blob"),
                    QString::fromLatin1(key.toBase64()));
    if (!query.exec())
        return QByteArray();
    return key;
}

QByteArray loadEncryptedPassword(const QString &name)
{
    QSqlDatabase db = database();
    if (!db.isValid() || !db.isOpen())
        return QByteArray();

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT blob FROM credentials WHERE name = :name"));
    query.bindValue(QStringLiteral(":name"), name);
    if (!query.exec() || !query.next())
        return QByteArray();
    return query.value(0).toByteArray();
}

} // namespace

void ConfigStore::initialize()
{
    // Materialises ~/.svnsyncdrive (AppPaths::dataDir mkpaths it) and the
    // config database, then imports the legacy repositories and writes the
    // default global settings on first run.
    QSqlDatabase db = database();
    if (!db.isValid() || !db.isOpen())
        return;

    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM global")))
        return;
    if (query.next() && query.value(0).toInt() > 0)
        return;

    migrateFromRegistry();
    saveGlobalConfig(GlobalConfig{});
    QSqlQuery version(db);
    version.exec(QStringLiteral(
        "INSERT OR REPLACE INTO global(key, value) VALUES ('schemaVersion', '1')"));
}

void ConfigStore::migrateFromRegistry()
{
    // Versions before the SQLite store kept repositories in the Windows
    // registry (HKCU\Software\SvnSyncDrive\SvnSyncDrive) via QSettings.
    // Import them once so existing users don't lose their repositories.
    QSettings settings(QSettings::NativeFormat, QSettings::UserScope,
                       QStringLiteral("SvnSyncDrive"), QStringLiteral("SvnSyncDrive"));
    settings.beginGroup(QStringLiteral("repositories"));

    QList<Repository> imported;
    for (const auto &name : settings.childGroups()) {
        Repository repo;
        repo.name = name;
        repo.path = settings.value(name + QLatin1String("/path")).toString();
        repo.url = settings.value(name + QLatin1String("/url")).toString();
        repo.username = settings.value(name + QLatin1String("/username")).toString();
        repo.state =
            static_cast<RepoState>(settings.value(name + QLatin1String("/state"), 0).toInt());
        if (!repo.name.isEmpty() && !repo.path.isEmpty())
            imported.append(repo);
    }
    settings.endGroup();
    if (imported.isEmpty())
        return;

    QList<Repository> current = loadRepositories();
    for (const auto &repo : imported) {
        bool exists = false;
        for (const auto &cur : current) {
            if (cur.name == repo.name) {
                exists = true;
                break;
            }
        }
        if (!exists)
            current.append(repo);
    }
    saveRepositories(current);
}

QList<Repository> ConfigStore::loadRepositories()
{
    QList<Repository> repositories;
    QSqlDatabase db = database();
    if (!db.isValid() || !db.isOpen())
        return repositories;

    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
            "SELECT name, path, url, username, state FROM repos ORDER BY name")))
        return repositories;
    while (query.next()) {
        Repository repo;
        repo.name = query.value(0).toString();
        repo.path = query.value(1).toString();
        repo.url = query.value(2).toString();
        repo.username = query.value(3).toString();
        repo.state = static_cast<RepoState>(query.value(4).toInt());
        if (!repo.path.isEmpty())
            repositories.append(repo);
    }

    // Restore the per-repo password from the encrypted credentials table;
    // the master key lives in the same database (defence against casual
    // reads, not against an attacker who can open the store itself).
    const QByteArray key = masterKey();
    for (auto &repo : repositories)
        repo.password = CredCrypto::decrypt(key, loadEncryptedPassword(repo.name));
    return repositories;
}

void ConfigStore::saveRepositories(const QList<Repository> &repositories)
{
    QSqlDatabase db = database();
    if (!db.isValid() || !db.isOpen())
        return;
    if (!db.transaction())
        return;

    bool ok = false;
    {
        QSqlQuery query(db);
        ok = query.exec(QStringLiteral("DELETE FROM repos"));
    }
    if (ok) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral(
            "INSERT INTO repos(name, path, url, username, state) "
            "VALUES (:name, :path, :url, :username, :state)"));
        for (const auto &repo : repositories) {
            // A default-constructed QString is *null*; the QSQLITE driver then
            // binds NULL, which trips the NOT NULL column. Normalise to a
            // (non-null) empty string so an unnamed repo still saves.
            QString username = repo.username;
            if (username.isNull())
                username = QStringLiteral("");
            query.bindValue(QStringLiteral(":name"), repo.name);
            query.bindValue(QStringLiteral(":path"), repo.path);
            query.bindValue(QStringLiteral(":url"), repo.url);
            query.bindValue(QStringLiteral(":username"), username);
            query.bindValue(QStringLiteral(":state"), static_cast<int>(repo.state));
            if (!query.exec()) {
                ok = false;
                break;
            }
        }
    }

    // Persist each repo's password encrypted (empty password removes the
    // stored entry), then drop the credential rows of removed repositories.
    const QByteArray key = masterKey();
    if (ok && !key.isEmpty()) {
        QSqlQuery cred(db);
        cred.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO credentials(name, blob) VALUES (:name, :blob)"));
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE FROM credentials WHERE name = :name"));
        for (const auto &repo : repositories) {
            if (repo.password.isEmpty()) {
                del.bindValue(QStringLiteral(":name"), repo.name);
                if (!del.exec()) {
                    ok = false;
                    break;
                }
            } else {
                const QByteArray blob = CredCrypto::encrypt(key, repo.password);
                if (blob.isEmpty()) {
                    ok = false;
                    break;
                }
                cred.bindValue(QStringLiteral(":name"), repo.name);
                cred.bindValue(QStringLiteral(":blob"), QString::fromLatin1(blob));
                if (!cred.exec()) {
                    ok = false;
                    break;
                }
            }
        }

        // Drop credential rows of removed repositories. The names go into the
        // SQL as quoted literals (single quotes doubled) - never unquoted, or
        // SQLite would read them as column names and the whole purge would
        // fail, rolling back the entire save.
        QStringList names;
        for (const auto &repo : repositories)
            names << repo.name;
        QStringList quoted;
        quoted.reserve(names.size());
        for (const auto &name : names) {
            QString escaped = name;
            escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
            quoted << (QLatin1Char('\'') + escaped + QLatin1Char('\''));
        }
        QString sql = QStringLiteral(
            "DELETE FROM credentials WHERE name <> '__master__'");
        if (!quoted.isEmpty())
            sql += QStringLiteral(" AND name NOT IN (")
                + quoted.join(QLatin1Char(',')) + QStringLiteral(")");
        QSqlQuery purge(db);
        if (ok && !purge.exec(sql))
            ok = false;
    }

    if (ok)
        db.commit();
    else
        db.rollback();
}

GlobalConfig ConfigStore::loadGlobalConfig()
{
    GlobalConfig config;
    QSqlDatabase db = database();
    if (!db.isValid() || !db.isOpen())
        return config;

    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT key, value FROM global")))
        return config;
    while (query.next()) {
        const QString key = query.value(0).toString();
        const QString value = query.value(1).toString();
        if (key == QStringLiteral("pollIntervalMs"))
            config.pollIntervalMs = value.toInt();
        else if (key == QStringLiteral("fullSyncIntervalMs"))
            config.fullSyncIntervalMs = value.toInt();
        else if (key == QStringLiteral("autoAddUnversioned"))
            config.autoAddUnversioned = value.toInt() != 0;
        else if (key == QStringLiteral("trustServerCertificate"))
            config.trustServerCertificate = value.toInt() != 0;
        else if (key == QStringLiteral("minimizeToTray"))
            config.minimizeToTray = value.toInt() != 0;
        else if (key == QStringLiteral("startMinimizedToTray"))
            config.startMinimizedToTray = value.toInt() != 0;
        else if (key == QStringLiteral("maxLogsPerRepo"))
            config.maxLogsPerRepo = value.toInt();
        else if (key == QStringLiteral("disconnectThreshold"))
            config.disconnectThreshold = value.toInt();
        else if (key == QStringLiteral("networkTimeoutSec"))
            config.networkTimeoutSec = value.toInt();
        else if (key == QStringLiteral("maxTransferSec"))
            config.maxTransferSec = value.toInt();
        else if (key == QStringLiteral("maxFileSizeMb"))
            config.maxFileSizeMb = value.toInt();
        else if (key == QStringLiteral("autoResolveConflicts"))
            config.autoResolveConflicts = value.toInt() != 0;
        else if (key == QStringLiteral("conflictResolution"))
            config.conflictResolution = value.toInt();
        else if (key == QStringLiteral("language"))
            config.language = value;
        else if (key == QStringLiteral("repoRoot"))
            config.repoRoot = value;
        else if (key == QStringLiteral("quickAccessEnabled"))
            config.quickAccessEnabled = value.toInt() != 0;
    }
    return config;
}

void ConfigStore::saveGlobalConfig(const GlobalConfig &config)
{
    QSqlDatabase db = database();
    if (!db.isValid() || !db.isOpen())
        return;
    if (!db.transaction())
        return;

    bool ok = false;
    {
        QSqlQuery query(db);
        ok = query.exec(QStringLiteral("DELETE FROM global"));
    }
    if (ok) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO global(key, value) VALUES (:key, :value)"));
        const auto put = [&query, &ok](const QString &key, const QString &value) {
            if (!ok)
                return;
            query.bindValue(QStringLiteral(":key"), key);
            query.bindValue(QStringLiteral(":value"), value);
            ok = query.exec();
        };
        put(QStringLiteral("pollIntervalMs"), QString::number(config.pollIntervalMs));
        put(QStringLiteral("fullSyncIntervalMs"), QString::number(config.fullSyncIntervalMs));
        put(QStringLiteral("autoAddUnversioned"), QString::number(config.autoAddUnversioned));
        put(QStringLiteral("trustServerCertificate"), QString::number(config.trustServerCertificate));
        put(QStringLiteral("minimizeToTray"), QString::number(config.minimizeToTray));
        put(QStringLiteral("startMinimizedToTray"), QString::number(config.startMinimizedToTray));
        put(QStringLiteral("maxLogsPerRepo"), QString::number(config.maxLogsPerRepo));
        put(QStringLiteral("disconnectThreshold"), QString::number(config.disconnectThreshold));
        put(QStringLiteral("networkTimeoutSec"), QString::number(config.networkTimeoutSec));
        put(QStringLiteral("maxTransferSec"), QString::number(config.maxTransferSec));
        put(QStringLiteral("maxFileSizeMb"), QString::number(config.maxFileSizeMb));
        put(QStringLiteral("autoResolveConflicts"), QString::number(config.autoResolveConflicts));
        put(QStringLiteral("conflictResolution"), QString::number(config.conflictResolution));
        put(QStringLiteral("language"), config.language);
        put(QStringLiteral("repoRoot"), config.repoRoot);
        put(QStringLiteral("quickAccessEnabled"), QString::number(config.quickAccessEnabled));
    }

    if (ok)
        db.commit();
    else
        db.rollback();
}

void ConfigStore::setDatabaseFileForTest(const QString &dbFile)
{
    g_testDbFile = dbFile;
    if (QSqlDatabase::contains(kConnection)) {
        QSqlDatabase db = QSqlDatabase::database(kConnection);
        if (db.isOpen())
            db.close();
        QSqlDatabase::removeDatabase(kConnection);
    }
}

} // namespace svnsync
