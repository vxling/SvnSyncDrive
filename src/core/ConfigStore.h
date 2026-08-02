#pragma once

#include "core/GlobalConfig.h"
#include "core/Repository.h"

#include <QList>

namespace svnsync {

/** Persists the configured repositories and global settings in a SQLite
 *  database at ~/.svnsyncdrive/config.db. Repository passwords are stored
 *  encrypted (AES-256-GCM) in the credentials table; the random master key
 *  lives in the same database, so the protection guards against casual reads
 *  of the store, not against an attacker with full access to it. */
class ConfigStore
{
public:
    /** Creates the data folder/database if absent, imports repositories
     *  from the legacy registry-based store once, and writes the default
     *  global settings on first run. Called at application startup. */
    static void initialize();

    static QList<Repository> loadRepositories();
    static void saveRepositories(const QList<Repository> &repositories);

    static GlobalConfig loadGlobalConfig();
    static void saveGlobalConfig(const GlobalConfig &config);

    /** Test hook: redirect the config database to a specific file
     *  ("" restores the default location). */
    static void setDatabaseFileForTest(const QString &dbFile);

private:
    static void migrateFromRegistry();
};

} // namespace svnsync
