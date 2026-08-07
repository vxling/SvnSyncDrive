#include "ui/RepoListPanel.h"

#include "core/I18n.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QString stateBadge(svnsync::RepoState state)
{
    switch (state) {
    case svnsync::RepoState::Active: return I18n::translate("● 同步中");
    case svnsync::RepoState::Background: return I18n::translate("◐ 后台");
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

/** Small folder glyph for a repository row. Monitored repos use the app
 *  icon's blue->teal->green gradient, stopped ones are grey, so the state is
 *  readable at a glance. */
QPixmap repoPixmap(svnsync::RepoState state)
{
    const int size = 20;
    const qreal dpr = 2.0;
    QPixmap pixmap(int(size * dpr), int(size * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    const bool running = state == svnsync::RepoState::Background
        || state == svnsync::RepoState::Active
        || state == svnsync::RepoState::Disconnected;
    // Mirrors resources/icon.svg: #1B6FD4 -> #17A0B0 -> #2FC25B.
    const QColor gradTop = running ? QColor(0x1B, 0x6F, 0xD4) : QColor(0xCD, 0xCD, 0xCD);
    const QColor gradMid = running ? QColor(0x17, 0xA0, 0xB0) : QColor(0xBC, 0xBC, 0xBC);
    const QColor gradBot = running ? QColor(0x2F, 0xC2, 0x5B) : QColor(0xAA, 0xAA, 0xAA);
    const QColor tab = running ? QColor(0x0F, 0x5C, 0x9C) : QColor(0x95, 0x95, 0x95);
    const QColor outline = running ? QColor(0x0E, 0x7F, 0x93) : QColor(0x8A, 0x8A, 0x8A);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // Folder tab.
    QPainterPath tabPath;
    tabPath.addRoundedRect(QRectF(2.5, 3.5, 8.5, 4.5), 1.6, 1.6);
    painter.fillPath(tabPath, tab);

    // Folder body.
    QPainterPath body;
    body.addRoundedRect(QRectF(1.5, 6, 17, 12), 2.5, 2.5);
    QLinearGradient gradient(0, 6, 0, 18);
    gradient.setColorAt(0, gradTop);
    gradient.setColorAt(0.5, gradMid);
    gradient.setColorAt(1, gradBot);
    painter.fillPath(body, gradient);
    painter.setPen(QPen(outline, 1.2));
    painter.drawPath(body);

    painter.end();
    return pixmap;
}

} // namespace

RepoListPanel::RepoListPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    m_title = new QLabel(I18n::translate("仓库"), this);
    QFont titleFont = m_title->font();
    titleFont.setPointSize(titleFont.pointSize() + 1);
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    layout->addWidget(m_title);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_list, 1);

    m_addButton = new QPushButton(I18n::translate("+ 添加仓库"), this);
    m_settingsButton = new QPushButton(I18n::translate("⚙ 设置"), this);
    m_aboutButton = new QPushButton(I18n::translate("ℹ 关于"), this);

    // Bottom action buttons: 50% of the main layout spacing (3px) and 80%
    // taller, for easier clicking.
    auto *actionsLayout = new QVBoxLayout;
    actionsLayout->setSpacing(3);
    for (auto *button : { static_cast<QWidget *>(m_addButton),
                          static_cast<QWidget *>(m_settingsButton),
                          static_cast<QWidget *>(m_aboutButton) }) {
        button->setMinimumHeight(
            qRound(static_cast<QPushButton *>(button)->sizeHint().height() * 1.44));
        actionsLayout->addWidget(button);
    }
    layout->addLayout(actionsLayout);

    connect(m_addButton, &QPushButton::clicked, this, &RepoListPanel::addRequested);
    connect(m_settingsButton, &QPushButton::clicked, this, &RepoListPanel::settingsRequested);
    connect(m_aboutButton, &QPushButton::clicked, this, &RepoListPanel::aboutRequested);
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this] {
        const auto items = m_list->selectedItems();
        if (!items.isEmpty())
            emit repositorySelected(items.first()->data(Qt::UserRole).toString());
    });
}

void RepoListPanel::setRepositories(const QList<svnsync::Repository> &repositories)
{
    // Programmatic rebuilds must not re-trigger repositorySelected
    // (which would loop back into promote() -> repositoryListChanged).
    m_list->blockSignals(true);
    m_list->clear();
    for (const auto &repo : repositories) {
        auto *item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole, repo.name);
        item->setData(Qt::UserRole + 1, repo.path);
        item->setData(Qt::UserRole + 2, static_cast<int>(repo.state));
        item->setSizeHint(QSize(0, 64));
        rebuildRow(item, repo);
    }
    m_list->blockSignals(false);
}

void RepoListPanel::rebuildRow(QListWidgetItem *item, const svnsync::Repository &repo)
{
    auto *row = new QWidget(m_list);
    row->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(8);

    auto *icon = new QLabel(row);
    icon->setPixmap(repoPixmap(repo.state));
    icon->setFixedSize(20, 20);
    icon->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    layout->addWidget(icon, 0, Qt::AlignVCenter);

    auto *texts = new QVBoxLayout;
    texts->setSpacing(0);
    auto *name = new QLabel(repo.name, row);
    QFont nameFont = name->font();
    nameFont.setBold(true);
    name->setFont(nameFont);
    auto *path = new QLabel(repo.path, row);
    path->setStyleSheet(QStringLiteral("color: #888; font-size: 11px;"));
    auto *badge = new QLabel(stateBadge(repo.state), row);
    badge->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(stateColor(repo.state).name()));
    // Row content must not swallow mouse events, otherwise clicks land on the
    // labels/widgets and the list never selects the item (no switch happens).
    for (auto *w : { static_cast<QWidget *>(name), static_cast<QWidget *>(path),
                     static_cast<QWidget *>(badge) })
        w->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    texts->addWidget(name);
    texts->addWidget(path);
    texts->addWidget(badge);
    row->setLayout(layout);

    layout->addLayout(texts, 1);

    m_list->setItemWidget(item, row);
}

void RepoListPanel::setSelectedName(const QString &name)
{
    m_list->blockSignals(true);
    if (QListWidgetItem *item = findItem(name))
        m_list->setCurrentItem(item);
    m_list->blockSignals(false);
}

void RepoListPanel::updateState(const QString &name, svnsync::RepoState state)
{
    if (QListWidgetItem *item = findItem(name)) {
        const svnsync::Repository repo = { name,
                                           item->data(Qt::UserRole + 1).toString(),
                                           QString(), QString(), QString(), state };
        item->setData(Qt::UserRole + 2, static_cast<int>(state));
        rebuildRow(item, repo);
    }
}

void RepoListPanel::retranslate()
{
    m_title->setText(I18n::translate("仓库"));
    m_addButton->setText(I18n::translate("+ 添加仓库"));
    m_settingsButton->setText(I18n::translate("⚙ 设置"));
    m_aboutButton->setText(I18n::translate("ℹ 关于"));

    // Rebuild the row widgets so the state badges pick up the new language.
    m_list->blockSignals(true);
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        const svnsync::Repository repo = {
            item->data(Qt::UserRole).toString(),
            item->data(Qt::UserRole + 1).toString(),
            QString(), QString(), QString(),
            static_cast<svnsync::RepoState>(item->data(Qt::UserRole + 2).toInt())
        };
        rebuildRow(item, repo);
    }
    m_list->blockSignals(false);
}

QString RepoListPanel::selectedName() const
{
    const auto items = m_list->selectedItems();
    return items.isEmpty() ? QString() : items.first()->data(Qt::UserRole).toString();
}

QListWidgetItem *RepoListPanel::findItem(const QString &name) const
{
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        if (item->data(Qt::UserRole).toString() == name)
            return item;
    }
    return nullptr;
}
