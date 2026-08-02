#pragma once

#include "core/GlobalConfig.h"
#include "core/Repository.h"

#include <QList>

namespace svnsync {

/** Persists the configured repositories and global settings in a SQLite
 *  database at ~/.svnsyncdrive/config.db. Passwords are never stored. */
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

private:
    static void migrateFromRegistry();
};

} // namespace svnsync
