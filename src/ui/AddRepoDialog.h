#pragma once

#include "core/Repository.h"

#include <QDialog>

class QLineEdit;
class QCheckBox;
class QPushButton;

/**
 * Dialog for adding a repository to the app: working copy path, URL,
 * optional credentials and an initial state.
 */
class AddRepoDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AddRepoDialog(QWidget *parent = nullptr);

    svnsync::Repository repository() const;

private:
    void choosePath();
    void validate();

    QLineEdit *m_name = nullptr;
    QLineEdit *m_path = nullptr;
    QLineEdit *m_url = nullptr;
    QLineEdit *m_username = nullptr;
    QLineEdit *m_password = nullptr;
    QCheckBox *m_enabled = nullptr;
    QPushButton *m_okButton = nullptr;
};
