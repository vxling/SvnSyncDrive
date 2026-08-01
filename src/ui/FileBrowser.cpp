#include "ui/FileBrowser.h"

#include "core/SyncEngine.h"

#include <QColor>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QString statusLetter(svnsync::StatusKind kind)
{
    using svnsync::StatusKind;
    switch (kind) {
    case StatusKind::Modified: return QStringLiteral("M");
    case StatusKind::Added: return QStringLiteral("A");
    case StatusKind::Deleted: return QStringLiteral("D");
    case StatusKind::Replaced: return QStringLiteral("R");
    case StatusKind::Conflicted: return QStringLiteral("C");
    case StatusKind::Merged: return QStringLiteral("G");
    case StatusKind::Missing: return QStringLiteral("!");
    case StatusKind::Unversioned: return QStringLiteral("?");
    case StatusKind::Ignored: return QStringLiteral("I");
    default: return QStringLiteral(" ");
    }
}

QColor statusColor(svnsync::StatusKind kind)
{
    using svnsync::StatusKind;
    switch (kind) {
    case StatusKind::Conflicted: return QColor(0xC6, 0x28, 0x28); // red
    case StatusKind::Added:
    case StatusKind::Replaced: return QColor(0x00, 0x70, 0x40);   // green
    case StatusKind::Deleted:
    case StatusKind::Missing: return QColor(0xC0, 0x50, 0x00);    // orange
    case StatusKind::Modified:
    case StatusKind::Merged: return QColor(0x00, 0x50, 0xC8);     // blue
    case StatusKind::Unversioned:
    case StatusKind::Ignored: return QColor(0x80, 0x80, 0x80);    // gray
    default: return QColor(0x00, 0x00, 0x00);                     // black
    }
}

QString formatSize(qlonglong bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 2);
}

} // namespace

FileBrowser::FileBrowser(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto *toolbar = new QWidget(this);
    auto *toolLayout = new QHBoxLayout(toolbar);
    toolLayout->setContentsMargins(0, 0, 0, 0);

    auto *upButton = new QPushButton(QStringLiteral("上一级"), toolbar);
    auto *refreshButton = new QPushButton(QStringLiteral("刷新"), toolbar);
    m_pathLabel = new QLabel(toolbar);
    m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    toolLayout->addWidget(upButton);
    toolLayout->addWidget(refreshButton);
    toolLayout->addWidget(m_pathLabel, 1);
    layout->addWidget(toolbar);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels(
        { QStringLiteral("名称"), QStringLiteral("类型"),
          QStringLiteral("大小"), QStringLiteral("修改时间"),
          QStringLiteral("状态") });
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 5; ++c)
        m_table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    layout->addWidget(m_table, 1);

    connect(upButton, &QPushButton::clicked, this, &FileBrowser::goUp);
    connect(refreshButton, &QPushButton::clicked, this, &FileBrowser::refresh);
    connect(m_table, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) {
                const QTableWidgetItem *nameItem = m_table->item(row, 0);
                if (!nameItem)
                    return;
                const QString name = nameItem->data(Qt::UserRole).toString();
                if (name == QStringLiteral("..")) {
                    goUp();
                } else {
                    const QString target = QDir::cleanPath(m_currentDir + QLatin1Char('/') + name);
                    const QFileInfo info(target);
                    if (info.isDir())
                        refreshFor(target);
                }
            });
}

FileBrowser::~FileBrowser() = default;

void FileBrowser::setEngine(svnsync::SyncEngine *engine)
{
    m_engine = engine;
}

void FileBrowser::setRepository(const QString &wcPath)
{
    m_wcRoot = QDir::cleanPath(QDir::fromNativeSeparators(wcPath));
    m_currentDir = m_wcRoot;
    refresh();
}

void FileBrowser::refresh()
{
    if (m_currentDir.isEmpty())
        return;
    refreshFor(m_currentDir);
}

void FileBrowser::refreshFor(const QString &dir)
{
    m_currentDir = QDir::cleanPath(dir);
    m_pathLabel->setText(m_currentDir.isEmpty() ? m_wcRoot : m_currentDir);

    m_table->setRowCount(0);
    emit statusTextChanged(QStringLiteral("加载中…"));

    if (!m_engine) {
        emit statusTextChanged(QStringLiteral("仓库未启用"));
        return;
    }

    svnsync::CommandItem item;
    item.command = svnsync::Command::Status;
    item.path = m_currentDir;
    const int seq = ++m_requestSeq;
    m_pendingRequest = seq;

    QPointer<FileBrowser> self(this);
    m_engine->submit(item, [self, seq, dir](const svnsync::CommandResult &r) {
        if (!self)
            return;  // browser was closed/destroyed while the status was in flight
        if (seq != self->m_pendingRequest)
            return;  // superseded by a newer request
        if (!self->m_engine)
            return;
        self->onStatusResult(dir, r);
    });
}

void FileBrowser::goUp()
{
    const QString parent = QDir::cleanPath(m_currentDir + QStringLiteral("/.."));
    if (parent != m_currentDir && !parent.isEmpty() && parent.length() >= m_wcRoot.length())
        refreshFor(parent);
}

void FileBrowser::onStatusResult(const QString &dir, const svnsync::CommandResult &result)
{
    if (!result.success) {
        emit statusTextChanged(QStringLiteral("状态查询失败: %1").arg(result.error));
        return;
    }

    QList<Entry> entries;
    const QString current = QDir::fromNativeSeparators(QDir::cleanPath(dir));
    bool haveParent = QDir::fromNativeSeparators(current) != QDir::fromNativeSeparators(m_wcRoot);

    QSet<QString> seen;
    if (haveParent) {
        Entry parentEntry;
        parentEntry.name = QStringLiteral("..");
        parentEntry.path = current;
        parentEntry.isDir = true;
        entries.append(parentEntry);
        seen.insert(QStringLiteral(".."));
    }

    for (const auto &e : result.statuses) {
        const QString path = QDir::fromNativeSeparators(e.path);
        const QString parentPath = QDir::cleanPath(path.section(QLatin1Char('/'), 0, -2));
        if (parentPath != current)
            continue;  // only direct children
        const QFileInfo info(path);
        Entry entry;
        entry.name = info.fileName();
        entry.path = path;
        entry.isDir = info.isDir();
        entry.size = info.isDir() ? 0 : info.size();
        entry.mtime = info.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        entry.kind = e.nodeStatus;
        if (entry.name.isEmpty() || entry.name == QStringLiteral("."))
            continue;
        if (seen.contains(entry.name))
            continue;
        seen.insert(entry.name);
        entries.append(entry);
    }

    populate(entries);
}

void FileBrowser::populate(const QList<Entry> &entries)
{
    QList<Entry> sorted = entries;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Entry &a, const Entry &b) {
                         if (a.isDir != b.isDir)
                             return a.isDir;  // directories first
                         return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
                     });

    static QFileIconProvider icons;
    m_table->setRowCount(sorted.size());

    for (int row = 0; row < sorted.size(); ++row) {
        const Entry &entry = sorted.at(row);

        auto *nameItem = new QTableWidgetItem(entry.name);
        nameItem->setData(Qt::UserRole, entry.name);
        const QIcon icon = entry.isDir
            ? icons.icon(QFileIconProvider::Folder)
            : icons.icon(QFileInfo(entry.path));
        nameItem->setIcon(icon);
        if (entry.isDir)
            nameItem->setToolTip(entry.path);
        m_table->setItem(row, 0, nameItem);

        auto *typeItem = new QTableWidgetItem(entry.isDir ? QStringLiteral("目录") : QString());
        m_table->setItem(row, 1, typeItem);

        auto *sizeItem = new QTableWidgetItem(entry.isDir ? QString() : formatSize(entry.size));
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_table->setItem(row, 2, sizeItem);

        auto *timeItem = new QTableWidgetItem(entry.mtime);
        m_table->setItem(row, 3, timeItem);

        const QString letter = entry.name == QStringLiteral("..")
            ? QString() : statusLetter(entry.kind);
        auto *statusItem = new QTableWidgetItem(letter);
        statusItem->setForeground(statusColor(entry.kind));
        statusItem->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(row, 4, statusItem);
    }

    const int count = sorted.size();
    emit statusTextChanged(
        count > 0 ? QStringLiteral("%1 项").arg(count) : QStringLiteral("（空目录）"));
}

int FileBrowser::itemCount() const
{
    return m_table->rowCount();
}
