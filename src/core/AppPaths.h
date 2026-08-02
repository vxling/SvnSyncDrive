#pragma once

#include <QDir>
#include <QString>

namespace svnsync {

/** Central location for all app data (config, logs, log store), kept in a
 *  single hidden folder (~/.svnsyncdrive) under the user's home directory.
 *  The folder is created on first access; the app also calls dataDir() at
 *  startup so an absent folder is materialised right away. */
namespace AppPaths {

inline QString dataDir()
{
    QString dir = QDir::homePath() + QLatin1String("/.svnsyncdrive");
    QDir().mkpath(dir);
    return dir;
}

} // namespace AppPaths
} // namespace svnsync
