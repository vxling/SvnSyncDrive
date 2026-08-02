#include "ui/AddRepoDialog.h"

#include "core/SvnCommand.h"
#include "core/SvnWorker.h"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

AddRepoDialog::AddRepoDialog(bool configure, QWidget *parent)
    : QDialog(parent)
    , m_configure(configure)
    , m_worker(std::make_unique<svnsync::SvnWorker>(this))
{
    setWindowTitle(configure ? QStringLiteral("仓库配置") : QStringLiteral("添加仓库"));
    setMinimumWidth(480);

    auto *layout = new QVBoxLayout(this);

    auto *form = new QFormLayout;
    m_name = new QLineEdit(this);
    m_path = new QLineEdit(this);
    // Freely editable in add mode; locked in configure mode.
    m_path->setReadOnly(m_configure);
    m_path->setPlaceholderText(QStringLiteral("本地工作副本路径（不存在则自动创建）"));
    m_url = new QLineEdit(this);
    m_url->setPlaceholderText(QStringLiteral("仓库 URL，例如 https://svn.example.com/repo/project"));
    m_username = new QLineEdit(this);
    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);

    // Path row: read-only field with the browse button right after it.
    auto *pathRow = new QHBoxLayout;
    pathRow->setSpacing(6);
    auto *browse = new QPushButton(QStringLiteral("浏览…"), this);
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(browse);
    if (m_configure)
        browse->setVisible(false);

    form->addRow(QStringLiteral("名称"), m_name);
    form->addRow(QStringLiteral("路径"), pathRow);
    form->addRow(QStringLiteral("URL"), m_url);
    form->addRow(QStringLiteral("用户名"), m_username);
    form->addRow(QStringLiteral("密码"), m_password);
    layout->addLayout(form);

    if (!m_configure) {
        m_enabled = new QCheckBox(QStringLiteral("添加后立即启用同步"), this);
        m_enabled->setChecked(true);
        layout->addWidget(m_enabled);
    }

    auto *credHint = new QLabel(
        QStringLiteral("凭据会交给 libsvn 加密存储，不会保存到应用配置中。"), this);
    credHint->setStyleSheet(QStringLiteral("color: #888;"));
    layout->addWidget(credHint);

    auto *testRow = new QHBoxLayout;
    m_testButton = new QPushButton(QStringLiteral("测试连接"), this);
    m_testResult = new QLabel(this);
    m_testResult->setWordWrap(true);
    testRow->addWidget(m_testButton);
    testRow->addWidget(m_testResult, 1);
    layout->addLayout(testRow);

    auto *buttons = new QHBoxLayout;
    auto *cancel = new QPushButton(QStringLiteral("取消"), this);
    m_okButton = new QPushButton(
        configure ? QStringLiteral("更新") : QStringLiteral("添加"), this);
    m_okButton->setDefault(true);
    m_okButton->setEnabled(false);
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(m_okButton);
    layout->addLayout(buttons);

    if (m_configure) {
        m_name->setReadOnly(true);
        m_url->setReadOnly(true);
    }

    m_worker->start();
    connect(m_worker.get(), &svnsync::SvnWorker::resultReady,
            this, &AddRepoDialog::onTestResult);

    connect(browse, &QPushButton::clicked, this, &AddRepoDialog::choosePath);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_okButton, &QPushButton::clicked, this, &AddRepoDialog::validate);
    connect(m_testButton, &QPushButton::clicked, this,
            [this] { testConnection(false); });
    connect(m_name, &QLineEdit::textChanged, this, [this](const QString &name) {
        if (!m_configure && !m_pathCustomized)
            m_path->setText(QDir::toNativeSeparators(defaultPathFor(name)));
        if (!m_configure)
            m_okButton->setEnabled(!name.trimmed().isEmpty());
    });
    connect(m_path, &QLineEdit::textEdited, this, [this](const QString &) {
        m_pathCustomized = true;
    });
    if (m_configure) {
        // Refreshing credentials means typing a new password; "更新" stays
        // disabled until one is entered.
        connect(m_password, &QLineEdit::textChanged, this, [this](const QString &text) {
            m_okButton->setEnabled(!text.isEmpty());
        });
    }
}

AddRepoDialog::~AddRepoDialog() = default;

void AddRepoDialog::setTrustServerCertificate(bool trust)
{
    m_trustCert = trust;
    m_worker->setTrustServerCertificate(trust);
}

QString AddRepoDialog::defaultPathFor(const QString &name)
{
    // Cross-platform default: ~/SvnSyncDrive/<name>. Keeps every platform's
    // working copies in the same predictable place under the user's home.
    QString dir = QDir::homePath() + QLatin1String("/SvnSyncDrive");
    QString folder = name.trimmed();
    folder.replace(
        QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));
    if (folder.isEmpty())
        folder = QStringLiteral("repo");
    return dir + QLatin1Char('/') + folder;
}

void AddRepoDialog::setRepository(const svnsync::Repository &repo)
{
    m_state = repo.state;
    m_name->setText(repo.name);
    m_path->setText(QDir::toNativeSeparators(repo.path));
    m_url->setText(repo.url);
    m_username->setText(repo.username);
    // The password is deliberately left empty: refreshing credentials means
    // re-entering it, and "更新" stays disabled until a password is typed.
    m_password->clear();
    if (!m_configure)
        m_okButton->setEnabled(true);
}

void AddRepoDialog::choosePath()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择工作副本目录"),
        m_path->text().isEmpty() ? QDir::homePath() : m_path->text());
    if (!dir.isEmpty()) {
        m_pathCustomized = true;
        m_path->setText(QDir::toNativeSeparators(dir));
        if (m_name->text().trimmed().isEmpty())
            m_name->setText(QDir(dir).dirName());
    }
}

void AddRepoDialog::testConnection(bool validate)
{
    m_validateOnSuccess = validate;
    const QString url = m_url->text().trimmed();
    if (url.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少 URL"),
                             QStringLiteral("请输入仓库 URL 后再测试连接。"));
        return;
    }
    m_testButton->setEnabled(false);
    m_testButton->setText(QStringLiteral("测试中…"));
    m_okButton->setEnabled(false);
    m_testResult->clear();

    svnsync::CommandItem item;
    item.command = svnsync::Command::TestConnection;
    item.id = ++m_testId;
    item.repoUrl = url;
    item.username = m_username->text().trimmed();
    item.password = m_password->text();
    m_worker->setCredentials(item.username, item.password);
    m_worker->submit(item);
}

void AddRepoDialog::onTestResult(quint64 id, const svnsync::CommandResult &result)
{
    if (id != m_testId)
        return;

    if (m_checkoutPending) {
        m_checkoutPending = false;
        if (result.success) {
            accept();
        } else {
            m_testButton->setEnabled(true);
            m_testButton->setText(QStringLiteral("测试连接"));
            m_testResult->setText(
                QStringLiteral("创建工作副本失败：%1").arg(result.error));
            m_testResult->setStyleSheet(QStringLiteral("color: #C62828;"));
            m_okButton->setEnabled(!m_name->text().trimmed().isEmpty());
            if (m_okButton->isEnabled())
                restoreOkFocus();
        }
        return;
    }

    m_testButton->setEnabled(true);
    m_testButton->setText(QStringLiteral("测试连接"));

    if (m_configure) {
        if (result.success) {
            if (m_validateOnSuccess) {
                // "更新": the user is confirming new credentials, so the
                // dialog closes once the connection is accepted.
                accept();
            } else {
                // "测试连接": report success but keep the dialog open.
                m_testResult->setText(
                    result.revision > 0
                        ? QStringLiteral("连接成功（HEAD 版本 r%1）").arg(result.revision)
                        : QStringLiteral("连接成功"));
                m_testResult->setStyleSheet(QStringLiteral("color: #009A3E;"));
                m_okButton->setEnabled(!m_password->text().isEmpty());
                if (m_okButton->isEnabled())
                    restoreOkFocus();
            }
        } else {
            m_testResult->setText(
                QStringLiteral("用户名或密码不正确：%1").arg(result.error));
            m_testResult->setStyleSheet(QStringLiteral("color: #C62828;"));
            m_okButton->setEnabled(!m_password->text().isEmpty());
            if (m_okButton->isEnabled())
                restoreOkFocus();
        }
        return;
    }

    m_okButton->setEnabled(!m_name->text().trimmed().isEmpty());
    if (m_okButton->isEnabled())
        restoreOkFocus();
    if (result.success) {
        m_testResult->setText(
            result.revision > 0
                ? QStringLiteral("连接成功（HEAD 版本 r%1）").arg(result.revision)
                : QStringLiteral("连接成功"));
        m_testResult->setStyleSheet(QStringLiteral("color: #009A3E;"));
    } else {
        m_testResult->setText(QStringLiteral("连接失败：%1").arg(result.error));
        m_testResult->setStyleSheet(QStringLiteral("color: #C62828;"));
    }
}

void AddRepoDialog::validate()
{
    if (m_configure) {
        // Validate the (new) credentials against the server; the dialog
        // closes only when the connection succeeds.
        testConnection(true);
        return;
    }

    if (m_name->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少名称"), QStringLiteral("请输入仓库名称。"));
        return;
    }
    const QString path = QDir::fromNativeSeparators(m_path->text().trimmed());
    if (path.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少路径"),
                             QStringLiteral("请输入工作副本路径。"));
        return;
    }
    const QFileInfo fi(path);
    if (fi.exists() && fi.isFile()) {
        QMessageBox::warning(this, QStringLiteral("路径无效"),
                             QStringLiteral("所选路径指向一个文件，请选择目录。"));
        return;
    }

    // A fresh working copy is checked out into the target directory when it
    // is missing or empty; a non-empty directory is accepted only if it is
    // already a working copy.
    const QDir dir(path);
    bool needsCheckout = false;
    if (!dir.exists()) {
        if (!QDir().mkpath(path)) {
            QMessageBox::warning(this, QStringLiteral("路径无效"),
                                 QStringLiteral("无法创建工作副本目录，请更换路径后重试。"));
            return;
        }
        needsCheckout = true;
    } else {
        const QStringList entries =
            dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System);
        if (!entries.isEmpty()) {
            if (!dir.exists(QLatin1String(".svn"))) {
                QMessageBox::warning(
                    this, QStringLiteral("目录不为空"),
                    QStringLiteral("所选目录不为空且不是工作副本，请选择空目录或更换路径。"));
                return;
            }
            // Existing working copy: attach without checkout.
        } else {
            needsCheckout = true;
        }
    }

    if (needsCheckout) {
        startCheckout(path);
        return;
    }
    accept();
}

void AddRepoDialog::startCheckout(const QString &path)
{
    const QString url = m_url->text().trimmed();
    if (url.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少 URL"),
                             QStringLiteral("请输入仓库 URL 后再添加。"));
        return;
    }
    m_checkoutPending = true;
    m_okButton->setEnabled(false);
    m_testButton->setEnabled(false);
    m_testResult->setText(QStringLiteral("正在创建工作副本…"));
    m_testResult->setStyleSheet(QStringLiteral("color: #666;"));

    svnsync::CommandItem item;
    item.command = svnsync::Command::Checkout;
    item.id = ++m_testId;
    item.repoUrl = url;
    item.path = QDir::cleanPath(path);
    item.username = m_username->text().trimmed();
    item.password = m_password->text();
    m_worker->setCredentials(item.username, item.password);
    m_worker->submit(item);
}

void AddRepoDialog::restoreOkFocus()
{
    // Re-enabling a button does not return the dialog's default/focus to it:
    // disabling the focused "测试连接" during the test pushed focus to
    // "取消", so put the primary action back in charge on every re-enable.
    m_okButton->setDefault(true);
    m_okButton->setFocus();
}

svnsync::Repository AddRepoDialog::repository() const
{
    svnsync::Repository repo;
    repo.name = m_name->text().trimmed();
    repo.path = QDir::cleanPath(QDir::fromNativeSeparators(m_path->text().trimmed()));
    repo.url = m_url->text().trimmed();
    repo.username = m_username->text().trimmed();
    repo.password = m_password->text();
    repo.state = m_configure
        ? m_state
        : (m_enabled->isChecked() ? svnsync::RepoState::Background
                                  : svnsync::RepoState::Deactive);
    return repo;
}
