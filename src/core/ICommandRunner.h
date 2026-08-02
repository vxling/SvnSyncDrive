#pragma once

#include "core/SvnCommand.h"

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
    /** Ask the running execute() to abort as soon as possible. */
    virtual void cancel() = 0;
};

} // namespace svnsync
