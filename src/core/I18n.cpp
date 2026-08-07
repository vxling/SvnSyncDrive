#include "core/I18n.h"

namespace {

// Chinese source string -> English translation. Strings without an entry
// stay Chinese in English mode. Keys must match the source literals passed
// to I18n::translate() exactly (including %N placeholders and newlines).
const QHash<QString, QString> kEnglish = {
    // General / status
    { QStringLiteral("就绪"), QStringLiteral("Ready") },
    { QStringLiteral("设置已保存"), QStringLiteral("Settings saved") },
    { QStringLiteral("仓库未启用"), QStringLiteral("Repository not enabled") },
    { QStringLiteral("操作成功"), QStringLiteral("Operation succeeded") },
    { QStringLiteral("操作失败"), QStringLiteral("Operation failed") },
    { QStringLiteral("SVN 操作失败"), QStringLiteral("SVN operation failed") },
    { QStringLiteral("目录"), QStringLiteral("Folder") },
    { QStringLiteral("文件"), QStringLiteral("Files") },
    { QStringLiteral("日志"), QStringLiteral("Log") },
    { QStringLiteral("名称"), QStringLiteral("Name") },
    { QStringLiteral("类型"), QStringLiteral("Type") },
    { QStringLiteral("大小"), QStringLiteral("Size") },
    { QStringLiteral("修改时间"), QStringLiteral("Modified") },
    { QStringLiteral("状态"), QStringLiteral("Status") },
    { QStringLiteral("%1 项"), QStringLiteral("%1 items") },
    { QStringLiteral("（空目录）"), QStringLiteral("(empty folder)") },
    { QStringLiteral("名称："), QStringLiteral("Name:") },
    { QStringLiteral("新名称："), QStringLiteral("New name:") },
    { QStringLiteral("新建文件夹"), QStringLiteral("New Folder") },
    { QStringLiteral("新建文本文件"), QStringLiteral("New Text File") },
    { QStringLiteral("新建文本文件.txt"), QStringLiteral("New Text File.txt") },

    // Tray / sidebar
    { QStringLiteral("显示主窗口"), QStringLiteral("Show main window") },
    { QStringLiteral("退出"), QStringLiteral("Quit") },
    { QStringLiteral("仓库"), QStringLiteral("Repositories") },
    { QStringLiteral("+ 添加仓库"), QStringLiteral("+ Add Repository") },
    { QStringLiteral("⚙ 设置"), QStringLiteral("⚙ Settings") },
    { QStringLiteral("ℹ 关于"), QStringLiteral("ℹ About") },
    { QStringLiteral("● 同步中"), QStringLiteral("● Syncing") },
    { QStringLiteral("◐ 后台"), QStringLiteral("◐ Background") },
    { QStringLiteral("◐ 后台同步"), QStringLiteral("◐ Background sync") },
    { QStringLiteral("○ 停止监控"), QStringLiteral("○ Monitoring stopped") },
    { QStringLiteral("✕ 认证失败"), QStringLiteral("✕ Auth failed") },
    { QStringLiteral("✖ 断开链接"), QStringLiteral("✖ Disconnected") },
    { QStringLiteral("⚙ 仓库配置"), QStringLiteral("⚙ Configure") },
    { QStringLiteral("🗑 移除仓库"), QStringLiteral("🗑 Remove") },
    { QStringLiteral("移除仓库"), QStringLiteral("Remove Repository") },
    { QStringLiteral("确定要移除仓库「%1」吗？\n（不会删除本地文件，只会停止同步并从列表移除。）"),
      QStringLiteral("Remove repository \"%1\"?\n(Local files are kept; only syncing stops and the repository is removed from the list.)") },
    { QStringLiteral("选择一个仓库查看文件与同步日志，\n或点击左侧「+ 添加仓库」。"),
      QStringLiteral("Select a repository to browse files and sync logs,\nor click \"+ Add Repository\" on the left.") },

    // File browser
    { QStringLiteral("🔁 立即同步"), QStringLiteral("🔁 Sync Now") },
    { QStringLiteral("🛠 解决冲突"), QStringLiteral("🛠 Resolve Conflicts") },
    { QStringLiteral("🔄 刷新"), QStringLiteral("🔄 Refresh") },
    { QStringLiteral("打开目录"), QStringLiteral("Open Folder") },
    { QStringLiteral("打开"), QStringLiteral("Open") },
    { QStringLiteral("复制完整路径"), QStringLiteral("Copy Full Path") },
    { QStringLiteral("高级操作"), QStringLiteral("Advanced") },
    { QStringLiteral("添加到版本库 (Add)"), QStringLiteral("Add to Repository (Add)") },
    { QStringLiteral("提交… (Commit)"), QStringLiteral("Commit…") },
    { QStringLiteral("更新 (Update)"), QStringLiteral("Update") },
    { QStringLiteral("还原 (Revert)"), QStringLiteral("Revert") },
    { QStringLiteral("从版本库删除 (Delete)"), QStringLiteral("Delete from Repository (Delete)") },
    { QStringLiteral("重命名… (Move)"), QStringLiteral("Rename… (Move)") },
    { QStringLiteral("提交"), QStringLiteral("Commit") },
    { QStringLiteral("提交日志：\n%1"), QStringLiteral("Commit message:\n%1") },
    { QStringLiteral("还原"), QStringLiteral("Revert") },
    { QStringLiteral("确定要还原以下路径的全部本地修改？\n\n%1"),
      QStringLiteral("Revert all local modifications of the following path?\n\n%1") },
    { QStringLiteral("删除"), QStringLiteral("Delete") },
    { QStringLiteral("确定要从版本库删除以下路径（保留本地文件）？\n\n%1"),
      QStringLiteral("Delete the following path from the repository (keeping local files)?\n\n%1") },
    { QStringLiteral("重命名"), QStringLiteral("Rename") },

    // Add repository dialog
    { QStringLiteral("仓库配置"), QStringLiteral("Repository Settings") },
    { QStringLiteral("添加仓库"), QStringLiteral("Add Repository") },
    { QStringLiteral("本地工作副本路径（不存在则自动创建）"),
      QStringLiteral("Local working copy path (created automatically if missing)") },
    { QStringLiteral("仓库 URL，例如 https://svn.example.com/repo/project"),
      QStringLiteral("Repository URL, e.g. https://svn.example.com/repo/project") },
    { QStringLiteral("浏览…"), QStringLiteral("Browse…") },
    { QStringLiteral("路径"), QStringLiteral("Path") },
    { QStringLiteral("URL"), QStringLiteral("URL") },
    { QStringLiteral("用户名"), QStringLiteral("Username") },
    { QStringLiteral("密码"), QStringLiteral("Password") },
    { QStringLiteral("添加后立即启用同步"), QStringLiteral("Enable sync immediately after adding") },
    { QStringLiteral("凭据会交给 libsvn 加密存储，不会保存到应用配置中。"),
      QStringLiteral("Credentials are encrypted by libsvn and never stored in the app configuration.") },
    { QStringLiteral("测试连接"), QStringLiteral("Test Connection") },
    { QStringLiteral("测试中…"), QStringLiteral("Testing…") },
    { QStringLiteral("取消"), QStringLiteral("Cancel") },
    { QStringLiteral("更新"), QStringLiteral("Update") },
    { QStringLiteral("添加"), QStringLiteral("Add") },
    { QStringLiteral("选择工作副本目录"), QStringLiteral("Select Working Copy Directory") },
    { QStringLiteral("缺少 URL"), QStringLiteral("URL Required") },
    { QStringLiteral("请输入仓库 URL 后再测试连接。"),
      QStringLiteral("Enter the repository URL before testing the connection.") },
    { QStringLiteral("创建工作副本失败：%1"), QStringLiteral("Failed to create working copy: %1") },
    { QStringLiteral("连接成功（HEAD 版本 r%1）"), QStringLiteral("Connected (HEAD revision r%1)") },
    { QStringLiteral("连接成功"), QStringLiteral("Connected") },
    { QStringLiteral("用户名或密码不正确：%1"), QStringLiteral("Incorrect username or password: %1") },
    { QStringLiteral("连接失败：%1"), QStringLiteral("Connection failed: %1") },
    { QStringLiteral("缺少名称"), QStringLiteral("Name Required") },
    { QStringLiteral("请输入仓库名称。"), QStringLiteral("Please enter a repository name.") },
    { QStringLiteral("缺少路径"), QStringLiteral("Path Required") },
    { QStringLiteral("请输入工作副本路径。"), QStringLiteral("Please enter the working copy path.") },
    { QStringLiteral("路径无效"), QStringLiteral("Invalid Path") },
    { QStringLiteral("所选路径指向一个文件，请选择目录。"),
      QStringLiteral("The selected path is a file; please choose a directory.") },
    { QStringLiteral("无法创建工作副本目录，请更换路径后重试。"),
      QStringLiteral("Could not create the working copy directory. Please try another path.") },
    { QStringLiteral("目录不为空"), QStringLiteral("Directory Not Empty") },
    { QStringLiteral("所选目录不为空且不是工作副本，请选择空目录或更换路径。"),
      QStringLiteral("The directory is not empty and is not a working copy. Choose an empty directory or a different path.") },
    { QStringLiteral("请输入仓库 URL 后再添加。"),
      QStringLiteral("Enter the repository URL before adding.") },
    { QStringLiteral("正在创建工作副本…"), QStringLiteral("Creating working copy…") },

    // About dialog
    { QStringLiteral("关于 SvnSyncDrive"), QStringLiteral("About SvnSyncDrive") },
    { QStringLiteral("SVN 工作副本双向自动同步工具"),
      QStringLiteral("Two-way automatic sync tool for SVN working copies") },
    { QStringLiteral("SvnSyncDrive 在本地 SVN 工作副本和服务器之间自动同步：\n"
                    "· 监控本地文件变化并批量提交到服务器\n"
                    "· 周期检查服务器新版本并自动更新到本地\n"
                    "· 冲突检测与可视化解决\n"
                    "· 多仓库后台同步，互不阻塞\n"
                    "· 认证凭据交由 libsvn 加密存储（Windows 凭据管理器）"),
      QStringLiteral("SvnSyncDrive keeps local SVN working copies in two-way sync with the server:\n"
                    "· watches local file changes and commits them to the server in batches\n"
                    "· periodically checks the server for new revisions and updates locally\n"
                    "· detects conflicts and resolves them visually\n"
                    "· syncs multiple repositories in the background without blocking each other\n"
                    "· credentials are encrypted via libsvn (Windows Credential Manager)") },
    { QStringLiteral("基于 Qt %1 · libsvnplus · Apache Subversion"),
      QStringLiteral("Built with Qt %1 · libsvnplus · Apache Subversion") },
    { QStringLiteral("关闭"), QStringLiteral("Close") },

    // Conflict dialog
    { QStringLiteral("解决冲突"), QStringLiteral("Resolve Conflicts") },
    { QStringLiteral("以下文件存在冲突，请选择处理方式："),
      QStringLiteral("The following files have conflicts. Choose how to handle them:") },
    { QStringLiteral("%1（树冲突）"), QStringLiteral("%1 (tree conflict)") },
    { QStringLiteral("解决方式"), QStringLiteral("Resolution") },
    { QStringLiteral("使用我的版本（MineFull）"), QStringLiteral("Use my version (MineFull)") },
    { QStringLiteral("使用他们的版本（TheirsFull）"), QStringLiteral("Use their version (TheirsFull)") },
    { QStringLiteral("标记为已合并（Merged）"), QStringLiteral("Mark as merged (Merged)") },
    { QStringLiteral("使用基线版本（Base）"), QStringLiteral("Use base version (Base)") },
    { QStringLiteral("树冲突是目录增删改等结构性冲突，SVN 不支持对它们选择版本，将一律按"
                    "「保留当前工作副本状态」标记解决（图中以红色标注）。"),
      QStringLiteral("Tree conflicts are structural conflicts (added/removed/renamed folders). "
                    "SVN cannot choose a revision for them, so they are always resolved by keeping "
                    "the current working copy state (highlighted in red).") },
    { QStringLiteral("解决全部"), QStringLiteral("Resolve All") },
    { QStringLiteral("稍后处理"), QStringLiteral("Later") },
    { QStringLiteral("正在解决冲突…"), QStringLiteral("Resolving conflicts…") },
    { QStringLiteral("已解决 %1 个冲突。"), QStringLiteral("Resolved %1 conflict(s).") },
    { QStringLiteral("有 %1 个文件解决失败：%2"),
      QStringLiteral("%1 file(s) failed to resolve: %2") },
    { QStringLiteral("；"), QStringLiteral("; ") },

    // Settings dialog
    { QStringLiteral("设置"), QStringLiteral("Settings") },
    { QStringLiteral("常规"), QStringLiteral("General") },
    { QStringLiteral("语言"), QStringLiteral("Language") },
    { QStringLiteral("中文"), QStringLiteral("中文") },
    { QStringLiteral("English"), QStringLiteral("English") },
    { QStringLiteral("关闭窗口时最小化到系统托盘"),
      QStringLiteral("Minimize to tray when the window closes") },
    { QStringLiteral("开启后常驻后台同步，可随时从托盘恢复窗口"),
      QStringLiteral("Keeps background sync running; restore the window anytime from the tray") },
    { QStringLiteral("启动时最小化到系统托盘（不显示窗口）"),
      QStringLiteral("Start minimized to tray (no window)") },
    { QStringLiteral("开启后启动程序不显示主窗口，直接常驻系统托盘"),
      QStringLiteral("Starts without showing the main window, staying in the system tray") },
    { QStringLiteral("关闭窗口时最小化"), QStringLiteral("Minimize on window close") },
    { QStringLiteral("启动时隐藏到托盘"), QStringLiteral("Hide to tray on startup") },
    { QStringLiteral(" 秒"), QStringLiteral(" s") },
    { QStringLiteral(" 分钟"), QStringLiteral(" min") },
    { QStringLiteral(" 次"), QStringLiteral(" times") },
    { QStringLiteral(" 条"), QStringLiteral(" entries") },
    { QStringLiteral("向下同步检查周期"), QStringLiteral("Down-sync check interval") },
    { QStringLiteral("全量提交周期"), QStringLiteral("Full commit interval") },
    { QStringLiteral("自动添加新文件"), QStringLiteral("Auto-add new files") },
    { QStringLiteral("自动添加未纳入版本控制的新文件"),
      QStringLiteral("Auto-add new files not under version control") },
    { QStringLiteral("多久检查一次服务器是否有新版本"),
      QStringLiteral("How often to check the server for new revisions") },
    { QStringLiteral("周期性地把本地所有更改提交到服务器"),
      QStringLiteral("Periodically commits all local changes to the server") },
    { QStringLiteral("关闭后只提交已有版本控制的修改"),
      QStringLiteral("When off, only already-versioned changes are committed") },
    { QStringLiteral("信任自签名 / 未知证书"), QStringLiteral("Trust self-signed / unknown certificates") },
    { QStringLiteral("信任自签名证书"), QStringLiteral("Trust self-signed certificates") },
    { QStringLiteral("断网判定阈值"), QStringLiteral("Disconnect threshold") },
    { QStringLiteral("网络超时"), QStringLiteral("Network timeout") },
    { QStringLiteral("连续多少次网络访问失败后，将仓库标记为“连接断开”。任意一次成功访问都会清零重新计数"),
      QStringLiteral("After this many consecutive network failures the repository is marked "
                    "disconnected. Any successful access resets the counter.") },
    { QStringLiteral("单次网络操作（如 HTTPS 握手）最多阻塞的秒数；超过后按网络错误处理并继续重试，避免卡住同步"),
      QStringLiteral("Maximum seconds a single network operation (e.g. an HTTPS handshake) may "
                    "block; afterwards it is treated as a network error and retried so syncing "
                    "never stalls.") },
    { QStringLiteral("自动解决冲突（不再弹窗提示）"), QStringLiteral("Auto-resolve conflicts (no dialogs)") },
    { QStringLiteral("发现冲突后自动按下方默认方式解决，不再弹出确认对话框；树冲突一律按“保留当前工作副本状态”处理"),
      QStringLiteral("Conflicts are resolved automatically with the default choice below without "
                    "asking; tree conflicts always keep the current working copy state.") },
    { QStringLiteral("自动解决冲突"), QStringLiteral("Auto-resolve conflicts") },
    { QStringLiteral("默认处理方式"), QStringLiteral("Default resolution") },
    { QStringLiteral("自动解决冲突时对文本冲突使用的默认处理方式（树冲突不适用，一律保留当前工作副本状态）"),
      QStringLiteral("Default handling for text conflicts when auto-resolving (not used for tree "
                    "conflicts, which always keep the working copy state).") },
    { QStringLiteral("冲突处理"), QStringLiteral("Conflicts") },
    { QStringLiteral("每个仓库保留日志条数"), QStringLiteral("Log entries kept per repo") },
    { QStringLiteral("每个仓库在本地数据库中最多保留的日志条数（上限 10000 条），超出后自动丢弃最早的"),
      QStringLiteral("Maximum log entries kept per repository in the local database (up to 10000); "
                    "older entries are dropped automatically.") },
    { QStringLiteral("更改将应用到所有正在同步的仓库。"),
      QStringLiteral("Changes apply to all syncing repositories.") },
    { QStringLiteral("保存"), QStringLiteral("Save") },

    // Sync engine notifications
    { QStringLiteral("无法监听目录: %1"), QStringLiteral("Cannot watch directory: %1") },
    { QStringLiteral("已删除: %1"), QStringLiteral("Deleted: %1") },
    { QStringLiteral("删除失败: %1（%2）"), QStringLiteral("Delete failed: %1 (%2)") },
    { QStringLiteral("批量同步扫描失败: %1"), QStringLiteral("Sync scan failed: %1") },
    { QStringLiteral("已添加: %1"), QStringLiteral("Added: %1") },
    { QStringLiteral("添加失败: %1（%2）"), QStringLiteral("Add failed: %1 (%2)") },
    { QStringLiteral("自动提交: %1（%2 个文件）→ r%3"), QStringLiteral("Auto-commit: %1 (%2 files) → r%3") },
    { QStringLiteral("提交失败: %1（%2）"), QStringLiteral("Commit failed: %1 (%2)") },
    { QStringLiteral("批量同步完成：新增 %1 个文件，提交 %2 个目录"),
      QStringLiteral("Batch sync done: %1 files added, %2 directories committed") },
    { QStringLiteral("文件监听处于挂起状态，已自动恢复。"),
      QStringLiteral("File watcher was suspended; automatically restored.") },
    { QStringLiteral("定时全量同步失败: %1"), QStringLiteral("Scheduled full sync failed: %1") },
    { QStringLiteral("定时全量同步完成（更新到 r%1，%2 个文件/目录）"),
      QStringLiteral("Scheduled full sync done (updated to r%1, %2 files/dirs)") },
    { QStringLiteral("定时全量同步完成（更新到 r%1）"),
      QStringLiteral("Scheduled full sync done (updated to r%1)") },
    { QStringLiteral("定时全量同步完成"), QStringLiteral("Scheduled full sync done") },
    { QStringLiteral("获取远端变更失败: %1"), QStringLiteral("Failed to fetch remote changes: %1") },
    { QStringLiteral("已从服务器更新 r%1 → r%2"), QStringLiteral("Updated from server r%1 → r%2") },
    { QStringLiteral("、"), QStringLiteral(", ") },
    { QStringLiteral("%1：%2（%3 个文件/目录）"), QStringLiteral("%1: %2 (%3 files/dirs)") },
    { QStringLiteral("%1：%2 个文件/目录"), QStringLiteral("%1: %2 files/dirs") },
    { QStringLiteral("已自动解决冲突: %1"), QStringLiteral("Conflict auto-resolved: %1") },
    { QStringLiteral("自动解决冲突失败: %1（%2）"), QStringLiteral("Conflict auto-resolve failed: %1 (%2)") },
    { QStringLiteral("已按默认方式自动解决 %1 个冲突（树冲突一律保留当前工作副本状态）"),
      QStringLiteral("Auto-resolved %1 conflicts with the default choice (tree conflicts keep the "
                    "working copy state)") },
    { QStringLiteral("使用基线版本"), QStringLiteral("Use base version") },
    { QStringLiteral("使用他们的版本"), QStringLiteral("Use their version") },
    { QStringLiteral("使用我的版本"), QStringLiteral("Use my version") },
    { QStringLiteral("使用他们的版本（冲突标记）"), QStringLiteral("Use their version (conflict markers)") },
    { QStringLiteral("使用我的版本（冲突标记）"), QStringLiteral("Use my version (conflict markers)") },
    { QStringLiteral("标记为已合并"), QStringLiteral("Mark as merged") },
    { QStringLiteral("未知（%1）"), QStringLiteral("Unknown (%1)") },

    // Repo manager notifications
    { QStringLiteral("认证失败，已停止同步。请在仓库配置中更新凭据后恢复。"),
      QStringLiteral("Authentication failed; syncing stopped. Update the credentials in the "
                    "repository settings to resume.") },
    { QStringLiteral("连接断开（连续多次服务器访问失败），将继续重试…"),
      QStringLiteral("Disconnected (multiple consecutive server failures); will keep retrying…") },
    { QStringLiteral("连接已恢复。"), QStringLiteral("Connection restored.") },

    // Repo watcher
    { QStringLiteral("无法打开监听目录: %1"), QStringLiteral("Cannot open watched directory: %1") },
    { QStringLiteral("文件监听中断，正在重连…"), QStringLiteral("File watcher interrupted; reconnecting…") },
    { QStringLiteral("读取文件变化失败，正在重连…"), QStringLiteral("Failed to read file changes; reconnecting…") },
    { QStringLiteral("重新监听失败，正在重连…"), QStringLiteral("Failed to re-watch; reconnecting…") },

    // Main window conflict log lines
    { QStringLiteral("[冲突] %1 个文件：%2"), QStringLiteral("[Conflict] %1 files: %2") },
    { QStringLiteral("[冲突] 其中 %1 个为树冲突（将按保留当前状态处理）：%2"),
      QStringLiteral("[Conflict] %1 of them are tree conflicts (kept as-is): %2") },
    { QStringLiteral("保留当前工作副本状态"), QStringLiteral("Keep current working copy state") },
    { QStringLiteral("[冲突] 已解决：%1（%2）"), QStringLiteral("[Conflict] Resolved: %1 (%2)") },
    { QStringLiteral("[冲突] 解决失败：%1（%2）：%3"), QStringLiteral("[Conflict] Resolve failed: %1 (%2): %3") },
    { QStringLiteral("仓库已停用，无法扫描冲突。"), QStringLiteral("Repository is disabled; cannot scan for conflicts.") },
    { QStringLiteral("没有发现冲突文件。"), QStringLiteral("No conflicted files found.") },
    { QStringLiteral("已更新仓库凭据。"), QStringLiteral("Repository credentials updated.") },
};

} // namespace

I18n *I18n::instance()
{
    static I18n singleton;
    return &singleton;
}

QString I18n::translate(const char *sourceText)
{
    const QString key = QString::fromUtf8(sourceText);
    I18n *self = instance();
    if (self->m_language == QLatin1String("en")) {
        const auto it = kEnglish.constFind(key);
        if (it != kEnglish.constEnd())
            return it.value();
    }
    return key;
}

QString I18n::language()
{
    return instance()->m_language;
}

void I18n::setLanguage(const QString &code)
{
    I18n *self = instance();
    const QString normalized = (code == QLatin1String("en")) ? QStringLiteral("en")
                                                             : QStringLiteral("zh_CN");
    if (self->m_language == normalized)
        return;
    self->m_language = normalized;
    emit self->languageChanged(normalized);
}

QStringList I18n::supportedLanguages()
{
    return { QStringLiteral("zh_CN"), QStringLiteral("en") };
}

I18n::I18n() = default;
