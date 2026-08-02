#pragma once

#include <QDialog>

/**
 * About dialog: application identity, versions and a short description.
 */
class AboutDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AboutDialog(QWidget *parent = nullptr);
};
