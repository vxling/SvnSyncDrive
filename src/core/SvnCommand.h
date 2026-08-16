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

/**
 * True if the given error text reports a locked working copy (svn messages
 * like "Working copy '...' locked." / "'...' is already locked."). Used by
 * the runner to trigger an automatic `svn cleanup` + one retry before an
 * update/commit is reported as failed.
 */
bool isWcLockErrorText(const QString &errorText);

/**
 * Localized SVN status depth, mirroring libsvnplus SvnDepth (core stays
 * libsvnplus-free). Default for Status is Infinity (whole tree); a file
 * browser only needs Immediates to render a directory's direct children.
 */
enum class StatusDepth : int {
    Empty = 0,
    Files = 1,
    Immediates = 2,
    Infinity = 3
};

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
    bool treeConflicted = false;
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
    QString repo;             // owning repository name (for program logs)
    QString path;
    QString fromPath;
    QString message;          // commit log message
    QString repoUrl;          // checkout / head revision target
    QString username;
    QString password;
    QStringList updatePaths;  // update sub-paths (empty = whole working copy)
    bool checkOutOfDate = false;
    bool bypassDedup = false;  // never coalesced away; must run and report a result
    int conflictChoice = 2;   // resolve choice: 0=Base 1=TheirsFull 2=MineFull 3=TheirsConflict 4=MineConflict 5=Merged
    StatusDepth statusDepth = StatusDepth::Infinity;  // Status/GetConflictedFiles recursion scope
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
    QStringList treeConflicts;  // conflicted paths that are tree conflicts (GetConflictedFiles)
    QStringList oversizedFiles; // commit input skipped because they are >= the size gate
};

CommandResult makeResult(const CommandItem &item, bool success, const QString &error = QString());

} // namespace svnsync
