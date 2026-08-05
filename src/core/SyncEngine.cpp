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

bool isServerCommand(Command command)
{
    switch (command) {
    case Command::GetRevision:
        // Nominally local, but for remote working copies (svn://, http(s)://)
        // `svn info WC@working` still performs a server round-trip and fails
        // with a network error when the repository is unreachable, so it must
        // participate in disconnect detection too.
    case Command::GetHeadRevision:
    case Command::GetServerUpdatePaths:
    case Command::Update:
    case Command::Commit:
    case Command::Checkout:
        return true;
    default:
        return false;
    }
}

/** Best-effort detection of SVN authentication errors. The app only sees the
 *  (English) error text from libsvn, so the match is textual; it covers the
 *  observed "No more credentials or we tried too many times. Authentication
 *  failed" chain and the related authorization variants. */
bool isAuthError(const QString &error)
{
    const QString e = error.toLower();
    return e.contains(QStringLiteral("no more credentials"))
        || e.contains(QStringLiteral("authentication failed"))
        || e.contains(QStringLiteral("authorization failed"))
        || e.contains(QStringLiteral("not authorized"));
}

/** Best-effort detection of network-access errors (timeouts, refused/reset
 *  connections, DNS failures, TLS/serf setup problems). Only these count
 *  towards the "disconnected" state; local working-copy errors do not. */
bool isNetworkError(const QString &error)
{
    const QString e = error.toLower();
    static const char *const kPatterns[] = {
        "unable to connect",
        "cannot connect",
        "can't connect",
        "connection timed out",
        "timed out",
        "connection refused",
        "actively refused",
        "no route to host",
        "network is unreachable",
        "network unreachable",
        "could not resolve host",
        "name or service not known",
        "host not found",
        "unknown host",
        "ssl setup",
        "ssl negotiation",
        "ssl connect",
        "certificate verify failed",
        "connection reset by peer",
        "connection reset",
        "closed by remote",
        "server unexpectedly closed the connection",
        "error running context",
        "connection to server closed",
        "bad gateway",
        "gateway timeout",
        "service unavailable",
        "could not contact proxy",
    };
    for (const char *pattern : kPatterns) {
        if (e.contains(QLatin1String(pattern)))
            return true;
    }
    return false;
}

} // namespace

SyncEngine::SyncEngine(const Repository &repository, QObject *parent)
    : QObject(parent)
    , m_repo(repository)
    , m_worker(std::make_unique<SvnWorker>(nullptr))
    , m_watcher(std::make_unique<RepoWatcher>(nullptr))
{
    m_pollTimer.setInterval(m_config.pollIntervalMs);
    m_fullSyncTimer.setInterval(m_config.fullSyncIntervalMs);

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

    m_worker->start(m_runnerFactory);
    m_worker->setCredentials(m_repo.username, m_repo.password);
    m_worker->setTrustServerCertificate(m_config.trustServerCertificate);
    m_worker->setNetworkTimeout(m_config.networkTimeoutSec);
    m_worker->setCommandTimeoutSec(m_config.networkTimeoutSec + 10);

    if (!m_watcher->start(m_repo.path))
        notify(tr("无法监听目录: %1").arg(m_repo.path));

    m_pollTimer.start();
    m_fullSyncTimer.start();

    // Immediate full sync on startup: a whole-tree `svn update` picks up
    // other machines' changes in one pass (replacing the old incremental
    // poll), then the full upward scan+commit runs when the update finishes.
    fullSync();
}

void SyncEngine::setConfig(const GlobalConfig &config)
{
    m_config = config;
    m_pollTimer.setInterval(m_config.pollIntervalMs);
    m_fullSyncTimer.setInterval(m_config.fullSyncIntervalMs);
    m_worker->setTrustServerCertificate(m_config.trustServerCertificate);
    m_worker->setNetworkTimeout(m_config.networkTimeoutSec);
    m_worker->setCommandTimeoutSec(m_config.networkTimeoutSec + 10);
}

void SyncEngine::setCredentials(const QString &username, const QString &password)
{
    m_repo.username = username;
    m_repo.password = password;
    m_worker->setCredentials(username, password);
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
    if (!m_started)
        return;
    if (m_scanning) {
        // A scan is already running; schedule a follow-up so this batch is
        // not silently dropped (the current scan's status snapshot may
        // already be past the changed files).
        m_rescanPending = true;
        return;
    }
    scanAndCommit();
}

quint64 SyncEngine::submit(const CommandItem &item, Callback callback)
{
    CommandItem copy = item;
    copy.id = m_idCounter.fetch_add(1) + 1;
    if (copy.repo.isEmpty())
        copy.repo = m_repo.name;
    if (callback)
        m_pending.insert(copy.id, std::move(callback));
    m_worker->submit(copy);
    return copy.id;
}

void SyncEngine::onResult(quint64 id, const CommandResult &result)
{
    const auto it = m_pending.constFind(id);
    if (it != m_pending.constEnd()) {
        Callback callback = it.value();
        m_pending.erase(it);
        callback(result);
    }
    // Classify last: on an auth failure RepoManager stops (and thereby
    // destroys) this engine in response to the emitted signal, so nothing
    // below may touch `this` afterwards.
    classify(result);
}

void SyncEngine::classify(const CommandResult &result)
{
    if (!isServerCommand(result.command))
        return;
    if (result.success) {
        // Any successful server access clears the failure streak and, if the
        // repo had been reported disconnected, restores its previous state.
        m_consecutiveNetworkFailures = 0;
        if (m_connectionLost) {
            m_connectionLost = false;
            emit connectionRestored();
        }
        return;
    }
    if (isAuthError(result.error)) {
        emit authenticationFailed();
        return;
    }
    // Only network-access errors prove the server is unreachable; local
    // working-copy problems (e.g. "None of the targets are working copies")
    // must not flip the repo into the disconnected state.
    if (!isNetworkError(result.error))
        return;
    const int threshold = qMax(1, m_config.disconnectThreshold);
    ++m_consecutiveNetworkFailures;
    if (!m_connectionLost && m_consecutiveNetworkFailures >= threshold) {
        m_connectionLost = true;
        emit connectionLost();
    }
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
    if (!QFileInfo::exists(path)) {
        CommandItem query;
        query.command = Command::IsVersioned;
        query.path = path;

        submit(query, [this, path](const CommandResult &r) {
            // Re-check existence: the path may have been recreated (e.g. by
            // an in-flight `svn update` replacing it) between the watcher
            // event and this query, in which case it must NOT be deleted.
            if (r.success && r.value == QStringLiteral("true")
                && !QFileInfo::exists(path)) {
                CommandItem del;
                del.command = Command::Delete;
                del.path = path;
                submit(del, [this, path](const CommandResult &dr) {
                    if (dr.success)
                        notify(tr("已删除: %1").arg(path));
                    else
                        notify(tr("删除失败: %1（%2）").arg(path).arg(dr.error));
                });
            }
        });
    }
    // Existing files need no direct action: the status scan triggered by the
    // batch auto-adds unversioned files and commits versioned changes.
}

void SyncEngine::scanAndCommit()
{
    if (m_scanning)
        return;
    m_scanning = true;
    m_scanAdds = 0;
    m_scanCommits = 0;

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
        if (e.nodeStatus != StatusKind::Normal) {
            changes.append(e);
        }
    }

    // Auto-add unversioned files, but never submit a second Add for a path
    // that already has one in flight: the worker dedups LocalWrite commands
    // by path, so a duplicate submit would never produce a result and would
    // corrupt our completion bookkeeping. Once all pending adds land we
    // re-scan so the freshly-added files get committed.
    if (m_config.autoAddUnversioned) {
        for (const auto &p : unversioned) {
            if (m_pendingAdds.contains(p))
                continue;
            m_pendingAdds.insert(p);
            CommandItem add;
            add.command = Command::Add;
            add.path = p;
            submit(add, [this, p](const CommandResult &r) {
                m_pendingAdds.remove(p);
                if (r.success) {
                    ++m_scanAdds;
                    notify(tr("已添加: %1").arg(p));
                } else {
                    notify(tr("添加失败: %1（%2）").arg(p).arg(r.error));
                }
                onAutoAddCompleted();
            });
        }
    }

    const auto groups = groupByDir(changes, m_repo.path);
    for (const auto &g : groups) {
        if (m_pendingCommits.contains(g.dir))
            continue;
        m_pendingCommits.insert(g.dir);
        CommandItem commit;
        commit.command = Command::Commit;
        commit.path = g.dir;
        commit.message = commitMessage(g.dir, g.count, g.firstFile);
        submit(commit, [this, g](const CommandResult &r) {
            m_pendingCommits.remove(g.dir);
            if (r.success && r.revision > 0) {
                m_lastLocalRev = qMax(m_lastLocalRev, r.revision);
                ++m_scanCommits;
                notify(tr("自动提交: %1（%2 个文件）→ r%3")
                           .arg(g.dir).arg(g.count).arg(r.revision));
            } else {
                notify(tr("提交失败: %1（%2）").arg(g.dir).arg(r.error));
            }
            onCommitCompleted();
        });
    }

    maybeFinishScan();
}

void SyncEngine::onAutoAddCompleted()
{
    if (m_pendingAdds.isEmpty()) {
        // The scan that found the unversioned files could not commit them
        // (they were not versioned yet), so re-scan to commit what we added.
        m_rescanPending = true;
        maybeFinishScan();
    }
}

void SyncEngine::onCommitCompleted()
{
    maybeFinishScan();
}

void SyncEngine::maybeFinishScan()
{
    if (m_scanning && m_pendingCommits.isEmpty() && m_pendingAdds.isEmpty())
        finishScan();
}

void SyncEngine::finishScan()
{
    if (!m_scanning)
        return;
    m_scanning = false;
    emit filesChanged();
    notify(tr("批量同步完成：新增 %1 个文件，提交 %2 个目录")
               .arg(m_scanAdds).arg(m_scanCommits));
    if (m_rescanPending) {
        m_rescanPending = false;
        scanAndCommit();
    }
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
        g.dir = dir;
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
        const qlonglong localRev = qMax(lr.revision, m_lastLocalRev);

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
    // 15-min periodic full sync: a whole-repo `svn update` (downward, in
    // one pass, no GetServerUpdatePaths chunking) followed by a full
    // upward scan+commit. Both actions always run, in that order.
    if (m_fullSyncing || m_polling || m_scanning)
        return;
    m_fullSyncing = true;
    // Watchdog: if a previous cycle ever left the watcher suspended (should
    // not happen, but recover defensively), restore it before proceeding so
    // monitoring is never silently lost.
    if (m_watcher->isSuspended()) {
        m_watcher->setSuspended(false);
        notify(tr("文件监听处于挂起状态，已自动恢复。"));
    }
    // Silence the watcher while update rewrites the working copy; resumed in
    // the update callback (both success and failure).
    m_watcher->setSuspended(true);

    CommandItem upd;
    upd.command = Command::Update;
    upd.path = m_repo.path;
    upd.bypassDedup = true;  // must run and report, even if a user update is queued
    submit(upd, [this](const CommandResult &r) {
        m_watcher->setSuspended(false);
        m_fullSyncing = false;
        if (!r.success) {
            // Nothing was updated, so the whole full-sync cycle failed: report
            // it as a failure and skip the pointless conflict scan + scan/commit
            // that would only produce more network errors.
            notify(tr("定时全量同步失败: %1").arg(r.error));
            return;
        }
        detectConflicts([this, r]() {
            emit filesChanged();
            bool countOk = false;
            const int count = r.value.toInt(&countOk);
            if (r.revision > 0) {
                if (countOk && count > 0)
                    notify(tr("定时全量同步完成（更新到 r%1，%2 个文件/目录）")
                               .arg(r.revision).arg(count));
                else
                    notify(tr("定时全量同步完成（更新到 r%1）").arg(r.revision));
            } else {
                notify(tr("定时全量同步完成"));
            }
            scanAndCommit();
        });
    });
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
        auto totalUpdated = std::make_shared<int>(0);
        m_pendingUpdates = 0;
        // Silence the watcher while update rewrites the working copy, so its
        // own writes never trigger a scan (which could mistake them for user
        // edits). Resumed in afterUpdateDone.
        m_watcher->setSuspended(true);
        for (const auto &dir : dirs) {
            CommandItem upd;
            upd.command = Command::Update;
            upd.path = m_repo.path;
            upd.updatePaths = { dir };
            // Never dedup-dropped: every chunk must report so the last one
            // reaches afterUpdateDone, which resumes the suspended watcher.
            // A dropped item would never fire its callback, leaving
            // m_pendingUpdates > 0 and the watcher suspended forever.
            upd.bypassDedup = true;
            ++m_pendingUpdates;
            submit(upd, [this, serverRev, localRev, remotePaths, totalUpdated](
                            const CommandResult &ur) {
                bool countOk = false;
                const int count = ur.value.toInt(&countOk);
                if (countOk && count > 0)
                    *totalUpdated += count;
                --m_pendingUpdates;
                if (m_pendingUpdates <= 0)
                    afterUpdateDone(serverRev, localRev, remotePaths, *totalUpdated);
            });
        }
        if (m_pendingUpdates == 0)
            afterUpdateDone(serverRev, localRev, remotePaths, 0);
    });
}

void SyncEngine::afterUpdateDone(qlonglong serverRev, qlonglong localRev,
                                 const QStringList &remotePaths, int updatedCount)
{
    m_watcher->setSuspended(false);
    detectConflicts([this, serverRev, localRev, remotePaths, updatedCount]() {
        const QString base = tr("已从服务器更新 r%1 → r%2")
                                 .arg(localRev).arg(serverRev);
        if (updatedCount > 0) {
            const QString pathsText = remotePaths.join(QStringLiteral("、"));
            if (pathsText.size() <= 80)
                notify(tr("%1：%2（%3 个文件/目录）")
                           .arg(base, pathsText).arg(updatedCount));
            else
                notify(tr("%1：%2 个文件/目录").arg(base).arg(updatedCount));
        } else {
            notify(base);
        }
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
        QStringList treeConflicts;
        if (r.success) {
            for (const auto &e : r.statuses)
                if (e.conflicted)
                    conflicted << e.path;
            for (const auto &e : r.statuses)
                if (e.treeConflicted)
                    treeConflicts << e.path;
        }
        if (!conflicted.isEmpty())
            emit conflictDetected(conflicted, treeConflicts);
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
        QString dir = pathLooksLikeFile(normalized)
            ? normalized.section(QLatin1Char('/'), 0, -2)
            : normalized;
        if (dir.isEmpty())
            dir = root;
        // svn status -u reports every intermediate of a remote tree that is
        // not present locally, but svn update on a non-existent target fails
        // with E155007 ("None of the targets are working copies"). Walk up to
        // the nearest ancestor that actually exists; the WC root always does.
        while (dir != root && !QFileInfo::exists(dir))
            dir = dir.section(QLatin1Char('/'), 0, -2);
        if (dir.isEmpty())
            dir = root;
        dirs.insert(dir);
    }

    QStringList result = dirs.values();
    std::sort(result.begin(), result.end(), [](const QString &a, const QString &b) {
        return a.count(QLatin1Char('/')) > b.count(QLatin1Char('/'));
    });
    return result;
}

int SyncEngine::resolveConflictCode(const QString &path, const QStringList &treeConflicts,
                                    int userChoiceCode)
{
    // Tree conflicts can only be resolved to the "working" state through the
    // legacy svn_client_resolve API; any other choice makes libsvn_wc fail
    // with "Tree conflict can only be resolved to 'working' state". Code 5
    // (Merged) is the "keep the current working copy state" choice.
    return treeConflicts.contains(path) ? 5 : userChoiceCode;
}

void SyncEngine::notify(const QString &message)
{
    emit syncNotification(message);
}

} // namespace svnsync
