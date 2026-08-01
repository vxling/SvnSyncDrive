#pragma once

#include "core/Repository.h"
#include "core/SvnCommand.h"

#include <QHash>
#include <QObject>
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
 * Downward (server -> local): a poll timer compares the working copy
 * revision with the server HEAD; when the server is newer, remote-changed
 * paths are merged into parent directories and updated deepest-first.
 * Conflicts are reported but never auto-resolved.
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

    const Repository &repository() const { return m_repo; }

signals:
    void syncNotification(const QString &message);
    void filesChanged();
    void conflictDetected(const QStringList &conflictedPaths);

private:
    using Callback = std::function<void(const CommandResult &)>;

    quint64 submit(const CommandItem &item, Callback callback = Callback());
    void onResult(quint64 id, const CommandResult &result);

    // Upward sync.
    void onWatcherBatch(const QStringList &paths);
    void enqueueFileChange(const QString &path);
    void scanAndCommit();
    void handleScanStatus(const CommandResult &result);
    void finishScan();
    static bool isTempFile(const QString &path);

    // Downward sync.
    void poll();
    void fullSync();
    void startUpdateInChunks(qlonglong serverRev, qlonglong localRev);
    void afterUpdateDone(qlonglong serverRev, qlonglong localRev);
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
    std::unique_ptr<SvnWorker> m_worker;
    std::unique_ptr<RepoWatcher> m_watcher;

    QTimer m_pollTimer;
    QTimer m_fullSyncTimer;

    QHash<quint64, Callback> m_pending;
    std::atomic<quint64> m_idCounter{ 0 };

    bool m_started = false;
    bool m_scanning = false;
    bool m_polling = false;
    int m_pendingCommits = 0;
    int m_pendingUpdates = 0;
};

} // namespace svnsync
