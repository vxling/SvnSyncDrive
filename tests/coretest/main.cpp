#include "core/ConfigStore.h"
#include "core/ICommandRunner.h"
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

    // Wait a moment for the commit to settle, then verify the file is versioned.
    QThread::msleep(1500);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    bool versioned = false;
    std::vector<SvnPlus::SvnStatus> statuses;
    SvnPlus::SvnClient client;
    SvnPlus::SvnStatusOptions opts;
    const SvnPlus::SvnError err = client.status(probeFile.toStdString(), statuses, opts);
    if (err.ok()) {
        for (const auto &s : statuses)
            if (s.versioned)
                versioned = true;
    }
    check(versioned, "probe file is under version control after auto-commit");
    if (err.ok() && !versioned)
        std::printf("  note: status ok but probe not versioned (commit may still be pending)\n");

    std::printf("  notifications (%d):\n", int(notifications.size()));
    for (const auto &n : notifications)
        std::printf("    %s\n", qPrintable(n));
    if (!conflicts.isEmpty()) {
        std::printf("  conflicts (%d):\n", int(conflicts.size()));
        for (const auto &c : conflicts)
            std::printf("    %s\n", qPrintable(c));
    }

    engine.stop();
    return committed && versioned;
}

int main(int argc, char *argv[])
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QCoreApplication app(argc, argv);

    testCategoryOf();
    testWorkerOrdering();
    testWorkerDedup();
    testWorkerResultCorrelation();
    testRepoWatcher();

    // Optional live validation: synccoretest --live <wc> <url> [user] [pass]
    const QStringList args = QCoreApplication::arguments();
    const int idx = args.indexOf(QStringLiteral("--live"));
    if (idx >= 0 && idx + 2 < args.size()) {
        const QString user = idx + 3 < args.size() ? args.at(idx + 3) : QString();
        const QString pass = idx + 4 < args.size() ? args.at(idx + 4) : QString();
        runLiveRepo(args.at(idx + 1), args.at(idx + 2), user, pass);
    }

    std::printf("==== %s: %d failure(s) ====\n",
                g_failures == 0 ? "ALL PASS" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
