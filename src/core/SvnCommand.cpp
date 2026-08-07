#include "core/SvnCommand.h"

namespace svnsync {

bool isWcLockErrorText(const QString &errorText)
{
    if (errorText.contains(QLatin1String("is already locked")))
        return true;
    return errorText.contains(QLatin1String("Working copy"), Qt::CaseInsensitive)
        && errorText.contains(QLatin1String("locked"), Qt::CaseInsensitive);
}

Category categoryOf(Command command)
{
    switch (command) {
    case Command::Info:
    case Command::Status:
    case Command::GetRevision:
    case Command::GetHeadRevision:
    case Command::GetConflictedFiles:
    case Command::GetLastChangedTime:
    case Command::IsVersioned:
    case Command::IsValidWorkingCopy:
    case Command::TestConnection:
    case Command::GetServerUpdatePaths:
        return Category::ReadOnly;

    case Command::Add:
    case Command::Delete:
    case Command::Move:
    case Command::Revert:
    case Command::Resolve:
    case Command::BreakLock:
        return Category::LocalWrite;

    case Command::Commit:
    case Command::Update:
    case Command::Checkout:
        return Category::HeavyWrite;
    }
    return Category::ReadOnly;
}

CommandResult makeResult(const CommandItem &item, bool success, const QString &error)
{
    CommandResult result;
    result.id = item.id;
    result.command = item.command;
    result.path = item.path;
    if (item.command == Command::Update && !item.updatePaths.isEmpty())
        result.paths = item.updatePaths;
    else if (!item.path.isEmpty())
        result.paths = QStringList{ item.path };
    result.success = success;
    result.error = error;
    return result;
}

} // namespace svnsync
