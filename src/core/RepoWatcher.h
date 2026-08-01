#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <atomic>
#include <thread>

namespace svnsync {

/**
 * Recursive working-copy file watcher with debounce, mirroring
 * SVNFileBox.FileWatcherService.
 *
 * On Windows it uses a single ReadDirectoryChangesW handle with
 * bWatchSubtree, so replacing a subdirectory does not silently stop
 * notifications (unlike one-handle-per-directory watchers).
 *
 * Change events are accumulated and delivered as one batch after
 * `debounceMs` of quiet. Paths under ".svn" and temp files are ignored.
 * The watcher auto-reconnects on failure.
 */
class RepoWatcher : public QObject
{
    Q_OBJECT

public:
    explicit RepoWatcher(QObject *parent = nullptr);
    ~RepoWatcher() override;

    bool start(const QString &path, int debounceMs = 2000);
    void stop();

    bool isWatching() const { return m_running.load(); }

signals:
    void filesChanged(const QStringList &paths);
    void failed(const QString &reason);

private:
    void run();
    void collectAndMaybeEmit();

    QString m_watchPath;
    int m_debounceMs = 2000;

    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_stopRequested{ false };
    std::thread m_thread;

    // Touched only on the watcher thread.
    QSet<QString> m_pending;
    qint64 m_lastEventMs = 0;
};

} // namespace svnsync
