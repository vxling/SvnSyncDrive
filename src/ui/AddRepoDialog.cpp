#include "ui/AddRepoDialog.h"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

AddRepoDialog::AddRepoDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("添加仓库"));
    setMinimumWidth(460);

    auto *layout = new QVBoxLayout(this);

    auto *form = new QFormLayout;
    m_name = new QLineEdit(this);
    m_path = new QLineEdit(this);
    m_path->setPlaceholderText(QStringLiteral("本地工作副本路径，例如 C:\\work\\project"));
    m_url = new QLineEdit(this);
    m_url->setPlaceholderText(QStringLiteral("仓库 URL，例如 https://svn.example.com/repo/project"));
    m_username = new QLineEdit(this);
    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    m_enabled = new QCheckBox(QStringLiteral("添加后立即启用同步"), this);
    m_enabled->setChecked(true);

    form->addRow(QStringLiteral("名称"), m_name);
    form->addRow(QStringLiteral("路径"), m_path);
    form->addRow(QStringLiteral("URL"), m_url);
    form->addRow(QStringLiteral("用户名"), m_username);
    form->addRow(QStringLiteral("密码"), m_password);
    layout->addLayout(form);
    layout->addWidget(m_enabled);

    auto *buttons = new QHBoxLayout;
    auto *browse = new QPushButton(QStringLiteral("浏览…"), this);
    auto *cancel = new QPushButton(QStringLiteral("取消"), this);
    m_okButton = new QPushButton(QStringLiteral("添加"), this);
    m_okButton->setDefault(true);
    m_okButton->setEnabled(false);
    buttons->addWidget(browse);
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(m_okButton);
    layout->addLayout(buttons);

    connect(browse, &QPushButton::clicked, this, &AddRepoDialog::choosePath);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_okButton, &QPushButton::clicked, this, &AddRepoDialog::validate);
    connect(m_name, &QLineEdit::textChanged, this, [this] { m_okButton->setEnabled(!m_name->text().trimmed().isEmpty()); });
    connect(m_path, &QLineEdit::textChanged, this, [this] { m_okButton->setEnabled(!m_name->text().trimmed().isEmpty()); });
}

void AddRepoDialog::choosePath()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择工作副本目录"),
        m_path->text().isEmpty() ? QDir::homePath() : m_path->text());
    if (!dir.isEmpty()) {
        m_path->setText(QDir::toNativeSeparators(dir));
        if (m_name->text().trimmed().isEmpty())
            m_name->setText(QDir(dir).dirName());
    }
}

void AddRepoDialog::validate()
{
    if (m_name->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少名称"), QStringLiteral("请输入仓库名称。"));
        return;
    }
    if (!QDir(m_path->text()).exists()) {
        QMessageBox::warning(this, QStringLiteral("路径无效"),
                             QStringLiteral("工作副本路径不存在，请选择正确的 SVN 工作副本目录。"));
        return;
    }
    accept();
}

svnsync::Repository AddRepoDialog::repository() const
{
    svnsync::Repository repo;
    repo.name = m_name->text().trimmed();
    repo.path = QDir::cleanPath(QDir::fromNativeSeparators(m_path->text().trimmed()));
    repo.url = m_url->text().trimmed();
    repo.username = m_username->text().trimmed();
    repo.password = m_password->text();
    repo.state = m_enabled->isChecked() ? svnsync::RepoState::Background
                                        : svnsync::RepoState::Deactive;
    return repo;
}
