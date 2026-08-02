#pragma once

#include "core/Repository.h"

#include <QStringList>
#include <QWidget>

class QLabel;
class QPlainTextEdit;

namespace svnsync {
class SyncEngine;
}

class FileBrowser;

/**
 * Detail page for one repository: header with state, a read-only file
 * browser and that repository's own log. Each repository gets its own
 * page instance so logs accumulate per repository even while it is in
 * the background.
 */
class RepoDetailPage : public QWidget
{
    Q_OBJECT

public:
    explicit RepoDetailPage(QWidget *parent = nullptr);

    void setRepository(const svnsync::Repository &repo);
    void setEngine(svnsync::SyncEngine *engine);
    void setState(svnsync::RepoState state);
    void appendLog(const QString &message);
    void setLogHistory(const QStringList &lines);
    void refreshFiles();

    QString repoName() const { return m_name; }

signals:
    void syncRequested();
    void toggleStateRequested();
    void configureRequested();
    void removeRequested();
    void conflictScanRequested();

private:
    QString m_name;
    svnsync::RepoState m_state = svnsync::RepoState::Deactive;

    QLabel *m_nameLabel = nullptr;
    QLabel *m_stateLabel = nullptr;
    QPlainTextEdit *m_log = nullptr;
    FileBrowser *m_browser = nullptr;
};
