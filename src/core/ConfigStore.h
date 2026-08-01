#pragma once

#include "core/Repository.h"

#include <QList>

namespace svnsync {

/** Persists the configured repositories using QSettings. */
class ConfigStore
{
public:
    static QList<Repository> loadRepositories();
    static void saveRepositories(const QList<Repository> &repositories);
};

} // namespace svnsync
