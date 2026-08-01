#include "ui/RepoDetailPage.h"

#include "core/SyncEngine.h"
#include "ui/FileBrowser.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QString stateText(svnsync::RepoState state)
{
    switch (state) {
    case svnsync::RepoState::Active: return QStringLiteral("● 同步中");
    case svnsync::RepoState::Background: return QStringLiteral("◐ 后台同步");
    case svnsync::RepoState::Deactive: return QStringLiteral("○ 已停用");
    }
    return QString();
}

QColor stateColor(svnsync::RepoState state)
{
    switch (state) {
    case svnsync::RepoState::Active: return QColor(0x00, 0x9A, 0x3E);
    case svnsync::RepoState::Background: return QColor(0xE8, 0x8A, 0x00);
    case svnsync::RepoState::Deactive: return QColor(0x9E, 0x9E, 0x9E);
    }
    return QColor(Qt::black);
}

} // namespace

RepoDetailPage::RepoDetailPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    // Header: name + state + url/path + actions
    auto *header = new QWidget(this);
    auto *headLayout = new QVBoxLayout(header);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->setSpacing(2);

    auto *titleRow = new QHBoxLayout;
    m_nameLabel = new QLabel(header);
    QFont titleFont = m_nameLabel->font();
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleFont.setBold(true);
    m_nameLabel->setFont(titleFont);
    m_stateLabel = new QLabel(header);
    m_stateLabel->setFont(m_nameLabel->font());
    titleRow->addWidget(m_nameLabel);
    titleRow->addSpacing(12);
    titleRow->addWidget(m_stateLabel);
    titleRow->addStretch(1);
    headLayout->addLayout(titleRow);

    m_urlLabel = new QLabel(header);
    m_urlLabel->setStyleSheet(QStringLiteral("color: #666;"));
    m_urlLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    headLayout->addWidget(m_urlLabel);

    m_pathLabel = new QLabel(header);
    m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    headLayout->addWidget(m_pathLabel);

    auto *actions = new QHBoxLayout;
    auto *syncButton = new QPushButton(QStringLiteral("立即同步"), header);
    m_toggleButton = new QPushButton(QStringLiteral("启用同步"), header);
    auto *openButton = new QPushButton(QStringLiteral("打开目录"), header);
    auto *removeButton = new QPushButton(QStringLiteral("移除仓库"), header);
    actions->addWidget(syncButton);
    actions->addWidget(m_toggleButton);
    actions->addStretch(1);
    actions->addWidget(openButton);
    actions->addWidget(removeButton);
    headLayout->addLayout(actions);

    layout->addWidget(header);

    auto *tabs = new QTabWidget(this);
    m_browser = new FileBrowser(tabs);
    m_log = new QPlainTextEdit(tabs);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(5000);
    tabs->addTab(m_browser, QStringLiteral("文件"));
    tabs->addTab(m_log, QStringLiteral("日志"));
    layout->addWidget(tabs, 1);

    connect(syncButton, &QPushButton::clicked, this, &RepoDetailPage::syncRequested);
    connect(m_toggleButton, &QPushButton::clicked, this, &RepoDetailPage::toggleStateRequested);
    connect(openButton, &QPushButton::clicked, this, [this] {
        emit openInExplorerRequested();
    });
    connect(removeButton, &QPushButton::clicked, this, &RepoDetailPage::removeRequested);

    setState(svnsync::RepoState::Deactive);
}

void RepoDetailPage::setRepository(const svnsync::Repository &repo)
{
    m_name = repo.name;
    m_nameLabel->setText(repo.name);
    m_urlLabel->setText(repo.url);
    m_pathLabel->setText(repo.path);
    m_browser->setRepository(repo.path);
    setState(repo.state);
}

void RepoDetailPage::setEngine(svnsync::SyncEngine *engine)
{
    m_browser->setEngine(engine);
    m_browser->refresh();
}

void RepoDetailPage::setState(svnsync::RepoState state)
{
    m_state = state;
    m_stateLabel->setText(stateText(state));
    m_stateLabel->setStyleSheet(
        QStringLiteral("color: %1; font-weight: bold;")
            .arg(stateColor(state).name()));
    m_toggleButton->setText(state == svnsync::RepoState::Deactive
        ? QStringLiteral("启用同步")
        : QStringLiteral("停用同步"));
}

void RepoDetailPage::appendLog(const QString &message)
{
    m_log->appendPlainText(
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")) +
        QStringLiteral("  ") + message);
}

void RepoDetailPage::refreshFiles()
{
    m_browser->refresh();
}
