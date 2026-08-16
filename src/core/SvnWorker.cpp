#include "core/SvnWorker.h"

#include "core/AppLog.h"
#include "core/ICommandRunner.h"

#include <svnplus/SvnClient.h>

#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <chrono>

namespace svnsync {

namespace {

QString errorText(const SvnPlus::SvnError &err)
{
    return QString::fromStdString(err.message());
}

QString commandName(Command command)
{
    switch (command) {
    case Command::Info: return QStringLiteral("info");
    case Command::Status: return QStringLiteral("status");
    case Command::GetRevision: return QStringLiteral("get-revision");
    case Command::GetHeadRevision: return QStringLiteral("get-head-revision");
    case Command::GetConflictedFiles: return QStringLiteral("get-conflicted-files");
    case Command::GetLastChangedTime: return QStringLiteral("get-last-changed-time");
    case Command::IsVersioned: return QStringLiteral("is-versioned");
    case Command::IsValidWorkingCopy: return QStringLiteral("is-valid-working-copy");
    case Command::TestConnection: return QStringLiteral("test-connection");
    case Command::GetServerUpdatePaths: return QStringLiteral("get-server-update-paths");
    case Command::Add: return QStringLiteral("add");
    case Command::Delete: return QStringLiteral("delete");
    case Command::Move: return QStringLiteral("move");
    case Command::Revert: return QStringLiteral("revert");
    case Command::Resolve: return QStringLiteral("resolve");
    case Command::BreakLock: return QStringLiteral("break-lock");
    case Command::Commit: return QStringLiteral("commit");
    case Command::Update: return QStringLiteral("update");
    case Command::Checkout: return QStringLiteral("checkout");
    }
    return QStringLiteral("unknown");
}

StatusKind toStatusKind(SvnPlus::SvnStatusKind kind)
{
    using K = SvnPlus::SvnStatusKind;
    switch (kind) {
    case K::None: return StatusKind::None;
    case K::Unversioned: return StatusKind::Unversioned;
    case K::Normal: return StatusKind::Normal;
    case K::Added: return StatusKind::Added;
    case K::Missing: return StatusKind::Missing;
    case K::Deleted: return StatusKind::Deleted;
    case K::Replaced: return StatusKind::Replaced;
    case K::Modified: return StatusKind::Modified;
    case K::Merged: return StatusKind::Merged;
    case K::Conflicted: return StatusKind::Conflicted;
    case K::Ignored: return StatusKind::Ignored;
    case K::Obstructed: return StatusKind::Obstructed;
    case K::External: return StatusKind::External;
    case K::Incomplete: return StatusKind::Incomplete;
    }
    return StatusKind::None;
}

/** Real libsvnplus-backed runner. Created and used only on the worker thread. */
class SvnCommandRunner : public ICommandRunner
{
public:
    CommandResult execute(const CommandItem &item) override
    {
        CommandResult result;
        switch (item.command) {
        case Command::Info: runInfo(item, result); break;
        case Command::Status: runStatus(item, result); break;
        case Command::GetRevision: runGetRevision(item, result); break;
        case Command::GetHeadRevision: runGetHeadRevision(item, result); break;
        case Command::GetConflictedFiles: runGetConflictedFiles(item, result); break;
        case Command::GetLastChangedTime: runGetLastChangedTime(item, result); break;
        case Command::IsVersioned: runIsVersioned(item, result); break;
        case Command::IsValidWorkingCopy: runIsValidWorkingCopy(item, result); break;
        case Command::TestConnection: runTestConnection(item, result); break;
        case Command::GetServerUpdatePaths: runGetServerUpdatePaths(item, result); break;
        case Command::Add: runAdd(item, result); break;
        case Command::Delete: runDelete(item, result); break;
        case Command::Move: runMove(item, result); break;
        case Command::Revert: runRevert(item, result); break;
        case Command::Resolve: runResolve(item, result); break;
        case Command::BreakLock: runBreakLock(item, result); break;
        case Command::Commit:
            runHeavyWithLockHeal(item, result, &SvnCommandRunner::runCommit);
            break;
        case Command::Update:
            runHeavyWithLockHeal(item, result, &SvnCommandRunner::runUpdate);
            break;
        case Command::Checkout: runCheckout(item, result); break;
        }
        return result;
    }

    void setCredentials(const QString &username, const QString &password) override
    {
        m_client.setUsername(username.toStdString());
        m_client.setPassword(password.toStdString());
    }

    void setTrustServerCertificate(bool trust) override
    {
        m_client.setTrustServerCertificate(trust);
    }

    void setNetworkTimeout(int timeoutSeconds) override
    {
        m_client.setNetworkTimeout(timeoutSeconds);
    }

    void setKeepAlive(std::function<void()> keepAlive) override
    {
        m_keepAlive = std::move(keepAlive);
    }

    void setMaxFileSizeMb(int megabytes) override
    {
        m_maxFileBytes = megabytes > 0
            ? static_cast<qlonglong>(megabytes) * qlonglong(1024) * qlonglong(1024)
            : 0;
    }

    void cancel() override
    {
        m_client.cancel();
    }

private:
    /** Liveness heartbeat forwarded to the worker watchdog. Called from the
     *  libsvnplus progress/notify callbacks while execute() blocks on the
     *  worker thread, so it must not touch the runner's SvnClient. */
    void pump()
    {
        if (m_keepAlive)
            m_keepAlive();
    }
    static CommandResult fail(const CommandItem &item, const SvnPlus::SvnError &err)
    {
        return makeResult(item, false, errorText(err));
    }

    /** Working-copy root for a path: the nearest ancestor holding .svn/wc.db. */
    static QString wcRootFor(const QString &path)
    {
        QString cur = QDir::cleanPath(path);
        while (true) {
            if (QFile::exists(cur + QLatin1String("/.svn/wc.db")))
                return cur;
            const QString parent = QFileInfo(cur).absolutePath();
            if (parent == cur)
                return QString();
            cur = parent;
        }
    }

    /** Cheap lock probe: is the working copy root (or its children) locked?
     *  Best effort — on failure returns false and the retry path takes over.
     *  Cached for a few seconds so a commit burst does not rescan each time. */
    bool wcRootLocked(const QString &wcRoot)
    {
        if (wcRoot.isEmpty())
            return false;
        if (m_hasLockProbe && m_lastLockRoot == wcRoot && m_lastLockElapsed.elapsed() < 5000)
            return m_lastLocked;
        bool locked = false;
        std::vector<SvnPlus::SvnStatus> statuses;
        SvnPlus::SvnStatusOptions opts;
        opts.depth = SvnPlus::SvnDepth::Immediates;
        const SvnPlus::SvnError err = m_client.status(wcRoot.toStdString(), statuses, opts);
        if (err.ok()) {
            for (const auto &s : statuses) {
                if (s.wcIsLocked) {
                    locked = true;
                    break;
                }
            }
        }
        m_lastLockRoot = wcRoot;
        m_lastLockElapsed.start();
        m_hasLockProbe = true;
        m_lastLocked = locked;
        return locked;
    }

    void runCleanup(const QString &path)
    {
        const SvnPlus::SvnError err = m_client.cleanup(path.toStdString());
        if (err.ok())
            AppLog::warn(QStringLiteral("working copy cleanup ok: %1").arg(path));
        else
            AppLog::warn(QStringLiteral("working copy cleanup failed: %1 (%2)").arg(path, errorText(err)));
    }

    using OpFn = void (SvnCommandRunner::*)(const CommandItem &, CommandResult &);

    /** Runs a heavy write (commit/update) after clearing any leftover working
     *  copy lock, and retries once if it still fails with a lock error. */
    void runHeavyWithLockHeal(const CommandItem &item, CommandResult &result, OpFn op)
    {
        const QString wcRoot = wcRootFor(item.path);
        if (wcRootLocked(wcRoot)) {
            AppLog::warn(QStringLiteral("working copy is locked; cleanup before %1: %2")
                             .arg(commandName(item.command), wcRoot));
            runCleanup(wcRoot);
        }
        (this->*op)(item, result);
        if (!result.success && isWcLockErrorText(result.error) && !wcRoot.isEmpty()) {
            AppLog::warn(QStringLiteral("%1 failed with a working-copy lock; cleanup and retry: %2")
                             .arg(commandName(item.command), item.path));
            runCleanup(wcRoot);
            (this->*op)(item, result);
        }
    }

    void runStatus(const CommandItem &item, CommandResult &result)
    {
        std::vector<SvnPlus::SvnStatus> statuses;
        SvnPlus::SvnStatusOptions opts;
        opts.depth = static_cast<SvnPlus::SvnDepth>(static_cast<int>(item.statusDepth));
        opts.checkOutOfDate = item.checkOutOfDate;

        const SvnPlus::SvnError err =
            m_client.status(item.path.toStdString(), statuses, opts);
        if (!err.ok()) {
            result = fail(item, err);
            return;
        }
        result = makeResult(item, true);
        for (const auto &s : statuses) {
            StatusEntry e;
            e.path = QString::fromStdString(s.localAbspath);
            e.nodeStatus = toStatusKind(s.nodeStatus);
            e.textStatus = toStatusKind(s.textStatus);
            e.versioned = s.versioned;
            e.conflicted = s.conflicted;
            e.treeConflicted = s.treeConflicted;
            e.outOfDate = s.outOfDate;
            e.revision = s.revision;
            if (s.reposNodeStatus != SvnPlus::SvnStatusKind::None)
                e.reposStatus = toStatusKind(s.reposNodeStatus);
            else if (s.reposTextStatus != SvnPlus::SvnStatusKind::None)
                e.reposStatus = toStatusKind(s.reposTextStatus);
            result.statuses.append(e);
        }
    }

    void runInfo(const CommandItem &item, CommandResult &result)
    {
        runInfoRev(item, result, SvnPlus::SvnRevision::head());
    }

    void runInfoRev(const CommandItem &item, CommandResult &result,
                    const SvnPlus::SvnRevision &revision)
    {
        std::vector<SvnPlus::SvnInfo> infos;
        const SvnPlus::SvnError err = m_client.info(
            item.path.toStdString(), infos, revision,
            SvnPlus::SvnDepth::Infinity, false);
        if (!err.ok()) {
            result = fail(item, err);
            return;
        }
        result = makeResult(item, true);
        for (const auto &i : infos) {
            InfoEntry e;
            e.path = QString::fromStdString(i.path);
            e.url = QString::fromStdString(i.url);
            e.revision = i.revision;
            result.infos.append(e);
        }
        if (!infos.empty()) {
            result.revision = infos.front().revision;
            result.value = QString::fromStdString(infos.front().url);
        }
    }

    void runGetRevision(const CommandItem &item, CommandResult &result)
    {
        // Report the working copy's own revision, not the server HEAD: a HEAD
        // query on a WC path returns the URL's HEAD info, which would make the
        // engine believe it is always up to date and never pull remote changes.
        runInfoRev(item, result, SvnPlus::SvnRevision::working());
    }

    void runGetHeadRevision(const CommandItem &item, CommandResult &result)
    {
        CommandItem remote = item;
        if (!item.repoUrl.isEmpty())
            remote.path = item.repoUrl;
        runInfo(remote, result);
    }

    void runGetConflictedFiles(const CommandItem &item, CommandResult &result)
    {
        CommandItem local = item;
        local.checkOutOfDate = false;
        runStatus(local, result);
        if (!result.success)
            return;
        QStringList paths;
        for (const auto &e : result.statuses)
            if (e.conflicted)
                paths << e.path;
        for (const auto &e : result.statuses)
            if (e.treeConflicted)
                result.treeConflicts << e.path;
        result.value = paths.join(QLatin1Char(';'));
    }

    void runGetLastChangedTime(const CommandItem &item, CommandResult &result)
    {
        // Server-side last-changed time of the path, saturating the previous
        // no-op stub: `svn info` reports the node's last-changed date at HEAD.
        std::vector<SvnPlus::SvnInfo> infos;
        const SvnPlus::SvnError err = m_client.info(
            item.path.toStdString(), infos, SvnPlus::SvnRevision::head(),
            SvnPlus::SvnDepth::Empty, false);
        if (!err.ok()) {
            result = fail(item, err);
            return;
        }
        result = makeResult(item, true);
        if (infos.empty())
            return;
        const auto when = infos.front().lastChangedDate;
        if (when != std::chrono::system_clock::time_point{}) {
            const qint64 secs =
                std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch()).count();
            result.value = QDateTime::fromSecsSinceEpoch(secs, Qt::UTC)
                               .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'"));
        }
        result.revision = infos.front().lastChangedRevision;
    }

    void runBreakLock(const CommandItem &item, CommandResult &result)
    {
        // `svn unlock --force`: remove a lock even when held by someone else.
        const SvnPlus::SvnError err =
            m_client.unlock({ item.path.toStdString() }, /*breakLock*/ true);
        result = err.ok() ? makeResult(item, true) : fail(item, err);
    }

    void runIsVersioned(const CommandItem &item, CommandResult &result)
    {
        CommandItem local = item;
        local.checkOutOfDate = false;
        runStatus(local, result);
        if (!result.success)
            return;
        bool versioned = false;
        for (const auto &e : result.statuses)
            if (e.versioned)
                versioned = true;
        result.value = versioned ? QStringLiteral("true") : QStringLiteral("false");
    }

    void runIsValidWorkingCopy(const CommandItem &item, CommandResult &result)
    {
        std::vector<SvnPlus::SvnInfo> infos;
        const SvnPlus::SvnError err = m_client.info(
            item.path.toStdString(), infos, SvnPlus::SvnRevision::head(),
            SvnPlus::SvnDepth::Empty, false);
        result = err.ok() ? makeResult(item, true) : fail(item, err);
    }

    void runTestConnection(const CommandItem &item, CommandResult &result)
    {
        std::vector<SvnPlus::SvnInfo> infos;
        const SvnPlus::SvnError err = m_client.info(
            item.repoUrl.toStdString(), infos, SvnPlus::SvnRevision::head(),
            SvnPlus::SvnDepth::Empty, false);
        if (!err.ok()) {
            result = fail(item, err);
            return;
        }
        result = makeResult(item, true);
        if (!infos.empty())
            result.revision = infos.front().revision;
    }

    void runGetServerUpdatePaths(const CommandItem &item, CommandResult &result)
    {
        // `svn status -u` against HEAD: report every path that has remote
        // changes (including nodes that exist only in the repository). The
        // libsvnplus status call must compare against HEAD for this to work;
        // a WORKING comparison misses pure remote additions entirely.
        CommandItem remote = item;
        remote.checkOutOfDate = true;
        runStatus(remote, result);
        if (!result.success)
            return;
        QStringList paths;
        for (const auto &e : result.statuses)
            if (e.outOfDate)
                paths << e.path;
        result.value = paths.join(QLatin1Char(';'));
    }

    void runAdd(const CommandItem &item, CommandResult &result)
    {
        const SvnPlus::SvnError err = m_client.add(
            item.path.toStdString(), SvnPlus::SvnDepth::Infinity,
            /*force*/ false, /*noIgnore*/ false, /*addParents*/ false);
        result = err.ok() ? makeResult(item, true) : fail(item, err);
    }

    void runDelete(const CommandItem &item, CommandResult &result)
    {
        const SvnPlus::SvnError err = m_client.remove(
            { item.path.toStdString() }, /*force*/ true, /*keepLocal*/ true);
        result = err.ok() ? makeResult(item, true) : fail(item, err);
    }

    void runMove(const CommandItem &item, CommandResult &result)
    {
        const SvnPlus::SvnError err = m_client.move(
            { item.fromPath.toStdString() }, item.path.toStdString(),
            false, false, nullptr);
        result = err.ok() ? makeResult(item, true) : fail(item, err);
    }

    void runRevert(const CommandItem &item, CommandResult &result)
    {
        const SvnPlus::SvnError err = m_client.revert(
            { item.path.toStdString() }, SvnPlus::SvnDepth::Infinity);
        result = err.ok() ? makeResult(item, true) : fail(item, err);
    }

    void runResolve(const CommandItem &item, CommandResult &result)
    {
        SvnPlus::SvnConflictChoice choice = SvnPlus::SvnConflictChoice::MineFull;
        switch (item.conflictChoice) {
        case 0: choice = SvnPlus::SvnConflictChoice::Base; break;
        case 1: choice = SvnPlus::SvnConflictChoice::TheirsFull; break;
        case 2: choice = SvnPlus::SvnConflictChoice::MineFull; break;
        case 3: choice = SvnPlus::SvnConflictChoice::TheirsConflict; break;
        case 4: choice = SvnPlus::SvnConflictChoice::MineConflict; break;
        case 5: choice = SvnPlus::SvnConflictChoice::Merged; break;
        default: choice = SvnPlus::SvnConflictChoice::MineFull; break;
        }
        const SvnPlus::SvnError err = m_client.resolve(
            item.path.toStdString(), choice, SvnPlus::SvnDepth::Infinity);
        result = err.ok() ? makeResult(item, true) : fail(item, err);
    }

    void runCommit(const CommandItem &item, CommandResult &result)
    {
        std::vector<std::string> targets;
        targets.push_back(item.path.toStdString());

        // Upload gate: single files at or above the configured threshold are
        // never committed (skipped and reported), so a huge file can neither
        // be uploaded by the auto-sync nor by a manual commit. The same walk
        // also yields the commit scope: when oversized files are present the
        // directory target is replaced by the explicit list of changed paths
        // minus the oversized ones, so the rest of the directory still lands
        // in the same revision while the big files stay behind.
        if (m_maxFileBytes > 0) {
            std::vector<SvnPlus::SvnStatus> statuses;
            SvnPlus::SvnStatusOptions opts;
            opts.depth = SvnPlus::SvnDepth::Infinity;
            const SvnPlus::SvnError serr =
                m_client.status(item.path.toStdString(), statuses, opts);
            if (!serr.ok()) {
                // Status failed: fall back to a plain directory commit rather
                // than refusing valid uploads.
                statuses.clear();
            }
            QStringList oversized;
            QStringList changedFiles;
            bool hasOversize = false;
            for (const auto &s : statuses) {
                if (!s.versioned)
                    continue;
                const StatusKind kind = toStatusKind(s.nodeStatus);
                if (kind == StatusKind::None || kind == StatusKind::Normal)
                    continue;
                const QString path = QDir::fromNativeSeparators(QString::fromStdString(s.localAbspath));
                const bool uploadsBytes = kind == StatusKind::Added
                    || kind == StatusKind::Modified
                    || kind == StatusKind::Replaced
                    || kind == StatusKind::Merged;
                if (uploadsBytes && QFileInfo(path).exists()
                    && QFileInfo(path).size() >= m_maxFileBytes) {
                    oversized << path;
                    hasOversize = true;
                    continue;
                }
                changedFiles << path;
            }
            if (hasOversize) {
                if (changedFiles.isEmpty()) {
                    // Nothing left to upload: report a clean no-op instead of
                    // a failure so callers do not surface a misleading error.
                    CommandResult blocked = makeResult(item, true);
                    blocked.oversizedFiles = oversized;
                    result = blocked;
                    return;
                }
                result.oversizedFiles = oversized;
                targets.clear();
                for (const auto &p : changedFiles)
                    targets.push_back(p.toStdString());
            }
        }

        m_client.setProgressCallback([this](long long, long long) { pump(); });
        SvnPlus::SvnCommitInfo info;
        const SvnPlus::SvnError err =
            m_client.commit(targets, item.message.toStdString(), false, &info);
        m_client.setProgressCallback({});
        if (!err.ok()) {
            result = fail(item, err);
            return;
        }
        result = makeResult(item, true);
        result.revision = info.revision;
    }

    void runUpdate(const CommandItem &item, CommandResult &result)
    {
        std::vector<std::string> paths;
        if (item.updatePaths.isEmpty()) {
            paths.push_back(item.path.toStdString());
        } else {
            for (const auto &p : item.updatePaths)
                paths.push_back(p.toStdString());
        }

        int updatedCount = 0;
        m_client.setNotifyCallback([this, &updatedCount](const SvnPlus::SvnNotifyEvent &e) {
            pump();
            using A = SvnPlus::SvnNotifyAction;
            switch (e.action) {
            case A::UpdateAdd:
            case A::UpdateUpdate:
            case A::UpdateDelete:
            case A::UpdateReplace:
            case A::UpdateShadowedAdd:
            case A::UpdateShadowedUpdate:
            case A::UpdateShadowedDelete:
                ++updatedCount;
                break;
            default:
                break;
            }
        });
        m_client.setProgressCallback([this](long long, long long) { pump(); });

        std::vector<SvnPlus::SvnRevision> revisions;
        const SvnPlus::SvnError err = m_client.update(
            paths, SvnPlus::SvnRevision::head(), SvnPlus::SvnDepth::Infinity,
            false, false, &revisions);
        m_client.setNotifyCallback({});
        m_client.setProgressCallback({});

        if (!err.ok()) {
            result = fail(item, err);
            return;
        }
        result = makeResult(item, true);
        result.value = QString::number(updatedCount);
        if (!revisions.empty())
            result.revision = revisions.back().number();
    }

    void runCheckout(const CommandItem &item, CommandResult &result)
    {
        SvnPlus::SvnRevision revision;
        m_client.setProgressCallback([this](long long, long long) { pump(); });
        m_client.setNotifyCallback([this](const SvnPlus::SvnNotifyEvent &) { pump(); });
        const SvnPlus::SvnError err = m_client.checkout(
            item.repoUrl.toStdString(), item.path.toStdString(),
            SvnPlus::SvnRevision::head(), SvnPlus::SvnDepth::Infinity,
            false, &revision);
        m_client.setProgressCallback({});
        m_client.setNotifyCallback({});
        if (!err.ok()) {
            result = fail(item, err);
            return;
        }
        result = makeResult(item, true);
        result.revision = revision.number();
    }

    SvnPlus::SvnClient m_client;

    std::function<void()> m_keepAlive;
    qlonglong m_maxFileBytes = 0;

    QString m_lastLockRoot;
    QElapsedTimer m_lastLockElapsed;
    bool m_hasLockProbe = false;
    bool m_lastLocked = false;
};

} // namespace

SvnWorker::SvnWorker(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<CommandItem>();
    qRegisterMetaType<CommandResult>();
    if (!m_factory) {
        m_factory = [] { return std::make_unique<SvnCommandRunner>(); };
    }
}

SvnWorker::~SvnWorker()
{
    stop();
}

void SvnWorker::start(RunnerFactory factory)
{
    if (m_started)
        return;
    m_started = true;
    if (factory)
        m_factory = std::move(factory);
    if (!m_factory)
        m_factory = [] { return std::make_unique<SvnCommandRunner>(); };
    m_thread = std::thread(&SvnWorker::workerLoop, this);
}

void SvnWorker::stop()
{
    ICommandRunner *runner = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_stopping = true;
        runner = m_runner.get();
    }
    // Cancel any in-flight command before joining: on an unreachable server a
    // status -u/commit can otherwise block the join until the network timeout
    // (e.g. 60 s), which makes quitting seem to hang.
    if (runner)
        runner->cancel();
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void SvnWorker::submit(const CommandItem &item)
{
    CommandItem copy = item;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (copy.id == 0)
            copy.id = m_nextId++;
        switch (categoryOf(copy.command)) {
        case Category::ReadOnly:
            m_readOnly.append(copy);
            break;
        case Category::LocalWrite:
            m_localWrite.append(copy);
            m_dedup.insert(copy.path, copy);
            break;
        case Category::HeavyWrite:
            if (heavyWriteAllowedLocked(copy)) {
                m_heavyWrite.append(copy);
                m_dedup.insert(dedupKey(copy), copy);
            }
            break;
        }
    }
    m_cv.notify_one();
}

void SvnWorker::setCredentials(const QString &username, const QString &password)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_username = username;
        m_password = password;
        m_credsDirty = true;
    }
    m_cv.notify_all();
}

void SvnWorker::setTrustServerCertificate(bool trust)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_trustCert = trust;
        m_credsDirty = true;
    }
    m_cv.notify_all();
}

void SvnWorker::setNetworkTimeout(int timeoutSeconds)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_networkTimeoutSec = timeoutSeconds;
        m_credsDirty = true;
    }
    m_cv.notify_all();
}

void SvnWorker::setCommandTimeoutSec(int seconds)
{
    m_commandTimeoutSec.store(seconds > 0 ? seconds : 0);
}

void SvnWorker::setMaxTransferSec(int seconds)
{
    m_maxTransferSec.store(seconds > 0 ? seconds : 0);
}

void SvnWorker::setMaxFileSizeMb(int megabytes)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_maxFileSizeMb.store(megabytes > 0 ? megabytes : 0);
        m_credsDirty = true;
    }
    m_cv.notify_all();
}

/** Liveness heartbeat from a running heavy-write command: refreshes the
 *  watchdog timestamp (guarded by m_mutex so the watchdog thread can observe
 *  it under the same lock) and naps the watchdog back to sleep. The callback
 *  only ever fires on the worker thread while a command is executing, at
 *  which point the loop owns m_runner and m_mutex is not held. */
void SvnWorker::pulse()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_lastActivity = std::chrono::steady_clock::now();
    m_watchdogCv.notify_all();
}

void SvnWorker::workerLoop()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_runner = m_factory();
        m_runner->setKeepAlive([this] { pulse(); });
        m_runner->setMaxFileSizeMb(m_maxFileSizeMb.load());
    }

    while (true) {
        CommandItem item;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            if (m_credsDirty) {
                m_runner->setCredentials(m_username, m_password);
                m_runner->setTrustServerCertificate(m_trustCert);
                m_runner->setNetworkTimeout(m_networkTimeoutSec);
                m_runner->setMaxFileSizeMb(m_maxFileSizeMb.load());
                m_credsDirty = false;
            }
            m_cv.wait(lk, [this] {
                return m_stopping || !m_readOnly.isEmpty() || !m_localWrite.isEmpty()
                    || !m_heavyWrite.isEmpty();
            });
            if (m_stopping)
                break;
            // Re-apply after the wait: a credential change that arrived while
            // this thread slept in m_cv.wait does not re-run the block above
            // (it is executed once per loop iteration, before the wait), so
            // the next item could otherwise run with stale empty credentials.
            if (m_credsDirty) {
                m_runner->setCredentials(m_username, m_password);
                m_runner->setTrustServerCertificate(m_trustCert);
                m_runner->setNetworkTimeout(m_networkTimeoutSec);
                m_runner->setMaxFileSizeMb(m_maxFileSizeMb.load());
                m_credsDirty = false;
            }
            item = takeNextLocked();
        }

        CommandResult result;
        const int timeoutSec = m_commandTimeoutSec.load();
        const bool isHeavy = categoryOf(item.command) == Category::HeavyWrite;
        const bool needWatchdog =
            timeoutSec > 0 || (isHeavy && m_maxTransferSec.load() > 0);
        if (!needWatchdog) {
            result = m_runner->execute(item);
        } else {
            if (isHeavy) {
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    m_lastActivity = std::chrono::steady_clock::now();
                }
            }
            // Watchdog: abort a command that hangs or that outlives its
            // budget. Heavy writes (commit/update/checkout) get two
            // independent conditions — an inactivity window between liveness
            // heartbeats (cancels a hung connection even when progress keeps
            // the socket open) and an absolute cap from the command start
            // (bounds any single file transfer). Other commands keep the
            // plain total-time window.
            bool watchdogDone = false;
            std::thread watchdog([this, timeoutSec, isHeavy, &watchdogDone]() {
                std::unique_lock<std::mutex> lk(m_mutex);
                const auto start = std::chrono::steady_clock::now();
                if (isHeavy) {
                    const auto gap = std::chrono::seconds(timeoutSec > 0 ? timeoutSec : 120);
                    const int capSec = m_maxTransferSec.load();
                    const auto capEnd = capSec > 0
                        ? start + std::chrono::seconds(capSec)
                        : std::chrono::steady_clock::time_point::max();
                    auto idleEnd = m_lastActivity + gap;
                    while (!m_stopping && !watchdogDone) {
                        const auto now = std::chrono::steady_clock::now();
                        if (now >= idleEnd)   // no liveness inside the gap -> hung
                            break;
                        if (now >= capEnd)    // transfer ran past the hard cap
                            break;
                        m_watchdogCv.wait_until(lk, std::min(idleEnd, capEnd));
                        idleEnd = m_lastActivity + gap;
                    }
                } else {
                    m_watchdogCv.wait_for(lk, std::chrono::seconds(timeoutSec),
                                          [this, &watchdogDone] {
                                              return m_stopping || watchdogDone;
                                          });
                }
                if (!watchdogDone)
                    m_runner->cancel();
            });
            result = m_runner->execute(item);
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                watchdogDone = true;
            }
            m_watchdogCv.notify_one();
            watchdog.join();
        }

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            removeFromDedupLocked(item);
        }
        logModify(item, result);
        emit resultReady(item.id, result);
    }

    m_runner.reset();
    emit finished();
}

CommandItem SvnWorker::takeNextLocked()
{
    if (!m_readOnly.isEmpty())
        return m_readOnly.takeFirst();
    if (!m_localWrite.isEmpty())
        return m_localWrite.takeFirst();
    return m_heavyWrite.takeFirst();
}

bool SvnWorker::heavyWriteAllowedLocked(const CommandItem &item)
{
    if (item.command == Command::Checkout || item.bypassDedup)
        return true;
    const QString key = dedupKey(item);
    const auto it = m_dedup.constFind(key);
    if (item.command == Command::Commit) {
        if (it != m_dedup.constEnd()
            && (it->command == Command::Commit || it->command == Command::Update))
            return false;
    } else if (it != m_dedup.constEnd()
               && (it->command == Command::Update || it->command == Command::Commit)) {
        return false;
    }
    return true;
}

void SvnWorker::removeFromDedupLocked(const CommandItem &item)
{
    const QString key = (categoryOf(item.command) == Category::LocalWrite) ? item.path
                                                                           : dedupKey(item);
    m_dedup.remove(key);
}

QString SvnWorker::dedupKey(const CommandItem &item) const
{
    QString key = item.path;
    if (item.command == Command::Update && !item.updatePaths.isEmpty()) {
        QStringList sorted = item.updatePaths;
        sorted.sort();
        key += QLatin1Char('|') + sorted.join(QLatin1Char(','));
    }
    return key;
}

void SvnWorker::logModify(const CommandItem &item, const CommandResult &result)
{
    // Only working-copy modifying commands are recorded in the program log.
    if (categoryOf(item.command) == Category::ReadOnly)
        return;

    QString message = commandName(item.command);
    if (!item.repo.isEmpty())
        message += QStringLiteral(" repo=") + item.repo;
    if (!item.path.isEmpty())
        message += QStringLiteral(" path=") + item.path;
    if (item.command == Command::Update && !item.updatePaths.isEmpty())
        message += QStringLiteral(" paths=") + item.updatePaths.join(QLatin1Char(';'));
    if (item.command == Command::Commit && !item.message.isEmpty())
        message += QStringLiteral(" message=") + item.message;

    if (result.success) {
        if (result.revision > 0)
            message += QStringLiteral(" -> ok r%1").arg(result.revision);
        else
            message += QStringLiteral(" -> ok");
        if (item.command == Command::Update) {
            bool countOk = false;
            const int count = result.value.toInt(&countOk);
            if (countOk && count > 0)
                message += QStringLiteral(" (%1 items)").arg(count);
        }
        AppLog::info(message);
    } else {
        message += QStringLiteral(" -> failed: ") + result.error;
        AppLog::error(message);
    }
}

} // namespace svnsync
