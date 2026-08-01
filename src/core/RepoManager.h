#pragma once

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

    /** Request an immediate upward sync for one repository. */
    void syncNow(const QString &name);

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

    QList<Repository> m_repos;
    std::unordered_map<QString, std::unique_ptr<SyncEngine>> m_engines;
};

} // namespace svnsync
