#include "ui/SettingsDialog.h"

#include "core/I18n.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(I18n::translate("设置"));
    setMinimumWidth(460);

    auto *layout = new QVBoxLayout(this);

    auto *tabs = new QTabWidget(this);

    auto *generalPage = new QWidget(tabs);
    auto *generalForm = new QFormLayout(generalPage);
    m_language = new QComboBox(generalPage);
    m_language->addItem(I18n::translate("中文"), QStringLiteral("zh_CN"));
    m_language->addItem(I18n::translate("English"), QStringLiteral("en"));
    generalForm->addRow(I18n::translate("语言"), m_language);
    m_minimizeToTray = new QCheckBox(I18n::translate("关闭窗口时最小化到系统托盘"), generalPage);
    m_minimizeToTray->setToolTip(I18n::translate("开启后常驻后台同步，可随时从托盘恢复窗口"));
    m_startMinimizedToTray = new QCheckBox(I18n::translate("启动时最小化到系统托盘（不显示窗口）"), generalPage);
    m_startMinimizedToTray->setToolTip(I18n::translate("开启后启动程序不显示主窗口，直接常驻系统托盘"));
    generalForm->addRow(I18n::translate("关闭窗口时最小化"), m_minimizeToTray);
    generalForm->addRow(I18n::translate("启动时隐藏到托盘"), m_startMinimizedToTray);

    // Repository storage folder: where new working copies get created by
    // default. Existing repositories are not moved by changing this.
    auto *repoRootRow = new QHBoxLayout;
    repoRootRow->setSpacing(6);
    m_repoRoot = new QLineEdit(generalPage);
    m_repoRoot->setToolTip(
        I18n::translate("新增仓库时工作副本默认存放的目录；已有仓库不受影响"));
    auto *browseRoot = new QPushButton(I18n::translate("浏览…"), generalPage);
    repoRootRow->addWidget(m_repoRoot, 1);
    repoRootRow->addWidget(browseRoot);
    generalForm->addRow(I18n::translate("仓库存储目录"), repoRootRow);
    connect(browseRoot, &QPushButton::clicked, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(
            this, I18n::translate("选择仓库存储目录"),
            m_repoRoot->text().trimmed().isEmpty()
                ? svnsync::defaultRepoRoot()
                : m_repoRoot->text());
        if (!dir.isEmpty())
            m_repoRoot->setText(QDir::toNativeSeparators(dir));
    });

    m_quickAccess = new QCheckBox(
        I18n::translate("在文件管理器中添加快捷入口（指向仓库存储目录）"),
        generalPage);
    m_quickAccess->setToolTip(
        I18n::translate("开启后在文件管理器侧栏添加指向存储目录的快捷方式并固定到快速访问；关闭时移除。仅 Windows / Linux 支持"));
    generalForm->addRow(I18n::translate("文件管理器快捷入口"), m_quickAccess);

    tabs->addTab(generalPage, I18n::translate("常规"));

    auto *syncPage = new QWidget(tabs);
    auto *syncForm = new QFormLayout(syncPage);
    m_pollSeconds = new QSpinBox(syncPage);
    m_pollSeconds->setRange(10, 24 * 3600);
    m_pollSeconds->setSuffix(I18n::translate(" 秒"));
    m_pollSeconds->setToolTip(I18n::translate("多久检查一次服务器是否有新版本"));
    m_fullSyncMinutes = new QSpinBox(syncPage);
    m_fullSyncMinutes->setRange(1, 24 * 60);
    m_fullSyncMinutes->setSuffix(I18n::translate(" 分钟"));
    m_fullSyncMinutes->setToolTip(I18n::translate("周期性地把本地所有更改提交到服务器"));
    m_autoAdd = new QCheckBox(I18n::translate("自动添加未纳入版本控制的新文件"), syncPage);
    m_autoAdd->setToolTip(I18n::translate("关闭后只提交已有版本控制的修改"));
    syncForm->addRow(I18n::translate("向下同步检查周期"), m_pollSeconds);
    syncForm->addRow(I18n::translate("全量提交周期"), m_fullSyncMinutes);
    syncForm->addRow(I18n::translate("自动添加新文件"), m_autoAdd);
    m_pollSeconds->setEnabled(false);
    m_fullSyncMinutes->setEnabled(false);
    m_autoAdd->setEnabled(false);
    tabs->addTab(syncPage, I18n::translate("同步"));

    auto *networkPage = new QWidget(tabs);
    auto *networkForm = new QFormLayout(networkPage);
    m_trustCert = new QCheckBox(I18n::translate("信任自签名 / 未知证书"), networkPage);
    m_disconnectThreshold = new QSpinBox(networkPage);
    m_disconnectThreshold->setRange(1, 100);
    m_disconnectThreshold->setSuffix(I18n::translate(" 次"));
    m_disconnectThreshold->setToolTip(I18n::translate("连续多少次网络访问失败后，将仓库标记为“连接断开”。任意一次成功访问都会清零重新计数"));
    m_networkTimeoutSeconds = new QSpinBox(networkPage);
    m_networkTimeoutSeconds->setRange(5, 600);
    m_networkTimeoutSeconds->setSuffix(I18n::translate(" 秒"));
    m_networkTimeoutSeconds->setToolTip(I18n::translate("单次网络操作（如 HTTPS 握手）最多阻塞的秒数；超过后按网络错误处理并继续重试，避免卡住同步"));
    m_maxTransferMinutes = new QSpinBox(networkPage);
    m_maxTransferMinutes->setRange(2, 30);
    m_maxTransferMinutes->setSuffix(I18n::translate(" 分钟"));
    m_maxTransferMinutes->setToolTip(I18n::translate("一次提交/更新/检出（含单个大文件传输）最多持续的时间，超过后即使仍在传输也会中止，防止大文件长时间占用同步。默认 10 分钟，最大 30 分钟"));
    m_maxFileSizeMb = new QSpinBox(networkPage);
    m_maxFileSizeMb->setRange(10, 1024);
    m_maxFileSizeMb->setSingleStep(10);
    m_maxFileSizeMb->setSuffix(I18n::translate(" MB"));
    m_maxFileSizeMb->setToolTip(I18n::translate("单个文件大小超过该门限时不执行提交，并在日志中记录“超大文件”。默认 100 MB，可设置 10 MB ~ 1 GB"));
    networkForm->addRow(I18n::translate("信任自签名证书"), m_trustCert);
    networkForm->addRow(I18n::translate("断网判定阈值"), m_disconnectThreshold);
    networkForm->addRow(I18n::translate("网络超时"), m_networkTimeoutSeconds);
    networkForm->addRow(I18n::translate("单次传输最长时长"), m_maxTransferMinutes);
    networkForm->addRow(I18n::translate("单文件提交大小门限"), m_maxFileSizeMb);
    tabs->addTab(networkPage, I18n::translate("网络"));

    auto *conflictPage = new QWidget(tabs);
    auto *conflictForm = new QFormLayout(conflictPage);
    m_autoResolve = new QCheckBox(I18n::translate("自动解决冲突（不再弹窗提示）"), conflictPage);
    m_autoResolve->setToolTip(I18n::translate("发现冲突后自动按下方默认方式解决，不再弹出确认对话框；树冲突一律按“保留当前工作副本状态”处理"));
    m_conflictResolution = new QComboBox(conflictPage);
    m_conflictResolution->addItem(I18n::translate("使用我的版本（MineFull）"));
    m_conflictResolution->addItem(I18n::translate("使用他们的版本（TheirsFull）"));
    m_conflictResolution->addItem(I18n::translate("标记为已合并（Merged）"));
    m_conflictResolution->addItem(I18n::translate("使用基线版本（Base）"));
    m_conflictResolution->setToolTip(I18n::translate("自动解决冲突时对文本冲突使用的默认处理方式（树冲突不适用，一律保留当前工作副本状态）"));
    conflictForm->addRow(I18n::translate("自动解决冲突"), m_autoResolve);
    conflictForm->addRow(I18n::translate("默认处理方式"), m_conflictResolution);
    m_conflictResolution->setEnabled(false);
    connect(m_autoResolve, &QCheckBox::toggled,
            m_conflictResolution, &QWidget::setEnabled);
    tabs->addTab(conflictPage, I18n::translate("冲突处理"));

    auto *logPage = new QWidget(tabs);
    auto *logForm = new QFormLayout(logPage);
    m_maxLogsPerRepo = new QSpinBox(logPage);
    m_maxLogsPerRepo->setRange(100, 10000);
    m_maxLogsPerRepo->setSingleStep(100);
    m_maxLogsPerRepo->setSuffix(I18n::translate(" 条"));
    m_maxLogsPerRepo->setToolTip(I18n::translate("每个仓库在本地数据库中最多保留的日志条数（上限 10000 条），超出后自动丢弃最早的"));
    logForm->addRow(I18n::translate("每个仓库保留日志条数"), m_maxLogsPerRepo);
    tabs->addTab(logPage, I18n::translate("日志"));

    layout->addWidget(tabs);

    auto *hint = new QLabel(
        I18n::translate("更改将应用到所有正在同步的仓库。"), this);
    hint->setStyleSheet(QStringLiteral("color: #888;"));
    layout->addWidget(hint);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(I18n::translate("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(I18n::translate("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void SettingsDialog::setConfig(const svnsync::GlobalConfig &config)
{
    const int langIdx = m_language->findData(config.language);
    m_language->setCurrentIndex(langIdx >= 0 ? langIdx : 0);
    m_pollSeconds->setValue(config.pollIntervalMs / 1000);
    m_fullSyncMinutes->setValue(config.fullSyncIntervalMs / 60000);
    m_autoAdd->setChecked(config.autoAddUnversioned);
    m_trustCert->setChecked(config.trustServerCertificate);
    m_minimizeToTray->setChecked(config.minimizeToTray);
    m_startMinimizedToTray->setChecked(config.startMinimizedToTray);
    m_maxLogsPerRepo->setValue(config.maxLogsPerRepo);
    m_disconnectThreshold->setValue(config.disconnectThreshold);
    m_networkTimeoutSeconds->setValue(config.networkTimeoutSec);
    m_maxTransferMinutes->setValue(qBound(2, config.maxTransferSec / 60, 30));
    m_maxFileSizeMb->setValue(qBound(10, config.maxFileSizeMb, 1024));
    m_autoResolve->setChecked(config.autoResolveConflicts);
    switch (config.conflictResolution) {
    case 0: m_conflictResolution->setCurrentIndex(3); break; // Base
    case 1: m_conflictResolution->setCurrentIndex(1); break; // TheirsFull
    case 2: m_conflictResolution->setCurrentIndex(0); break; // MineFull
    default: m_conflictResolution->setCurrentIndex(2); break; // Merged
    }
    m_conflictResolution->setEnabled(config.autoResolveConflicts);
    m_repoRoot->setText(QDir::toNativeSeparators(config.repoRoot));
    m_quickAccess->setChecked(config.quickAccessEnabled);
}

svnsync::GlobalConfig SettingsDialog::config() const
{
    svnsync::GlobalConfig config;
    config.language = m_language->currentData().toString();
    config.pollIntervalMs = m_pollSeconds->value() * 1000;
    config.fullSyncIntervalMs = m_fullSyncMinutes->value() * 60000;
    config.autoAddUnversioned = m_autoAdd->isChecked();
    config.trustServerCertificate = m_trustCert->isChecked();
    config.minimizeToTray = m_minimizeToTray->isChecked();
    config.startMinimizedToTray = m_startMinimizedToTray->isChecked();
    config.maxLogsPerRepo = m_maxLogsPerRepo->value();
    config.disconnectThreshold = m_disconnectThreshold->value();
    config.networkTimeoutSec = m_networkTimeoutSeconds->value();
    config.maxTransferSec = m_maxTransferMinutes->value() * 60;
    config.maxFileSizeMb = m_maxFileSizeMb->value();
    config.autoResolveConflicts = m_autoResolve->isChecked();
    switch (m_conflictResolution->currentIndex()) {
    case 0: config.conflictResolution = 2; break; // MineFull
    case 1: config.conflictResolution = 1; break; // TheirsFull
    case 3: config.conflictResolution = 0; break; // Base
    default: config.conflictResolution = 5; break; // Merged
    }
    const QString root = m_repoRoot->text().trimmed();
    config.repoRoot = QDir::fromNativeSeparators(
        root.isEmpty() ? svnsync::defaultRepoRoot() : root);
    config.quickAccessEnabled = m_quickAccess->isChecked();
    return config;
}
