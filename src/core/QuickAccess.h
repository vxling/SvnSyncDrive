#pragma once

#include <QString>

namespace svnsync {

/**
 * Registers (or removes) a shortcut to the configured repository storage
 * folder in the OS file manager:
 *  - Windows: an entry under "This PC" + a pin in Quick Access / Home
 *             (both per-user, no elevation required);
 *  - Linux:   a GTK/Nautilus bookmark in ~/.config/gtk-3.0/bookmarks;
 *  - macOS:   not implemented yet (isSupported() == false).
 *
 * install() creates the target folder if it is missing and is idempotent.
 * uninstall() is tolerant when nothing was installed.
 */
class QuickAccess
{
public:
    static bool isSupported();

    static bool install(const QString &root);
    static bool uninstall(const QString &root);

    /** Edit a GTK bookmarks file at `bookmarksPath`: add (add == true) or
     *  remove (add == false) the line `uri [label]`. Returns true when the
     *  file is left in the requested state (missing files are created on add,
     *  and removal of an absent line is not an error). Platform-independent,
     *  exposed for tests. */
    static bool editBookmarksFile(const QString &bookmarksPath, const QString &uri,
                                  const QString &label, bool add);

    /** Test hook: relocate the GTK bookmarks file used on Linux ("" restores
     *  the platform default). */
    static void setBookmarksFileForTest(const QString &path);
};

} // namespace svnsync