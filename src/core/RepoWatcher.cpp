#include "core/RepoWatcher.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include <vector>

namespace svnsync {

namespace {

bool isSvnRelated(const QString &path)
{
    const QStringList segments = path.split(QLatin1Char('/'));
    for (const auto &segment : segments)
        if (segment == QStringLiteral(".svn"))
            return true;
    return false;
}

bool isTempFile(const QString &path)
{
    const QString fileName = path.split(QLatin1Char('/')).constLast();
    return fileName.startsWith(QStringLiteral("~$"))
        || fileName.startsWith(QLatin1Char('~'))
        || fileName.endsWith(QStringLiteral(".tmp"))
        || fileName.endsWith(QStringLiteral(".temp"))
        || fileName == QStringLiteral(".DS_Store");
}

bool shouldIgnore(const QString &path)
{
    const QString normalized = QDir::fromNativeSeparators(path);
    return isSvnRelated(normalized) || isTempFile(normalized);
}

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

RepoWatcher::RepoWatcher(QObject *parent)
    : QObject(parent)
{
}

RepoWatcher::~RepoWatcher()
{
    stop();
}

bool RepoWatcher::start(const QString &path, int debounceMs)
{
    if (m_running.load())
        return true;
    if (!QFileInfo::exists(path))
        return false;
    m_watchPath = QDir::toNativeSeparators(path);
    m_debounceMs = qMax(2000, debounceMs);
    m_stopRequested = false;
    m_lastEmitMs = nowMs();
    m_running = true;
    m_thread = std::thread(&RepoWatcher::run, this);
    return true;
}

void RepoWatcher::stop()
{
    m_stopRequested = true;
    if (m_thread.joinable())
        m_thread.join();
    m_running = false;
}

void RepoWatcher::setSuspended(bool suspended)
{
    m_suspended = suspended;
}

void RepoWatcher::run()
{
#ifdef _WIN32
    const std::wstring watchDir = m_watchPath.toStdWString();

    while (!m_stopRequested.load()) {
        HANDLE hDir = CreateFileW(watchDir.c_str(),
                                  FILE_LIST_DIRECTORY,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                                  nullptr);
        if (hDir == INVALID_HANDLE_VALUE) {
            emit failed(QStringLiteral("无法打开监听目录: %1").arg(m_watchPath));
            for (int i = 0; i < 15 && !m_stopRequested.load(); ++i)
                Sleep(200);
            continue;
        }

        const DWORD bufferSize = 64 * 1024;
        std::vector<BYTE> buffer(bufferSize);
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        auto issueRead = [&]() -> BOOL {
            ZeroMemory(buffer.data(), bufferSize);
            ResetEvent(overlapped.hEvent);
            return ReadDirectoryChangesW(
                hDir,
                buffer.data(),
                bufferSize,
                TRUE,  // watch subtree
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME
                    | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                nullptr,
                &overlapped,
                nullptr);
        };

        bool ok = (overlapped.hEvent != nullptr);
        if (ok && !issueRead())
            ok = false;

        if (!ok) {
            if (overlapped.hEvent)
                CloseHandle(overlapped.hEvent);
            CloseHandle(hDir);
            for (int i = 0; i < 15 && !m_stopRequested.load(); ++i)
                Sleep(200);
            continue;
        }

        while (!m_stopRequested.load()) {
            const DWORD wait = WaitForSingleObject(overlapped.hEvent, 200);
            if (wait == WAIT_TIMEOUT) {
                collectAndMaybeEmit();
                continue;
            }
            if (wait != WAIT_OBJECT_0) {
                emit failed(QStringLiteral("文件监听中断，正在重连…"));
                break;
            }

            DWORD bytes = 0;
            if (!GetOverlappedResult(hDir, &overlapped, &bytes, FALSE) || bytes == 0) {
                emit failed(QStringLiteral("读取文件变化失败，正在重连…"));
                break;
            }

            BYTE *ptr = buffer.data();
            DWORD remaining = bytes;
            while (remaining >= offsetof(FILE_NOTIFY_INFORMATION, FileName) + sizeof(WCHAR)) {
                const auto *record = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(ptr);
                const int nameChars = static_cast<int>(record->FileNameLength) / static_cast<int>(sizeof(WCHAR));
                const QString name = QString::fromWCharArray(record->FileName, nameChars);
                const QString full =
                    QDir::fromNativeSeparators(m_watchPath + QLatin1Char('/') + name);
                if (!shouldIgnore(full) && !m_suspended.load())
                    m_pending.insert(full);
                if (record->NextEntryOffset == 0)
                    break;
                ptr += record->NextEntryOffset;
                remaining -= record->NextEntryOffset;
            }

            if (!issueRead()) {
                emit failed(QStringLiteral("重新监听失败，正在重连…"));
                break;
            }
            collectAndMaybeEmit();
        }

        if (overlapped.hEvent)
            CloseHandle(overlapped.hEvent);
        CloseHandle(hDir);
    }
#else
    Q_UNUSED(m_watchPath);
#endif

    // Flush anything still pending on shutdown.
    if (!m_pending.isEmpty()) {
        const QStringList batch = m_pending.values();
        m_pending.clear();
        emit filesChanged(batch);
    }
}

void RepoWatcher::collectAndMaybeEmit()
{
    if (m_suspended.load() || m_pending.isEmpty())
        return;
    // Throttle, not an idle-timeout: emit at most one batch per debounceMs.
    // An idle-timeout means continuous filesystem activity (e.g. SVN writing
    // to the working copy while we sync) keeps resetting the timer and can
    // postpone a batch indefinitely. With a throttle, a batch is guaranteed
    // to go out every debounceMs while anything is pending, so a busy working
    // copy still makes progress.
    if (nowMs() - m_lastEmitMs < m_debounceMs)
        return;
    m_lastEmitMs = nowMs();
    const QStringList batch = m_pending.values();
    m_pending.clear();
    emit filesChanged(batch);
}

} // namespace svnsync
