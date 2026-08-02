#pragma once

#include "core/Repository.h"

#include <QDialog>

#include <memory>

class QLabel;
class QLineEdit;
class QCheckBox;
class QPushButton;

namespace svnsync {
class SvnWorker;
struct CommandResult;
}

/**
 * Dialog for defining a repository: working copy path, URL, optional
 * credentials and an initial state. A "test connection" button verifies
 * the URL through a temporary SvnWorker.
 *
 * In add mode the working copy path defaults to Documents/<name> and is
 * freely editable (overridable via "浏览…"). On "添加" the path is
 * validated: it is created if missing, must be empty (or an existing
 * working copy) to be accepted, and a fresh working copy is checked out
 * into it before the dialog closes. In configure mode (configure == true)
 * the name / URL / path are locked and only the credentials can be
 * changed; the "添加" button becomes "更新" and, on click, validates the
 * new credentials against the server, closing the dialog only when they
 * are accepted.
 */
class AddRepoDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AddRepoDialog(bool configure, QWidget *parent = nullptr);
    ~AddRepoDialog() override;

    void setRepository(const svnsync::Repository &repo);
    svnsync::Repository repository() const;

    /** Apply the global "trust self-signed / unknown certificates" setting
     *  to the dialog's temporary SvnWorker. */
    void setTrustServerCertificate(bool trust);

private:
    static QString defaultPathFor(const QString &name);

    void choosePath();
    void testConnection(bool validate);
    void onTestResult(quint64 id, const svnsync::CommandResult &result);
    void validate();
    void startCheckout(const QString &path);
    void restoreOkFocus();

    const bool m_configure;
    svnsync::RepoState m_state = svnsync::RepoState::Background;
    bool m_pathCustomized = false;
    bool m_checkoutPending = false;
    // In configure mode a successful test only closes the dialog when the
    // "更新" button initiated it; "测试连接" must keep the dialog open.
    bool m_validateOnSuccess = false;

    QLineEdit *m_name = nullptr;
    QLineEdit *m_path = nullptr;
    QLineEdit *m_url = nullptr;
    QLineEdit *m_username = nullptr;
    QLineEdit *m_password = nullptr;
    QCheckBox *m_enabled = nullptr;
    QPushButton *m_testButton = nullptr;
    QLabel *m_testResult = nullptr;
    QPushButton *m_okButton = nullptr;

    std::unique_ptr<svnsync::SvnWorker> m_worker;
    quint64 m_testId = 0;
    bool m_trustCert = true;
};
