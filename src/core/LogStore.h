#pragma once

#include <QString>
#include <QStringList>

namespace svnsync {

/**
 * Persists per-repository sync log messages in a SQLite database so each
 * repository's log survives restarts. A single `logs` table holds every
 * repository's lines, filtered by repository name when displayed; the
 * lines of a removed repository are deleted with it.
 *
 * The database file lives in the application data directory
 * (QStandardPaths::AppDataLocation/logs.db). Entries beyond the
 * configured per-repository cap are pruned on insert.
 *
 * All callers today are on the GUI thread, so no locking is needed.
 */
class LogStore
{
public:
    /** Append one message for a repository, pruning to maxPerRepo lines. */
    static void append(const QString &repo, const QString &message, int maxPerRepo);

    /** Oldest-first formatted lines ("yyyy-MM-dd HH:mm:ss  message"),
     *  truncated to maxLines (all lines if maxLines < 0). */
    static QStringList history(const QString &repo, int maxLines = -1);

    /** Drop every log line belonging to a repository (used on removal). */
    static void clearRepository(const QString &repo);

    /** Point the store at a throw-away database (unit tests only). */
    static void setDatabaseFileForTest(const QString &dbFile);

private:
    static QString databaseFile();
    static void ensureOpen();
};

} // namespace svnsync
