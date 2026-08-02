#include "ui/MainWindow.h"

#include "core/LogStore.h"
#include "core/RepoManager.h"
#include "core/SyncEngine.h"
#include "ui/AddRepoDialog.h"
#include "ui/AboutDialog.h"
#include "ui/ConflictDialog.h"
#include "ui/RepoDetailPage.h"
#include "ui/RepoListPanel.h"
#include "ui/SettingsDialog.h"

#include <QAction>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QStatusBar>
#include <QStyle>
#include <QSystemTrayIcon>

#include <svnplus/SvnClient.h>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_manager(std::make_unique<svnsync::RepoManager>(this))
{
    setWindowTitle(QStringLiteral("SvnSyncDrive"));
    resize(1100, 640);

    auto *central = new QWidget(this);
    auto *layout = new QHBoxLayout(central);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    m_sidebar = new RepoListPanel(central);
    m_sidebar->setMinimumWidth(123);
    m_sidebar->setMaximumWidth(190);
    layout->addWidget(m_sidebar);

    m_pages = new QStackedWidget(central);
    m_emptyPage = new QLabel(
        QStringLiteral("选择一个仓库查看文件与同步日志，\n或点击左侧「+ 添加仓库」。"),
        m_pages);
    m_emptyPage->setAlignment(Qt::AlignCenter);
    m_pages->addWidget(m_emptyPage);
    layout->addWidget(m_pages, 1);

    setCentralWidget(central);

    m_statusText = new QLabel(QStringLiteral("就绪"), this);
    statusBar()->addWidget(m_statusText, 1);
    auto *versionLabel = new QLabel(
        QStringLiteral("SvnSyncDrive %1 · libsvnplus %2 · Qt %3")
            .arg(QStringLiteral("0.2.0"),
                 QString::fromStdString(SvnPlus::SvnClient::version().toString()),
                 QString::fromLatin1(qVersion())),
        this);
    versionLabel->setStyleSheet(QStringLiteral("color: #888;"));
    statusBar()->addPermanentWidget(versionLabel);

    connect(m_sidebar, &RepoListPanel::repositorySelected,
            this, [this](const QString &name) {
                // Clicking a repository activates it and brings it to the
                // top of the list; whoever drops below the monitoring limit
                // stops syncing.
                m_manager->promote(name);
                selectRepository(name);
            });
    connect(m_sidebar, &RepoListPanel::addRequested,
            this, &MainWindow::onAddRequested);
    connect(m_sidebar, &RepoListPanel::settingsRequested,
            this, &MainWindow::onSettingsRequested);
    connect(m_sidebar, &RepoListPanel::aboutRequested, this, [this] {
        AboutDialog dlg(this);
        dlg.exec();
    });

    connect(m_manager.get(), &svnsync::RepoManager::repositoryListChanged,
            this, &MainWindow::rebuildSidebar);
    connect(m_manager.get(), &svnsync::RepoManager::repositoryStateChanged,
            this, &MainWindow::onStateChanged);
    connect(m_manager.get(), &svnsync::RepoManager::notification,
            this, &MainWindow::showNotification);
    connect(m_manager.get(), &svnsync::RepoManager::filesChanged,
            this, [this](const QString &name) {
                if (RepoDetailPage *page = pageFor(name))
                    page->refreshFiles();
            });
    connect(m_manager.get(), &svnsync::RepoManager::conflictDetected,
            this, &MainWindow::onConflictDetected);

    m_manager->load();
    rebuildSidebar();

    setupTrayIcon();

    // Select the first running repository (prefer the persisted Active one).
    const auto repos = m_manager->repositories();
    const auto pick = [&repos]() -> QString {
        for (const auto &r : repos)
            if (r.state == svnsync::RepoState::Active)
                return r.name;
        for (const auto &r : repos)
            if (r.running())
                return r.name;
        return repos.isEmpty() ? QString() : repos.first().name;
    };
    const QString initial = pick();
    if (!initial.isEmpty())
        selectRepository(initial);
}

MainWindow::~MainWindow() = default;

bool MainWindow::startMinimizedToTray() const
{
    return m_trayIcon && m_manager->config().startMinimizedToTray;
}

void MainWindow::startRepository(const svnsync::Repository &repository)
{
    m_manager->addRepository(repository);
    selectRepository(repository.name);
}

void MainWindow::rebuildSidebar()
{
    m_sidebar->setRepositories(m_manager->repositories());
    if (!m_selectedName.isEmpty())
        m_sidebar->setSelectedName(m_selectedName);
}

void MainWindow::selectRepository(const QString &name)
{
    const auto *repo = m_manager->repository(name);
    if (!repo)
        return;

    // The previously shown repo drops back to Background if it was running.
    if (!m_selectedName.isEmpty() && m_selectedName != name) {
        const auto *old = m_manager->repository(m_selectedName);
        if (old && old->running())
            m_manager->setState(m_selectedName, svnsync::RepoState::Background);
    }

    m_selectedName = name;

    // Selecting a running repo makes it Active; a Deactive repo is shown
    // but stays stopped until the user enables it.
    if (repo->running() && repo->state != svnsync::RepoState::Active)
        m_manager->setState(name, svnsync::RepoState::Active);

    RepoDetailPage *page = pageFor(name);
    m_pages->setCurrentWidget(page);
    m_sidebar->setSelectedName(name);

    setGlobalStatus(QStringLiteral("%1 · %2").arg(name, repo->path));
}

void MainWindow::onStateChanged(const QString &name, svnsync::RepoState state)
{
    m_sidebar->updateState(name, state);
    if (RepoDetailPage *page = pageFor(name)) {
        page->setState(state);
        page->setEngine(state == svnsync::RepoState::Deactive
                || state == svnsync::RepoState::AuthFailed
            ? nullptr : m_manager->engine(name));
    }
}

void MainWindow::onRemoveRequested(const QString &name)
{
    const auto answer = QMessageBox::question(
        this, QStringLiteral("移除仓库"),
        QStringLiteral("确定要移除仓库「%1」吗？\n（不会删除本地文件，只会停止同步并从列表移除。）")
            .arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    m_manager->removeRepository(name);

    if (RepoDetailPage *page = m_pagesByRepo.take(name)) {
        m_pages->removeWidget(page);
        page->deleteLater();
    }

    if (m_selectedName == name) {
        m_selectedName.clear();
        const auto repos = m_manager->repositories();
        if (!repos.isEmpty())
            selectRepository(repos.first().name);
        else
            m_pages->setCurrentWidget(m_emptyPage);
    }
}

void MainWindow::onAddRequested()
{
    AddRepoDialog dialog(false, this);
    dialog.setTrustServerCertificate(m_manager->config().trustServerCertificate);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const svnsync::Repository repo = dialog.repository();
    m_manager->addRepository(repo);
    selectRepository(repo.name);
}

void MainWindow::onSettingsRequested()
{
    SettingsDialog dialog(this);
    dialog.setConfig(m_manager->config());
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_manager->setConfig(dialog.config());
    setGlobalStatus(QStringLiteral("设置已保存"));
}

void MainWindow::setupTrayIcon()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setIcon(windowIcon().isNull()
        ? style()->standardIcon(QStyle::SP_ComputerIcon)
        : windowIcon());
    m_trayIcon->setToolTip(QStringLiteral("SvnSyncDrive"));
    if (m_trayIcon->icon().isNull())
        m_trayIcon->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));

    auto *menu = new QMenu(this);
    auto *showAction = menu->addAction(QStringLiteral("显示主窗口"));
    auto *quitAction = menu->addAction(QStringLiteral("退出"));
    menu->addSeparator();
    m_trayIcon->setContextMenu(menu);

    connect(showAction, &QAction::triggered, this, &MainWindow::showWindowFromTray);
    connect(quitAction, &QAction::triggered, this, &MainWindow::quitApplication);
    connect(m_trayIcon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick)
                    showWindowFromTray();
            });

    m_trayIcon->show();
}

void MainWindow::showWindowFromTray()
{
    show();
    setWindowState(windowState() & ~Qt::WindowMinimized);
    raise();
    activateWindow();
}

void MainWindow::quitApplication()
{
    // Engines and their worker threads are torn down by RepoManager's
    // destructor once the event loop exits.
    qApp->quit();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_trayIcon && m_manager->config().minimizeToTray) {
        hide();
        event->ignore();
        return;
    }
    event->accept();
}

void MainWindow::showNotification(const QString &name, const QString &message)
{
    logToRepo(name, message);
    if (name == m_selectedName)
        setGlobalStatus(QStringLiteral("%1 · %2").arg(name, message));
}

void MainWindow::logToRepo(const QString &name, const QString &message)
{
    svnsync::LogStore::append(name, message, m_manager->config().maxLogsPerRepo);
    if (RepoDetailPage *page = pageFor(name))
        page->appendLog(message);
}

void MainWindow::onConflictDetected(const QString &name, const QStringList &paths)
{
    logToRepo(name, QStringLiteral("[冲突] %1 个文件：%2")
              .arg(paths.size()).arg(paths.join(QStringLiteral(", "))));
    svnsync::SyncEngine *engine = m_manager->engine(name);
    if (!engine)
        return;
    ConflictDialog dlg(engine, paths, this);
    dlg.exec();
    if (RepoDetailPage *page = pageFor(name))
        page->refreshFiles();
}

void MainWindow::scanConflicts(const QString &name)
{
    const auto *repo = m_manager->repository(name);
    if (!repo)
        return;
    svnsync::SyncEngine *engine = m_manager->engine(name);
    if (!engine) {
        logToRepo(name, QStringLiteral("仓库已停用，无法扫描冲突。"));
        return;
    }
    svnsync::CommandItem item;
    item.command = svnsync::Command::GetConflictedFiles;
    item.path = repo->path;
    engine->submit(item, [this, name](const svnsync::CommandResult &r) {
        QStringList paths;
        if (r.success && !r.value.isEmpty()) {
            const QStringList parts = r.value.split(QLatin1Char(';'), Qt::SkipEmptyParts);
            for (const QString &p : parts)
                paths.append(QString::fromUtf8(p.toUtf8()));
        }
        if (!paths.isEmpty()) {
            onConflictDetected(name, paths);
        } else {
            logToRepo(name, QStringLiteral("没有发现冲突文件。"));
        }
    });
}

RepoDetailPage *MainWindow::pageFor(const QString &name)
{
    auto it = m_pagesByRepo.constFind(name);
    if (it != m_pagesByRepo.constEnd())
        return it.value();

    const auto *repo = m_manager->repository(name);
    if (!repo)
        return nullptr;

    auto *page = new RepoDetailPage(m_pages);
    page->setRepository(*repo);
    page->setLogHistory(svnsync::LogStore::history(name, m_manager->config().maxLogsPerRepo));
    if (repo->running())
        page->setEngine(m_manager->engine(name));

    connect(page, &RepoDetailPage::syncRequested, this, [this, name] {
        m_manager->syncNow(name);
    });
    connect(page, &RepoDetailPage::toggleStateRequested, this, [this, name] {
        const auto *r = m_manager->repository(name);
        if (!r)
            return;
        // Enabling promotes the repo to the monitored set; disabling demotes
        // it to the bottom of the list where it stays stopped.
        if (r->running())
            m_manager->demote(name);
        else
            m_manager->promote(name);
        if (m_selectedName == name) {
            const auto *cur = m_manager->repository(name);
            if (cur && cur->running())
                m_manager->setState(name, svnsync::RepoState::Active);
        }
    });
    connect(page, &RepoDetailPage::removeRequested, this, [this, name] {
        onRemoveRequested(name);
    });
    connect(page, &RepoDetailPage::configureRequested, this, [this, name] {
        const auto *repo = m_manager->repository(name);
        if (!repo)
            return;
        AddRepoDialog dialog(true, this);
        dialog.setRepository(*repo);
        dialog.setTrustServerCertificate(m_manager->config().trustServerCertificate);
        if (dialog.exec() != QDialog::Accepted)
            return;
        const auto updated = dialog.repository();
        m_manager->setCredentials(name, updated.username, updated.password);
        logToRepo(name, QStringLiteral("已更新仓库凭据。"));
    });
    connect(page, &RepoDetailPage::conflictScanRequested, this, [this, name] {
        scanConflicts(name);
    });

    m_pagesByRepo.insert(name, page);
    m_pages->addWidget(page);
    return page;
}

void MainWindow::setGlobalStatus(const QString &text)
{
    m_statusText->setText(text);
}
