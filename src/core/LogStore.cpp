#include "core/LogStore.h"

#include "core/AppPaths.h"

#include <QDateTime>
#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace svnsync {

namespace {
const QString kConnection = QStringLiteral("svnsync_logs");
QString g_testDbFile;
} // namespace

QString LogStore::databaseFile()
{
    if (!g_testDbFile.isEmpty())
        return g_testDbFile;
    return AppPaths::dataDir() + QLatin1String("/logs.db");
}

void LogStore::ensureOpen()
{
    if (QSqlDatabase::contains(kConnection)) {
        if (QSqlDatabase::database(kConnection).isOpen())
            return;
        QSqlDatabase::removeDatabase(kConnection);
    }

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), kConnection);
    db.setDatabaseName(databaseFile());
    if (!db.open())
        return;  // Logging degrades to a no-op when the store is unavailable.

    QSqlQuery query(db);
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS logs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "repo TEXT NOT NULL, "
        "ts TEXT NOT NULL, "
        "message TEXT NOT NULL)"));
    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_logs_repo ON logs(repo, id)"));
}

void LogStore::append(const QString &repo, const QString &message, int maxPerRepo)
{
    if (maxPerRepo <= 0)
        return;
    ensureOpen();
    QSqlDatabase db = QSqlDatabase::database(kConnection, false);
    if (!db.isValid() || !db.isOpen())
        return;

    const QString ts = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyy-MM-dd HH:mm:ss"));

    if (!db.transaction())
        return;

    bool ok = true;
    {
        QSqlQuery query(db);
        query.prepare(QStringLiteral(
            "INSERT INTO logs(repo, ts, message) VALUES (:repo, :ts, :message)"));
        query.bindValue(QStringLiteral(":repo"), repo);
        query.bindValue(QStringLiteral(":ts"), ts);
        query.bindValue(QStringLiteral(":message"), message);
        ok = query.exec();
    }
    if (ok) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral(
            "DELETE FROM logs WHERE repo = :repo AND id NOT IN ("
            "SELECT id FROM logs WHERE repo = :repo ORDER BY id DESC LIMIT :max)"));
        query.bindValue(QStringLiteral(":repo"), repo);
        query.bindValue(QStringLiteral(":max"), maxPerRepo);
        ok = query.exec();
    }

    if (ok)
        db.commit();
    else
        db.rollback();
}

QStringList LogStore::history(const QString &repo, int maxLines)
{
    QStringList lines;
    ensureOpen();
    QSqlDatabase db = QSqlDatabase::database(kConnection, false);
    if (!db.isValid() || !db.isOpen())
        return lines;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT ts, message FROM logs WHERE repo = :repo ORDER BY id ASC"));
    query.bindValue(QStringLiteral(":repo"), repo);
    if (!query.exec())
        return lines;
    while (query.next()) {
        if (maxLines >= 0 && lines.size() >= maxLines)
            break;
        lines.append(query.value(0).toString()
                     + QStringLiteral("  ")
                     + query.value(1).toString());
    }
    return lines;
}

void LogStore::clearRepository(const QString &repo)
{
    ensureOpen();
    QSqlDatabase db = QSqlDatabase::database(kConnection, false);
    if (!db.isValid() || !db.isOpen())
        return;

    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM logs WHERE repo = :repo"));
    query.bindValue(QStringLiteral(":repo"), repo);
    query.exec();
}

void LogStore::setDatabaseFileForTest(const QString &dbFile)
{
    if (QSqlDatabase::contains(kConnection)) {
        QSqlDatabase::database(kConnection).close();
        QSqlDatabase::removeDatabase(kConnection);
    }
    g_testDbFile = dbFile;
}

} // namespace svnsync
