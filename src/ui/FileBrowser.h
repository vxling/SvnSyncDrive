#pragma once

#include "core/Repository.h"
#include "core/SvnCommand.h"

#include <QList>
#include <QSet>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QTableWidget;
namespace svnsync {
class SyncEngine;
}

/**
 * Read-only file browser for one working copy. Shows the immediate
 * children of the current directory with their SVN status. Lists are
 * fetched through the repository's own SvnWorker queue (ReadOnly, high
 * priority), so browsing one repo never blocks another.
 *
 * No copy/paste/rename/edit operations are provided - this is a viewer
 * only, like SVNFileBox's file list without the file-management actions.
 */
class FileBrowser : public QWidget
{
    Q_OBJECT

public:
    explicit FileBrowser(QWidget *parent = nullptr);
    ~FileBrowser() override;

    void setEngine(svnsync::SyncEngine *engine);

    /** Point the browser at a working copy and jump to its root. */
    void setRepository(const QString &wcPath);
    void refresh();

    /** Keep the enable/disable sync button label in sync with repo state. */
    void setToggleState(svnsync::RepoState state);

    int itemCount() const;

signals:
    void statusTextChanged(const QString &text);
    void syncRequested();
    void toggleStateRequested();
    void conflictScanRequested();

private:
    struct Entry
    {
        QString name;
        QString path;
        bool isDir = false;
        qlonglong size = 0;
        QString mtime;
        svnsync::StatusKind kind = svnsync::StatusKind::None;
    };

    void goUp();
    void refreshFor(const QString &dir);
    void populateLocal();
    void applyStatus(const QString &dir, const svnsync::CommandResult &result);
    void populate(const QList<Entry> &entries);

    /** Shows the row context menu (open / svn actions). */
    void showContextMenu(const QPoint &pos);

    /** Schedules an SVN command on the current row's path; refreshes after. */
    void submitAction(svnsync::Command command,
                      const QString &path,
                      const QString &message = QString(),
                      const QString &toPath = QString());

    /** Tracks the hovered row on the viewport to paint a full-row highlight. */
    bool eventFilter(QObject *watched, QEvent *event) override;

    /** True if dir equals or lies under a known unversioned directory. */
    bool underUnversioned(const QString &dir) const;
    void markAllRowsUnversioned();
    void pruneUnversioned(const QString &current,
                          const svnsync::CommandResult &result);

    svnsync::SyncEngine *m_engine = nullptr;
    QString m_wcRoot;
    QString m_currentDir;
    int m_requestSeq = 0;
    int m_pendingRequest = 0;
    int m_hoverRow = -1;

    /**
     * Directories reported as unversioned by the last status call, so
     * entering any directory below one of them (svn cannot stat those
     * paths - "node was not found") still shows '?' without a round-trip.
     */
    QSet<QString> m_unversioned;

    QLabel *m_pathLabel = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_toggleButton = nullptr;
    QTableWidget *m_table = nullptr;
};
