#include "ui/ConflictDialog.h"

#include "core/SvnCommand.h"
#include "core/SyncEngine.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

ConflictDialog::ConflictDialog(svnsync::SyncEngine *engine,
                               const QStringList &conflicts,
                               QWidget *parent)
    : QDialog(parent)
    , m_engine(engine)
    , m_conflicts(conflicts)
{
    setWindowTitle(QStringLiteral("解决冲突"));
    setMinimumSize(540, 380);

    auto *layout = new QVBoxLayout(this);

    layout->addWidget(new QLabel(QStringLiteral("以下文件存在冲突，请选择处理方式："), this));

    m_list = new QListWidget(this);
    for (const QString &path : m_conflicts)
        m_list->addItem(path);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(m_list, 1);

    auto *choiceRow = new QHBoxLayout;
    choiceRow->addWidget(new QLabel(QStringLiteral("解决方式"), this));
    m_choice = new QComboBox(this);
    m_choice->addItem(QStringLiteral("使用我的版本（MineFull）"));
    m_choice->addItem(QStringLiteral("使用他们的版本（TheirsFull）"));
    m_choice->addItem(QStringLiteral("标记为已合并（Merged）"));
    m_choice->addItem(QStringLiteral("使用基线版本（Base）"));
    choiceRow->addWidget(m_choice, 1);
    layout->addLayout(choiceRow);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *buttons = new QHBoxLayout;
    m_resolveButton = new QPushButton(QStringLiteral("解决全部"), this);
    auto *closeButton = new QPushButton(QStringLiteral("稍后处理"), this);
    buttons->addWidget(m_resolveButton);
    buttons->addStretch(1);
    buttons->addWidget(closeButton);
    layout->addLayout(buttons);

    connect(m_resolveButton, &QPushButton::clicked, this, &ConflictDialog::resolveAll);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
}

void ConflictDialog::resolveAll()
{
    if (!m_engine || m_conflicts.isEmpty())
        return;

    m_resolveButton->setEnabled(false);
    m_failures.clear();
    m_succeeded = 0;

    // Combo index -> svnsync conflict choice used by SvnWorker::runResolve.
    int choiceCode = 2;
    switch (m_choice->currentIndex()) {
    case 0: choiceCode = 2; break; // MineFull
    case 1: choiceCode = 1; break; // TheirsFull
    case 2: choiceCode = 5; break; // Merged
    case 3: choiceCode = 0; break; // Base
    }

    m_resolving = m_conflicts.size();
    m_status->setText(QStringLiteral("正在解决冲突…"));

    for (const QString &path : m_conflicts) {
        svnsync::CommandItem item;
        item.command = svnsync::Command::Resolve;
        item.path = path;
        item.conflictChoice = choiceCode;
        m_engine->submit(item, [this](const svnsync::CommandResult &r) {
            --m_resolving;
            if (r.success)
                ++m_succeeded;
            else
                m_failures.append(r.error);
            if (m_resolving <= 0)
                finishIfDone();
        });
    }
}

void ConflictDialog::finishIfDone()
{
    m_resolveButton->setEnabled(true);
    if (m_failures.isEmpty()) {
        m_status->setText(QStringLiteral("已解决 %1 个冲突。").arg(m_succeeded));
        m_list->clear();
        accept();
    } else {
        m_status->setText(QStringLiteral("有 %1 个文件解决失败：%2")
                              .arg(m_failures.size())
                              .arg(m_failures.join(QStringLiteral("；"))));
    }
}
