#pragma once

#include "core/Repository.h"

#include <QHash>
#include <QMainWindow>
#include <QStackedWidget>

#include <memory>

class QLabel;
class RepoListPanel;
class RepoDetailPage;

namespace svnsync {
class RepoManager;
class SyncEngine;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    /** Live mode entry point: add a repo from the command line and select it. */
    void startRepository(const svnsync::Repository &repository);

private:
    void rebuildSidebar();
    void selectRepository(const QString &name);
    void onStateChanged(const QString &name, svnsync::RepoState state);
    void onRemoveRequested(const QString &name);
    void onAddRequested();
    void showNotification(const QString &name, const QString &message);
    void onConflictDetected(const QString &name, const QStringList &paths);
    RepoDetailPage *pageFor(const QString &name);
    void setGlobalStatus(const QString &text);

    std::unique_ptr<svnsync::RepoManager> m_manager;
    RepoListPanel *m_sidebar = nullptr;
    QStackedWidget *m_pages = nullptr;
    QLabel *m_emptyPage = nullptr;
    QHash<QString, RepoDetailPage *> m_pagesByRepo;
    QLabel *m_statusText = nullptr;
    QString m_selectedName;
};
