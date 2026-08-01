#pragma once

#include "core/SvnCommand.h"

#include <QList>
#include <QString>
#include <QWidget>

class QLabel;
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

    int itemCount() const;

signals:
    void statusTextChanged(const QString &text);

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
    void onStatusResult(const QString &dir, const svnsync::CommandResult &result);
    void populate(const QList<Entry> &entries);

    svnsync::SyncEngine *m_engine = nullptr;
    QString m_wcRoot;
    QString m_currentDir;
    int m_requestSeq = 0;
    int m_pendingRequest = 0;

    QLabel *m_pathLabel = nullptr;
    QTableWidget *m_table = nullptr;
};
