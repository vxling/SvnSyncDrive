#pragma once

#include "core/SvnCommand.h"

#include <functional>

namespace svnsync {

/**
 * Executes a single SVN command synchronously. Called exclusively on the
 * SvnWorker thread, so implementations may own a single SvnClient.
 */
class ICommandRunner
{
public:
    virtual ~ICommandRunner() = default;

    virtual CommandResult execute(const CommandItem &item) = 0;
    virtual void setCredentials(const QString &username, const QString &password) = 0;
    virtual void setTrustServerCertificate(bool trust) = 0;
    /** Bound the runtime of one network operation on the underlying client. */
    virtual void setNetworkTimeout(int timeoutSeconds) = 0;

    /** Install a liveness callback: long-running operations (commit/update/
     *  checkout) call it for every progress/notify event so the worker's
     *  watchdog can tell a busy transfer apart from a hung connection. The
     *  empty callback (default) disables the heartbeat. */
    virtual void setKeepAlive(std::function<void()> keepAlive)
    {
        (void)keepAlive;
    }

    /** Per-file upload gate in MB; files at or above this size are skipped by
     *  commits. 0 (default) disables the check. */
    virtual void setMaxFileSizeMb(int megabytes)
    {
        (void)megabytes;
    }

    /** Ask the running execute() to abort as soon as possible. */
    virtual void cancel() = 0;
};

} // namespace svnsync
