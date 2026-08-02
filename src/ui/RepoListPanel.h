#pragma once

#include "core/Repository.h"

#include <QList>
#include <QWidget>

class QListWidget;
class QListWidgetItem;

/**
 * Left sidebar: the list of configured repositories with a per-repo
 * state badge (active / background / deactive), a remove button per row
 * and an "add repository" button at the bottom.
 */
class RepoListPanel : public QWidget
{
    Q_OBJECT

public:
    explicit RepoListPanel(QWidget *parent = nullptr);

    void setRepositories(const QList<svnsync::Repository> &repositories);
    void setSelectedName(const QString &name);
    void updateState(const QString &name, svnsync::RepoState state);

    QString selectedName() const;

signals:
    void repositorySelected(const QString &name);
    void addRequested();
    void settingsRequested();
    void aboutRequested();

private:
    void rebuildRow(QListWidgetItem *item, const svnsync::Repository &repo);
    QListWidgetItem *findItem(const QString &name) const;

    QListWidget *m_list = nullptr;
};
