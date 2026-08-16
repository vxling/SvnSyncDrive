# SvnSyncDrive 设计文档

> 目标：把项目的关键设计、核心流程、坑与决策一次性写清楚，避免下次靠重读代码重建上下文。
> 代码组织参考：SVNFileBox（参考实现位于 `C:\Users\xuser\AppData\Local\Temp\opencode\SVNFileBox`，核心参照 `src/Services/SyncService.cs`、`SvnCommandExecutor`、`FileWatcherService`）。

---

## 1. 项目概述

桌面 SVN 双向自动同步工具（Windows / Linux / macOS，Qt Widgets）：

- **上行同步（本地 → 服务器）**：监听工作副本文件变化，自动 `svn add` 未版本化文件、按目录分组自动 `commit` 已版本化变更。
- **下行同步（服务器 → 本地）**：定时查服务器 HEAD 与本地版本比较，若有差异则 `GetServerUpdatePaths`（本质 `svn status -u`）得到远端变更路径，按父目录合并后**分块 `svn update`**；不自动解决冲突，只上报。
- 全局配置、仓库配置、同步日志均持久化到本地 SQLite / 日志文件。

应用名 `SvnSyncDrive`，版本 0.3.0。

---

## 2. 技术栈与依赖

| 项 | 值 |
|---|---|
| 语言/标准 | C++17 |
| GUI | Qt 6.5+（组件 `Widgets Network Sql`；Windows 开发机为 6.11.1 msvc2022_64） |
| 构建 | CMake 3.24+ / Ninja；Windows MSVC，Linux GCC/Clang，macOS Clang |
| SVN | 自研封装库 **LibSVNPlus**（静态链接 `svnplus.lib`），内部链接 subversion 1.15（`libsvn_client-1` 等） |
| 存储 | SQLite（`QSQLITE` 驱动） |
| 平台 | Windows / Linux / macOS（文件监听：Windows 单句柄 `ReadDirectoryChangesW`，其他平台 `QFileSystemWatcher` + 目录树快照 diff） |

### 构建（Windows）

```powershell
# cmake/ninja 不在 PATH，需经 vcvars64 注入 VS 环境后构建
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && ninja -C build'
```

- 构建前若 GUI 在运行需先 `Stop-Process svnsyncdrive`（exe/pdb 被占用会失败）。
- 启动 GUI：`Start-Process build\svnsyncdrive.exe`。
- `CMakeLists.txt` 通过 `LIBSVNPLUS_ROOT`（默认 `C:/Users/xuser/Documents/LibSVNPlus/build/_stage`）找 `svnplus/include` 与 `svnplus/lib/svnplus.lib`，并链接 `libsvn_client-1 / libsvn_subr-1 / libapr-1 / libaprutil-1` 的 .lib。
- `copy_libsvn_runtime_dlls()`：把 `_stage/bin/*.dll`（svn 全家桶运行库）拷到目标旁；`deploy_qt()` 自动跑 `windeployqt`。

### 测试

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\live-sync-test.ps1        # 默认
powershell -ExecutionPolicy Bypass -File .\scripts\live-sync-test.ps1 -Keep  # 保留 fixture 供手工/GUI 复测
```

---

## 3. 架构总览（分层与线程模型）

```
┌───────────────────────────────────────────────────────────────┐
│ GUI 线程（QApplication）                                        │
│   MainWindow / RepoListPanel / RepoDetailPage / FileBrowser … │
│   RepoManager  ── owns ──> SyncEngine（每仓库一个，GUI 线程上） │
│                              │  submit(CommandItem)           │
│                              ▼                                │
│   SvnWorker（单线程）  ── 专属 ICommandRunner/SvnClient ──> SVN │
│   RepoWatcher（独立线程）─ ReadDirectoryChangesW ──> 文件事件     │
└───────────────────────────────────────────────────────────────┘
```

- **每仓库完全独立**：各自的 `SyncEngine` 拥有自己的 `RepoWatcher`、`SvnWorker`（含队列）、`poll/fullSync` 定时器。互相不阻塞（每个引擎独立 worker 线程）。
- 线程边界：
  - `SyncEngine` 只活在 GUI 线程；SVN 调用全部在 `SvnWorker` 线程。
  - 结果通过 `resultReady(id, result)` 信号（`Qt::QueuedConnection`）回传 GUI 线程。
  - `SyncEngine::submit()` 用 `m_pending`（`QHash<quint64, Callback>`）+ 自增 id 建立 回调↔结果 关联；回调在 GUI 线程执行。

### 库结构（`src/core`，无 GUI 依赖 → `svnsync_core` 静态库）

| 文件 | 职责 |
|---|---|
| `SvnCommand.h/.cpp` | 命令枚举、分类、`CommandItem/CommandResult/StatusEntry` |
| `SvnWorker.h/.cpp` | 后台命令执行器：三队列 + 去重 + 单线程循环 |
| `ICommandRunner.h` | 命令执行抽象（`SvnWorker` 线程内串行调用） |
| `RepoWatcher.h/.cpp` | 工作副本文件监听（debounce + 过滤） |
| `SyncEngine.h/.cpp` | 双向同步引擎（上行/下行/全量/syncNow） |
| `RepoManager.h/.cpp` | 每仓库引擎的拥有者、状态机、信号转发 |
| `Repository.h` | 仓库模型 + `RepoState` |
| `GlobalConfig.h` | 全局配置结构 |
| `ConfigStore.h/.cpp` | SQLite 持久化（仓库 + 全局配置）+ 注册表迁移 |
| `LogStore.h/.cpp` | 每仓库同步日志（SQLite） |
| `AppLog.h/.cpp` | 程序运行日志（追加式文件 + 轮转） |
| `AppPaths.h` | 数据目录 `~/.svnsyncdrive` |

GUI 在 `src/ui`：`MainWindow / RepoListPanel / RepoDetailPage / FileBrowser / AddRepoDialog / AboutDialog / ConflictDialog / SettingsDialog`。

---

## 4. 数据存储（`~/.svnsyncdrive`，隐藏目录）

`AppPaths::dataDir()` = `QDir::homePath() + "/.svnsyncdrive"`，首次访问自动建目录；`main` 启动即调用 `ConfigStore::initialize()` 物化目录。

| 文件 | 内容 |
|---|---|
| `config.db` | SQLite：仓库表 + 全局配置表（见下） |
| `logs.db` | SQLite：每仓库同步日志（单 `logs` 表，按仓库名过滤/清理） |
| `svnsyncdrive.log` | 程序运行日志（AppLog），超过大小限制轮转 |

> 注意：`LogStore.h`/`AppLog.h` 头文件里的旧注释写的路径（`AppDataLocation`、`~/svnsyncdrive`）已过时，**以 `.cpp` 实际实现为准**（都是 `AppPaths::dataDir()`）。

### config.db schema

```sql
repos(name TEXT PRIMARY KEY, path TEXT NOT NULL, url TEXT NOT NULL,
      username TEXT NOT NULL, state INTEGER NOT NULL)
global(key TEXT PRIMARY KEY, value TEXT NOT NULL)
```

- `global` 键：`pollIntervalMs`、`fullSyncIntervalMs`、`autoAddUnversioned`、`trustServerCertificate`、`minimizeToTray`、`maxLogsPerRepo`、`schemaVersion=1`。
- **密码刻意不持久化**：由 libsvn 自己的 auth cache 存（加密）；`username` 仅用于配置对话框展示。
- `ConfigStore::initialize()`：首次运行（`global` 为空）时，从注册表迁移旧仓库一次（`HKCU\Software\SvnSyncDrive\SvnSyncDrive\repositories`，`QSettings` 读入去重合并），再写默认 `GlobalConfig` + `schemaVersion=1`。

### 默认 GlobalConfig

```cpp
pollIntervalMs   = 60 * 1000      // 下行轮询间隔（查 HEAD）
fullSyncIntervalMs = 15 * 60 * 1000 // 定时全量同步间隔
autoAddUnversioned = true
trustServerCertificate = true     // 接受自签名证书
minimizeToTray = true
maxLogsPerRepo = 10000
```

---

## 5. 核心类型

### 5.1 Command 枚举与分类（`SvnCommand.h`）

```cpp
enum class Command {
    // ReadOnly（无队列、最高优先级）
    Info, Status, GetRevision, GetHeadRevision, GetConflictedFiles,
    GetLastChangedTime, IsVersioned, IsValidWorkingCopy, TestConnection,
    // LocalWrite（每次 HeavyWrite 前排空）
    Add, Delete, Move, Revert, Resolve, BreakLock,
    // HeavyWrite（一次一个）
    Commit, Update, Checkout
};
enum class Category { ReadOnly, LocalWrite, HeavyWrite };
Category categoryOf(Command);
```

- 调用方不关心分类，`submit()` 后由 worker 自行决定进哪个队列。

### 5.2 StatusDepth / StatusKind / StatusEntry

- `StatusDepth`（与 libsvnplus `SvnDepth` 数值一致）：`Empty=0, Files=1, Immediates=2, Infinity=3`。Status 默认 `Infinity`（整树）。
- `StatusKind`：None/Unversioned/Normal/Added/Missing/Deleted/Replaced/Modified/Merged/Conflicted/Ignored/Obstructed/External/Incomplete。
- `StatusEntry`：`path, nodeStatus, textStatus, reposStatus, versioned, conflicted, outOfDate, revision`。
  - `outOfDate` 仅在 `checkOutOfDate=true`（即 `-u`）时才有意义。

### 5.3 CommandItem / CommandResult

```cpp
struct CommandItem {
    quint64 id = 0;          // 0=由 worker 分配；非 0=保留调用方 id
    Command command;
    QString repo;            // 所属仓库名（写日志用），SyncEngine::submit 自动填
    QString path;            // 目标路径 / 工作副本根
    QString fromPath;        // Move 源
    QString message;         // commit 日志
    QString repoUrl;         // Checkout 目标 / GetHeadRevision 目标
    QString username, password;
    QStringList updatePaths; // update 子路径（空=整个工作副本）
    bool checkOutOfDate = false;  // status 是否 -u（远程比较）
    int conflictChoice = 2;  // resolve: 0Base 1TheirsFull 2MineFull 3TheirsConflict 4MineConflict 5Merged
    StatusDepth statusDepth = StatusDepth::Infinity;
};
struct CommandResult {
    quint64 id; Command command; QString path;
    QStringList paths;      // 实际生效路径（update 子路径时）
    bool success; QString error; qlonglong revision;
    QString value;          // 通用字符串载荷
    QList<StatusEntry> statuses; QList<InfoEntry> infos;
};
```

---

## 6. SvnWorker（后台命令执行器）

单线程循环，持有唯一 `ICommandRunner`（默认 `SvnCommandRunner`，内部一个 `SvnPlus::SvnClient`），保证 SvnClient 绝不被并发使用。

### 队列与优先级

```cpp
// submit() 按 categoryOf 分派：
//   ReadOnly  -> m_readOnly
//   LocalWrite-> m_localWrite + m_dedup[path]
//   HeavyWrite-> 若 heavyWriteAllowedLocked() 才入队 m_heavyWrite + m_dedup[dedupKey]
// takeNextLocked() 优先级：ReadOnly > LocalWrite > HeavyWrite
```

- **只读命令（含 `Status`）永远先于写命令执行**，不被写队列阻塞。唯一例外：若某个 HeavyWrite 正在执行（单线程占用中），只读命令要等它跑完才轮到——这是单 worker 的固有约束。
- **dedup（去重）**：
  - LocalWrite 按 `path` 去重（同路径重复 Add/Delete 只执行一次）。
  - HeavyWrite 按 `dedupKey = path + 排序后的 updatePaths` 去重；`Commit` 会压制同 key 的后续 `Commit/Update`，`Update` 会压制同 key 的 `Update/Commit`（避免冲突）。`Checkout` 与 `bypassDedup=true` 的命令永远放行。
  - 命令执行完从 `m_dedup` 移除（LocalWrite 按 path、HeavyWrite 按 dedupKey）。
  - **注意**：被去重掉的提交不会产生 resultReady，调用方的完成计数必须自己防重复（见 `SyncEngine::handleScanStatus` 的 `m_pendingAdds/m_pendingCommits`）。
  - **坑**：fullSync 的整库 Update（key=`path`，updatePaths 空）若与文件浏览器对仓库根目录的"更新"撞 key，会被 dedup 吞掉且永不产生结果 → `m_fullSyncing` 永久卡死。因此 fullSync 的 Update 必须 `bypassDedup=true`，与用户同 key 的 Update 串行执行（见决策 #11）。

### workerLoop

1. 等待 CV（条件：停止或任一队列非空）；`m_credsDirty` 时先刷凭据。
2. `takeNextLocked()` 取一条 → 在锁外执行 `m_runner->execute(item)`。
3. 移除 dedup → `logModify()`（非 ReadOnly 命令写 AppLog：命令名、repo、path、结果、revision）→ `emit resultReady(id, result)`。

### SvnCommandRunner 命令实现要点（`SvnWorker.cpp` 匿名空间）

| 命令 | 实现 |
|---|---|
| `Status` | `svn status`，深度按 `statusDepth`；`checkOutOfDate` 透传 libsvnplus。**上行扫描用本地模式（不加 -u）** |
| `GetRevision` | `info(path, SvnRevision::working())` —— 取**工作副本自身版本**（不是 HEAD！否则引擎永远以为最新、永不下拉） |
| `GetHeadRevision` | `info(repoUrl, head())` |
| `GetConflictedFiles` | status（本地）→ 过滤 `conflicted`，`value` 以 `;` 连接 |
| `IsVersioned` | status（本地）→ 任一 versioned 为 true |
| `IsValidWorkingCopy` | `info(path, head(), Empty)` 是否成功 |
| `TestConnection` | `info(repoUrl, head(), Empty)` |
| `GetLastChangedTime` | 桩实现（直接 success，未真正实现） |
| `Add` | `svn add`（Infinity，无 force/noIgnore/addParents） |
| `Delete` | `svn rm --force --keep-local` |
| `Move` | `svn move` |
| `Revert` | `svn revert`（Infinity） |
| `Resolve` | `svn resolve`，`conflictChoice`→`SvnConflictChoice` |
| `BreakLock` | **不支持**，直接失败 |
| `Commit` | `svn commit`，revision 取 `SvnCommitInfo` |
| `Update` | `svn update` 到 HEAD（Infinity），路径 = `updatePaths` 或 `path`；revision 取返回值的末尾项 |
| `Checkout` | `svn checkout` 到 HEAD（Infinity） |

---

## 7. RepoWatcher（文件监听）

- Windows 用**单个 `ReadDirectoryChangesW` 句柄 + `bWatchSubtree`**，子目录被替换也能继续收到通知（不像每目录一个句柄的方案会静默失效）。
- 事件 debounce 2s 后作为一个批次经 `filesChanged(paths)` 发出；`.svn` 与临时文件被过滤。
- 失败自动重连；独立线程运行。

---

## 8. SyncEngine（双向同步核心）

每仓库一个；GUI 线程；定时器 + 回调链驱动。

### 8.1 启动 / 停止

```cpp
start(): worker.start → setCredentials → setTrustServerCertificate
       → watcher.start(repo.path)（失败仅提示）
       → pollTimer.start(); fullSyncTimer.start()
       → 立即 poll()      // 拉取他机变更
       → 立即 scanAndCommit() // 补上 watcher 启动窗口期可能丢失的事件/上次残留
stop(): 停两个定时器 → watcher.stop → worker.stop
```

### 8.2 上行同步（本地 → 服务器）

流程链：

```
RepoWatcher::filesChanged ─> onWatcherBatch(paths)
    ├─ 每个 path：enqueueFileChange()
    │    不存在 → IsVersioned？是 → Delete
    │    存在   → 不做直接操作（靠 status 扫描自动 add/commit）
    └─ syncNow() → scanAndCommit()
```

`scanAndCommit()`（**m_scanning 防重入**）：
1. `submit(Command::Status, path=repo.path)` —— **纯本地，不加 -u**。
2. `handleScanStatus`：
   - 未版本化：`Unversioned` 且非临时文件 → `unversioned` 列表（临时文件规则：`~$`/`~` 前缀、`.tmp`/`.temp` 后缀、`.DS_Store`）。
   - 已版本化：排除 `conflicted`，`nodeStatus != Normal` 的进 `changes`。
   - 若 `autoAddUnversioned`：逐个 `submit(Add)`，路径记入 `m_pendingAdds` 防重复提交（worker 按 path 去重，重复提交会永远无结果、污染计数）。
   - `changes` 按目录分组（`groupByDir`，文件取其父目录、目录取自身，最深层目录先提交）→ 每个目录 `submit(Commit)`，记入 `m_pendingCommits` 防重复；提交成功且 `revision>0` 时 `m_lastLocalRev = max(...)`。
3. `maybeFinishScan`：`m_scanning && m_pendingCommits 空 && m_pendingAdds 空` → `finishScan`。
4. `finishScan`：清 `m_scanning`，`emit filesChanged`，`notify("批量同步完成")`；若 `m_rescanPending`（添加的未版本化文件需要再扫一次才能被 commit）→ 立即再 `scanAndCommit()`。

> 添加的 Add 完成后 `onAutoAddCompleted` 置 `m_rescanPending`，因为本轮 status 快照里它们还不是版本化的，必须重扫一次才能提交。

### 8.3 下行同步（服务器 → 本地）

**目标设计（对照 SVNFileBox `PollCoreAsync` + `UpdateInChunksAsync`）：**

```
poll()（60s 定时器；m_polling 防重入）
  ├─ Command::GetRevision        → 本地工作副本版本 localRev
  ├─ localRev = max(本地版本, m_lastLocalRev)
  ├─ Command::GetHeadRevision    → 服务器 HEAD serverRev
  ├─ serverRev <= localRev ? 结束
  └─ serverRev > localRev ? startUpdateInChunks(serverRev, localRev):
       ├─ Command::GetServerUpdatePaths  （status -u，取 outOfDate 路径）
       ├─ 无远端变更 → 结束
       ├─ mergeToDirs：远端路径 → 唯一父目录，最深层优先
       │    目标不存在 → 上溯最近存在祖先（WC 根必存在），避免 E155007
       └─ 每个目录一个 Command::Update（updatePaths={dir}），全部完成后：
            detectConflicts（本地 status → conflicted → emit conflictDetected）
            notify("已从服务器更新 r%1 → r%2")
            emit filesChanged；m_polling=false
```

**已实现**（对照 SVNFileBox `PollCoreAsync` + `UpdateInChunksAsync`）。`GetServerUpdatePaths` 依赖 LibSVNPlus 的 `status -u` 对 **HEAD** 比较（见 §10）；深层缺失目录由 `mergeToDirs` 上溯最近存在祖先，避免 E155007。

### 8.4 定时全量同步 fullSync()（15min）

- **语义**：`update 整个仓库（下行，一次性 `svn update`，不查 head、不做 GetServerUpdatePaths 分块）` **+** `全量扫描上行同步` **两个动作都执行**，先下行后上行串行。
- `m_fullSyncing` 防重入；guard `if (m_fullSyncing || m_polling || m_scanning) return;`
- **该 tick 被丢弃时不再补跑**（用户决策）：fullSync 只是兜底，错过一次就等下一个 15min 边界，时间拉长可接受，无需 `m_fullSyncPending` 补跑。
- 流程：`submit(Update, path=repo.path, bypassDedup)` → `detectConflicts` → `emit filesChanged` → 清 `m_fullSyncing` → `notify("定时全量同步完成")` → `scanAndCommit()`。
- 这样整库一次 update 规避分块 update 的边界问题；上行扫描随后把本地新变更提交上去。

### 8.5 syncNow()（立即同步）

- **只做上行**：`scanAndCommit()`；若已在扫描则置 `m_rescanPending`（不丢本次变更）。

### 8.6 关键状态与防重入

| 标志 | 作用 |
|---|---|
| `m_scanning` | 上行扫描进行中（防并发扫描） |
| `m_rescanPending` | 需要再来一轮扫描（add 完成后 / 扫描中又来新变更） |
| `m_polling` | 下行轮询进行中 |
| `m_fullSyncing` | 15min 全量同步进行中（防重入） |
| `m_pendingCommits / m_pendingAdds` | 在途 commit/add 的去重记账 |
| `m_pendingUpdates` | 分块 update 在途计数（归零才收尾） |
| `m_lastLocalRev` | 本引擎最后一次本地 commit 的版本号 |

**`m_lastLocalRev` 为什么必须**：子目录 commit 会提升服务器 HEAD，但**不会**提升工作副本根节点的版本号。若 poll 只用根节点版本比较，会把自己刚提交的变更又当“远端新变更”拉下来。所以 `localRev = max(working, m_lastLocalRev)`。

---

## 9. RepoManager

- 拥有 `unordered_map<QString, unique_ptr<SyncEngine>>`；`load()` 从 ConfigStore 读仓库并启动所有 `running()`（Active/Background）的引擎。
- `RepoState`：`Deactive`（完全停止）/ `Background`（运行中、未显示）/ `Active`（运行中且当前 GUI 显示）。**同一时刻最多一个 Active**。
- 转发信号并打上仓库名：`notification(name,msg)`、`filesChanged(name)`、`conflictDetected(name,paths)`、`repositoryListChanged`、`repositoryStateChanged`。
- 提供 `addRepository / removeRepository / setState / syncNow / setCredentials / setConfig`，变更即持久化。

---

## 10. LibSVNPlus 集成

- 位置：`C:\Users\xuser\Documents\LibSVNPlus`（独立 git 仓库，非本工作区）。
- 用法：静态链接 `svnplus.lib`（`SVNPLUS_STATIC`），公共头 `svnplus/SvnClient.h`；运行期依赖 `_stage/bin/*.dll`（libsvn 全家桶 + apr/serf/ssl）。CLI 参照物：`_stage\bin\svn.exe`。
- 改了 `LibSVNPlus/src/*.cpp` 后要**重建 svnplus 库并重新 install 到 `_stage`**，否则应用链接的是旧产物：
  `cmake --build <LibSVNPlus>/build && cmake --install <LibSVNPlus>/build --prefix <LibSVNPlus>/build/_stage`
- 关键语义（踩过的坑）：
  - `SvnClient::status(..., checkOutOfDate=true)` 必须对 **HEAD** 做比较（等价 CLI `svn status -u`）。旧实现写死 `SvnRevision::working()`，导致**纯远端新增（本地不存在）的节点 report 0 条 out-of-date**——这是深层目录拉取失效的根因。修复：`checkOutOfDate ? head() : working()`。
  - `SvnDepth` 枚举与 `StatusDepth` 数值一致，无映射问题。
  - `info(path, working())` 取工作副本版本；`info(path/url, head())` 取 HEAD——两者用途完全不同（见 GetRevision vs GetHeadRevision）。

---

## 11. 测试（tests/coretest/main.cpp + scripts）

`synccoretest`（控制台，`QCoreApplication`）固定先跑单元测试，再按参数跑集成：

| 测试 | 内容 |
|---|---|
| `testCategoryOf` | 命令→分类映射 |
| `testWorkerOrdering` | 提交 4 条不同类别命令，验证执行顺序 ReadOnly→LocalWrite→HeavyWrite、全部有结果 |
| `testWorkerDedup` | 6 次提交被 dedup 成 4 次执行；commit 每路径一次；update 按子路径复合 key |
| `testWorkerResultCorrelation` | 调用方 id 保留、载荷随结果回传 |
| `testRepoWatcher` | 启动/上报/debounce/过滤 `.svn` 与临时文件/停止 |
| `testLogStore` | 每仓库容量上限修剪、重启持久化、删仓库清日志（用 `setDatabaseFileForTest` 指向临时库） |
| `--live <wc> <url> [user] [pass]` | 手工单仓库 live 验证 |
| `--livesync <url> <wc-app> <wc-other>` | 双向回归：①上行自动 add+commit ②下行他机提交被拉取 ③**深层嵌套目录树**被拉取（回归 E155007 / status -u HEAD 问题） |
| `--probestatus <wc> [ood] [depth]` | status 探针（曾用于复现 status -u 问题，可清理） |

`live-sync-test.ps1`：临时目录建 repo + 两个 WC（wc-app=被测引擎、wc-other=另一个用户），跑 `--livesync`，退出码即测试退出码；`-Keep` 保留 fixture 并打印路径（供 GUI 复测）。深层用例把整棵 `deep_a` 提交，避免子目录不在树内报错。

---

## 12. 决策记录 / 踩坑备忘

1. **status -u 必须对 HEAD 比较**（LibSVNPlus 修复），否则远端新增节点漏报 → 深层目录不拉取。
2. **对不存在的目录执行 update 报 E155007**（"None of the targets are working copies"）→ `mergeToDirs` 对不存在的目录上溯到最近存在的祖先（WC 根必存在）。
3. **watcher 启动窗口会丢事件** → `start()` 末尾立即 `scanAndCommit()`；曾导致向上偶发丢失（文件事件丢后无扫描入口），已修复。
4. **worker 按 path/复合 key 去重** → 重复提交不产生结果；引擎侧必须用 `m_pendingAdds/m_pendingCommits` 防重复，否则完成计数错乱、扫描永不结束。
5. **子目录 commit 不提升 WC 根版本** → `m_lastLocalRev` 参与下行比较，防把自己提交的内容拉回来。
6. **密码不落盘**（config.db 无密码列），交给 libsvn auth cache。
7. **`GetRevision` 用 working()，不是 head()**；否则下行永不触发。
8. **只读命令优先**：`takeNextLocked` 固定先取 ReadOnly；下行 HEAD 检查（GetRevision/GetHeadRevision/GetServerUpdatePaths）都是只读，不被写队列阻塞。
9. **上行 Status 纯本地（不加 -u）**；`-u` 只出现在 GetServerUpdatePaths（专门的下行探查）。
10. `m_lastLocalRev` 只参与 `poll()` 的下行比较；15min `fullSync` 直接整库 `svn update`（本身就是幂等的，无需比较），避免把自己刚提交的内容当作“远端变更”的特殊分支问题。
11. **fullSync 的 Update 必须 `bypassDedup=true`**：整库 Update（key=`path`）与用户对仓库根目录的“更新”撞 key；被 dedup 吞掉则永远无结果 → `m_fullSyncing` 永久卡死、15min 全量同步从此失效。bypassDedup 让两者都入队、由单 worker 串行执行。
12. **`svn_client_unlock` 签名随版本变化**：SVN 1.14 及更早是 `(targets, comment, force, pool)`，本仓库目标 SVN 1.15 是 `(targets, force, ctx, pool)`——没有 `comment` 参数且需要传 `ctx`。libsvnplus `unlock()` 按 1.15 签名实现；**unlock 只接受文件目标**，对目录目标报 “is not a working copy”。
13. **`GetLastChangedTime` 从“恒成功”桩变为真实 ReadOnly `svn info`（HEAD）调用（0.5.3 起）**：现在能返回服务器 last-changed 时间与修订号，但远端不可达 / URL 无效时返回失败——旧桩恒成功。未来冲突对话框等调用方必须按失败处理。
14. **超时护栏必须覆盖所有网络入口**：同步引擎（SyncEngine）会对 worker 设 `http-timeout`(networkTimeoutSec，默认 60s) + 命令看门狗(+10s)；libsvnplus 在每个操作前重推 http-timeout。但 `AddRepoDialog` 的临时 worker（测试连接 / 创建工作副本）曾未设超时 → 服务端无响应时会被 libsvn 默认 600s 阻塞。0.5.3 起对话框同样用全局配置注入两层超时。
15. **HeavyWrite 改为“空闲看门狗 + 传输上限”双阈值**：旧的看门狗按命令墙钟总时长（networkTimeout+10s）一刀切，大文件在正常上传但没跑完时限时会被误杀。0.5.3 起 Commit/Update/Checkout 执行前挂 progress/notify 回调作为“心跳”（runner `pump()` → worker `pulse()`，刷新 `m_lastActivity`）；看门狗改为：①**空闲窗口**＝距最后一次心跳超过 networkTimeout+10s 仍无任何事件 → `cancel()`（真·网络挂死）；②**传输上限**＝`maxTransferSec`（默认 600s，设置项 2~30 分钟）从命令开始计时，到点即使仍在传也 `cancel()`（防单个大文件无限占用 worker）。持续有心跳的慢传输不会被误杀。非 HeavyWrite 命令维持原固定总时长看门狗。libsvnplus 层无需新增 API：progress/notify 回调在上下文创建时已就绪，且 commit 上传路径（ra 层公共代码）确实会触发 progress。
16. **单文件上传尺寸门限（`maxFileSizeMb`，默认 100MB，可配 10MB~1GB）**：文件达到阈值时不再提交：
    - 上行扫描（`handleScanStatus`）：`Unversioned` 的超大文件**不自动 `svn add`**，`changes` 中的超大文件**不参与目录分组提交**（保证分组计数准确，也不白跑一次提交）。
    - `SvnCommandRunner::runCommit` 是权威兜底：先 `svn status`（Infinity）找出本次会被上传的变更文件，≥ 门限的放入 `CommandResult::oversizedFiles` 并从提交目标中剔除；目录内其余变更仍会提交（同一 revision），全部被跳过时返回“成功但无提交”（避免误报“提交失败”）。
    - 手动提交（FileBrowser，不经上行扫描）同样被该漏斗拦截；`SyncEngine::submit` 对 Commit 统一包装，把 `oversizedFiles` 写入仓库同步日志（每个路径每会话只记一次，避免每次全量同步刷屏）。
    - 日志文案示例："有超大文件（≥ 100 MB），已跳过提交：…"。
    - Status 探测失败时回退为普通目录提交，绝不因检查失败而拒放合法上传。

---

## 13. 已知边界与待办

### 已实现特性（0.3.0）

- **下行分块更新**：`Command::GetServerUpdatePaths`、`mergeToDirs`（含存在性上溯）、`startUpdateInChunks`；`poll()` = head 比较 → GetServerUpdatePaths → 分块 update。
- **fullSync 双动作**：整仓库 `svn update`（不再分块）→ 上行全量扫描，串行执行。
- **LibSVNPlus status HEAD 修复已落库**：改动在 `LibSVNPlus/src/SvnClient.cpp`，已重建并 install 到 `_stage`（`svnplus.lib` 已更新）。
- **dedup 死锁修复**：`CommandItem::bypassDedup` + fullSync Update 置位，杜绝与用户根目录更新撞 key 导致的 `m_fullSyncing` 卡死；新增 `testWorkerDedupBypass` 回归（见决策 #11）。文件浏览的 `svn status`（ReadOnly 优先 + 异步回调）已确认无阻塞。
- **网络韧性**：每命令网络超时（`networkTimeoutSec`）+ worker watchdog（`setCommandTimeoutSec`）+ 断网判定阈值（`disconnectThreshold`）。

### 已知边界

- `syncNow` 只上行不下行（用户已确认此语义）。
- `GetLastChangedTime` 已实现（真实服务器 last-changed 时间，0.5.3）。
- `BreakLock` 已实现（libsvnplus `unlock(breakLock=true)`，即 `svn unlock --force`，0.5.3）。

### 可选清理（未做）

- `--probestatus` 探针代码；
- config.db 里遗留的 `repo1/repo2`（旧 svnsynctest 测试仓库）。
