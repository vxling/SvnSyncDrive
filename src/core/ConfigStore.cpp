#include "core/ConfigStore.h"

#include <QSettings>
#include <QStringList>

namespace svnsync {

namespace {
const QString kOrganization = QStringLiteral("SvnSyncDrive");
const QString kApplication = QStringLiteral("SvnSyncDrive");
const QString kReposGroup = QStringLiteral("repositories");
} // namespace

QList<Repository> ConfigStore::loadRepositories()
{
    QSettings settings(kOrganization, kApplication);
    QList<Repository> repositories;

    const QStringList names = settings.childGroups();
    settings.beginGroup(kReposGroup);
    for (const auto &name : names) {
        if (settings.childGroups().contains(name)) {
            settings.beginGroup(name);
            Repository repo;
            repo.name = name;
            repo.path = settings.value(QStringLiteral("path")).toString();
            repo.url = settings.value(QStringLiteral("url")).toString();
            repo.username = settings.value(QStringLiteral("username")).toString();
            repo.password = settings.value(QStringLiteral("password")).toString();
            repo.isActive = settings.value(QStringLiteral("isActive"), false).toBool();
            settings.endGroup();
            if (!repo.path.isEmpty())
                repositories.append(repo);
        }
    }
    settings.endGroup();
    return repositories;
}

void ConfigStore::saveRepositories(const QList<Repository> &repositories)
{
    QSettings settings(kOrganization, kApplication);
    settings.remove(kReposGroup);
    settings.beginGroup(kReposGroup);
    for (const auto &repo : repositories) {
        settings.beginGroup(repo.name);
        settings.setValue(QStringLiteral("path"), repo.path);
        settings.setValue(QStringLiteral("url"), repo.url);
        settings.setValue(QStringLiteral("username"), repo.username);
        settings.setValue(QStringLiteral("password"), repo.password);
        settings.setValue(QStringLiteral("isActive"), repo.isActive);
        settings.endGroup();
    }
    settings.endGroup();
    settings.sync();
}

} // namespace svnsync
