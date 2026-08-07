#include <QApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>

#include "core/ConfigStore.h"
#include "core/I18n.h"
#include "core/Repository.h"
#include "ui/MainWindow.h"

#include "build_info.h"

namespace {

/** Unique per-user name so the single-instance lock does not collide
 *  across different Windows accounts. */
QString instanceKey()
{
    const QString user = qEnvironmentVariable("USERNAME");
    if (!user.isEmpty())
        return QStringLiteral("SvnSyncDrive-%1").arg(user);
    return QStringLiteral("SvnSyncDrive");
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("SvnSyncDrive");
    app.setOrganizationName("SvnSyncDrive");
    app.setApplicationVersion(QStringLiteral(SVNSYNC_VERSION));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

    // Apply the persisted UI language before any widget is constructed, so
    // everything built later already shows the right language.
    I18n::setLanguage(svnsync::ConfigStore::loadGlobalConfig().language);

    const QString key = instanceKey();

    // Single instance: if another process already listens, ask it to show
    // its window and exit instead of starting a second copy.
    {
        QLocalSocket probe;
        probe.connectToServer(key);
        if (probe.waitForConnected(500)) {
            probe.write("show\n");
            probe.flush();
            probe.waitForBytesWritten(500);
            return 0;
        }
    }

    QLocalServer server;
    const bool listening = server.listen(key);
    if (!listening && server.serverError() == QAbstractSocket::AddressInUseError
        && QLocalServer::removeServer(key))
        server.listen(key);

    MainWindow window;
    if (window.startMinimizedToTray())
        window.hide();
    else
        window.show();

    // A second launch asks the running instance to bring its window up.
    QObject::connect(&server, &QLocalServer::newConnection, &window, [&server, &window] {
        while (QLocalSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [socket, &window] {
                if (socket->canReadLine() && socket->readLine() == "show\n")
                    window.showWindowFromTray();
            });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    });

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
