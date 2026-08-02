#include "core/ConfigStore.h"
#include "core/CredCrypto.h"
#include "core/ICommandRunner.h"
#include "core/LogStore.h"
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
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <svnplus/SvnClient.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>

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

QList<CommandResult> collectResults(SvnWorker &worker, int expected)
{
    QList<CommandResult> results;
    QObject context;
    QObject::connect(&worker, &SvnWorker::resultReady, &context,
                     [&results](quint64, const CommandResult &r) { results.append(r); },
                     Qt::QueuedConnection);
    waitUntil([&] { return results.size() >= expected; });
    return results;
}

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

static bool testWorkerOrdering()
{
    std::printf("-- SvnWorker ordering --\n");
    WorkerAndFake wf;

    wf.worker->submit(makeItem(Command::Commit, QStringLiteral("/a")));
    wf.worker->submit(makeItem(Command::Status, QStringLiteral("/a")));
    wf.worker->submit(makeItem(Command::Add, QStringLiteral("/b")));
    wf.worker->submit(makeItem(Command::Update, QStringLiteral("/wc")));

    const auto results = collectResults(*wf.worker, 4);
    check(results.size() == 4, "all 4 commands delivered a result");

    QList<Command> expected = { Command::Status, Command::Add, Command::Commit, Command::Update };
    check(wf.fake->order == expected, "priority order: ReadOnly -> LocalWrite -> HeavyWrite");

    wf.worker->stop();
    return true;
}

static bool testWorkerDedup()
{
    std::printf("-- SvnWorker dedup --\n");
    WorkerAndFake wf;

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
    const auto results = collectResults(*wf.worker, 4);
    check(results.size() == 4, "dedup collapsed 6 submits into 4 executions");

    int commits = 0;
    int updatesA = 0;
    int updatesB = 0;
    for (const auto &r : results) {
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

    // Baseline: a plain Update with the same key is suppressed.
    wf.worker->submit(makeItem(Command::Update, QStringLiteral("/wc")));
    wf.worker->submit(makeItem(Command::Update, QStringLiteral("/wc")));

    // A bypassDedup Update (the 15-min fullSync one) on that same key must
    // run anyway: suppressing it would leave SyncEngine::fullSync waiting
    // forever for a result it never gets (m_fullSyncing stuck = dead).
    CommandItem scheduled = makeItem(Command::Update, QStringLiteral("/wc"));
    scheduled.bypassDedup = true;
    wf.worker->submit(scheduled);

    const auto results = collectResults(*wf.worker, 2);
    check(results.size() == 2,
          "bypassDedup update runs despite a queued same-key update");

    int updates = 0;
    for (const auto &r : results)
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
                     [&conflicts](const QStringList &c) { conflicts = c; });
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

int main(int argc, char *argv[])
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QCoreApplication app(argc, argv);

    testCategoryOf();
    testWorkerOrdering();
    testWorkerDedup();
    testWorkerDedupBypass();
    testWorkerResultCorrelation();
    testRepoWatcher();
    testRepoManagerLimit();
    testLogStore();
    testCredentialEncryption();

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

    std::printf("==== %s: %d failure(s) ====\n",
                g_failures == 0 ? "ALL PASS" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
