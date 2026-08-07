#include "ui/AboutDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtGlobal>

#include <svnplus/SvnClient.h>

namespace {

QString buildVersionLine()
{
    return QStringLiteral("SvnSyncDrive 0.4.0 · libsvnplus %1 · Qt %2")
        .arg(QString::fromStdString(SvnPlus::SvnClient::version().toString()),
             QString::fromLatin1(qVersion()));
}

} // namespace

AboutDialog::AboutDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("关于 SvnSyncDrive"));
    setMinimumWidth(420);

    auto *layout = new QVBoxLayout(this);

    auto *title = new QLabel(QStringLiteral("SvnSyncDrive"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 6);
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setAlignment(Qt::AlignHCenter);
    layout->addWidget(title);

    auto *subtitle = new QLabel(
        QStringLiteral("SVN 工作副本双向自动同步工具"), this);
    subtitle->setAlignment(Qt::AlignHCenter);
    subtitle->setStyleSheet(QStringLiteral("color: #666;"));
    layout->addWidget(subtitle);

    auto *version = new QLabel(buildVersionLine(), this);
    version->setAlignment(Qt::AlignHCenter);
    version->setStyleSheet(QStringLiteral("color: #666;"));
    layout->addWidget(version);

    layout->addSpacing(12);

    auto *desc = new QLabel(
        QStringLiteral(
            "SvnSyncDrive 在本地 SVN 工作副本和服务器之间自动同步：\n"
            "· 监控本地文件变化并批量提交到服务器\n"
            "· 周期检查服务器新版本并自动更新到本地\n"
            "· 冲突检测与可视化解决\n"
            "· 多仓库后台同步，互不阻塞\n"
            "· 认证凭据交由 libsvn 加密存储（Windows 凭据管理器）"),
        this);
    desc->setTextInteractionFlags(Qt::TextSelectableByMouse);
    desc->setWordWrap(true);
    desc->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(desc);

    layout->addSpacing(12);

    auto *credit = new QLabel(
        QStringLiteral("基于 Qt %1 · libsvnplus · Apache Subversion")
            .arg(QString::fromLatin1(qVersion())),
        this);
    credit->setAlignment(Qt::AlignHCenter);
    credit->setStyleSheet(QStringLiteral("color: #888;"));
    layout->addWidget(credit);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("关闭"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}
