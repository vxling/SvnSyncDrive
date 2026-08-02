#include "ui/FileBrowser.h"

#include "core/SyncEngine.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QUrl>
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
    case StatusKind::Missing:
    case StatusKind::Incomplete: return QStringLiteral("!");
    case StatusKind::Unversioned: return QStringLiteral("?");
    case StatusKind::Ignored: return QStringLiteral("I");
    case StatusKind::Normal: return QStringLiteral("✓");  // green check for clean files
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
    case StatusKind::Missing:
    case StatusKind::Incomplete: return QColor(0xC0, 0x50, 0x00);    // orange
    case StatusKind::Modified:
    case StatusKind::Merged: return QColor(0x00, 0x50, 0xC8);     // blue
    case StatusKind::Unversioned:
    case StatusKind::Ignored: return QColor(0x80, 0x80, 0x80);    // gray
    case StatusKind::Normal: return QColor(0x00, 0x9A, 0x3E);      // green check
    default: return QColor(0x00, 0x00, 0x00);                     // black
    }
}

/** Reverse of statusLetter(): maps the status column glyph back to a kind. */
svnsync::StatusKind statusKindForLetter(const QString &letter)
{
    using svnsync::StatusKind;
    if (letter == QStringLiteral("M")) return StatusKind::Modified;
    if (letter == QStringLiteral("A")) return StatusKind::Added;
    if (letter == QStringLiteral("D")) return StatusKind::Deleted;
    if (letter == QStringLiteral("R")) return StatusKind::Replaced;
    if (letter == QStringLiteral("C")) return StatusKind::Conflicted;
    if (letter == QStringLiteral("G")) return StatusKind::Merged;
    if (letter == QStringLiteral("!")) return StatusKind::Incomplete;
    if (letter == QStringLiteral("?")) return StatusKind::Unversioned;
    if (letter == QStringLiteral("I")) return StatusKind::Ignored;
    if (letter == QStringLiteral("✓")) return StatusKind::Normal;
    return StatusKind::Normal;
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

/**
 * Renders rows with a full-row hover/selection highlight (instead of the
 * per-cell highlight of the native style) using light backgrounds, with no
 * row separators or grid lines.
 */
class RowHoverDelegate : public QStyledItemDelegate
{
public:
    RowHoverDelegate(const int *hoverRow, QObject *parent)
        : QStyledItemDelegate(parent), m_hoverRow(hoverRow)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        const bool selected = opt.state & QStyle::State_Selected;
        const bool hover = m_hoverRow && index.row() == *m_hoverRow;

        QColor bg;
        if (hover)
            bg = QColor(0xEA, 0xF2, 0xFB);   // very light blue
        else if (selected)
            bg = QColor(0xD8, 0xE9, 0xFA);   // light blue
        else
            bg = opt.palette.base().color();
        painter->fillRect(opt.rect, bg);

        QStyleOptionViewItem f = opt;
        f.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver);
        f.state |= QStyle::State_Enabled;
        // Keep per-cell foreground colors (e.g. the green status glyph) even
        // while the row is selected; only plain cells fall back to
        // near-black text on selection.
        const QBrush itemBrush = index.data(Qt::ForegroundRole).value<QBrush>();
        if (itemBrush.style() != Qt::NoBrush)
            f.palette.setBrush(QPalette::Text, itemBrush);
        else if (selected)
            f.palette.setColor(QPalette::Text, QColor(0x10, 0x10, 0x10));

        QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();

        if ((opt.features & QStyleOptionViewItem::HasDecoration) && !opt.icon.isNull()) {
            const QRect iconRect =
                style->subElementRect(QStyle::SE_ItemViewItemDecoration, &f, opt.widget);
            opt.icon.paint(painter, iconRect, Qt::AlignCenter, QIcon::Normal, QIcon::Off);
        }

        const QRect textRect =
            style->subElementRect(QStyle::SE_ItemViewItemText, &f, opt.widget);
        style->drawItemText(painter, textRect, opt.displayAlignment, f.palette,
                            opt.widget != nullptr && opt.widget->isEnabled(),
                            opt.text, QPalette::Text);
    }

    // Give the middle columns (type/size/mtime) breathing room so the rows
    // are not crammed together. The name (0) and status (4) columns keep
    // their tight sizing.
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        const int column = index.column();
        if (column >= 1 && column <= 3)
            size.rwidth() += 20;
        return size;
    }

private:
    const int *m_hoverRow = nullptr;
};

} // namespace

FileBrowser::FileBrowser(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    // Path bar on top, action buttons below it.
    auto *toolbar = new QWidget(this);
    auto *toolLayout = new QVBoxLayout(toolbar);
    toolLayout->setContentsMargins(0, 0, 0, 0);
    toolLayout->setSpacing(4);

    m_pathLabel = new QLabel(toolbar);
    m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    toolLayout->addWidget(m_pathLabel);

    auto *buttonsRow = new QHBoxLayout;
    buttonsRow->setSpacing(6);

    auto *syncButton = new QPushButton(QStringLiteral("🔁 立即同步"), toolbar);
    m_toggleButton = new QPushButton(QStringLiteral("▶ 启用同步"), toolbar);
    auto *conflictButton = new QPushButton(QStringLiteral("🛠 解决冲突"), toolbar);
    m_refreshButton = new QPushButton(QStringLiteral("🔄 刷新"), toolbar);

    buttonsRow->addWidget(syncButton);
    buttonsRow->addWidget(m_toggleButton);
    buttonsRow->addWidget(conflictButton);
    buttonsRow->addWidget(m_refreshButton);
    buttonsRow->addStretch(1);
    toolLayout->addLayout(buttonsRow);

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
    m_table->setShowGrid(false);  // no grid lines; delegate draws no separators
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 5; ++c)
        m_table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    layout->addWidget(m_table, 1);

    m_table->setItemDelegate(new RowHoverDelegate(&m_hoverRow, m_table));
    m_table->setMouseTracking(true);
    m_table->viewport()->setMouseTracking(true);
    m_table->viewport()->installEventFilter(this);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(syncButton, &QPushButton::clicked, this, &FileBrowser::syncRequested);
    connect(m_toggleButton, &QPushButton::clicked, this, &FileBrowser::toggleStateRequested);
    connect(conflictButton, &QPushButton::clicked, this, &FileBrowser::conflictScanRequested);
    connect(m_refreshButton, &QPushButton::clicked, this, &FileBrowser::refresh);
    connect(m_table, &QTableWidget::customContextMenuRequested, this,
            &FileBrowser::showContextMenu);
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

void FileBrowser::setToggleState(svnsync::RepoState state)
{
    if (!m_toggleButton)
        return;
    m_toggleButton->setText(state == svnsync::RepoState::Deactive
        ? QStringLiteral("▶ 启用同步")
        : QStringLiteral("⏸ 停用同步"));
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

    // The local listing is fast and needs no SVN round-trip: show it at once
    // and let the async status refresh only fill in the state column.
    populateLocal();

    // svn cannot report status for a path inside an unversioned directory
    // ("node was not found"), so skip the round-trip and show '?' for all.
    if (underUnversioned(m_currentDir)) {
        markAllRowsUnversioned();
        return;
    }

    if (!m_engine) {
        emit statusTextChanged(QStringLiteral("仓库未启用"));
        return;
    }

    svnsync::CommandItem item;
    item.command = svnsync::Command::Status;
    item.path = m_currentDir;
    item.statusDepth = svnsync::StatusDepth::Immediates;  // only direct children, avoids full-tree recursion
    const int seq = ++m_requestSeq;
    m_pendingRequest = seq;

    QPointer<FileBrowser> self(this);
    m_engine->submit(item, [self, seq, dir](const svnsync::CommandResult &r) {
        if (!self)
            return;  // browser was closed/destroyed while the status was in flight
        if (seq != self->m_pendingRequest)
            return;  // superseded by a newer request
        self->applyStatus(dir, r);
    });
}

void FileBrowser::goUp()
{
    const QString parent = QDir::cleanPath(m_currentDir + QStringLiteral("/.."));
    if (parent != m_currentDir && !parent.isEmpty() && parent.length() >= m_wcRoot.length())
        refreshFor(parent);
}

void FileBrowser::showContextMenu(const QPoint &pos)
{
    const QTableWidgetItem *nameItem = m_table->itemAt(pos);
    if (!nameItem)
        return;
    const QTableWidgetItem *firstName = m_table->item(nameItem->row(), 0);
    if (!firstName)
        return;
    const QString name = firstName->data(Qt::UserRole).toString();
    if (name == QStringLiteral(".."))
        return;

    const QString fullPath =
        QDir::cleanPath(m_currentDir + QLatin1Char('/') + name);
    const QFileInfo info(fullPath);
    const auto *statusItem = m_table->item(nameItem->row(), 4);
    const svnsync::StatusKind kind = statusItem
        ? statusKindForLetter(statusItem->text())
        : svnsync::StatusKind::Normal;

    QMenu menu(this);
    QAction *openAction = nullptr;
    QAction *openFolderAction = nullptr;
    QAction *commitAction = nullptr;
    QAction *updateAction = nullptr;
    QAction *revertAction = nullptr;
    QAction *addAction = nullptr;
    QAction *deleteAction = nullptr;
    QAction *renameAction = nullptr;
    QAction *copyPathAction = nullptr;

    if (info.isDir()) {
        openAction = menu.addAction(QStringLiteral("打开目录"));
    } else {
        openAction = menu.addAction(QStringLiteral("打开"));
    }
    menu.addSeparator();

    const bool hasEngine = m_engine != nullptr;
    const bool isUnversioned = kind == svnsync::StatusKind::Unversioned;

    if (isUnversioned) {
        addAction = menu.addAction(QStringLiteral("添加到版本库 (Add)"));
    } else {
        commitAction = menu.addAction(QStringLiteral("提交… (Commit)"));
        updateAction = menu.addAction(QStringLiteral("更新 (Update)"));
        revertAction = menu.addAction(QStringLiteral("还原 (Revert)"));
    }
    deleteAction = menu.addAction(QStringLiteral("从版本库删除 (Delete)"));
    renameAction = menu.addAction(QStringLiteral("重命名… (Move)"));
    menu.addSeparator();
    copyPathAction = menu.addAction(QStringLiteral("复制完整路径"));

    for (QAction *a : { commitAction, updateAction, revertAction, addAction,
                        deleteAction, renameAction }) {
        if (a)
            a->setEnabled(hasEngine);
    }

    QAction *chosen = menu.exec(m_table->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;

    const QString dirPart = info.dir().absolutePath();
    const QString fullBase = QDir::fromNativeSeparators(fullPath);

    if (chosen == openAction) {
        if (info.isDir())
            refreshFor(fullBase);
        else
            QDesktopServices::openUrl(QUrl::fromLocalFile(fullBase));
        return;
    }
    if (chosen == copyPathAction) {
        QApplication::clipboard()->setText(QDir::toNativeSeparators(fullBase));
        return;
    }
    if (chosen == addAction) {
        submitAction(svnsync::Command::Add, fullBase);
        return;
    }
    if (chosen == commitAction) {
        bool ok = false;
        const QString message = QInputDialog::getMultiLineText(
            this, QStringLiteral("提交"),
            QStringLiteral("提交日志：\n%1").arg(fullPath),
            QString(), &ok);
        if (!ok || message.trimmed().isEmpty())
            return;
        submitAction(svnsync::Command::Commit, fullBase, message.trimmed());
        return;
    }
    if (chosen == updateAction) {
        submitAction(svnsync::Command::Update, fullBase);
        return;
    }
    if (chosen == revertAction) {
        const auto ret = QMessageBox::question(
            this, QStringLiteral("还原"),
            QStringLiteral("确定要还原以下路径的全部本地修改？\n\n%1")
                .arg(fullPath),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes)
            return;
        submitAction(svnsync::Command::Revert, fullBase);
        return;
    }
    if (chosen == deleteAction) {
        const auto ret = QMessageBox::question(
            this, QStringLiteral("删除"),
            QStringLiteral("确定要从版本库删除以下路径（保留本地文件）？\n\n%1")
                .arg(fullPath),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes)
            return;
        submitAction(svnsync::Command::Delete, fullBase);
        return;
    }
    if (chosen == renameAction) {
        bool ok = false;
        const QString newName = QInputDialog::getText(
            this, QStringLiteral("重命名"), QStringLiteral("新名称："),
            QLineEdit::Normal, name, &ok);
        if (!ok || newName.trimmed().isEmpty() || newName.trimmed() == name)
            return;
        const QString toPath =
            QDir::cleanPath(dirPart + QLatin1Char('/') + newName.trimmed());
        submitAction(svnsync::Command::Move, toPath, QString(), fullBase);
        return;
    }
}

void FileBrowser::submitAction(svnsync::Command command,
                               const QString &path,
                               const QString &message,
                               const QString &toPath)
{
    if (!m_engine)
        return;
    svnsync::CommandItem item;
    item.command = command;
    item.path = path;
    item.message = message;
    item.fromPath = toPath;

    QPointer<FileBrowser> self(this);
    const QString dirToRefresh = m_currentDir;
    m_engine->submit(item, [self, dirToRefresh](const svnsync::CommandResult &r) {
        if (!self)
            return;
        if (r.success) {
            self->emit statusTextChanged(QStringLiteral("操作成功"));
            if (self->m_currentDir == dirToRefresh)
                self->refresh();
        } else {
            self->emit statusTextChanged(QStringLiteral("操作失败"));
            QMessageBox::warning(
                self, QStringLiteral("SVN 操作失败"), r.error);
        }
    });
}

bool FileBrowser::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_table->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            const auto *me = static_cast<const QMouseEvent *>(event);
            const QModelIndex idx = m_table->indexAt(me->pos());
            const int row = idx.isValid() ? idx.row() : -1;
            if (row != m_hoverRow) {
                m_hoverRow = row;
                m_table->viewport()->update();
            }
        } else if (event->type() == QEvent::Leave) {
            if (m_hoverRow != -1) {
                m_hoverRow = -1;
                m_table->viewport()->update();
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FileBrowser::populateLocal()
{
    QList<Entry> entries;
    const QString current = m_currentDir;
    const bool haveParent = current != m_wcRoot;

    if (haveParent) {
        Entry parentEntry;
        parentEntry.name = QStringLiteral("..");
        parentEntry.path = current;
        parentEntry.isDir = true;
        entries.append(parentEntry);
    }

    const QFileInfoList infos = QDir(current).entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QFileInfo &info : infos) {
        const QString name = info.fileName();
        if (name == QStringLiteral(".svn"))
            continue;
        Entry entry;
        entry.name = name;
        entry.path = QDir::fromNativeSeparators(info.absoluteFilePath());
        entry.isDir = info.isDir();
        entry.size = info.isDir() ? 0 : info.size();
        entry.mtime = info.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        entry.kind = svnsync::StatusKind::Normal;  // default until svn status refreshes it
        entries.append(entry);
    }

    populate(entries);
}

void FileBrowser::applyStatus(const QString &dir, const svnsync::CommandResult &result)
{
    if (QDir::fromNativeSeparators(QDir::cleanPath(dir))
            != QDir::fromNativeSeparators(m_currentDir))
        return;  // user already navigated elsewhere
    const QString current = QDir::fromNativeSeparators(QDir::cleanPath(dir));

    if (!result.success) {
        // svn cannot report status for a path with no version-control
        // relationship at all - e.g. anything inside an unversioned directory
        // ("The node '...' was not found"). That itself means the current
        // directory is unknown to svn, so mark everything '?'.
        const QString &err = result.error;
        if (err.contains(QStringLiteral("was not found"))
            || err.contains(QStringLiteral("not under version control"))) {
            m_unversioned.insert(current);
            markAllRowsUnversioned();
        }
        return;
    }

    // Keep the "known unversioned directories" set in sync with reality so
    // refreshFor() can short-circuit when we navigate below one of them.
    pruneUnversioned(current, result);
    for (const auto &e : result.statuses)
        if (e.nodeStatus == svnsync::StatusKind::Unversioned)
            m_unversioned.insert(QDir::fromNativeSeparators(e.path));

    // A directory that itself is unversioned (?) is reported as one item and
    // its children are never scanned, so those children must not keep the
    // default "clean" check mark - mark the whole directory unversioned.
    bool dirUnversioned = false;
    for (const auto &e : result.statuses) {
        if (e.nodeStatus != svnsync::StatusKind::Unversioned)
            continue;
        const QString p = QDir::fromNativeSeparators(e.path);
        if (QString::compare(current, p, Qt::CaseInsensitive) == 0
            || current.startsWith(p + QLatin1Char('/'), Qt::CaseInsensitive)) {
            dirUnversioned = true;
            break;
        }
    }
    if (dirUnversioned) {
        markAllRowsUnversioned();
        return;
    }

    for (const auto &e : result.statuses) {
        const QString path = QDir::fromNativeSeparators(e.path);
        const QString parentPath = QDir::cleanPath(path.section(QLatin1Char('/'), 0, -2));
        if (parentPath != current)
            continue;  // only direct children
        const QString name = path.section(QLatin1Char('/'), -1);
        for (int row = 0; row < m_table->rowCount(); ++row) {
            const QTableWidgetItem *nameItem = m_table->item(row, 0);
            if (!nameItem || nameItem->data(Qt::UserRole).toString() != name)
                continue;
            const QString letter = statusLetter(e.nodeStatus);
            auto *statusItem = m_table->item(row, 4);
            statusItem->setText(letter);
            statusItem->setForeground(statusColor(e.nodeStatus));
            break;
        }
    }
}

bool FileBrowser::underUnversioned(const QString &dir) const
{
    const QString d = QDir::fromNativeSeparators(QDir::cleanPath(dir));
    for (const QString &u : m_unversioned)
        if (QString::compare(d, u, Qt::CaseInsensitive) == 0
            || d.startsWith(u + QLatin1Char('/'), Qt::CaseInsensitive))
            return true;
    return false;
}

void FileBrowser::markAllRowsUnversioned()
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QTableWidgetItem *nameItem = m_table->item(row, 0);
        if (!nameItem
            || nameItem->data(Qt::UserRole).toString() == QStringLiteral(".."))
            continue;
        auto *statusItem = m_table->item(row, 4);
        if (!statusItem)
            continue;
        statusItem->setText(statusLetter(svnsync::StatusKind::Unversioned));
        statusItem->setForeground(statusColor(svnsync::StatusKind::Unversioned));
    }
}

void FileBrowser::pruneUnversioned(const QString &current,
                                   const svnsync::CommandResult &result)
{
    if (m_unversioned.isEmpty())
        return;

    // A "known unversioned" path only stays valid while svn still reports it
    // as unversioned at its own level (current dir itself or a direct child).
    // Drop it - and any deeper entries below it - once it stops being so.
    QSet<QString> stillUnversioned;
    for (const auto &e : result.statuses)
        if (e.nodeStatus == svnsync::StatusKind::Unversioned)
            stillUnversioned.insert(QDir::fromNativeSeparators(e.path));

    QStringList dropRoots;
    for (const QString &u : m_unversioned) {
        const bool isCurrent =
            QString::compare(u, current, Qt::CaseInsensitive) == 0;
        const bool directChild =
            QDir::cleanPath(u.section(QLatin1Char('/'), 0, -2)) == current;
        if ((isCurrent || directChild) && !stillUnversioned.contains(u))
            dropRoots << u;
    }
    if (dropRoots.isEmpty())
        return;

    QSet<QString> next;
    for (const QString &u : m_unversioned) {
        bool drop = false;
        for (const QString &r : dropRoots)
            if (QString::compare(u, r, Qt::CaseInsensitive) == 0
                || u.startsWith(r + QLatin1Char('/'), Qt::CaseInsensitive)) {
                drop = true;
                break;
            }
        if (!drop)
            next.insert(u);
    }
    m_unversioned = next;
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
        typeItem->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(row, 1, typeItem);

        auto *sizeItem = new QTableWidgetItem(entry.isDir ? QString() : formatSize(entry.size));
        sizeItem->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(row, 2, sizeItem);

        auto *timeItem = new QTableWidgetItem(entry.mtime);
        timeItem->setTextAlignment(Qt::AlignCenter);
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
