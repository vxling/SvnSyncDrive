#include "core/RepoManager.h"
#include "core/Repository.h"

#include <QCoreApplication>
#include <QSettings>
#include <cstdio>

using namespace svnsync;

static void probe()
{
    QSettings settings(QStringLiteral("SvnSyncDrive"), QStringLiteral("SvnSyncDrive"));
    std::printf("filePath: %s\n", qPrintable(settings.fileName()));
    std::printf("top-level groups:\n");
    for (const auto &g : settings.childGroups())
        std::printf("  %s\n", qPrintable(g));
    settings.beginGroup(QStringLiteral("repositories"));
    std::printf("repositories group children:\n");
    for (const auto &g : settings.childGroups())
        std::printf("  %s\n", qPrintable(g));
    settings.endGroup();
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("SvnSyncDrive");
    app.setOrganizationName("SvnSyncDrive");

    probe();

    RepoManager manager;
    manager.load();

    std::printf("repositories loaded: %d\n", int(manager.repositories().size()));
    for (const auto &repo : manager.repositories()) {
        std::printf("  %s | %s | state=%d | engine=%p\n",
                    qPrintable(repo.name), qPrintable(repo.path),
                    int(repo.state), static_cast<void *>(manager.engine(repo.name)));
    }
    return 0;
}

