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
};

} // namespace svnsync
