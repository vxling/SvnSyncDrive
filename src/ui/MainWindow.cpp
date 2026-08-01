#include "ui/MainWindow.h"

#include "core/RepoManager.h"
#include "core/SyncEngine.h"
#include "ui/AddRepoDialog.h"
#include "ui/RepoDetailPage.h"
#include "ui/RepoListPanel.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QStatusBar>
#include <QUrl>

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
    m_sidebar->setMinimumWidth(220);
    m_sidebar->setMaximumWidth(340);
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
    statusBar()->showMessage(
        QStringLiteral("SvnSyncDrive %1 · libsvnplus %2 · Qt %3")
            .arg(QStringLiteral("0.2.0"),
                 QString::fromStdString(SvnPlus::SvnClient::version().toString()),
                 QString::fromLatin1(qVersion())));

    connect(m_sidebar, &RepoListPanel::repositorySelected,
            this, &MainWindow::selectRepository);
    connect(m_sidebar, &RepoListPanel::removeRequested,
            this, &MainWindow::onRemoveRequested);
    connect(m_sidebar, &RepoListPanel::addRequested,
            this, &MainWindow::onAddRequested);

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
    AddRepoDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const svnsync::Repository repo = dialog.repository();
    m_manager->addRepository(repo);
    selectRepository(repo.name);
}

void MainWindow::showNotification(const QString &name, const QString &message)
{
    if (RepoDetailPage *page = pageFor(name))
        page->appendLog(message);
    if (name == m_selectedName)
        setGlobalStatus(QStringLiteral("%1 · %2").arg(name, message));
}

void MainWindow::onConflictDetected(const QString &name, const QStringList &paths)
{
    const QString text = QStringLiteral("仓库「%1」有 %2 个文件冲突：\n%3")
        .arg(name).arg(paths.size()).arg(paths.join(QLatin1Char('\n')));
    if (RepoDetailPage *page = pageFor(name))
        page->appendLog(QStringLiteral("[冲突] %1 个文件：%2")
                        .arg(paths.size()).arg(paths.join(QStringLiteral(", "))));
    QMessageBox::warning(this, QStringLiteral("SVN 冲突"), text);
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
    if (repo->running())
        page->setEngine(m_manager->engine(name));

    connect(page, &RepoDetailPage::syncRequested, this, [this, name] {
        m_manager->syncNow(name);
    });
    connect(page, &RepoDetailPage::toggleStateRequested, this, [this, name] {
        const auto *r = m_manager->repository(name);
        if (!r)
            return;
        // The page is only ever shown for the selected (Active) repo, so
        // enabling goes straight to Active, disabling to Deactive.
        const auto next = r->running() ? svnsync::RepoState::Deactive
                                       : svnsync::RepoState::Active;
        m_manager->setState(name, next);
    });
    connect(page, &RepoDetailPage::removeRequested, this, [this, name] {
        onRemoveRequested(name);
    });
    connect(page, &RepoDetailPage::openInExplorerRequested, this, [this, name] {
        const auto *r = m_manager->repository(name);
        if (r)
            QDesktopServices::openUrl(QUrl::fromLocalFile(r->path));
    });

    m_pagesByRepo.insert(name, page);
    m_pages->addWidget(page);
    return page;
}

void MainWindow::setGlobalStatus(const QString &text)
{
    m_statusText->setText(text);
}
