#include "MainWindow.h"

#include "core/SyncEngine.h"

#include <QDateTime>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
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

    auto *header = new QLabel(
        QStringLiteral("SvnSyncDrive - prototype  |  %1  |  Qt %2")
            .arg(QString::fromStdString(SvnPlus::SvnClient::version().toString()),
                 QString::fromLatin1(qVersion())),
        central);
    layout->addWidget(header);

    m_log = new QPlainTextEdit(central);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    layout->addWidget(m_log, 1);

    m_syncButton = new QPushButton(QStringLiteral("立即同步"), central);
    m_syncButton->setEnabled(false);
    layout->addWidget(m_syncButton);

    connect(m_syncButton, &QPushButton::clicked, this, [this] {
        if (m_engine)
            m_engine->syncNow();
    });

    setCentralWidget(central);

    const auto svn = SvnPlus::SvnClient::version().toString();
    statusBar()->showMessage(
        QString::fromStdString(svn) +
        QStringLiteral("  |  Qt %1").arg(QString::fromLatin1(qVersion())));

    log(QStringLiteral("SvnSyncDrive started (no repository configured; "
                       "pass --repo <wc> <url> [user] [pass])"));
}

MainWindow::~MainWindow()
{
    if (m_engine)
        m_engine->stop();
}

void MainWindow::startRepository(const svnsync::Repository &repository)
{
    if (m_engine) {
        m_engine->stop();
        m_engine.reset();
    }

    m_engine = std::make_unique<svnsync::SyncEngine>(repository, this);

    connect(m_engine.get(), &svnsync::SyncEngine::syncNotification, this, [this](const QString &m) {
        log(m);
    });
    connect(m_engine.get(), &svnsync::SyncEngine::conflictDetected, this, [this](const QStringList &c) {
        log(QStringLiteral("[冲突] %1 个文件冲突").arg(c.size()));
        for (const auto &p : c)
            log(QStringLiteral("   %1").arg(p));
    });
    connect(m_engine.get(), &svnsync::SyncEngine::filesChanged, this, [this] {
        log(QStringLiteral("工作副本文件已变化"));
    });

    m_syncButton->setEnabled(true);
    log(QStringLiteral("开始同步仓库: %1").arg(repository.path));
    log(QStringLiteral("   URL: %1").arg(repository.url));
    m_engine->start();
}

void MainWindow::log(const QString &message)
{
    m_log->appendPlainText(
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")) +
        QStringLiteral("  ") + message);
}
