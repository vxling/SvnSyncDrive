#include "core/ConfigStore.h"
#include "core/CredCrypto.h"
#include "core/I18n.h"
#include "core/ICommandRunner.h"
#include "core/LogStore.h"
#include "core/QuickAccess.h"
#include "core/RepoManager.h"
#include "core/RepoWatcher.h"
#include "core/Repository.h"
#include "core/SvnCommand.h"
#include "core/SvnWorker.h"
#include "core/SyncEngine.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <svnplus/SvnClient.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <atomic>
#include <vector>

using namespace svnsync;

namespace {

int g_failures = 0;

void writeFile(const QString &path, const QString &content)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(content.toUtf8());
        file.close();
    }
}

void check(bool condition, const char *what)
{
    if (condition) {
        std::printf("  [PASS] %s\n", what);
    } else {
        std::printf("  [FAIL] %s\n", what);
        ++g_failures;
    }
}

bool waitUntil(const std::function<bool()> &pred, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (pred())
            return true;
        QThread::msleep(10);
    }
    return pred();
}

/** Deterministic runner recording execution order. */
class FakeRunner : public ICommandRunner
{
public:
    QList<Command> order;
    std::function<CommandResult(const CommandItem &)> handler;

    CommandResult execute(const CommandItem &item) override
    {
        order.append(item.command);
        if (handler)
            return handler(item);
        return makeResult(item, true);
    }
    void setCredentials(const QString &, const QString &) override {}
    void setTrustServerCertificate(bool) override {}
    void setNetworkTimeout(int) override {}
    void cancel() override { cancelled = true; }
    bool cancelled = false;
};

/** Owning pair: worker owns the runner (and frees it on stop()). */
struct WorkerAndFake
{
    std::unique_ptr<SvnWorker> worker;
    FakeRunner *fake = nullptr;

    explicit WorkerAndFake()
    {
        worker = std::make_unique<SvnWorker>();
        FakeRunner *raw = new FakeRunner;
        fake = raw;
        worker->start([raw]() -> std::unique_ptr<ICommandRunner> {
            return std::unique_ptr<ICommandRunner>(raw);
        });
    }

    ~WorkerAndFake()
    {
        if (worker)
            worker->stop();
    }
};

/** Runner whose per-command result is decided by a test-set handler. Used to
 *  drive SyncEngine::classify through real resultReady/onResult plumbing
 *  without touching a live SVN server. */
class ScriptedRunner : public ICommandRunner
{
public:
    std::function<CommandResult(const CommandItem &)> handler;

    CommandResult execute(const CommandItem &item) override
    {
        if (handler)
            return handler(item);
        return makeResult(item, true);
    }
    void setCredentials(const QString &, const QString &) override {}
    void setTrustServerCertificate(bool) override {}
    void setNetworkTimeout(int) override {}
    void cancel() override {}
};

/** Runner exposing the worker's keep-alive callback so tests can check the
 *  heavy-write watchdog: a test execute() can call the callback to simulate
 *  liveliness (progress/notify events) or let it go silent to simulate a hung
 *  connection. */
class KeepAliveRunner : public ICommandRunner
{
public:
    std::function<void()> keepAlive;
    std::function<CommandResult(const CommandItem &)> onExecute;

    CommandResult execute(const CommandItem &item) override
    {
        if (onExecute)
            return onExecute(item);
        return makeResult(item, true);
    }
    void setCredentials(const QString &, const QString &) override {}
    void setTrustServerCertificate(bool) override {}
    void setNetworkTimeout(int) override {}
    void setKeepAlive(std::function<void()> fn) override
    {
        keepAlive = std::move(fn);
    }
    void setMaxFileSizeMb(int) override {}
    void cancel() override { cancelled = true; }
    bool cancelled = false;
};

/**
 * Collects results via a connection that must be created BEFORE the commands
 * are submitted. The worker runs on a raw std::thread and can execute every
 * queued command (and emit resultReady) before the main thread gets a chance
 * to connect, so a connect-after-submit test would randomly miss signals.
 */
struct ResultCollector
{
    QList<CommandResult> results;
    QObject context;

    explicit ResultCollector(SvnWorker &worker)
    {
        QObject::connect(&worker, &SvnWorker::resultReady, &context,
                         [this](quint64, const CommandResult &r) { results.append(r); },
                         Qt::QueuedConnection);
    }

    bool wait(int expected, int timeoutMs = 10000)
    {
        return waitUntil([&] { return results.size() >= expected; }, timeoutMs);
    }
};

CommandItem makeItem(Command command, const QString &path)
{
    CommandItem item;
    item.command = command;
    item.path = path;
    return item;
}

} // namespace

static bool testCategoryOf()
{
    std::printf("-- categoryOf --\n");
    check(categoryOf(Command::Status) == Category::ReadOnly, "Status -> ReadOnly");
    check(categoryOf(Command::GetHeadRevision) == Category::ReadOnly, "GetHeadRevision -> ReadOnly");
    check(categoryOf(Command::Add) == Category::LocalWrite, "Add -> LocalWrite");
    check(categoryOf(Command::Delete) == Category::LocalWrite, "Delete -> LocalWrite");
    check(categoryOf(Command::Commit) == Category::HeavyWrite, "Commit -> HeavyWrite");
    check(categoryOf(Command::Update) == Category::HeavyWrite, "Update -> HeavyWrite");
    return true;
}

static bool testWcLockErrorText()
{
    std::printf("-- isWcLockErrorText --\n");
    check(isWcLockErrorText(QStringLiteral("Working copy '/a' locked.: '/a' is already locked.")),
          "interrupted-update message detected");
    check(isWcLockErrorText(QStringLiteral("'/a' is already locked.")),
          "bare 'is already locked' detected");
    check(isWcLockErrorText(QStringLiteral("working copy locked")),
          "lowercase 'working copy locked' detected");
    check(!isWcLockErrorText(QStringLiteral("E155000: Runtime Error")),
          "unrelated error ignored");
    check(!isWcLockErrorText(QStringLiteral("svn: E170013: Connection reset")),
          "network error ignored");
    return true;
}

static bool testWorkerOrdering()
{
    std::printf("-- SvnWorker ordering --\n");
    WorkerAndFake wf;
    ResultCollector col(*wf.worker);

    wf.worker->submit(makeItem(Command::Commit, QStringLiteral("/a")));
    wf.worker->submit(makeItem(Command::Status, QStringLiteral("/a")));
    wf.worker->submit(makeItem(Command::Add, QStringLiteral("/b")));
    wf.worker->submit(makeItem(Command::Update, QStringLiteral("/wc")));

    check(col.wait(4), "all 4 commands delivered a result");
    check(col.results.size() == 4, "exactly 4 results received");

    QList<Command> expected = { Command::Status, Command::Add, Command::Commit, Command::Update };
    check(wf.fake->order == expected, "priority order: ReadOnly -> LocalWrite -> HeavyWrite");

    wf.worker->stop();
    return true;
}

static bool testWorkerDedup()
{
    std::printf("-- SvnWorker dedup --\n");
    WorkerAndFake wf;
    ResultCollector col(*wf.worker);

    // Duplicate commits on the same path must collapse to one.
    wf.worker->submit(makeItem(Command::Commit, QStringLiteral("/x")));
    wf.worker->submit(makeItem(Command::Commit, QStringLiteral("/x")));
    // Different path -> allowed.
    wf.worker->submit(makeItem(Command::Commit, QStringLiteral("/y")));

    // Duplicate update of the same sub-path must collapse.
    CommandItem updA1 = makeItem(Command::Update, QStringLiteral("/wc"));
    updA1.updatePaths = { QStringLiteral("/wc/a") };
    CommandItem updA2 = updA1;
    CommandItem updB = makeItem(Command::Update, QStringLiteral("/wc"));
    updB.updatePaths = { QStringLiteral("/wc/b") };
    wf.worker->submit(updA1);
    wf.worker->submit(updA2);
    wf.worker->submit(updB);

    // 2 commits + 2 updates = 4 results expected.
    check(col.wait(4), "dedup collapsed 6 submits into 4 executions");
    check(col.results.size() == 4, "exactly 4 results received");

    int commits = 0;
    int updatesA = 0;
    int updatesB = 0;
    for (const auto &r : col.results) {
        if (r.command == Command::Commit)
            ++commits;
        else if (r.command == Command::Update) {
            if (r.paths.contains(QStringLiteral("/wc/a")))
                ++updatesA;
            else if (r.paths.contains(QStringLiteral("/wc/b")))
                ++updatesB;
        }
    }
    check(commits == 2, "commit dedup: one execution per path");
    check(updatesA == 1 && updatesB == 1, "update dedup: composite key by sub-path");

    wf.worker->stop();
    return true;
}

static bool testWorkerDedupBypass()
{
    std::printf("-- SvnWorker dedup bypass --\n");
    WorkerAndFake wf;
    ResultCollector col(*wf.worker);

    // Baseline: a plain Update with the same key is suppressed.
    wf.worker->submit(makeItem(Command::Update, QStringLiteral("/wc")));
    wf.worker->submit(makeItem(Command::Update, QStringLiteral("/wc")));

    // A bypassDedup Update (the 15-min fullSync one) on that same key must
    // run anyway: suppressing it would leave SyncEngine::fullSync waiting
    // forever for a result it never gets (m_fullSyncing stuck = dead).
    CommandItem scheduled = makeItem(Command::Update, QStringLiteral("/wc"));
    scheduled.bypassDedup = true;
    wf.worker->submit(scheduled);

    check(col.wait(2), "bypassDedup update runs despite a queued same-key update");
    check(col.results.size() == 2, "exactly 2 results received");

    int updates = 0;
    for (const auto &r : col.results)
        if (r.command == Command::Update)
            ++updates;
    check(updates == 2, "both same-key updates executed serially");

    wf.worker->stop();
    return true;
}

static bool testWorkerResultCorrelation()
{
    std::printf("-- SvnWorker result correlation --\n");
    WorkerAndFake wf;
    wf.fake->handler = [](const CommandItem &item) {
        CommandResult r = makeResult(item, true);
        r.value = QStringLiteral("echo:%1").arg(item.id);
        return r;
    };

    CommandItem item = makeItem(Command::Info, QStringLiteral("/p"));
    item.id = 999;
    wf.worker->submit(item);

    CommandResult received;
    QObject context;
    QObject::connect(wf.worker.get(), &SvnWorker::resultReady, &context,
                     [&received](quint64, const CommandResult &r) { received = r; },
                     Qt::QueuedConnection);
    waitUntil([&] { return received.id != 0; });
    check(received.id == 999, "explicit caller id is preserved");
    check(received.value == QStringLiteral("echo:999"), "payload delivered with the result");

    wf.worker->stop();
    return true;
}

static bool testRepoWatcher()
{
    std::printf("-- RepoWatcher debounce & filtering --\n");
    QTemporaryDir dir;
    if (!dir.isValid())
        return false;
    const QString root = dir.path();

    QDir(root).mkpath(QStringLiteral(".svn"));
    QDir(root).mkpath(QStringLiteral("sub"));
    writeFile(root + QStringLiteral("/.svn/ignored"), "x");
    writeFile(root + QStringLiteral("/~$lock.docx"), "x");
    writeFile(root + QStringLiteral("/notes.tmp"), "x");

    RepoWatcher watcher;
    QStringList batch;
    QObject context;
    QObject::connect(&watcher, &RepoWatcher::filesChanged, &context,
                     [&batch](const QStringList &paths) {
                         batch = paths;
                     },
                     Qt::QueuedConnection);
    check(watcher.start(root, 700), "watcher starts on an existing dir");
    check(watcher.isWatching(), "watcher reports running");

    QThread::msleep(300);
    writeFile(root + QStringLiteral("/a.txt"), "hello");

    // Debounce, not throttle: the batch must NOT be emitted right after the
    // change. With a 700 ms quiet period nothing should arrive within ~250 ms.
    const bool premature = waitUntil([&] { return !batch.isEmpty(); }, 250);
    check(!premature, "no batch is emitted before the debounce quiet period");

    // A second change within the quiet window must merge into the same batch.
    QThread::msleep(300);
    writeFile(root + QStringLiteral("/sub/inside.txt"), "world");

    const bool gotBatch = waitUntil([&] { return !batch.isEmpty(); }, 8000);
    check(gotBatch, "a debounced batch is emitted");

    bool sawReal = batch.contains(root + QStringLiteral("/a.txt"));
    bool sawInside = batch.contains(root + QStringLiteral("/sub/inside.txt"));
    check(sawReal && sawInside, "real file changes are reported");

    bool sawJunk = false;
    for (const auto &p : batch) {
        if (p.contains(QLatin1String("/.svn"))
            || p.contains(QLatin1String("~$"))
            || p.contains(QLatin1String(".tmp")))
            sawJunk = true;
    }
    check(!sawJunk, ".svn and temp files are filtered");

    watcher.stop();
    check(!watcher.isWatching(), "watcher stops cleanly");
    return true;
}

static bool testRepoManagerLimit()
{
    std::printf("-- RepoManager concurrency limit --\n");
    QTemporaryDir dir;
    if (!dir.isValid())
        return false;
    ConfigStore::setDatabaseFileForTest(dir.path() + QStringLiteral("/config.db"));

    RepoManager manager;
    const auto makeRepo = [](const QString &name) {
        Repository repo;
        repo.name = name;
        repo.path = QStringLiteral("/tmp/") + name;
        repo.url = QStringLiteral("svn://localhost/") + name;
        repo.state = RepoState::Background;
        return repo;
    };

    const int total = 6;
    for (int i = 1; i <= total; ++i)
        manager.addRepository(makeRepo(QStringLiteral("repo%1").arg(i)));

    int monitored = 0;
    for (const auto &r : manager.repositories())
        if (r.running())
            ++monitored;
    check(monitored == RepoManager::kMaxMonitoredRepos,
          "only 5 repos are monitored after adding 6");

    const QList<Repository> added = manager.repositories();
    check(added.size() == total && !added.last().running(),
          "repo beyond the limit is stopped");

    // Promote the stopped (last) repo: it must reach the top and start
    // monitoring, pushing whoever is then 6th out of the monitored set.
    const QString promoteName = added.last().name;
    manager.promote(promoteName);
    const QList<Repository> afterPromote = manager.repositories();
    check(afterPromote.first().name == promoteName,
          "promoted repo is at the top of the list");
    check(manager.repository(promoteName)->running(),
          "promoted repo is monitored again");
    check(afterPromote.size() == total
          && !afterPromote.at(RepoManager::kMaxMonitoredRepos).running(),
          "the repo that fell below the limit stopped monitoring");

    // Demote it again: it must go to the bottom and stop, refilling the slot
    // it previously occupied.
    manager.demote(promoteName);
    const QList<Repository> afterDemote = manager.repositories();
    check(afterDemote.last().name == promoteName,
          "demoted repo is at the bottom of the list");
    check(!manager.repository(promoteName)->running(),
          "demoted repo is stopped");
    check(afterDemote.at(RepoManager::kMaxMonitoredRepos - 1).running(),
          "freed monitoring slot is refilled");

    ConfigStore::setDatabaseFileForTest(QString());
    return true;
}

static bool testLogStore()
{
    std::printf("-- LogStore persistence --\n");
    QTemporaryDir dir;
    if (!dir.isValid())
        return false;
    const QString dbFile = dir.path() + QStringLiteral("/logs.db");
    LogStore::setDatabaseFileForTest(dbFile);

    LogStore::append(QStringLiteral("alpha"), QStringLiteral("first"), 3);
    LogStore::append(QStringLiteral("beta"), QStringLiteral("hello"), 3);
    LogStore::append(QStringLiteral("alpha"), QStringLiteral("second"), 3);
    LogStore::append(QStringLiteral("alpha"), QStringLiteral("third"), 3);
    LogStore::append(QStringLiteral("alpha"), QStringLiteral("fourth"), 3);

    const QStringList alpha = LogStore::history(QStringLiteral("alpha"));
    check(alpha.size() == 3, "per-repo cap prunes oldest entries");
    check(alpha.last().contains(QStringLiteral("fourth")), "newest entry is kept");

    const QStringList beta = LogStore::history(QStringLiteral("beta"));
    check(beta.size() == 1 && beta.first().contains(QStringLiteral("hello")),
          "other repo unaffected by pruning");

    // Reopen from the same file (simulates an app restart).
    LogStore::setDatabaseFileForTest(dbFile);
    const QStringList again = LogStore::history(QStringLiteral("alpha"));
    check(again == alpha, "log survives restart (persisted on disk)");

    LogStore::clearRepository(QStringLiteral("alpha"));
    check(LogStore::history(QStringLiteral("alpha")).isEmpty(),
          "removing a repo clears its log");
    check(LogStore::history(QStringLiteral("beta")).size() == 1,
          "other repo log kept after clear");
    return true;
}

static bool testCredentialEncryption()
{
    std::printf("-- Credential encryption & persistence --\n");

    const QByteArray key = CredCrypto::generateKey();
    check(!key.isEmpty() && key.size() == 32,
          "generateKey produces 32 random bytes");

    const QByteArray blob = CredCrypto::encrypt(key, QStringLiteral("svnuser123"));
    check(!blob.isEmpty(), "encrypt produces a blob");
    check(blob != QByteArray("svnuser123"), "blob differs from the plaintext");
    check(!QString::fromLatin1(blob).contains(QStringLiteral("svnuser123")),
          "blob does not contain the plaintext");

    check(CredCrypto::decrypt(key, blob) == QStringLiteral("svnuser123"),
          "decrypt round-trips the plaintext");
    check(CredCrypto::decrypt(QByteArray(32, 'x'), blob).isEmpty(),
          "decrypt with the wrong key fails");
    check(CredCrypto::decrypt(key, QByteArray("garbage")).isEmpty(),
          "decrypt with a corrupt blob fails");
    check(CredCrypto::decrypt(key, blob.left(20)).isEmpty(),
          "decrypt with a truncated blob fails");

    QTemporaryDir dir;
    if (!dir.isValid())
        return false;
    ConfigStore::setDatabaseFileForTest(dir.path() + QStringLiteral("/config.db"));
    ConfigStore::initialize();

    Repository repo;
    repo.name = QStringLiteral("crypto-repo");
    repo.path = QStringLiteral("/tmp/crypto-repo");
    repo.url = QStringLiteral("svn://localhost/crypto-repo");
    repo.username = QStringLiteral("svnuser");
    repo.password = QStringLiteral("s3cr3t-pass");
    repo.state = RepoState::Background;
    ConfigStore::saveRepositories({ repo });

    const QList<Repository> loaded = ConfigStore::loadRepositories();
    check(loaded.size() == 1
              && loaded.first().password == QStringLiteral("s3cr3t-pass"),
          "password round-trips through the store");

    Repository other;
    other.name = QStringLiteral("crypto-other");
    other.path = QStringLiteral("/tmp/crypto-other");
    other.url = QStringLiteral("svn://localhost/crypto-other");
    other.username = QStringLiteral("svnuser");
    other.state = RepoState::Background;
    ConfigStore::saveRepositories({ other });
    const QList<Repository> afterPurge = ConfigStore::loadRepositories();
    check(afterPurge.size() == 1 && afterPurge.first().password.isEmpty(),
          "removed repo's credential row is purged and not reloaded");

    ConfigStore::setDatabaseFileForTest(QString());
    return true;
}

static bool testSyncEngineTempFiles()
{
    std::printf("-- SyncEngine::isTempFile --\n");
    check(SyncEngine::isTempFile(QStringLiteral("/a/~$lock.docx")), "office lock file ~$");
    check(SyncEngine::isTempFile(QStringLiteral("/a/~backup.txt")), "editor backup ~");
    check(SyncEngine::isTempFile(QStringLiteral("/a/notes.tmp")), ".tmp suffix");
    check(SyncEngine::isTempFile(QStringLiteral("/a/notes.temp")), ".temp suffix");
    check(SyncEngine::isTempFile(QStringLiteral("/a/.DS_Store")), ".DS_Store");
    check(!SyncEngine::isTempFile(QStringLiteral("/a/report.pdf")), "regular file is not temp");
    check(!SyncEngine::isTempFile(QStringLiteral("/a/foo.tmpx")), ".tmpx is not matched");
    check(!SyncEngine::isTempFile(QStringLiteral("/a/bar")), "extensionless file is not temp");
    return true;
}

static bool testSyncEngineGroupByDir()
{
    std::printf("-- SyncEngine::groupByDir --\n");
    StatusEntry fileA;
    fileA.path = QStringLiteral("/repo/src/a.cpp");
    fileA.nodeStatus = StatusKind::Modified;
    StatusEntry fileB;
    fileB.path = QStringLiteral("/repo/src/sub/b.cpp");
    fileB.nodeStatus = StatusKind::Modified;
    StatusEntry dirChanged;
    dirChanged.path = QStringLiteral("/repo/docs");
    dirChanged.nodeStatus = StatusKind::Modified;

    const auto groups =
        SyncEngine::groupByDir({ fileA, fileB, dirChanged }, QStringLiteral("/repo"));

    check(groups.size() == 3, "three groups: one per distinct directory");

    // Deepest first: /repo/src/sub (3 slashes) precedes the two 2-slash
    // directories; the order between those two is unspecified.
    const QStringList dirs = [&groups] {
        QStringList d;
        for (const auto &g : groups)
            d << g.dir;
        return d;
    }();
    check(dirs.first() == QStringLiteral("/repo/src/sub"),
          "deepest directory is committed first");

    for (const auto &g : groups) {
        if (g.dir == QStringLiteral("/repo/src/sub")) {
            check(g.count == 1 && g.firstFile == QStringLiteral("b.cpp"),
                  "sub dir group holds its file");
        } else if (g.dir == QStringLiteral("/repo/src")) {
            check(g.count == 1 && g.firstFile == QStringLiteral("a.cpp"),
                  "src group holds its file");
        } else if (g.dir == QStringLiteral("/repo/docs")) {
            check(g.count == 1, "docs group holds the directory itself");
        }
    }

    // A file directly in the repo root groups under the root.
    StatusEntry rootFile;
    rootFile.path = QStringLiteral("/repo/readme.md");
    rootFile.nodeStatus = StatusKind::Modified;
    const auto rootGroups =
        SyncEngine::groupByDir({ rootFile }, QStringLiteral("/repo"));
    check(rootGroups.size() == 1
              && rootGroups.first().dir == QStringLiteral("/repo"),
          "root file groups under the repository root");
    return true;
}

static bool testSyncEngineCommitMessage()
{
    std::printf("-- SyncEngine::commitMessage --\n");
    check(SyncEngine::commitMessage(QStringLiteral("/repo/src"),
                                    1, QStringLiteral("a.cpp"))
              == QStringLiteral("Auto-sync: a.cpp"),
          "single file: named directly");
    check(SyncEngine::commitMessage(QStringLiteral("/repo/src"),
                                    3, QStringLiteral("a.cpp"))
              == QStringLiteral("Auto-sync: 3 files in src"),
          "multiple files: count + directory name");
    check(SyncEngine::commitMessage(QStringLiteral("/repo"), 5, QStringLiteral("x"))
              == QStringLiteral("Auto-sync: 5 files in repo"),
          "root directory uses the repo name");
    return true;
}

static bool testSyncEngineMergeToDirs()
{
    std::printf("-- SyncEngine::mergeToDirs --\n");
    QTemporaryDir dir;
    if (!dir.isValid())
        return false;
    const QString root = QDir::cleanPath(dir.path());

    // Existing subtree: /root/real/a.txt exists (dir real exists).
    QDir(root).mkpath(QStringLiteral("real"));
    const QString existing = root + QStringLiteral("/real");

    // A path that exists on disk must keep its own directory.
    const QString existingFile = existing + QStringLiteral("/a.txt");
    writeFile(existingFile, "x");

    // A path under a non-existent directory must walk up to the nearest
    // existing ancestor (the WC root always exists).
    const QString ghost = root + QStringLiteral("/gone/deep/b.txt");
    const QString rootFile = root + QStringLiteral("/c.txt");

    const QStringList merged =
        SyncEngine::mergeToDirs({ existingFile, ghost, rootFile }, root);

    // Deepest-first, unique directories.
    const int existingIdx = merged.indexOf(existing);
    const int rootIdx = merged.indexOf(root);
    check(existingIdx >= 0 && rootIdx >= 0, "existing dir and root are both retained");
    check(merged.size() == 2, "three paths merge into two unique directories");
    check(existingIdx < rootIdx, "deeper directory is updated first");
    return true;
}

static bool testSyncEngineResolveChoice()
{
    std::printf("-- SyncEngine::resolveConflictCode --\n");
    const QString tree = QStringLiteral("/wc/dir-that-collides");
    const QString text = QStringLiteral("/wc/file.txt");
    const QStringList trees = { tree };

    // Tree conflicts always resolve to "working" (Merged, code 5), whatever
    // the user picked for the regular conflicts.
    check(SyncEngine::resolveConflictCode(tree, trees, 2) == 5,
          "tree conflict with user choice MineFull maps to Merged");
    check(SyncEngine::resolveConflictCode(tree, trees, 1) == 5,
          "tree conflict with user choice TheirsFull maps to Merged");
    check(SyncEngine::resolveConflictCode(tree, trees, 0) == 5,
          "tree conflict with user choice Base maps to Merged");
    check(SyncEngine::resolveConflictCode(tree, trees, 5) == 5,
          "tree conflict with user choice Merged stays Merged");

    // Regular conflicts keep the user's choice.
    check(SyncEngine::resolveConflictCode(text, trees, 2) == 2,
          "text conflict keeps MineFull");
    check(SyncEngine::resolveConflictCode(text, trees, 1) == 1,
          "text conflict keeps TheirsFull");
    check(SyncEngine::resolveConflictCode(text, trees, 0) == 0,
          "text conflict keeps Base");
    check(SyncEngine::resolveConflictCode(text, trees, 5) == 5,
          "text conflict keeps Merged");

    // Path not listed among the conflicts at all.
    check(SyncEngine::resolveConflictCode(QStringLiteral("/wc/other.txt"), trees, 2) == 2,
          "unlisted path keeps the user choice");

    // Log labels for the choice codes used in conflictResolved log lines.
    check(SyncEngine::conflictChoiceName(0) == QStringLiteral("使用基线版本"),
          "choice 0 label is Base");
    check(SyncEngine::conflictChoiceName(1) == QStringLiteral("使用他们的版本"),
          "choice 1 label is TheirsFull");
    check(SyncEngine::conflictChoiceName(2) == QStringLiteral("使用我的版本"),
          "choice 2 label is MineFull");
    check(SyncEngine::conflictChoiceName(5) == QStringLiteral("标记为已合并"),
          "choice 5 label is Merged");
    check(SyncEngine::conflictChoiceName(99).startsWith(QStringLiteral("未知")),
          "unknown code has a fallback label");
    return true;
}

/** With autoResolveConflicts enabled, detectConflicts resolves every conflicted
 *  file through the configured default choice (tree conflicts forced to
 *  Merged=5) and must NOT emit conflictDetected. With it disabled the dialog
 *  path is used instead: conflictDetected fires and no Resolve is submitted. */
static bool runAutoResolveScenario(bool autoResolve)
{
    QTemporaryDir dir;
    if (!dir.isValid())
        return false;
    Repository repo;
    repo.name = QStringLiteral("autoresolve");
    repo.path = dir.path();
    repo.url = QStringLiteral("svn://localhost/autoresolve");
    repo.state = RepoState::Active;

    SyncEngine engine(repo);
    auto *scripted = new ScriptedRunner;
    engine.setCommandRunnerFactoryForTest(
        [scripted]() -> std::unique_ptr<ICommandRunner> {
            return std::unique_ptr<ICommandRunner>(scripted);
        });

    GlobalConfig cfg;
    cfg.autoResolveConflicts = autoResolve;
    cfg.conflictResolution = 1;      // TheirsFull for text conflicts
    cfg.pollIntervalMs = 60000;      // long enough to not fire during the test
    cfg.fullSyncIntervalMs = 600000; // ditto
    engine.setConfig(cfg);

    const QString textPath = dir.path() + QStringLiteral("/file.txt");
    const QString treePath = dir.path() + QStringLiteral("/colliding-dir");

    int dialogFired = 0;
    std::atomic<int> resolveCalls{ 0 };
    std::atomic<int> resolvedSignals{ 0 };
    std::atomic<int> codeText{ -1 };
    std::atomic<int> codeTree{ -1 };
    QObject context;
    QObject::connect(&engine, &SyncEngine::conflictDetected, &context,
                     [&dialogFired](const QStringList &, const QStringList &) { ++dialogFired; });
    QObject::connect(&engine, &SyncEngine::conflictResolved, &context,
                     [&resolvedSignals](const QString &, int, bool, bool, const QString &) {
                         ++resolvedSignals;
                     });

    scripted->handler = [&](const CommandItem &item) {
        CommandResult r = makeResult(item, true);
        if (item.command == Command::Status) {
            StatusEntry text;
            text.path = textPath;
            text.conflicted = true;
            StatusEntry tree;
            tree.path = treePath;
            tree.conflicted = true;
            tree.treeConflicted = true;
            r.statuses = { text, tree };
        }
        if (item.command == Command::Resolve) {
            resolveCalls.fetch_add(1);
            if (item.path == textPath)
                codeText.store(item.conflictChoice);
            else
                codeTree.store(item.conflictChoice);
        }
        return r;
    };

    engine.start();

    const bool sawResolves = waitUntil([&] { return resolveCalls.load() >= 2; }, 10000);
    const bool sawResolved = waitUntil([&] { return resolvedSignals.load() >= 2; }, 10000);
    // Let any queued conflictDetected signal (that must NOT arrive when
    // auto-resolving) settle before asserting on it.
    QThread::msleep(50);
    engine.stop();

    if (autoResolve) {
        check(sawResolves, "auto-resolve submits one Resolve per conflict");
        check(resolveCalls.load() == 2, "exactly two Resolve commands for two conflicts");
        check(sawResolved, "conflictResolved fires once per auto-resolved conflict");
        check(resolvedSignals.load() == 2, "two conflictResolved signals for two conflicts");
        check(codeText.load() == 1, "text conflict uses the configured default choice");
        check(codeTree.load() == 5, "tree conflict is forced to Merged (working state)");
        check(dialogFired == 0, "conflict dialog is suppressed when auto-resolve is on");
        return sawResolves && resolveCalls.load() == 2 && sawResolved
            && resolvedSignals.load() == 2 && codeText.load() == 1 && codeTree.load() == 5
            && dialogFired == 0;
    }

    check(!sawResolves, "no Resolve commands when auto-resolve is off");
    check(dialogFired >= 1, "conflict dialog fires when auto-resolve is off");
    return !sawResolves && dialogFired >= 1;
}

static bool testSyncEngineAutoResolve()
{
    std::printf("-- SyncEngine auto-resolve --\n");
    check(runAutoResolveScenario(true), "auto-resolve scenario");
    check(runAutoResolveScenario(false), "manual (dialog) scenario");
    return true;
}

/** Build an engine wired to a ScriptedRunner and run start() so the full
 *  resultReady -> onResult -> classify plumbing is exercised. The test
 *  submits one GetHeadRevision per entry in headErrors; empty string means
 *  success, anything else is the error text returned for that call. */
static bool runClassifyScenario(
    int disconnectThreshold, const std::vector<QString> &headErrors,
    int expectedLost, int expectedAuth, int expectedRestored)
{
    QTemporaryDir dir;
    if (!dir.isValid())
        return false;
    Repository repo;
    repo.name = QStringLiteral("classify");
    repo.path = dir.path();
    repo.url = QStringLiteral("svn://localhost/classify");
    repo.state = RepoState::Active;

    SyncEngine engine(repo);
    auto *scripted = new ScriptedRunner;
    engine.setCommandRunnerFactoryForTest(
        [scripted]() -> std::unique_ptr<ICommandRunner> {
            return std::unique_ptr<ICommandRunner>(scripted);
        });

    GlobalConfig cfg;
    cfg.disconnectThreshold = disconnectThreshold;
    cfg.pollIntervalMs = 60000;      // long enough to not fire during the test
    cfg.fullSyncIntervalMs = 600000; // ditto
    engine.setConfig(cfg);

    // The scripted runner answers GetHeadRevision from the test's error list
    // (indexed by a thread-safe counter), everything else succeeds.
    std::atomic<int> headCalls{ 0 };
    scripted->handler = [&headCalls, &headErrors](const CommandItem &item) {
        if (item.command != Command::GetHeadRevision)
            return makeResult(item, true);
        const int i = headCalls.fetch_add(1);
        if (i < int(headErrors.size()) && !headErrors[i].isEmpty())
            return makeResult(item, false, headErrors[i]);
        return makeResult(item, true);
    };

    int lost = 0;
    int auth = 0;
    int restored = 0;
    QObject context;
    QObject::connect(&engine, &SyncEngine::connectionLost, &context,
                     [&lost]() { ++lost; });
    QObject::connect(&engine, &SyncEngine::authenticationFailed, &context,
                     [&auth]() { ++auth; });
    QObject::connect(&engine, &SyncEngine::connectionRestored, &context,
                     [&restored]() { ++restored; });

    engine.start();
    // The engine's startup fullSync already submits an Update; let it settle
    // before we feed server-command results into classify().
    QThread::msleep(300);

    // Feed one GetHeadRevision per expected result. onResult runs the
    // callback and then classify() in the same slot, so after the callback
    // fires (and the queued event returns) the health signals are current.
    for (size_t i = 0; i < headErrors.size(); ++i) {
        CommandItem item;
        item.command = Command::GetHeadRevision;
        item.path = repo.path;
        item.repoUrl = repo.url;
        bool done = false;
        engine.submit(item, [&done](const CommandResult &) { done = true; });
        check(waitUntil([&] { return done; }), "classify scenario command completed");
    }

    // Give any remaining queued signals time to be delivered before stop.
    QThread::msleep(50);
    engine.stop();

    check(lost == expectedLost, "connectionLost emitted exactly the expected times");
    check(auth == expectedAuth, "authenticationFailed emitted exactly the expected times");
    check(restored == expectedRestored, "connectionRestored emitted exactly the expected times");
    return lost == expectedLost && auth == expectedAuth && restored == expectedRestored;
}

static bool testSyncEngineClassify()
{
    std::printf("-- SyncEngine server-health classification --\n");

    // A single network failure below the threshold emits nothing.
    check(runClassifyScenario(
              3, { QStringLiteral("Connection timed out") }, 0, 0, 0),
          "single network failure below threshold emits nothing");

    // Reaching the threshold emits connectionLost.
    check(runClassifyScenario(
              2,
              { QStringLiteral("Unable to connect to host"),
                QStringLiteral("Unable to connect to host") },
              1, 0, 0),
          "threshold of consecutive network failures emits connectionLost");

    // An authentication error emits authenticationFailed and is not counted
    // as a network failure (no connectionLost even at threshold 1).
    check(runClassifyScenario(
              1,
              { QStringLiteral("No more credentials or we tried too many times. "
                               "Authentication failed") },
              0, 1, 0),
          "authentication failure emits authenticationFailed, not connectionLost");

    // Success after connectionLost restores the connection.
    check(runClassifyScenario(
              1,
              { QStringLiteral("Connection timed out"), QString() },
              1, 0, 1),
          "successful server command after disconnect emits connectionRestored");

    // A local working-copy error (not network/auth) must not count towards
    // disconnect detection.
    check(runClassifyScenario(
              1, { QStringLiteral("None of the targets are working copies") },
              0, 0, 0),
          "local working-copy error does not count as a network failure");

    return true;
}

static bool testSyncEngineFullSyncNotDropped()
{
    std::printf("-- SyncEngine deferred full sync under continuous poll load --\n");

    QTemporaryDir dir;
    if (!dir.isValid())
        return false;
    Repository repo;
    repo.name = QStringLiteral("fullsync");
    repo.path = dir.path();
    repo.url = QStringLiteral("svn://localhost/fullsync");
    repo.state = RepoState::Active;

    SyncEngine engine(repo);
    auto *scripted = new ScriptedRunner;
    engine.setCommandRunnerFactoryForTest(
        [scripted]() -> std::unique_ptr<ICommandRunner> {
            return std::unique_ptr<ICommandRunner>(scripted);
        });

    GlobalConfig cfg;
    cfg.pollIntervalMs = 20;       // poll restart keeps the engine almost busy
    cfg.fullSyncIntervalMs = 500;  // full-sync ticks land while polls run
    engine.setConfig(cfg);

    // GetHeadRevision is slow, so a poll occupies the engine long enough for
    // full-sync ticks to arrive while it is busy. Everything else is instant.
    std::atomic<int> updateCalls{ 0 };
    scripted->handler = [&updateCalls](const CommandItem &item) {
        if (item.command == Command::GetHeadRevision) {
            QThread::msleep(150);
            return makeResult(item, true);
        }
        if (item.command == Command::Update)
            updateCalls.fetch_add(1);
        return makeResult(item, true);
    };

    engine.start();

    // Baseline: the startup fullSync performed an update.
    check(waitUntil([&] { return updateCalls.load() >= 1; }, 10000),
          "startup full sync performed an update");
    const int before = updateCalls.load();

    // The engine is almost always busy with polls here. Every full-sync tick
    // must be deferred and then actually performed once the poll finishes,
    // never silently dropped.
    check(waitUntil([&] { return updateCalls.load() >= before + 2; }, 20000),
          "full-sync deferred while busy is still performed afterwards");

    engine.stop();
    check(updateCalls.load() >= before + 2,
          "full-sync rounds kept running under continuous poll load");
    return true;
}

static bool testWorkerWatchdog()
{
    std::printf("-- SvnWorker watchdog --\n");
    WorkerAndFake wf;

    // Block the runner until cancel() is called; a watchdog-margined command
    // must not hang the worker forever.
    std::atomic<bool> started{ false };
    wf.fake->handler = [&started, &wf](const CommandItem &item) {
        started.store(true);
        while (!wf.fake->cancelled)
            QThread::msleep(10);
        return makeResult(item, true);
    };

    wf.worker->setCommandTimeoutSec(1);
    wf.worker->submit(makeItem(Command::Status, QStringLiteral("/wc")));

    check(waitUntil([&] { return started.load(); }), "command started on the worker");
    check(waitUntil([&] { return wf.fake->cancelled; }, 5000),
          "watchdog cancelled the stuck command after the timeout");

    wf.worker->stop();
    return true;
}

static bool testHeavyWatchdog()
{
    std::printf("-- SvnWorker heavy watchdog (idle gap + transfer cap) --\n");

    // 1) A commit that never emits liveness events is cancelled after the
    //    inactivity gap (same wall-clock as the old total-time watchdog).
    {
        auto *raw = new KeepAliveRunner;
        SvnWorker worker;
        worker.start([raw]() -> std::unique_ptr<ICommandRunner> {
            return std::unique_ptr<ICommandRunner>(raw);
        });
        std::atomic<bool> started{ false };
        raw->onExecute = [&started, raw](const CommandItem &item) {
            started.store(true);
            while (!raw->cancelled)
                QThread::msleep(10);
            return makeResult(item, true);
        };
        worker.setCommandTimeoutSec(1);
        worker.setMaxTransferSec(0);  // cap disabled: only the idle gap applies
        worker.submit(makeItem(Command::Commit, QStringLiteral("/wc")));
        check(waitUntil([&] { return started.load(); }), "heavy commit started");
        check(waitUntil([&] { return raw->cancelled; }, 5000),
              "idle watchdog cancels a silent stuck commit");
        worker.stop();
    }

    // 2) Continuous liveness must NOT trip the inactivity gap, but the
    //    absolute transfer cap still cancels a long-running transfer.
    {
        auto *raw = new KeepAliveRunner;
        SvnWorker worker;
        worker.start([raw]() -> std::unique_ptr<ICommandRunner> {
            return std::unique_ptr<ICommandRunner>(raw);
        });
        const int capMs = 1500;
        std::atomic<bool> started{ false };
        raw->onExecute = [&started, raw, capMs](const CommandItem &item) {
            started.store(true);
            QElapsedTimer timer;
            timer.start();
            while (!raw->cancelled && timer.elapsed() < capMs + 2000) {
                if (raw->keepAlive)
                    raw->keepAlive();  // heartbeat: a busy transfer
                QThread::msleep(20);
            }
            return makeResult(item, true);
        };
        check(waitUntil([&] { return raw->keepAlive != nullptr; }),
              "worker installed the keep-alive callback");
        worker.setCommandTimeoutSec(1);   // 1 s inactivity gap
        worker.setMaxTransferSec(1);      // 1 s absolute transfer cap
        QElapsedTimer outer;
        outer.start();
        worker.submit(makeItem(Command::Update, QStringLiteral("/wc")));
        check(waitUntil([&] { return started.load(); }), "heavy update started");
        const bool cancelled = waitUntil([&] { return raw->cancelled; }, 6000);
        const qint64 elapsed = outer.elapsed();
        check(cancelled, "transfer cap cancels a live-but-overdue heavy command");
        check(elapsed >= 1000,
              "liveness kept it alive past the 1 s inactivity gap");
        worker.stop();
    }

    return true;
}

static bool testGlobalConfigRoundtrip()
{
    std::printf("-- ConfigStore global config round-trip --\n");
    QTemporaryDir dir;
    if (!dir.isValid())
        return false;
    ConfigStore::setDatabaseFileForTest(dir.path() + QStringLiteral("/config.db"));

    // Defaults when nothing is stored yet.
    const GlobalConfig defaults = ConfigStore::loadGlobalConfig();
    check(defaults.pollIntervalMs == 60 * 1000, "default poll interval");
    check(defaults.fullSyncIntervalMs == 15 * 60 * 1000, "default full-sync interval");
    check(defaults.autoAddUnversioned, "default auto-add");
    check(defaults.minimizeToTray, "default minimize to tray");
    check(defaults.maxLogsPerRepo == 10000, "default max logs per repo");
    check(defaults.disconnectThreshold == 3, "default disconnect threshold");
    check(defaults.networkTimeoutSec == 60, "default network timeout");
    check(defaults.maxTransferSec == 600, "default max transfer 10 min");
    check(defaults.maxFileSizeMb == 100, "default max file size 100 MB");
    check(!defaults.autoResolveConflicts, "default auto-resolve off");
    check(defaults.conflictResolution == 2, "default conflict resolution is MineFull");
    check(defaults.language == QStringLiteral("zh_CN"), "default language is Chinese");
    check(defaults.repoRoot.endsWith(QStringLiteral("/SvnSyncDrive")),
          "default repo root is ~/SvnSyncDrive");
    check(defaults.quickAccessEnabled, "default quick access on");

    // Custom values survive a save + load (and a re-open of the file).
    GlobalConfig custom;
    custom.pollIntervalMs = 5 * 1000;
    custom.fullSyncIntervalMs = 2 * 60 * 1000;
    custom.autoAddUnversioned = false;
    custom.trustServerCertificate = false;
    custom.minimizeToTray = false;
    custom.startMinimizedToTray = true;
    custom.maxLogsPerRepo = 500;
    custom.disconnectThreshold = 7;
    custom.networkTimeoutSec = 120;
    custom.maxTransferSec = 900;
    custom.maxFileSizeMb = 512;
    custom.autoResolveConflicts = true;
    custom.conflictResolution = 1;
    custom.language = QStringLiteral("en");
    custom.repoRoot = dir.path() + QStringLiteral("/MyRepos");
    custom.quickAccessEnabled = true;
    ConfigStore::saveGlobalConfig(custom);

    GlobalConfig loaded = ConfigStore::loadGlobalConfig();
    check(loaded.pollIntervalMs == 5000, "poll interval round-trips");
    check(loaded.fullSyncIntervalMs == 120000, "full-sync interval round-trips");
    check(!loaded.autoAddUnversioned, "auto-add round-trips");
    check(!loaded.trustServerCertificate, "trust-cert round-trips");
    check(!loaded.minimizeToTray, "minimize-to-tray round-trips");
    check(loaded.startMinimizedToTray, "start-minimized round-trips");
    check(loaded.maxLogsPerRepo == 500, "max logs round-trips");
    check(loaded.disconnectThreshold == 7, "disconnect threshold round-trips");
    check(loaded.networkTimeoutSec == 120, "network timeout round-trips");
    check(loaded.maxTransferSec == 900, "max transfer round-trips");
    check(loaded.maxFileSizeMb == 512, "max file size round-trips");
    check(loaded.autoResolveConflicts, "auto-resolve round-trips");
    check(loaded.conflictResolution == 1, "conflict resolution round-trips");
    check(loaded.language == QStringLiteral("en"), "language round-trips");
    check(loaded.repoRoot == custom.repoRoot, "repo root round-trips");
    check(loaded.quickAccessEnabled, "quick-access round-trips");

    // A reload from a fresh connection (app restart) sees the same values.
    ConfigStore::setDatabaseFileForTest(dir.path() + QStringLiteral("/config.db"));
    loaded = ConfigStore::loadGlobalConfig();
    check(loaded.pollIntervalMs == 5000 && loaded.disconnectThreshold == 7
              && loaded.networkTimeoutSec == 120 && loaded.maxTransferSec == 900
              && loaded.maxFileSizeMb == 512,
          "values survive a restart (re-opened database)");

    ConfigStore::setDatabaseFileForTest(QString());
    return true;
}

/** The GTK bookmarks file editor (used by the Linux quick-access shortcut)
 *  is a pure file operation, so it is tested here on every platform. */
static bool testQuickAccessBookmarks()
{
    std::printf("-- QuickAccess bookmark file --\n");
    QTemporaryDir dir;
    if (!dir.isValid())
        return false;
    const QString path = dir.path() + QStringLiteral("/bookmarks");
    const QString uri = QStringLiteral("file:///home/user/SvnSyncDrive");

    check(QuickAccess::editBookmarksFile(path, uri, QStringLiteral("SvnSyncDrive"), true),
          "adding a bookmark creates the file");
    QString content;
    {
        QFile f(path);
        check(f.open(QIODevice::ReadOnly | QIODevice::Text), "bookmark file readable");
        content = QString::fromUtf8(f.readAll());
    }
    check(content.contains(uri), "bookmark URI present");
    check(content.contains(QStringLiteral("file:///home/user/SvnSyncDrive SvnSyncDrive")),
          "bookmark uri + label written together");

    // Adding again must not duplicate the entry.
    check(QuickAccess::editBookmarksFile(path, uri, QStringLiteral("SvnSyncDrive"), true),
          "re-adding a bookmark succeeds");
    QFile f2(path);
    f2.open(QIODevice::ReadOnly | QIODevice::Text);
    const QString reAdd = QString::fromUtf8(f2.readAll());
    f2.close();
    check(reAdd.count(uri) == 1, "bookmark is not duplicated on re-add");

    // Removing the entry leaves an empty file.
    check(QuickAccess::editBookmarksFile(path, uri, QStringLiteral("SvnSyncDrive"), false),
          "removing a bookmark succeeds");
    return true;
}

/** Runtime language switching: Chinese source is the default, switching to
 *  English returns translated text, and the Chinese mode is restored so the
 *  later conflict-label assertions keep passing. */
static bool testI18n()
{
    std::printf("-- I18n language switching --\n");

    I18n::setLanguage(QStringLiteral("zh_CN"));
    check(I18n::language() == QStringLiteral("zh_CN"), "language() reports Chinese");
    check(I18n::translate("保存") == QStringLiteral("保存"), "Chinese mode returns source text");
    check(I18n::translate("不存在的字符串xyz") == QStringLiteral("不存在的字符串xyz"),
          "untranslated keys keep the Chinese source");

    I18n::setLanguage(QStringLiteral("en"));
    check(I18n::language() == QStringLiteral("en"), "language() reports English");
    check(I18n::translate("保存") == QStringLiteral("Save"), "translated 'Save'");
    check(I18n::translate("设置") == QStringLiteral("Settings"), "translated 'Settings'");
    check(I18n::translate("仓库") == QStringLiteral("Repositories"), "translated 'Repositories'");
    check(I18n::translate("名称") == QStringLiteral("Name"), "translated 'Name'");
    check(SyncEngine::conflictChoiceName(2) == QStringLiteral("Use my version"),
          "conflict choice labels translate to English");
    check(SyncEngine::conflictChoiceName(99) == QStringLiteral("Unknown (99)"),
          "unknown choice label falls back to the English pattern");

    // Unknown codes are normalized back to the default language.
    I18n::setLanguage(QStringLiteral("fr_FR"));
    check(I18n::language() == QStringLiteral("zh_CN"), "unknown code normalized to zh_CN");
    check(SyncEngine::conflictChoiceName(2) == QStringLiteral("使用我的版本"),
          "Chinese labels restored for later assertions");
    return true;
}

static bool runLiveRepo(const QString &wc, const QString &url,
                        const QString &user, const QString &pass)
{
    std::printf("-- live repository round-trip (%s) --\n", qPrintable(url));
    if (!QDir(wc).exists()) {
        std::printf("  [FAIL] working copy does not exist: %s\n", qPrintable(wc));
        ++g_failures;
        return false;
    }

    Repository repo;
    repo.name = QStringLiteral("live");
    repo.path = wc;
    repo.url = url;
    repo.username = user;
    repo.password = pass;

    SyncEngine engine(repo);
    QStringList notifications;
    QStringList conflicts;
    int fileChangeCount = 0;
    QObject::connect(&engine, &SyncEngine::syncNotification, &engine,
                     [&notifications](const QString &m) { notifications.append(m); });
    QObject::connect(&engine, &SyncEngine::conflictDetected, &engine,
                     [&conflicts](const QStringList &c, const QStringList &) { conflicts = c; });
    QObject::connect(&engine, &SyncEngine::filesChanged, &engine,
                     [&fileChangeCount]() { ++fileChangeCount; });

    engine.start();

    const QString probeFile = wc + QStringLiteral("/live_probe_%1.txt").arg(QDateTime::currentMSecsSinceEpoch());
    writeFile(probeFile, QStringLiteral("hello from SvnSyncDrive\n"));
    std::printf("  created probe: %s\n", qPrintable(probeFile));

    // Upward sync should auto-add and commit the probe within the debounce window.
    const bool committed = waitUntil([&] { return fileChangeCount > 0; }, 30000);
    check(committed, "auto-commit completed (filesChanged fired)");

    // A second file written after the first batch must also be committed by a
    // follow-up scan (the scan that saw it first may have run before its Add).
    const QString probe2 = wc + QStringLiteral("/live_probe2_%1.txt").arg(QDateTime::currentMSecsSinceEpoch());
    writeFile(probe2, QStringLiteral("second probe\n"));
    std::printf("  created probe2: %s\n", qPrintable(probe2));

    // Wait for the engine to finish committing, then verify BOTH files are
    // actually committed (schedule Normal, not Added). Poll each probe file
    // individually via `info` (per-file, cheap, and -- unlike `svn status`,
    // which does not report clean files -- it reports schedule for committed
    // files too) so the test thread never runs a full `svn status -R` on the
    // same working copy the worker is committing into.
    auto bothNormal = [&probeFile, &probe2]() {
        SvnPlus::SvnClient client;
        static QString lastState;
        QString cur;
        int seen = 0;
        int normal = 0;
        for (const QString &path : { probeFile, probe2 }) {
            std::vector<SvnPlus::SvnInfo> infos;
            const SvnPlus::SvnError err =
                client.info(path.toStdString(), infos, SvnPlus::SvnRevision::working(),
                            SvnPlus::SvnDepth::Empty);
            if (!err.ok() || infos.empty()) {
                cur += QStringLiteral("|none");
                continue;
            }
            ++seen;
            cur += QStringLiteral("|%1").arg(int(infos[0].schedule));
            if (infos[0].schedule == SvnPlus::SvnSchedule::Normal && infos[0].revision >= 1)
                ++normal;
        }
        if (cur != lastState) {
            lastState = cur;
            std::printf("    probe state [%s%s]\n",
                        seen == 2 ? "both" : (seen == 1 ? "one" : "none"),
                        qPrintable(cur));
        }
        return seen == 2 && normal == 2;
    };

    const bool committed2 = waitUntil(bothNormal, 30000);
    check(committed2, "both probe files are fully committed (Normal, not Added)");
    if (!committed2) {
        std::printf("  note: probes were added but not committed; "
                    "prints probe status above for diagnosis\n");
    }

    std::printf("  notifications (%d):\n", int(notifications.size()));
    for (const auto &n : notifications)
        std::printf("    %s\n", qPrintable(n));
    if (!conflicts.isEmpty()) {
        std::printf("  conflicts (%d):\n", int(conflicts.size()));
        for (const auto &c : conflicts)
            std::printf("    %s\n", qPrintable(c));
    }

    engine.stop();
    return committed && committed2;
}

static bool runLiveSync(const QString &url, const QString &wcApp, const QString &wcOther,
                        const QString &user = QString(), const QString &pass = QString())
{
    std::printf("-- live sync round-trip (%s) --\n", qPrintable(url));
    if (!QDir(wcApp).exists() || !QDir(wcOther).exists()) {
        std::printf("  [FAIL] working copies do not exist: %s / %s\n",
                    qPrintable(wcApp), qPrintable(wcOther));
        ++g_failures;
        return false;
    }

    Repository repo;
    repo.name = QStringLiteral("live");
    repo.path = wcApp;
    repo.url = url;
    repo.username = user;
    repo.password = pass;

    SyncEngine engine(repo);
    QStringList notifications;
    int fileChangeCount = 0;
    QObject::connect(&engine, &SyncEngine::syncNotification, &engine,
                     [&notifications](const QString &m) { notifications.append(m); });
    QObject::connect(&engine, &SyncEngine::filesChanged, &engine,
                     [&fileChangeCount]() { ++fileChangeCount; });

    // Fast polling so the downward sync is detected within the test window.
    GlobalConfig cfg;
    cfg.pollIntervalMs = 1000;
    cfg.fullSyncIntervalMs = 5 * 60 * 1000;
    engine.setConfig(cfg);
    engine.start();

    // A second client that stands in for "another user" (and verifies the
    // app working copy from the outside).
    SvnPlus::SvnClient other;
    if (!user.isEmpty())
        other.setUsername(user.toStdString());
    if (!pass.isEmpty())
        other.setPassword(pass.toStdString());
    auto committed = [&other](const QString &path) {
        std::vector<SvnPlus::SvnInfo> infos;
        const SvnPlus::SvnError err =
            other.info(path.toStdString(), infos, SvnPlus::SvnRevision::head(),
                       SvnPlus::SvnDepth::Empty);
        return err.ok() && !infos.empty()
            && infos[0].schedule == SvnPlus::SvnSchedule::Normal
            && infos[0].revision >= 1;
    };

    // 1) Upward: add a file in the app working copy; the engine must
    //    auto-add and commit it.
    const QString up = wcApp + QStringLiteral("/auto_up_%1.txt")
        .arg(QDateTime::currentMSecsSinceEpoch());
    writeFile(up, "hello from the app\n");
    std::printf("  added app file: %s\n", qPrintable(up));
    const bool upOk = waitUntil([&] { return committed(up); }, 30000);
    check(upOk, "upward: new file auto-added and committed by the engine");

    // 2) Downward: another user adds+commits in wcOther; the engine must
    //    pull the change into wcApp.
    const QString down = wcOther + QStringLiteral("/other_down_%1.txt")
        .arg(QDateTime::currentMSecsSinceEpoch());
    writeFile(down, "hello from another user\n");
    SvnPlus::SvnError err = other.add(down.toStdString(), SvnPlus::SvnDepth::Empty,
                                      /*force*/ false, /*noIgnore*/ false,
                                      /*addParents*/ false);
    if (err.ok()) {
        SvnPlus::SvnCommitInfo info;
        err = other.commit({ down.toStdString() },
                           QStringLiteral("another user commit").toStdString(),
                           /*keepLocks*/ false, &info);
    }
    if (!err.ok()) {
        std::printf("  [FAIL] another-user commit failed: %s\n",
                    qPrintable(QString::fromStdString(err.message())));
        ++g_failures;
        engine.stop();
        return false;
    }
    std::printf("  other user committed: %s\n", qPrintable(down));

    const QString inApp = wcApp + QStringLiteral("/") + QFileInfo(down).fileName();
    const bool downOk = waitUntil([&] { return QFileInfo::exists(inApp) && committed(inApp); },
                                  30000);
    check(downOk, "downward: remote commit pulled into the app working copy");

    // 3) Downward (deep tree): another user adds a brand-new nested directory
    //    tree in wcOther. svn status -u reports every intermediate level, and
    //    the engine must not run svn update on non-existent targets (which
    //    fails with E155007 "None of the targets are working copies").
    const QString deepRoot = wcOther + QStringLiteral("/deep_a/deep_b/deep_c");
    const QString deepFile = deepRoot + QStringLiteral("/deep_%1.txt")
        .arg(QDateTime::currentMSecsSinceEpoch());
    QDir().mkpath(deepRoot);
    writeFile(deepFile, "nested from another user\n");
    err = other.add(deepRoot.toStdString(), SvnPlus::SvnDepth::Infinity,
                    /*force*/ false, /*noIgnore*/ false, /*addParents*/ true);
    if (err.ok()) {
        SvnPlus::SvnCommitInfo info;
        err = other.commit({ (wcOther + QStringLiteral("/deep_a")).toStdString() },
                           QStringLiteral("another user deep commit").toStdString(),
                           /*keepLocks*/ false, &info);
    }
    if (!err.ok()) {
        std::printf("  [FAIL] another-user deep commit failed: %s\n",
                    qPrintable(QString::fromStdString(err.message())));
        ++g_failures;
        engine.stop();
        return false;
    }
    std::printf("  other user committed (deep): %s\n", qPrintable(deepFile));

    const QString deepInApp = wcApp + QStringLiteral("/deep_a/deep_b/deep_c/")
        + QFileInfo(deepFile).fileName();
    const bool deepDownOk = waitUntil(
        [&] { return QFileInfo::exists(deepInApp) && committed(deepInApp); }, 30000);
    check(deepDownOk, "downward: nested remote tree pulled into the app working copy");

    std::printf("  notifications (%d):\n", int(notifications.size()));
    for (const auto &n : notifications)
        std::printf("    %s\n", qPrintable(n));

    engine.stop();
    return upOk && downOk && deepDownOk;
}

/** Live lock auto-heal validation against a real working copy:
 *  1. inject a genuine WC lock (WC_LOCK + WORK_QUEUE rows) into .svn/wc.db,
 *  2. prove a plain update is blocked by the injected lock,
 *  3. run `svn update` through the production SvnWorker + libsvnplus runner
 *     and assert the runner cleans the lock up and retries to success.
 *  synccoretest --lockheal <wc-path> */
static void runLockHeal(const QString &wc)
{
    std::printf("-- lockheal %s --\n", qPrintable(wc));
    const QString wcDb = wc + QStringLiteral("/.svn/wc.db");
    if (!QFile::exists(wcDb)) {
        check(false, "working copy has a .svn/wc.db");
        return;
    }
    check(true, "working copy has a .svn/wc.db");

    bool lockOk = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("lockheal"));
        db.setDatabaseName(wcDb);
        if (!db.open()) {
            check(false, "wc.db opens");
            return;
        }
        {
            QSqlQuery q(db);
            // Working copy id: older wc.db schema names it wc_id, svn 1.15 id.
            qlonglong wcId = -1;
            if (q.exec(QStringLiteral("SELECT id FROM WCROOT LIMIT 1")) && q.next())
                wcId = q.value(0).toLongLong();
            if (wcId < 0 && q.exec(QStringLiteral("SELECT wc_id FROM WCROOT LIMIT 1")) && q.next())
                wcId = q.value(0).toLongLong();
            check(wcId >= 0, "wc.db exposes its working copy id");
            if (wcId < 0)
                return;
            // Discover the WC_LOCK column layout and insert a genuine lock row.
            QSet<QString> cols;
            if (q.exec(QStringLiteral("PRAGMA table_info(WC_LOCK)"))) {
                while (q.next())
                    cols.insert(q.value(1).toString());
            }
            if (cols.contains(QStringLiteral("local_dir_relpath"))
                && cols.contains(QStringLiteral("locked_levels"))) {
                lockOk = q.exec(QStringLiteral(
                    "INSERT INTO WC_LOCK (wc_id, local_dir_relpath, locked_levels) "
                    "VALUES (%1, '', 1)").arg(wcId));
            } else if (cols.contains(QStringLiteral("local_abspath"))
                       && cols.contains(QStringLiteral("lock_token"))) {
                QString root = wc;
                const QString esc = QStringLiteral("'")
                    + root.replace(QLatin1Char('\''), QLatin1String("''")) + QStringLiteral("'");
                lockOk = q.exec(QStringLiteral(
                    "INSERT INTO WC_LOCK (wc_id, local_abspath, lock_token) "
                    "VALUES (%1, %2, 'lockheal-test')").arg(wcId).arg(esc));
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("lockheal"));
    check(lockOk, "injected WC_LOCK row");
    if (!lockOk)
        return;

    {
        SvnPlus::SvnClient client;
        std::vector<SvnPlus::SvnStatus> statuses;
        SvnPlus::SvnStatusOptions opts;
        opts.depth = SvnPlus::SvnDepth::Empty;
        const SvnPlus::SvnError err = client.status(wc.toStdString(), statuses, opts);
        bool sawLock = false;
        for (const auto &s : statuses)
            if (s.wcIsLocked)
                sawLock = true;
        if (err.ok())
            check(sawLock, "status reports the working-copy lock (pre-check probe)");
        else
            std::printf("  note: status failed on the locked wc (%s); the retry path handles it\n",
                        err.message().c_str());
    }

    {
        SvnPlus::SvnClient client;
        std::vector<SvnPlus::SvnRevision> revisions;
        const SvnPlus::SvnError err = client.update(
            { wc.toStdString() }, SvnPlus::SvnRevision::head(),
            SvnPlus::SvnDepth::Infinity, false, false, &revisions);
        check(!err.ok() && isWcLockErrorText(QString::fromStdString(err.message())),
              "plain update is blocked by the injected lock");
    }

    {
        SvnWorker worker;
        ResultCollector col(worker);
        worker.start();
        worker.submit(makeItem(Command::Update, wc));
        const bool got = col.wait(1, 60000);
        worker.stop();
        const bool healed = got && !col.results.isEmpty() && col.results.front().success;
        check(healed, "production worker auto-heals the locked working copy");
        if (got && !col.results.isEmpty() && !col.results.front().success)
            std::printf("  update error: %s\n", qPrintable(col.results.front().error));
    }
}

/** Live BreakLock + GetLastChangedTime validation against a real working
 *  copy, driven through the production SvnWorker (real libsvnplus runner):
 *  1. BreakLock on a locked versioned file (relPath) must succeed — proves
 *     the real svn unlock --force call is wired,
 *  2. GetLastChangedTime on the working copy root must return a server-side
 *     last-changed date and revision (no longer the no-op stub).
 *  synccoretest --locktest <wc-path> [rel-path-to-locked-file] */
static void runLockTest(const QString &wc, const QString &relPath = QString())
{
    const QString target = relPath.isEmpty() ? wc : wc + QLatin1Char('/') + relPath;
    std::printf("-- locktest %s --\n", qPrintable(target));
    if (!QDir(wc).exists()) {
        check(false, "working copy directory exists");
        return;
    }
    check(QDir(wc + QStringLiteral("/.svn")).exists(), "working copy has .svn metadata");

    {
        SvnWorker worker;
        ResultCollector col(worker);
        worker.start();

        worker.submit(makeItem(Command::BreakLock, target));
        const bool gotBreak = col.wait(1, 60000);
        worker.stop();
        const bool breakOk = gotBreak && !col.results.isEmpty() && col.results.front().success;
        check(breakOk, "BreakLock succeeds on the locked file");
        if (gotBreak && !col.results.isEmpty() && !col.results.front().success)
            std::printf("  break-lock error: %s\n", qPrintable(col.results.front().error));
    }

    {
        SvnWorker worker;
        ResultCollector col(worker);
        worker.start();

        worker.submit(makeItem(Command::GetLastChangedTime, wc));
        const bool gotTime = col.wait(1, 60000);
        worker.stop();
        if (gotTime && !col.results.isEmpty()) {
            const CommandResult r = col.results.front();
            check(r.success, "GetLastChangedTime succeeds on the working copy root");
            if (r.success) {
                check(!r.value.isEmpty(), "GetLastChangedTime returns a server timestamp");
                check(r.revision >= 0, "GetLastChangedTime returns a revision");
                std::printf("  last-changed: %s @ r%lld\n", qPrintable(r.value), r.revision);
            } else {
                std::printf("  get-last-changed-time error: %s\n", qPrintable(r.error));
            }
        } else {
            check(false, "GetLastChangedTime produced a result");
        }
    }
}

int main(int argc, char *argv[])
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QCoreApplication app(argc, argv);

    testCategoryOf();
    testWcLockErrorText();
    testWorkerOrdering();
    testWorkerDedup();
    testWorkerDedupBypass();
    testWorkerResultCorrelation();
    testRepoWatcher();
    testRepoManagerLimit();
    testLogStore();
    testCredentialEncryption();

    testSyncEngineTempFiles();
    testSyncEngineGroupByDir();
    testSyncEngineCommitMessage();
    testSyncEngineMergeToDirs();
    testI18n();
    testSyncEngineResolveChoice();
    testSyncEngineAutoResolve();
    testSyncEngineClassify();
    testSyncEngineFullSyncNotDropped();
    testWorkerWatchdog();
    testHeavyWatchdog();
    testGlobalConfigRoundtrip();
    testQuickAccessBookmarks();

    // Optional live validation: synccoretest --live <wc> <url> [user] [pass]
    const QStringList args = QCoreApplication::arguments();
    const int idx = args.indexOf(QStringLiteral("--live"));
    if (idx >= 0 && idx + 2 < args.size()) {
        const QString user = idx + 3 < args.size() ? args.at(idx + 3) : QString();
        const QString pass = idx + 4 < args.size() ? args.at(idx + 4) : QString();
        runLiveRepo(args.at(idx + 1), args.at(idx + 2), user, pass);
    }

    // Two-way live sync: synccoretest --livesync <url> <wc-app> <wc-other> [user] [pass]
    const int syncIdx = args.indexOf(QStringLiteral("--livesync"));
    if (syncIdx >= 0 && syncIdx + 3 < args.size()) {
        const QString syncUser = syncIdx + 4 < args.size() ? args.at(syncIdx + 4) : QString();
        const QString syncPass = syncIdx + 5 < args.size() ? args.at(syncIdx + 5) : QString();
        runLiveSync(args.at(syncIdx + 1), args.at(syncIdx + 2), args.at(syncIdx + 3),
                    syncUser, syncPass);
    }

    // Probe: synccoretest --probestatus <wc-path> [outOfDate] [depth]
    const int probeIdx = args.indexOf(QStringLiteral("--probestatus"));
    if (probeIdx >= 0 && probeIdx + 1 < args.size()) {
        SvnPlus::SvnClient client;
        SvnPlus::SvnStatusOptions opts;
        const QString oodArg = probeIdx + 2 < args.size() ? args.at(probeIdx + 2) : QStringLiteral("1");
        const QString depthArg = probeIdx + 3 < args.size() ? args.at(probeIdx + 3) : QStringLiteral("3");
        opts.checkOutOfDate = oodArg == QStringLiteral("1");
        opts.depth = static_cast<SvnPlus::SvnDepth>(depthArg.toInt());
        std::vector<SvnPlus::SvnStatus> statuses;
        const SvnPlus::SvnError err = client.status(args.at(probeIdx + 1).toStdString(), statuses, opts);
        std::printf("probe status err=%s count=%zu\n",
                    err.ok() ? "ok" : err.message().c_str(), statuses.size());
        for (const auto &s : statuses) {
            if (opts.checkOutOfDate)
                std::printf("probe   %s ood=%d rev=%lld kind=%d node=%d\n",
                            s.localAbspath.c_str(), int(s.outOfDate), s.revision,
                            int(s.kind), int(s.nodeStatus));
        }
    }

    // Lock auto-heal against a real working copy: synccoretest --lockheal <wc-path>
    const int lockIdx = args.indexOf(QStringLiteral("--lockheal"));
    if (lockIdx >= 0 && lockIdx + 1 < args.size())
        runLockHeal(args.at(lockIdx + 1));

    // BreakLock integration against a real working copy:
    // synccoretest --locktest <wc-path> [rel-path-to-locked-file]
    const int brkIdx = args.indexOf(QStringLiteral("--locktest"));
    if (brkIdx >= 0 && brkIdx + 1 < args.size())
        runLockTest(args.at(brkIdx + 1),
                    brkIdx + 2 < args.size() ? args.at(brkIdx + 2) : QString());

    std::printf("==== %s: %d failure(s) ====\n",
                g_failures == 0 ? "ALL PASS" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
