#pragma once

#include <QString>

namespace svnsync {

/** A single SVN repository configured in the app. */
struct Repository
{
    QString name;
    QString path;        // working copy path
    QString url;         // repository URL
    QString username;
    QString password;
    bool isActive = false;
};

} // namespace svnsync
