#pragma once

#include "core/Repository.h"

#include <QHash>
#include <QMainWindow>
#include <QStackedWidget>
#include <QStringList>

#include <memory>

class QLabel;
class QSystemTrayIcon;
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

    /** Whether the app should launch hidden in the system tray. */
    bool startMinimizedToTray() const;

    /** Restore the main window from the system tray. */
    void showWindowFromTray();

private:
    void rebuildSidebar();
    void selectRepository(const QString &name);
    void onStateChanged(const QString &name, svnsync::RepoState state);
    void onRemoveRequested(const QString &name);
    void onAddRequested();
    void onSettingsRequested();
    void quitApplication();
    void setupTrayIcon();
    void closeEvent(QCloseEvent *event) override;
    void showNotification(const QString &name, const QString &message);
    void logToRepo(const QString &name, const QString &message);
    void onConflictDetected(const QString &name, const QStringList &paths,
                            const QStringList &treeConflictPaths);
    void onConflictResolved(const QString &name, const QString &path, int choiceCode,
                            bool treeConflict, bool success, const QString &error);
    void scanConflicts(const QString &name);
    RepoDetailPage *pageFor(const QString &name);
    void setGlobalStatus(const QString &text);

    std::unique_ptr<svnsync::RepoManager> m_manager;
    RepoListPanel *m_sidebar = nullptr;
    QStackedWidget *m_pages = nullptr;
    QLabel *m_emptyPage = nullptr;
    QHash<QString, RepoDetailPage *> m_pagesByRepo;
    QLabel *m_statusText = nullptr;
    QSystemTrayIcon *m_trayIcon = nullptr;
    QString m_selectedName;
};
