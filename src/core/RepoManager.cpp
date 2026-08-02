#include "core/RepoManager.h"

#include "core/ConfigStore.h"
#include "core/LogStore.h"
#include "core/SyncEngine.h"

namespace svnsync {

RepoManager::RepoManager(QObject *parent)
    : QObject(parent)
{
}

RepoManager::~RepoManager() = default;

void RepoManager::load()
{
    ConfigStore::initialize();
    m_repos = ConfigStore::loadRepositories();
    m_config = ConfigStore::loadGlobalConfig();
    enforceLimit();
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
    // A newly added repo is expected to be used: put it at the top of the
    // list so it is monitored (bumping the 5th one out if needed). A repo
    // added disabled goes to the bottom.
    if (repo.running())
        m_repos.prepend(repo);
    else
        m_repos.append(repo);
    enforceLimit();
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
    LogStore::clearRepository(name);
    enforceLimit();
    persist();
    emit repositoryListChanged();
}

void RepoManager::setState(const QString &name, RepoState state)
{
    const int i = indexOf(name);
    if (i < 0)
        return;
    // A repository beyond the monitoring limit cannot be started directly;
    // only promote() moves it back into the top-kMaxMonitoredRepos slots.
    if (state != RepoState::Deactive && i >= kMaxMonitoredRepos)
        state = RepoState::Deactive;
    if (m_repos[i].state == state)
        return;
    m_repos[i].state = state;

    if (state == RepoState::Deactive || state == RepoState::AuthFailed)
        stopEngine(name);
    else
        startEngine(name);

    persist();
    emit repositoryStateChanged(name, state);
}

void RepoManager::promote(const QString &name)
{
    const int i = indexOf(name);
    if (i < 0)
        return;
    const bool moved = i != 0;
    if (moved) {
        Repository repo = m_repos.takeAt(i);
        m_repos.prepend(repo);
    }
    const bool statesChanged = enforceLimit();
    if (moved || statesChanged) {
        persist();
        if (moved)
            emit repositoryListChanged();
    }
}

void RepoManager::demote(const QString &name)
{
    const int i = indexOf(name);
    if (i < 0)
        return;
    if (i == m_repos.size() - 1 && m_repos.at(i).state == RepoState::Deactive)
        return;
    Repository repo = m_repos.takeAt(i);
    repo.state = RepoState::Deactive;
    stopEngine(name);
    m_repos.append(repo);
    emit repositoryStateChanged(name, RepoState::Deactive);
    enforceLimit();
    persist();
    emit repositoryListChanged();
}

bool RepoManager::enforceLimit()
{
    bool changed = false;
    for (int i = 0; i < m_repos.size(); ++i) {
        Repository &repo = m_repos[i];
        if (i < kMaxMonitoredRepos) {
            if (repo.state == RepoState::Deactive) {
                repo.state = RepoState::Background;
                changed = true;
                emit repositoryStateChanged(repo.name, repo.state);
            }
            // An AuthFailed repo stays stopped (awaiting fixed credentials);
            // it must never be silently restarted by the limit enforcement.
            if (repo.state != RepoState::AuthFailed)
                startEngine(repo.name);
        } else {
            if (repo.state != RepoState::Deactive) {
                repo.state = RepoState::Deactive;
                changed = true;
                emit repositoryStateChanged(repo.name, repo.state);
            }
            stopEngine(repo.name);
        }
    }
    return changed;
}

void RepoManager::syncNow(const QString &name)
{
    SyncEngine *e = engine(name);
    if (e)
        e->syncNow();
}

void RepoManager::setCredentials(const QString &name, const QString &username,
                                 const QString &password)
{
    const int i = indexOf(name);
    if (i < 0)
        return;
    m_repos[i].username = username;
    // The edit dialog intentionally leaves the password field empty unless
    // the user retypes one. Keep the stored password in that case instead of
    // wiping it from the encrypted store.
    if (!password.isEmpty())
        m_repos[i].password = password;
    persist();
    if (m_repos[i].state == RepoState::AuthFailed) {
        // Credentials were why it stopped; restart with the new ones. Using
        // enforceLimit() keeps the monitoring-limit semantics (a repo beyond
        // the limit stays stopped even after the credential fix).
        m_priorStates.remove(name);
        m_repos[i].state = RepoState::Background;
        enforceLimit();
        emit repositoryStateChanged(name, m_repos.at(i).state);
    } else if (SyncEngine *e = engine(name)) {
        e->setCredentials(username, password);
    }
}

void RepoManager::markAuthFailed(const QString &name)
{
    const int i = indexOf(name);
    if (i < 0)
        return;
    if (m_repos[i].state == RepoState::AuthFailed)
        return;
    m_priorStates.remove(name);
    stopEngine(name);
    m_repos[i].state = RepoState::AuthFailed;
    persist();  // writes the durable Background equivalent, not the transient state
    emit repositoryStateChanged(name, RepoState::AuthFailed);
    emit notification(name, tr("认证失败，已停止同步。请在仓库配置中更新凭据后恢复。"));
}

void RepoManager::markDisconnected(const QString &name)
{
    const int i = indexOf(name);
    if (i < 0)
        return;
    if (m_repos[i].state == RepoState::Disconnected)
        return;
    m_priorStates[name] = m_repos[i].state;
    m_repos[i].state = RepoState::Disconnected;
    emit repositoryStateChanged(name, RepoState::Disconnected);
    emit notification(name, tr("连接断开（连续多次服务器访问失败），将继续重试…"));
}

void RepoManager::clearDisconnected(const QString &name)
{
    const int i = indexOf(name);
    if (i < 0)
        return;
    if (m_repos[i].state != RepoState::Disconnected)
        return;
    const RepoState prior = m_priorStates.take(name);
    m_repos[i].state = (prior == RepoState::Active || prior == RepoState::Background)
        ? prior : RepoState::Background;
    emit repositoryStateChanged(name, m_repos.at(i).state);
    emit notification(name, tr("连接已恢复。"));
}

void RepoManager::setConfig(const GlobalConfig &config)
{
    m_config = config;
    ConfigStore::saveGlobalConfig(config);
    for (auto &entry : m_engines)
        entry.second->setConfig(config);
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
    // AuthFailed/Disconnected are transient, in-memory-only states: the
    // persisted config always keeps the last durable running/stopped state.
    QList<Repository> durable = m_repos;
    for (Repository &r : durable) {
        if (r.state == RepoState::AuthFailed || r.state == RepoState::Disconnected)
            r.state = RepoState::Background;
    }
    ConfigStore::saveRepositories(durable);
}

void RepoManager::startEngine(const QString &name)
{
    if (m_engines.count(name) != 0)
        return;
    const int i = indexOf(name);
    if (i < 0)
        return;

    auto engine = std::make_unique<SyncEngine>(m_repos.at(i));
    engine->setConfig(m_config);
    const QString repoName = name;  // captured by value for lambdas

    connect(engine.get(), &SyncEngine::syncNotification, this,
            [this, repoName](const QString &m) { emit notification(repoName, m); });
    connect(engine.get(), &SyncEngine::filesChanged, this,
            [this, repoName]() { emit filesChanged(repoName); });
    connect(engine.get(), &SyncEngine::conflictDetected, this,
            [this, repoName](const QStringList &p) { emit conflictDetected(repoName, p); });
    connect(engine.get(), &SyncEngine::authenticationFailed, this,
            [this, repoName]() { markAuthFailed(repoName); });
    connect(engine.get(), &SyncEngine::connectionLost, this,
            [this, repoName]() { markDisconnected(repoName); });
    connect(engine.get(), &SyncEngine::connectionRestored, this,
            [this, repoName]() { clearDisconnected(repoName); });

    engine->start();
    m_engines.emplace(name, std::move(engine));
}

void RepoManager::stopEngine(const QString &name)
{
    m_engines.erase(name);
}

} // namespace svnsync
