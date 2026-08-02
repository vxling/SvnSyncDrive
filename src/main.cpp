#include <QApplication>
#include <QIcon>

#include "core/Repository.h"
#include "ui/MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("SvnSyncDrive");
    app.setOrganizationName("SvnSyncDrive");
    app.setApplicationVersion("0.2.0");
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

    MainWindow window;
    window.show();

    // Optional live mode: SvnSyncDrive --repo <wc> <url> [user] [pass]
    const QStringList args = app.arguments();
    const int idx = args.indexOf(QStringLiteral("--repo"));
    if (idx >= 0 && idx + 2 < args.size()) {
        svnsync::Repository repo;
        repo.name = QStringLiteral("CLI");
        repo.path = args.at(idx + 1);
        repo.url = args.at(idx + 2);
        if (idx + 3 < args.size())
            repo.username = args.at(idx + 3);
        if (idx + 4 < args.size())
            repo.password = args.at(idx + 4);
        repo.state = svnsync::RepoState::Active;
        window.startRepository(repo);
    }

    return app.exec();
}
