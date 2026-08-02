#pragma once

#include "core/SvnCommand.h"

#include <QHash>
#include <QList>
#include <QObject>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace svnsync {

class ICommandRunner;

/**
 * Background SVN command executor with two-tier priority queues,
 * mirroring SVNFileBox.SvnCommandExecutor.
 *
 * Execution model (all commands run on a single dedicated worker thread
 * that owns one ICommandRunner, so the underlying SvnClient is never
 * used concurrently):
 *   ReadOnly    -> highest priority (drained first)
 *   LocalWrite  -> drained before every HeavyWrite
 *   HeavyWrite  -> executed one at a time, after LocalWrite is empty
 *
 * Commands are submitted from any thread via submit(). Results are
 * delivered on the receiver's thread through the resultReady() signal
 * (use a queued connection). If item.id == 0 a unique id is assigned;
 * otherwise the caller-provided id is preserved.
 */
class SvnWorker : public QObject
{
    Q_OBJECT

public:
    using RunnerFactory = std::function<std::unique_ptr<ICommandRunner>()>;

    explicit SvnWorker(QObject *parent = nullptr);
    ~SvnWorker() override;

    /** Spawn the worker thread. Default factory creates the libsvnplus runner. */
    void start(RunnerFactory factory = nullptr);

    /** Stop the worker thread (drops queued commands, joins). */
    void stop();

    /** Submit a command from any thread. */
    void submit(const CommandItem &item);

    /** Credentials applied to the runner before the next command. */
    void setCredentials(const QString &username, const QString &password);
    void setTrustServerCertificate(bool trust);

signals:
    void resultReady(quint64 id, const CommandResult &result);
    void finished();

private:
    void workerLoop();
    CommandItem takeNextLocked();
    bool heavyWriteAllowedLocked(const CommandItem &item);
    void removeFromDedupLocked(const CommandItem &item);
    QString dedupKey(const CommandItem &item) const;
    void logModify(const CommandItem &item, const CommandResult &result);

    std::thread m_thread;
    RunnerFactory m_factory;
    bool m_started = false;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stopping = false;

    QList<CommandItem> m_readOnly;
    QList<CommandItem> m_localWrite;
    QList<CommandItem> m_heavyWrite;
    QHash<QString, CommandItem> m_dedup;
    quint64 m_nextId = 1;

    std::unique_ptr<ICommandRunner> m_runner;
    QString m_username;
    QString m_password;
    bool m_trustCert = false;
    bool m_credsDirty = true;
};

} // namespace svnsync
