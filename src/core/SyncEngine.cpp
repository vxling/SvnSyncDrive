#include "core/SyncEngine.h"

#include "core/RepoWatcher.h"
#include "core/SvnWorker.h"

#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QSet>

namespace svnsync {

namespace {

bool pathLooksLikeFile(const QString &path)
{
    return path.section(QLatin1Char('/'), -1).contains(QLatin1Char('.'));
}

} // namespace

SyncEngine::SyncEngine(const Repository &repository, QObject *parent)
    : QObject(parent)
    , m_repo(repository)
    , m_worker(std::make_unique<SvnWorker>(nullptr))
    , m_watcher(std::make_unique<RepoWatcher>(nullptr))
{
    m_pollTimer.setInterval(60 * 1000);
    m_fullSyncTimer.setInterval(15 * 60 * 1000);

    connect(&m_pollTimer, &QTimer::timeout, this, &SyncEngine::poll);
    connect(&m_fullSyncTimer, &QTimer::timeout, this, &SyncEngine::fullSync);
    connect(m_worker.get(), &SvnWorker::resultReady,
            this, &SyncEngine::onResult, Qt::QueuedConnection);
    connect(m_watcher.get(), &RepoWatcher::filesChanged,
            this, &SyncEngine::onWatcherBatch, Qt::QueuedConnection);
}

SyncEngine::~SyncEngine()
{
    stop();
}

void SyncEngine::start()
{
    if (m_started)
        return;
    m_started = true;

    m_worker->start();
    m_worker->setCredentials(m_repo.username, m_repo.password);
    m_worker->setTrustServerCertificate(true);

    if (!m_watcher->start(m_repo.path))
        notify(tr("无法监听目录: %1").arg(m_repo.path));

    m_pollTimer.start();
    m_fullSyncTimer.start();

    // Immediate downward sync to pick up other machines' changes.
    poll();
}

void SyncEngine::stop()
{
    if (!m_started)
        return;
    m_pollTimer.stop();
    m_fullSyncTimer.stop();
    m_watcher->stop();
    m_worker->stop();
    m_started = false;
}

void SyncEngine::syncNow()
{
    if (m_started)
        scanAndCommit();
}

quint64 SyncEngine::submit(const CommandItem &item, Callback callback)
{
    CommandItem copy = item;
    copy.id = m_idCounter.fetch_add(1) + 1;
    if (callback)
        m_pending.insert(copy.id, std::move(callback));
    m_worker->submit(copy);
    return copy.id;
}

void SyncEngine::onResult(quint64 id, const CommandResult &result)
{
    const auto it = m_pending.constFind(id);
    if (it == m_pending.constEnd())
        return;
    Callback callback = it.value();
    m_pending.erase(it);
    callback(result);
}

// ───────────────────────────────────────────────────────────── upward sync ──

void SyncEngine::onWatcherBatch(const QStringList &paths)
{
    for (const auto &path : paths)
        enqueueFileChange(path);
    syncNow();
}

void SyncEngine::enqueueFileChange(const QString &path)
{
    const bool exists = QFileInfo::exists(path);
    const QString versionedQ = QStringLiteral("true");

    CommandItem query;
    query.command = Command::IsVersioned;
    query.path = path;

    if (!exists) {
        submit(query, [this, path, versionedQ](const CommandResult &r) {
            if (r.success && r.value == versionedQ) {
                CommandItem del;
                del.command = Command::Delete;
                del.path = path;
                submit(del);
            }
        });
        return;
    }

    submit(query, [this, path, versionedQ](const CommandResult &r) {
        if (r.success && r.value != versionedQ) {
            CommandItem add;
            add.command = Command::Add;
            add.path = path;
            submit(add);
        }
    });
}

void SyncEngine::scanAndCommit()
{
    if (m_scanning)
        return;
    m_scanning = true;

    CommandItem statusItem;
    statusItem.command = Command::Status;
    statusItem.path = m_repo.path;
    submit(statusItem, [this](const CommandResult &r) {
        if (!r.success) {
            notify(tr("批量同步扫描失败: %1").arg(r.error));
            finishScan();
            return;
        }
        handleScanStatus(r);
    });
}

void SyncEngine::handleScanStatus(const CommandResult &result)
{
    QStringList unversioned;
    QList<StatusEntry> changes;

    for (const auto &e : result.statuses) {
        if (!e.versioned) {
            if (e.nodeStatus == StatusKind::Unversioned && !isTempFile(e.path))
                unversioned << e.path;
            continue;
        }
        if (e.conflicted)
            continue;
        if (e.nodeStatus != StatusKind::Normal)
            changes.append(e);
    }

    for (const auto &p : unversioned) {
        CommandItem add;
        add.command = Command::Add;
        add.path = p;
        submit(add);
    }

    const auto groups = groupByDir(changes, m_repo.path);
    m_pendingCommits = 0;
    for (const auto &g : groups) {
        CommandItem commit;
        commit.command = Command::Commit;
        commit.path = g.dir;
        commit.message = commitMessage(g.dir, g.count, g.firstFile);
        ++m_pendingCommits;
        submit(commit, [this](const CommandResult &) {
            --m_pendingCommits;
            if (m_pendingCommits <= 0)
                finishScan();
        });
    }

    if (m_pendingCommits == 0)
        finishScan();
}

void SyncEngine::finishScan()
{
    if (!m_scanning)
        return;
    m_scanning = false;
    emit filesChanged();
    notify(tr("批量同步完成"));
}

bool SyncEngine::isTempFile(const QString &path)
{
    const QString fileName = path.section(QLatin1Char('/'), -1);
    return fileName.startsWith(QStringLiteral("~$"))
        || fileName.startsWith(QLatin1Char('~'))
        || fileName.endsWith(QStringLiteral(".tmp"))
        || fileName.endsWith(QStringLiteral(".temp"))
        || fileName == QStringLiteral(".DS_Store");
}

QList<SyncEngine::CommitGroup> SyncEngine::groupByDir(const QList<StatusEntry> &changes,
                                                      const QString &repoRoot)
{
    const QString root = QDir::fromNativeSeparators(repoRoot).trimmed();
    QMap<QString, CommitGroup> byDir;

    for (const auto &e : changes) {
        const QString path = QDir::fromNativeSeparators(e.path);
        QString dir = pathLooksLikeFile(path)
            ? path.section(QLatin1Char('/'), 0, -2)
            : path;
        if (dir.isEmpty() || dir == root)
            dir = root;
        CommitGroup &g = byDir[dir];
        ++g.count;
        if (g.firstFile.isEmpty())
            g.firstFile = path.section(QLatin1Char('/'), -1);
    }

    QStringList dirs = byDir.keys();
    std::sort(dirs.begin(), dirs.end(), [](const QString &a, const QString &b) {
        return a.count(QLatin1Char('/')) > b.count(QLatin1Char('/'));
    });

    QList<CommitGroup> result;
    result.reserve(dirs.size());
    for (const auto &dir : dirs)
        result.append(byDir.value(dir));
    return result;
}

QString SyncEngine::commitMessage(const QString &dir, int count, const QString &firstFile)
{
    if (count == 1)
        return QStringLiteral("Auto-sync: %1").arg(firstFile);
    const QString name = dir.section(QLatin1Char('/'), -1);
    return QStringLiteral("Auto-sync: %1 files in %2").arg(count).arg(name);
}

// ─────────────────────────────────────────────────────────── downward sync ──

void SyncEngine::poll()
{
    if (m_polling || m_scanning)
        return;
    m_polling = true;

    CommandItem localItem;
    localItem.command = Command::GetRevision;
    localItem.path = m_repo.path;
    submit(localItem, [this](const CommandResult &lr) {
        if (!lr.success) {
            m_polling = false;
            return;
        }
        const qlonglong localRev = lr.revision;

        CommandItem headItem;
        headItem.command = Command::GetHeadRevision;
        headItem.path = m_repo.path;
        headItem.repoUrl = m_repo.url;
        submit(headItem, [this, localRev](const CommandResult &hr) {
            if (!hr.success) {
                m_polling = false;
                return;
            }
            const qlonglong serverRev = hr.revision;
            if (serverRev <= localRev) {
                m_polling = false;
                return;
            }
            startUpdateInChunks(serverRev, localRev);
        });
    });
}

void SyncEngine::fullSync()
{
    if (m_polling)
        return;
    if (m_scanning)
        return;
    scanAndCommit();
}

void SyncEngine::startUpdateInChunks(qlonglong serverRev, qlonglong localRev)
{
    CommandItem statusItem;
    statusItem.command = Command::GetServerUpdatePaths;
    statusItem.path = m_repo.path;
    statusItem.checkOutOfDate = true;
    submit(statusItem, [this, serverRev, localRev](const CommandResult &r) {
        if (!r.success) {
            notify(tr("获取远端变更失败: %1").arg(r.error));
            m_polling = false;
            return;
        }

        QStringList remotePaths;
        for (const auto &e : r.statuses)
            if (e.outOfDate)
                remotePaths << e.path;
        if (remotePaths.isEmpty()) {
            m_polling = false;
            return;
        }

        const QStringList dirs = mergeToDirs(remotePaths, m_repo.path);
        m_pendingUpdates = 0;
        for (const auto &dir : dirs) {
            CommandItem upd;
            upd.command = Command::Update;
            upd.path = m_repo.path;
            upd.updatePaths = { dir };
            ++m_pendingUpdates;
            submit(upd, [this, serverRev, localRev](const CommandResult &) {
                --m_pendingUpdates;
                if (m_pendingUpdates <= 0)
                    afterUpdateDone(serverRev, localRev);
            });
        }
        if (m_pendingUpdates == 0)
            afterUpdateDone(serverRev, localRev);
    });
}

void SyncEngine::afterUpdateDone(qlonglong serverRev, qlonglong localRev)
{
    detectConflicts([this, serverRev, localRev]() {
        notify(tr("已从服务器更新 r%1 → r%2").arg(localRev).arg(serverRev));
        emit filesChanged();
        m_polling = false;
    });
}

void SyncEngine::detectConflicts(const std::function<void()> &done)
{
    CommandItem statusItem;
    statusItem.command = Command::Status;
    statusItem.path = m_repo.path;
    submit(statusItem, [this, done](const CommandResult &r) {
        QStringList conflicted;
        if (r.success) {
            for (const auto &e : r.statuses)
                if (e.conflicted)
                    conflicted << e.path;
        }
        if (!conflicted.isEmpty())
            emit conflictDetected(conflicted);
        if (done)
            done();
    });
}

QStringList SyncEngine::mergeToDirs(const QStringList &paths, const QString &repoRoot)
{
    const QString root = QDir::fromNativeSeparators(repoRoot).trimmed();
    QSet<QString> dirs;
    for (const auto &path : paths) {
        const QString normalized = QDir::fromNativeSeparators(path);
        const QString dir = pathLooksLikeFile(normalized)
            ? normalized.section(QLatin1Char('/'), 0, -2)
            : normalized;
        if (dir.isEmpty())
            dirs.insert(root);
        else
            dirs.insert(dir);
    }

    QStringList result = dirs.values();
    std::sort(result.begin(), result.end(), [](const QString &a, const QString &b) {
        return a.count(QLatin1Char('/')) > b.count(QLatin1Char('/'));
    });
    return result;
}

void SyncEngine::notify(const QString &message)
{
    emit syncNotification(message);
}

} // namespace svnsync
