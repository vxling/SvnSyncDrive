#pragma once

#include "core/GlobalConfig.h"
#include "core/Repository.h"
#include "core/SvnCommand.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>

namespace svnsync {

class SvnWorker;
class RepoWatcher;

/**
 * Per-repository bidirectional sync engine, mirroring SVNFileBox.SyncService.
 *
 * Upward (local -> server): RepoWatcher batches local changes, then a
 * status scan auto-adds unversioned files and commits versioned changes
 * grouped by directory (deepest first).
 *
 * Downward (server -> local): the poll timer compares the working copy
 * revision with the server HEAD; when the server is newer, remote-changed
 * paths (via GetServerUpdatePaths) are merged into parent directories and
 * updated deepest-first. The 15-min full sync instead updates the whole
 * working copy in one pass, then runs a full upward scan. Conflicts are
 * reported but never auto-resolved.
 *
 * The engine lives on the GUI thread; all SVN calls happen on the
 * SvnWorker thread, and results are marshalled back through queued
 * signals.
 */
class SyncEngine : public QObject
{
    Q_OBJECT

public:
    explicit SyncEngine(const Repository &repository, QObject *parent = nullptr);
    ~SyncEngine() override;

    void start();
    void stop();
    void syncNow();

    /** Apply (new) global settings: timer intervals, auto-add, TLS trust. */
    void setConfig(const GlobalConfig &config);

    /** Refresh the username/password used for every SVN call, e.g. after the
     *  user changed their password. */
    void setCredentials(const QString &username, const QString &password);

    const Repository &repository() const { return m_repo; }

    using Callback = std::function<void(const CommandResult &)>;

    /** Submit a command to this repo's own queue; the callback runs on
     *  the GUI thread when the result is ready. ReadOnly commands (e.g.
     *  Status) are executed with priority and never block other repos. */
    quint64 submit(const CommandItem &item, Callback callback = Callback());

signals:
    void syncNotification(const QString &message);
    void filesChanged();
    void conflictDetected(const QStringList &conflictedPaths);

private:
    void onResult(quint64 id, const CommandResult &result);

    // Upward sync.
    void onWatcherBatch(const QStringList &paths);
    void enqueueFileChange(const QString &path);
    void scanAndCommit();
    void handleScanStatus(const CommandResult &result);
    void onAutoAddCompleted();
    void onCommitCompleted();
    void maybeFinishScan();
    void finishScan();
    static bool isTempFile(const QString &path);

    // Downward sync.
    void poll();
    void fullSync();
    void startUpdateInChunks(qlonglong serverRev, qlonglong localRev);
    void afterUpdateDone(qlonglong serverRev, qlonglong localRev,
                         const QStringList &remotePaths, int updatedCount);
    void detectConflicts(const std::function<void()> &done);

    static QStringList mergeToDirs(const QStringList &paths, const QString &repoRoot);

    struct CommitGroup
    {
        QString dir;
        int count = 0;
        QString firstFile;
    };
    static QList<CommitGroup> groupByDir(const QList<StatusEntry> &changes,
                                         const QString &repoRoot);
    static QString commitMessage(const QString &dir, int count, const QString &firstFile);

    void notify(const QString &message);

    Repository m_repo;
    GlobalConfig m_config;
    std::unique_ptr<SvnWorker> m_worker;
    std::unique_ptr<RepoWatcher> m_watcher;

    QTimer m_pollTimer;
    QTimer m_fullSyncTimer;

    QHash<quint64, Callback> m_pending;
    std::atomic<quint64> m_idCounter{ 0 };

    bool m_started = false;
    bool m_scanning = false;
    bool m_rescanPending = false;
    bool m_polling = false;
    bool m_fullSyncing = false;
    QSet<QString> m_pendingCommits;
    QSet<QString> m_pendingAdds;
    int m_pendingUpdates = 0;
    // Per-scan counters for the completion summary log line.
    int m_scanAdds = 0;
    int m_scanCommits = 0;

    // Highest revision this engine has committed locally. A local commit
    // bumps the server HEAD but not the working-copy root node's revision,
    // so the poll must consider it when deciding whether the server is
    // "newer"; otherwise it would pull down its own changes on the next poll.
    qlonglong m_lastLocalRev = 0;
};

} // namespace svnsync
