#include "MainWindow.h"

#include <QLabel>
#include <QStatusBar>
#include <QVBoxLayout>

#include <svnplus/SvnClient.h>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("SvnSyncDrive"));
    resize(900, 600);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->addWidget(new QLabel(QStringLiteral("SvnSyncDrive - prototype"), central));
    setCentralWidget(central);

    const auto svn = SvnPlus::SvnClient::version().toString();
    statusBar()->showMessage(
        QString::fromStdString(svn) +
        QStringLiteral("  |  Qt %1").arg(QString::fromLatin1(qVersion())));
}
