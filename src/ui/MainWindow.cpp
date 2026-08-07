#include "ui/MainWindow.h"

#include "core/I18n.h"
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

#include "build_info.h"

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
        I18n::translate("选择一个仓库查看文件与同步日志，\n或点击左侧「+ 添加仓库」。"),
        m_pages);
    m_emptyPage->setAlignment(Qt::AlignCenter);
    m_pages->addWidget(m_emptyPage);
    layout->addWidget(m_pages, 1);

    setCentralWidget(central);

    m_statusText = new QLabel(I18n::translate("就绪"), this);
    statusBar()->addWidget(m_statusText, 1);
    auto *versionLabel = new QLabel(
        QStringLiteral("SvnSyncDrive %1 · build %2")
            .arg(QStringLiteral(SVNSYNC_VERSION),
                 QStringLiteral(SVNSYNC_BUILD_TIMESTAMP)),
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
    connect(m_manager.get(), &svnsync::RepoManager::conflictResolved,
            this, &MainWindow::onConflictResolved);

    m_manager->load();
    rebuildSidebar();

    setupTrayIcon();

    connect(I18n::instance(), &I18n::languageChanged, this,
            [this] { retranslateUi(); });

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
        this, I18n::translate("移除仓库"),
        I18n::translate("确定要移除仓库「%1」吗？\n（不会删除本地文件，只会停止同步并从列表移除。）")
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
    const svnsync::GlobalConfig config = dialog.config();
    m_manager->setConfig(config);
    I18n::setLanguage(config.language);
    setGlobalStatus(I18n::translate("设置已保存"));
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
    m_trayShowAction = menu->addAction(I18n::translate("显示主窗口"));
    m_trayQuitAction = menu->addAction(I18n::translate("退出"));
    menu->addSeparator();
    m_trayIcon->setContextMenu(menu);

    connect(m_trayShowAction, &QAction::triggered, this, &MainWindow::showWindowFromTray);
    connect(m_trayQuitAction, &QAction::triggered, this, &MainWindow::quitApplication);
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

void MainWindow::onConflictDetected(const QString &name, const QStringList &paths,
                                    const QStringList &treeConflictPaths)
{
    logToRepo(name, I18n::translate("[冲突] %1 个文件：%2")
              .arg(paths.size()).arg(paths.join(QStringLiteral(", "))));
    if (!treeConflictPaths.isEmpty())
        logToRepo(name, I18n::translate("[冲突] 其中 %1 个为树冲突（将按保留当前状态处理）：%2")
                  .arg(treeConflictPaths.size())
                  .arg(treeConflictPaths.join(QStringLiteral(", "))));
    svnsync::SyncEngine *engine = m_manager->engine(name);
    if (!engine)
        return;
    ConflictDialog dlg(engine, paths, treeConflictPaths, this);
    dlg.exec();
    if (RepoDetailPage *page = pageFor(name))
        page->refreshFiles();
}

void MainWindow::onConflictResolved(const QString &name, const QString &path,
                                    int choiceCode, bool treeConflict,
                                    bool success, const QString &error)
{
    const QString choice = treeConflict
        ? I18n::translate("保留当前工作副本状态")
        : svnsync::SyncEngine::conflictChoiceName(choiceCode);
    if (success)
        logToRepo(name, I18n::translate("[冲突] 已解决：%1（%2）").arg(path, choice));
    else
        logToRepo(name, I18n::translate("[冲突] 解决失败：%1（%2）：%3")
                      .arg(path, choice, error));
}

void MainWindow::scanConflicts(const QString &name)
{
    const auto *repo = m_manager->repository(name);
    if (!repo)
        return;
    svnsync::SyncEngine *engine = m_manager->engine(name);
    if (!engine) {
        logToRepo(name, I18n::translate("仓库已停用，无法扫描冲突。"));
        return;
    }
    svnsync::CommandItem item;
    item.command = svnsync::Command::GetConflictedFiles;
    item.path = repo->path;
    engine->submit(item, [this, name](const svnsync::CommandResult &r) {
        QStringList paths;
        QStringList treePaths;
        if (r.success && !r.value.isEmpty()) {
            const QStringList parts = r.value.split(QLatin1Char(';'), Qt::SkipEmptyParts);
            for (const QString &p : parts)
                paths.append(QString::fromUtf8(p.toUtf8()));
        }
        if (r.success)
            treePaths = r.treeConflicts;
        if (!paths.isEmpty()) {
            onConflictDetected(name, paths, treePaths);
        } else {
            logToRepo(name, I18n::translate("没有发现冲突文件。"));
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
        logToRepo(name, I18n::translate("已更新仓库凭据。"));
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

void MainWindow::retranslateUi()
{
    m_emptyPage->setText(
        I18n::translate("选择一个仓库查看文件与同步日志，\n或点击左侧「+ 添加仓库」。"));
    if (m_trayShowAction)
        m_trayShowAction->setText(I18n::translate("显示主窗口"));
    if (m_trayQuitAction)
        m_trayQuitAction->setText(I18n::translate("退出"));
    m_sidebar->retranslate();
    for (auto it = m_pagesByRepo.constBegin(); it != m_pagesByRepo.constEnd(); ++it)
        it.value()->retranslate();
}
