#pragma once

#include <QString>

namespace svnsync {

/**
 * Operational state of a configured repository.
 *
 *   Active        -> running, and currently selected/shown in the GUI
 *   Background    -> running (watcher, queue, poll) but not shown
 *   Deactive      -> fully stopped; no watcher, no SVN queue
 *   AuthFailed    -> transient: an SVN authentication error stopped the
 *                    engine (same as Deactive); fix credentials to resume
 *   Disconnected  -> transient: repeated server-access failures; the engine
 *                    keeps running and retrying on the normal poll interval
 *
 * A repository is "running" in Background, Active and Disconnected. Only one
 * repo is ever Active at a time (the one displayed in the detail pane).
 * AuthFailed and Disconnected are in-memory-only states: they are never
 * persisted, and are written back as Background when saving.
 */
enum class RepoState {
    Deactive,
    Background,
    Active,
    AuthFailed,
    Disconnected
};

/** A single SVN repository configured in the app. */
struct Repository
{
    QString name;
    QString path;        // working copy path
    QString url;         // repository URL
    QString username;
    QString password;
    RepoState state = RepoState::Background;

    bool running() const
    {
        return state == RepoState::Background || state == RepoState::Active
            || state == RepoState::Disconnected;
    }
};

} // namespace svnsync
