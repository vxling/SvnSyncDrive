#include "ui/RepoDetailPage.h"

#include "core/SyncEngine.h"
#include "ui/FileBrowser.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
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

    // Header: name + state, with the remove action on the right.
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
    auto *configureButton = new QPushButton(QStringLiteral("⚙ 仓库配置"), header);
    auto *removeButton = new QPushButton(QStringLiteral("🗑 移除仓库"), header);
    titleRow->addWidget(configureButton);
    titleRow->addWidget(removeButton);
    headLayout->addLayout(titleRow);

    layout->addWidget(header);

    auto *tabs = new QTabWidget(this);
    m_browser = new FileBrowser(tabs);
    m_log = new QPlainTextEdit(tabs);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(20000);
    tabs->addTab(m_browser, QStringLiteral("文件"));
    tabs->addTab(m_log, QStringLiteral("日志"));
    layout->addWidget(tabs, 1);

    connect(configureButton, &QPushButton::clicked, this, &RepoDetailPage::configureRequested);
    connect(removeButton, &QPushButton::clicked, this, &RepoDetailPage::removeRequested);
    connect(m_browser, &FileBrowser::syncRequested,
            this, &RepoDetailPage::syncRequested);
    connect(m_browser, &FileBrowser::toggleStateRequested,
            this, &RepoDetailPage::toggleStateRequested);
    connect(m_browser, &FileBrowser::conflictScanRequested,
            this, &RepoDetailPage::conflictScanRequested);

    setState(svnsync::RepoState::Deactive);
}

void RepoDetailPage::setRepository(const svnsync::Repository &repo)
{
    m_name = repo.name;
    m_nameLabel->setText(repo.name);
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
    m_browser->setToggleState(state);
}

void RepoDetailPage::appendLog(const QString &message)
{
    m_log->appendPlainText(
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")) +
        QStringLiteral("  ") + message);
}

void RepoDetailPage::setLogHistory(const QStringList &lines)
{
    if (!lines.isEmpty())
        m_log->appendPlainText(lines.join(QLatin1Char('\n')));
}

void RepoDetailPage::refreshFiles()
{
    m_browser->refresh();
}
