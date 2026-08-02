#include "ui/SettingsDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("设置"));
    setMinimumWidth(420);

    auto *layout = new QVBoxLayout(this);

    auto *form = new QFormLayout;
    m_pollSeconds = new QSpinBox(this);
    m_pollSeconds->setRange(10, 24 * 3600);
    m_pollSeconds->setSuffix(QStringLiteral(" 秒"));
    m_pollSeconds->setToolTip(QStringLiteral("多久检查一次服务器是否有新版本"));
    m_fullSyncMinutes = new QSpinBox(this);
    m_fullSyncMinutes->setRange(1, 24 * 60);
    m_fullSyncMinutes->setSuffix(QStringLiteral(" 分钟"));
    m_fullSyncMinutes->setToolTip(QStringLiteral("周期性地把本地所有更改提交到服务器"));

    form->addRow(QStringLiteral("向下同步检查周期"), m_pollSeconds);
    form->addRow(QStringLiteral("全量提交周期"), m_fullSyncMinutes);

    m_maxLogsPerRepo = new QSpinBox(this);
    m_maxLogsPerRepo->setRange(100, 10000);
    m_maxLogsPerRepo->setSingleStep(100);
    m_maxLogsPerRepo->setSuffix(QStringLiteral(" 条"));
    m_maxLogsPerRepo->setToolTip(QStringLiteral("每个仓库在本地数据库中最多保留的日志条数（上限 10000 条），超出后自动丢弃最早的"));
    form->addRow(QStringLiteral("每个仓库保留日志条数"), m_maxLogsPerRepo);

    m_disconnectThreshold = new QSpinBox(this);
    m_disconnectThreshold->setRange(1, 100);
    m_disconnectThreshold->setSuffix(QStringLiteral(" 次"));
    m_disconnectThreshold->setToolTip(QStringLiteral("连续多少次网络访问失败后，将仓库标记为“连接断开”。任意一次成功访问都会清零重新计数"));
    form->addRow(QStringLiteral("断网判定阈值"), m_disconnectThreshold);

    m_networkTimeoutSeconds = new QSpinBox(this);
    m_networkTimeoutSeconds->setRange(5, 600);
    m_networkTimeoutSeconds->setSuffix(QStringLiteral(" 秒"));
    m_networkTimeoutSeconds->setToolTip(QStringLiteral("单次网络操作（如 HTTPS 握手）最多阻塞的秒数；超过后按网络错误处理并继续重试，避免卡住同步"));
    form->addRow(QStringLiteral("网络超时"), m_networkTimeoutSeconds);

    m_autoAdd = new QCheckBox(QStringLiteral("自动添加未纳入版本控制的新文件"), this);
    m_autoAdd->setToolTip(QStringLiteral("关闭后只提交已有版本控制的修改"));
    m_trustCert = new QCheckBox(QStringLiteral("信任自签名 / 未知证书"), this);
    m_minimizeToTray = new QCheckBox(QStringLiteral("关闭窗口时最小化到系统托盘"), this);
    m_minimizeToTray->setToolTip(QStringLiteral("开启后常驻后台同步，可随时从托盘恢复窗口"));
    m_startMinimizedToTray = new QCheckBox(QStringLiteral("启动时最小化到系统托盘（不显示窗口）"), this);
    m_startMinimizedToTray->setToolTip(QStringLiteral("开启后启动程序不显示主窗口，直接常驻系统托盘"));

    layout->addLayout(form);
    layout->addWidget(m_autoAdd);
    layout->addWidget(m_trustCert);
    layout->addWidget(m_minimizeToTray);
    layout->addWidget(m_startMinimizedToTray);

    auto *hint = new QLabel(
        QStringLiteral("更改将应用到所有正在同步的仓库。"), this);
    hint->setStyleSheet(QStringLiteral("color: #888;"));
    layout->addWidget(hint);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void SettingsDialog::setConfig(const svnsync::GlobalConfig &config)
{
    m_pollSeconds->setValue(config.pollIntervalMs / 1000);
    m_fullSyncMinutes->setValue(config.fullSyncIntervalMs / 60000);
    m_autoAdd->setChecked(config.autoAddUnversioned);
    m_trustCert->setChecked(config.trustServerCertificate);
    m_minimizeToTray->setChecked(config.minimizeToTray);
    m_startMinimizedToTray->setChecked(config.startMinimizedToTray);
    m_maxLogsPerRepo->setValue(config.maxLogsPerRepo);
    m_disconnectThreshold->setValue(config.disconnectThreshold);
    m_networkTimeoutSeconds->setValue(config.networkTimeoutSec);
}

svnsync::GlobalConfig SettingsDialog::config() const
{
    svnsync::GlobalConfig config;
    config.pollIntervalMs = m_pollSeconds->value() * 1000;
    config.fullSyncIntervalMs = m_fullSyncMinutes->value() * 60000;
    config.autoAddUnversioned = m_autoAdd->isChecked();
    config.trustServerCertificate = m_trustCert->isChecked();
    config.minimizeToTray = m_minimizeToTray->isChecked();
    config.startMinimizedToTray = m_startMinimizedToTray->isChecked();
    config.maxLogsPerRepo = m_maxLogsPerRepo->value();
    config.disconnectThreshold = m_disconnectThreshold->value();
    config.networkTimeoutSec = m_networkTimeoutSeconds->value();
    return config;
}
