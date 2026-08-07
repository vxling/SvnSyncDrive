#include "ui/RepoDetailPage.h"

#include "core/I18n.h"
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
    case svnsync::RepoState::Active: return I18n::translate("● 同步中");
    case svnsync::RepoState::Background: return I18n::translate("◐ 后台同步");
    case svnsync::RepoState::Deactive: return I18n::translate("○ 停止监控");
    case svnsync::RepoState::AuthFailed: return I18n::translate("✕ 认证失败");
    case svnsync::RepoState::Disconnected: return I18n::translate("✖ 断开链接");
    }
    return QString();
}

QColor stateColor(svnsync::RepoState state)
{
    switch (state) {
    case svnsync::RepoState::Active: return QColor(0x00, 0x9A, 0x3E);
    case svnsync::RepoState::Background: return QColor(0xE8, 0x8A, 0x00);
    case svnsync::RepoState::Deactive: return QColor(0x9E, 0x9E, 0x9E);
    case svnsync::RepoState::AuthFailed: return QColor(0xD3, 0x2F, 0x2F);
    case svnsync::RepoState::Disconnected: return QColor(0xE6, 0x4A, 0x19);
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
    m_configureButton = new QPushButton(I18n::translate("⚙ 仓库配置"), header);
    m_removeButton = new QPushButton(I18n::translate("🗑 移除仓库"), header);
    titleRow->addWidget(m_configureButton);
    titleRow->addWidget(m_removeButton);
    headLayout->addLayout(titleRow);

    layout->addWidget(header);

    m_tabs = new QTabWidget(this);
    m_browser = new FileBrowser(m_tabs);
    m_log = new QPlainTextEdit(m_tabs);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(20000);
    m_tabs->addTab(m_browser, I18n::translate("文件"));
    m_tabs->addTab(m_log, I18n::translate("日志"));
    layout->addWidget(m_tabs, 1);

    connect(m_configureButton, &QPushButton::clicked, this, &RepoDetailPage::configureRequested);
    connect(m_removeButton, &QPushButton::clicked, this, &RepoDetailPage::removeRequested);
    connect(m_browser, &FileBrowser::syncRequested,
            this, &RepoDetailPage::syncRequested);
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

void RepoDetailPage::retranslate()
{
    m_configureButton->setText(I18n::translate("⚙ 仓库配置"));
    m_removeButton->setText(I18n::translate("🗑 移除仓库"));
    m_tabs->setTabText(0, I18n::translate("文件"));
    m_tabs->setTabText(1, I18n::translate("日志"));
    setState(m_state);
    m_browser->retranslate();
}
