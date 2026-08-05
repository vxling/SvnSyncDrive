#pragma once

#include "core/GlobalConfig.h"
#include "core/Repository.h"
#include "core/SvnCommand.h"
#include "core/SvnWorker.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>

namespace svnsync {

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

    /** Test hook: replace the worker's command runner factory before
     *  start(). Pass a factory returning a fake ICommandRunner to drive the
     *  engine without touching a real SVN server. Must be called before
     *  start(); afterwards the default libsvnplus runner is used. */
    void setCommandRunnerFactoryForTest(SvnWorker::RunnerFactory factory)
    {
        m_runnerFactory = std::move(factory);
    }

    struct CommitGroup
    {
        QString dir;
        int count = 0;
        QString firstFile;
    };

    // ── Pure helpers (public static so the console tests can cover them
    //    without a running engine) ────────────────────────────────────────

    /** Temp-file detection for the upward scan: office lock files (~$*),
     *  editor backups (~*), *.tmp / *.temp and .DS_Store are never added or
     *  committed. */
    static bool isTempFile(const QString &path);

    /** Group working-copy changes by the directory to commit: a file takes
     *  its parent directory, a directory its own path. Result is ordered
     *  deepest-first so child directories are committed before their parents. */
    static QList<CommitGroup> groupByDir(const QList<StatusEntry> &changes,
                                         const QString &repoRoot);

    /** Auto-sync commit message: single file -> its name, multiple files ->
     *  "N files in <dir>". */
    static QString commitMessage(const QString &dir, int count, const QString &firstFile);

    /** Reduce remote update paths to the deepest existing ancestor directory
     *  of each entry (files take their parent, directories themselves), so
     *  `svn update` never runs against a target that does not exist locally
     *  (E155007). Returns unique directories, deepest-first. */
    static QStringList mergeToDirs(const QStringList &paths, const QString &repoRoot);

    /** Effective resolve choice for a conflicted path. Tree conflicts can only
     *  be resolved to the "working" state via the legacy svn_client_resolve
     *  API (any other choice makes libsvn_wc fail), so a path in
     *  treeConflicts always maps to code 5 (Merged) no matter what the user
     *  picked; other paths keep userChoiceCode unchanged. */
    static int resolveConflictCode(const QString &path, const QStringList &treeConflicts,
                                   int userChoiceCode);

signals:
    void syncNotification(const QString &message);
    void filesChanged();
    void conflictDetected(const QStringList &conflictedPaths, const QStringList &treeConflictPaths);

    // Server-health classification of remote command results (see classify()).
    void authenticationFailed();   // auth error -> engine will be stopped
    void connectionLost();         // threshold of consecutive network failures reached
    void connectionRestored();     // a server command succeeded again

private:
    void onResult(quint64 id, const CommandResult &result);

    /** Classify a finished command's result for the server-health signals.
     *  Only server-touching commands are considered; local commands are
     *  ignored. Called last in onResult: on an auth failure the engine may
     *  be destroyed by the receiver, so callers must not touch `this`
     *  afterwards. */
    void classify(const CommandResult &result);

    // Upward sync.
    void onWatcherBatch(const QStringList &paths);
    void enqueueFileChange(const QString &path);
    void scanAndCommit();
    void handleScanStatus(const CommandResult &result);
    void onAutoAddCompleted();
    void onCommitCompleted();
    void maybeFinishScan();
    void finishScan();

    // Downward sync.
    void poll();
    void fullSync();
    void startUpdateInChunks(qlonglong serverRev, qlonglong localRev);
    void afterUpdateDone(qlonglong serverRev, qlonglong localRev,
                         const QStringList &remotePaths, int updatedCount);
    void detectConflicts(const std::function<void()> &done);

    void notify(const QString &message);

    Repository m_repo;
    GlobalConfig m_config;
    std::unique_ptr<SvnWorker> m_worker;
    std::unique_ptr<RepoWatcher> m_watcher;
    SvnWorker::RunnerFactory m_runnerFactory;

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

    // Server-health bookkeeping (see classify()).
    int m_consecutiveNetworkFailures = 0;
    bool m_connectionLost = false;
};

} // namespace svnsync
