#pragma once

#include <QString>

namespace svnsync {

/** Application program log: timestamped lines appended to a single file
 *  (~/svnsyncdrive/svnsyncdrive.log). Thread-safe; call from any thread.
 *  The file is rotated once it grows past a size limit. */
class AppLog
{
public:
    static QString filePath();
    static void write(const QString &level, const QString &message);

    static void info(const QString &message) { write(QStringLiteral("INFO"), message); }
    static void warn(const QString &message) { write(QStringLiteral("WARN"), message); }
    static void error(const QString &message) { write(QStringLiteral("ERROR"), message); }
};

} // namespace svnsync
