#pragma once

#include "core/GlobalConfig.h"
#include "core/Repository.h"

#include <QObject>
#include <QStringList>

#include <memory>
#include <unordered_map>

namespace svnsync {

class SyncEngine;

/**
 * Owns the engines for every configured repository.
 *
 * Each repository is fully independent: its own SyncEngine owns its own
 * RepoWatcher, its own SvnWorker queue and its own poll timers. This
 * manager only starts/stops those engines based on the repository's
 * RepoState and forwards their signals to the UI, tagging them with the
 * repository name.
 *
 * Only one repository may be Active (the one shown in the GUI); all other
 * running repositories are Background.
 */
class RepoManager : public QObject
{
    Q_OBJECT

public:
    /** Maximum number of repositories monitored at once. The list order
     *  decides who runs: the first kMaxMonitoredRepos entries are monitored,
     *  every entry after position kMaxMonitoredRepos is stopped. */
    static constexpr int kMaxMonitoredRepos = 5;

    explicit RepoManager(QObject *parent = nullptr);
    ~RepoManager() override;

    /** Load persisted repositories from the config store and start the
     *  engines of every running (Active/Background) repository. */
    void load();

    QList<Repository> repositories() const;
    const Repository *repository(const QString &name) const;
    SyncEngine *engine(const QString &name) const;

    void addRepository(const Repository &repo);
    void removeRepository(const QString &name);
    void setState(const QString &name, RepoState state);

    /** Move a repository to the top of the list and make sure it is being
     *  monitored, enforcing the concurrency limit (the entry that falls past
     *  kMaxMonitoredRepos stops monitoring). */
    void promote(const QString &name);

    /** Stop monitoring a repository and move it to the bottom of the list. */
    void demote(const QString &name);

    /** Refresh the credentials used by a repository's engine. */
    void setCredentials(const QString &name, const QString &username,
                        const QString &password);

    /** Request an immediate upward sync for one repository. */
    void syncNow(const QString &name);

    GlobalConfig config() const { return m_config; }

    /** Persist and apply new global settings to every running engine. */
    void setConfig(const GlobalConfig &config);

    signals:
    void repositoryListChanged();
    void repositoryStateChanged(const QString &name, RepoState state);
    void notification(const QString &name, const QString &message);
    void filesChanged(const QString &name);
    void conflictDetected(const QString &name, const QStringList &paths);

private:
    int indexOf(const QString &name) const;
    void persist();
    void startEngine(const QString &name);
    void stopEngine(const QString &name);

    /** Apply the monitoring limit to the current list order: the first
     *  kMaxMonitoredRepos entries keep (or get) a running engine, every
     *  later entry is stopped. Returns true if any state changed. */
    bool enforceLimit();

    QList<Repository> m_repos;
    GlobalConfig m_config;
    std::unordered_map<QString, std::unique_ptr<SyncEngine>> m_engines;
};

} // namespace svnsync
