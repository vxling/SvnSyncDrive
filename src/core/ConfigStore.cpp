#include "core/ConfigStore.h"

#include "core/AppPaths.h"

#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace svnsync {

namespace {
const QString kConnection = QStringLiteral("svnsync_config");

QString databaseFile()
{
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
}

QSqlDatabase database()
{
    ensureOpen();
    return QSqlDatabase::database(kConnection, false);
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
        // The password is deliberately NOT persisted here: it is stored
        // (and encrypted) by libsvn itself in its auth cache, so the app
        // never touches plaintext passwords. The username is kept so the
        // configure dialog can show whose credentials are in use.
        repo.state = static_cast<RepoState>(query.value(4).toInt());
        if (!repo.path.isEmpty())
            repositories.append(repo);
    }
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
            query.bindValue(QStringLiteral(":name"), repo.name);
            query.bindValue(QStringLiteral(":path"), repo.path);
            query.bindValue(QStringLiteral(":url"), repo.url);
            query.bindValue(QStringLiteral(":username"), repo.username);
            query.bindValue(QStringLiteral(":state"), static_cast<int>(repo.state));
            if (!query.exec()) {
                ok = false;
                break;
            }
        }
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
    }

    if (ok)
        db.commit();
    else
        db.rollback();
}

} // namespace svnsync
