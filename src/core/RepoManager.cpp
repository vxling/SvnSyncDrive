#include "core/RepoManager.h"

#include "core/ConfigStore.h"
#include "core/SyncEngine.h"

namespace svnsync {

RepoManager::RepoManager(QObject *parent)
    : QObject(parent)
{
}

RepoManager::~RepoManager() = default;

void RepoManager::load()
{
    m_repos = ConfigStore::loadRepositories();
    for (const auto &repo : m_repos)
        if (repo.running())
            startEngine(repo.name);
    emit repositoryListChanged();
}

QList<Repository> RepoManager::repositories() const
{
    return m_repos;
}

const Repository *RepoManager::repository(const QString &name) const
{
    const int i = indexOf(name);
    return i >= 0 ? &m_repos.at(i) : nullptr;
}

SyncEngine *RepoManager::engine(const QString &name) const
{
    const auto it = m_engines.find(name);
    return it != m_engines.end() ? it->second.get() : nullptr;
}

void RepoManager::addRepository(const Repository &repo)
{
    if (indexOf(repo.name) >= 0)
        return;
    m_repos.append(repo);
    if (repo.running())
        startEngine(repo.name);
    persist();
    emit repositoryListChanged();
}

void RepoManager::removeRepository(const QString &name)
{
    const int i = indexOf(name);
    if (i < 0)
        return;
    stopEngine(name);
    m_repos.removeAt(i);
    persist();
    emit repositoryListChanged();
}

void RepoManager::setState(const QString &name, RepoState state)
{
    const int i = indexOf(name);
    if (i < 0)
        return;
    if (m_repos[i].state == state)
        return;
    m_repos[i].state = state;

    if (state == RepoState::Deactive)
        stopEngine(name);
    else
        startEngine(name);

    persist();
    emit repositoryStateChanged(name, state);
}

void RepoManager::syncNow(const QString &name)
{
    SyncEngine *e = engine(name);
    if (e)
        e->syncNow();
}

int RepoManager::indexOf(const QString &name) const
{
    for (int i = 0; i < m_repos.size(); ++i)
        if (m_repos.at(i).name == name)
            return i;
    return -1;
}

void RepoManager::persist()
{
    ConfigStore::saveRepositories(m_repos);
}

void RepoManager::startEngine(const QString &name)
{
    if (m_engines.count(name) != 0)
        return;
    const int i = indexOf(name);
    if (i < 0)
        return;

    auto engine = std::make_unique<SyncEngine>(m_repos.at(i));
    const QString repoName = name;  // captured by value for lambdas

    connect(engine.get(), &SyncEngine::syncNotification, this,
            [this, repoName](const QString &m) { emit notification(repoName, m); });
    connect(engine.get(), &SyncEngine::filesChanged, this,
            [this, repoName]() { emit filesChanged(repoName); });
    connect(engine.get(), &SyncEngine::conflictDetected, this,
            [this, repoName](const QStringList &p) { emit conflictDetected(repoName, p); });

    engine->start();
    m_engines.emplace(name, std::move(engine));
}

void RepoManager::stopEngine(const QString &name)
{
    m_engines.erase(name);
}

} // namespace svnsync
