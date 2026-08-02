#pragma once

#include <QDialog>

#include <QStringList>

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;

namespace svnsync {
class SyncEngine;
}

/**
 * Dialog that lists conflicted files and lets the user resolve all of them
 * with one choice (mine / theirs / merged / base) through the repo's own
 * SvnWorker queue. It is modal but the resolve commands run asynchronously
 * on the worker thread; the dialog closes itself once all have succeeded.
 */
class ConflictDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ConflictDialog(svnsync::SyncEngine *engine,
                            const QStringList &conflicts,
                            QWidget *parent = nullptr);

private:
    void resolveAll();
    void finishIfDone();

    svnsync::SyncEngine *m_engine = nullptr;
    QStringList m_conflicts;
    QStringList m_failures;
    int m_resolving = 0;
    int m_succeeded = 0;

    QListWidget *m_list = nullptr;
    QComboBox *m_choice = nullptr;
    QPushButton *m_resolveButton = nullptr;
    QLabel *m_status = nullptr;
};
