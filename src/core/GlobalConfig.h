#pragma once

namespace svnsync {

/**
 * Application-wide (global) settings shared by every repository engine,
 * mirroring SVNFileBox's global configuration. Stored through
 * ConfigStore and applied to all SyncEngines when changed.
 */
struct GlobalConfig
{
    /** Downward sync check interval (poll server HEAD), milliseconds. */
    int pollIntervalMs = 60 * 1000;

    /** Periodic full upward sync interval, milliseconds. */
    int fullSyncIntervalMs = 15 * 60 * 1000;

    /** Auto-add unversioned files during upward sync (otherwise commit
     *  only already-versioned changes). */
    bool autoAddUnversioned = true;

    /** Accept unknown server certificates (HTTPS/self-signed). */
    bool trustServerCertificate = true;

    /** Hide to the system tray instead of quitting when the window closes,
     *  keeping the background sync alive. */
    bool minimizeToTray = true;

    /** Start hidden in the system tray on launch instead of showing the
     *  main window. */
    bool startMinimizedToTray = true;

    /** Maximum number of log lines kept per repository. */
    int maxLogsPerRepo = 10000;

    /** Consecutive network-access failures (per repo) after which the repo
     *  is marked disconnected. A single successful server command resets
     *  the counter. */
    int disconnectThreshold = 3;

    /** Per-command network timeout in seconds applied to each SVN operation
     *  (libsvn http-timeout). The worker adds a small watchdog margin on top;
     *  a stuck connection therefore gives up after roughly this long instead
     *  of blocking the worker for libsvn's default 600 seconds. */
    int networkTimeoutSec = 60;

    /** Resolve conflicts automatically with conflictResolution instead of
     *  showing the conflict dialog. Tree conflicts are always resolved to
     *  the "working" state (code 5) regardless of this setting. */
    bool autoResolveConflicts = false;

    /** Default resolve choice used when autoResolveConflicts is enabled:
     *  0=Base 1=TheirsFull 2=MineFull 3=TheirsConflict 4=MineConflict
     *  5=Merged (same codes as CommandItem::conflictChoice). */
    int conflictResolution = 2;   // MineFull
};

} // namespace svnsync
