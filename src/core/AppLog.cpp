#include "core/AppLog.h"

#include "core/AppPaths.h"

#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QTextStream>

namespace svnsync {

namespace {
const qint64 kMaxFileSize = 5 * 1024 * 1024;

QMutex &mutex()
{
    static QMutex m;
    return m;
}

void rotateIfNeeded(const QString &path)
{
    QFile file(path);
    if (file.exists() && file.size() >= kMaxFileSize) {
        QFile old(path + QLatin1String(".1"));
        old.remove();
        QFile::rename(path, path + QLatin1String(".1"));
    }
}
} // namespace

QString AppLog::filePath()
{
    return AppPaths::dataDir() + QLatin1String("/svnsyncdrive.log");
}

void AppLog::write(const QString &level, const QString &message)
{
    QMutexLocker<QMutex> lock(&mutex());
    const QString path = filePath();
    rotateIfNeeded(path);

    QFile file(path);
    if (!file.open(QIODevice::Append | QIODevice::Text))
        return;
    QTextStream out(&file);
    out << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
        << QLatin1Char(' ') << level << QLatin1Char(' ') << message << QLatin1Char('\n');
}

} // namespace svnsync
