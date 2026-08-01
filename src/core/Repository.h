#pragma once

#include <QString>

namespace svnsync {

/**
 * Operational state of a configured repository.
 *
 *   Active     -> running, and currently selected/shown in the GUI
 *   Background -> running (watcher, queue, poll) but not shown
 *   Deactive   -> fully stopped; no watcher, no SVN queue
 *
 * A repository is "running" whenever state != Deactive. Only one repo is
 * ever Active at a time (the one displayed in the detail pane).
 */
enum class RepoState {
    Deactive,
    Background,
    Active
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

    bool running() const { return state != RepoState::Deactive; }
};

} // namespace svnsync
