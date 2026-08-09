#pragma once

#include "core/GlobalConfig.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

/**
 * Global settings dialog: sync intervals and sync behaviour, shared by
 * every repository. Changes are applied to all running engines on accept.
 */
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);

    void setConfig(const svnsync::GlobalConfig &config);
    svnsync::GlobalConfig config() const;

private:
    QComboBox *m_language = nullptr;
    QSpinBox *m_pollSeconds = nullptr;
    QSpinBox *m_fullSyncMinutes = nullptr;
    QCheckBox *m_autoAdd = nullptr;
    QCheckBox *m_trustCert = nullptr;
    QCheckBox *m_minimizeToTray = nullptr;
    QCheckBox *m_startMinimizedToTray = nullptr;
    QSpinBox *m_maxLogsPerRepo = nullptr;
    QSpinBox *m_disconnectThreshold = nullptr;
    QSpinBox *m_networkTimeoutSeconds = nullptr;
    QCheckBox *m_autoResolve = nullptr;
    QComboBox *m_conflictResolution = nullptr;
    QLineEdit *m_repoRoot = nullptr;
    QCheckBox *m_quickAccess = nullptr;
};
