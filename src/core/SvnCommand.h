#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>

namespace svnsync {

/**
 * All SVN commands exposed to callers, mirroring SVNFileBox.SvnCommand.
 * The executor (SvnWorker) decides internally whether a command runs
 * immediately (ReadOnly), in the small queue (LocalWrite) or in the
 * heavy queue (HeavyWrite). Callers just submit the command.
 */
enum class Command {
    // ReadOnly: no queue, highest priority
    Info,
    Status,
    GetRevision,
    GetHeadRevision,
    GetConflictedFiles,
    GetLastChangedTime,
    IsVersioned,
    IsValidWorkingCopy,
    TestConnection,
    GetServerUpdatePaths,

    // LocalWrite: drained before every HeavyWrite
    Add,
    Delete,
    Move,
    Revert,
    Resolve,
    BreakLock,

    // HeavyWrite: executed one at a time
    Commit,
    Update,
    Checkout
};

enum class Category {
    ReadOnly,
    LocalWrite,
    HeavyWrite
};

Category categoryOf(Command command);

/** Localized SVN status kind (kept free of libsvnplus types). */
enum class StatusKind {
    None,
    Unversioned,
    Normal,
    Added,
    Missing,
    Deleted,
    Replaced,
    Modified,
    Merged,
    Conflicted,
    Ignored,
    Obstructed,
    External,
    Incomplete
};

struct StatusEntry {
    QString path;
    StatusKind nodeStatus = StatusKind::None;
    StatusKind textStatus = StatusKind::None;
    StatusKind reposStatus = StatusKind::None;
    bool versioned = false;
    bool conflicted = false;
    bool outOfDate = false;
    qlonglong revision = -1;
};

struct InfoEntry {
    QString path;
    QString url;
    qlonglong revision = -1;
};

/** A pending SVN command ready to be executed by the background worker. */
struct CommandItem
{
    quint64 id = 0;
    Command command = Command::Info;
    QString path;
    QString fromPath;
    QString message;          // commit log message
    QString repoUrl;          // checkout / head revision target
    QString username;
    QString password;
    QStringList updatePaths;  // update sub-paths (empty = whole working copy)
    bool checkOutOfDate = false;
    int conflictChoice = 2;   // resolve choice: 0=Base 1=TheirsFull 2=MineFull 3=TheirsConflict 4=MineConflict 5=Merged
};

/** Result of a background command, delivered via SvnWorker::resultReady. */
struct CommandResult
{
    quint64 id = 0;
    Command command = Command::Info;
    QString path;
    QStringList paths;          // effective paths (update sub-paths when set)
    bool success = false;
    QString error;
    qlonglong revision = -1;
    QString value;              // generic string payload
    QList<StatusEntry> statuses;
    QList<InfoEntry> infos;
};

CommandResult makeResult(const CommandItem &item, bool success, const QString &error = QString());

} // namespace svnsync
