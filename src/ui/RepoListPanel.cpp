#include "ui/RepoListPanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QString stateBadge(svnsync::RepoState state)
{
    switch (state) {
    case svnsync::RepoState::Active: return QStringLiteral("● 同步中");
    case svnsync::RepoState::Background: return QStringLiteral("◐ 后台");
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
    m_list->clear();
    for (const auto &repo : repositories) {
        auto *item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole, repo.name);
        item->setData(Qt::UserRole + 1, repo.path);
        item->setSizeHint(QSize(0, 64));
        rebuildRow(item, repo);
    }
}

void RepoListPanel::rebuildRow(QListWidgetItem *item, const svnsync::Repository &repo)
{
    auto *row = new QWidget(m_list);
    row->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(8);

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
    if (QListWidgetItem *item = findItem(name))
        m_list->setCurrentItem(item);
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
