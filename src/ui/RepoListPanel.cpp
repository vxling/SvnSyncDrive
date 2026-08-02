#include "ui/RepoListPanel.h"

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
    case svnsync::RepoState::Active: return QStringLiteral("● 同步中");
    case svnsync::RepoState::Background: return QStringLiteral("◐ 后台");
    case svnsync::RepoState::Deactive: return QStringLiteral("○ 停止监控");
    case svnsync::RepoState::AuthFailed: return QStringLiteral("✕ 认证失败");
    case svnsync::RepoState::Disconnected: return QStringLiteral("✖ 断开链接");
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

    auto *title = new QLabel(QStringLiteral("仓库"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 1);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_list, 1);

    auto *addButton = new QPushButton(QStringLiteral("+ 添加仓库"), this);
    auto *settingsButton = new QPushButton(QStringLiteral("⚙ 设置"), this);
    auto *aboutButton = new QPushButton(QStringLiteral("ℹ 关于"), this);

    // Bottom action buttons: 50% of the main layout spacing (3px) and 80%
    // taller, for easier clicking.
    auto *actionsLayout = new QVBoxLayout;
    actionsLayout->setSpacing(3);
    for (auto *button : { static_cast<QWidget *>(addButton),
                          static_cast<QWidget *>(settingsButton),
                          static_cast<QWidget *>(aboutButton) }) {
        button->setMinimumHeight(
            qRound(static_cast<QPushButton *>(button)->sizeHint().height() * 1.44));
        actionsLayout->addWidget(button);
    }
    layout->addLayout(actionsLayout);

    connect(addButton, &QPushButton::clicked, this, &RepoListPanel::addRequested);
    connect(settingsButton, &QPushButton::clicked, this, &RepoListPanel::settingsRequested);
    connect(aboutButton, &QPushButton::clicked, this, &RepoListPanel::aboutRequested);
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
        rebuildRow(item, repo);
    }
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
