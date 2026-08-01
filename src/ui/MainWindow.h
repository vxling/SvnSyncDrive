#pragma once

#include "core/Repository.h"

#include <QMainWindow>

#include <memory>

class QPlainTextEdit;
class QPushButton;

namespace svnsync {
class SyncEngine;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void startRepository(const svnsync::Repository &repository);

private:
    void log(const QString &message);

    QPlainTextEdit *m_log = nullptr;
    QPushButton *m_syncButton = nullptr;
    std::unique_ptr<svnsync::SyncEngine> m_engine;
};
