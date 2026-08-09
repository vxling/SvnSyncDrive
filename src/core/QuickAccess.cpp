#include "core/QuickAccess.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStringList>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <cwchar>
#include <cstring>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#endif

namespace svnsync {

namespace {

QString g_testBookmarksFile;

#ifdef _WIN32

template <typename T>
struct ComPtr
{
    T *p = nullptr;
    ~ComPtr()
    {
        if (p)
            p->Release();
    }
    void reset()
    {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }
    T **operator&()
    {
        reset();
        return &p;
    }
    T *get() const { return p; }
    T *operator->() const { return p; }
    bool ok() const { return p != nullptr; }
};

struct ComGuard {
    ComGuard() { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ComGuard() { CoUninitialize(); }
};

// ── stable GUID per storage folder ─────────────────────────────────────────
QString guidFor(const QString &root)
{
    const QByteArray hash = QCryptographicHash::hash(
        (QStringLiteral("SvnSyncDrive quick access:") + root).toUtf8(),
        QCryptographicHash::Md5);
    const QString hex = QString::fromLatin1(hash.toHex());
    return QStringLiteral("{%1-%2-%3-%4-%5}")
        .arg(hex.mid(0, 8), hex.mid(8, 4), hex.mid(12, 4), hex.mid(16, 4),
             hex.mid(20, 12));
}

QString clsKeyFor(const QString &guid)
{
    return QStringLiteral("Software\\Classes\\CLSID\\") + guid;
}

bool regsetSz(const QString &subKey, const QString &name, const QString &value)
{
    HKEY key = nullptr;
    const std::wstring sub = subKey.toStdWString();
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
        return false;
    const std::wstring wide = value.toStdWString();
    const std::wstring wideName = name.toStdWString();
    const LONG err = RegSetValueExW(
        key, name.isEmpty() ? nullptr : wideName.c_str(), 0, REG_SZ,
        reinterpret_cast<const BYTE *>(wide.c_str()),
        DWORD((wide.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return err == ERROR_SUCCESS;
}

bool regsetDword(const QString &subKey, const QString &name, DWORD value)
{
    HKEY key = nullptr;
    const std::wstring sub = subKey.toStdWString();
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
        return false;
    const std::wstring wideName = name.toStdWString();
    const LONG err = RegSetValueExW(key, wideName.c_str(), 0, REG_DWORD,
                                    reinterpret_cast<const BYTE *>(&value),
                                    sizeof(value));
    RegCloseKey(key);
    return err == ERROR_SUCCESS;
}

void regDeleteTreeKey(const QString &subKey)
{
    RegDeleteTreeW(HKEY_CURRENT_USER, subKey.toStdWString().c_str());
}

// Run a language/registry-independent verb on a shell item.
bool invokeContextVerb(IShellFolder *parent, const ITEMIDLIST *item,
                       const wchar_t *wantedVerb)
{
    if (!parent || !item)
        return false;
    ComPtr<IContextMenu> menu;
    HRESULT hr = parent->GetUIObjectOf(0, 1, &item, IID_IContextMenu, nullptr,
                                       reinterpret_cast<void **>(&menu));
    if (FAILED(hr) || !menu.ok())
        return false;

    HMENU h = CreatePopupMenu();
    if (!h)
        return false;
    const UINT kFirst = 1;
    menu->QueryContextMenu(h, 0, kFirst, 0xFFFF, CMF_NORMAL);

    bool invoked = false;
    const int n = GetMenuItemCount(h);
    for (int i = 0; i < n; ++i) {
        const UINT cmd = static_cast<UINT>(GetMenuItemID(h, i));
        if (cmd == static_cast<UINT>(-1))
            continue;
        wchar_t verb[MAX_PATH] = {0};
        hr = menu->GetCommandString(cmd, GCS_VERBW, nullptr,
                                    reinterpret_cast<LPSTR>(verb),
                                    MAX_PATH);
        if (FAILED(hr) || wcscmp(verb, wantedVerb) != 0)
            continue;
        CMINVOKECOMMANDINFO info = {};
        info.cbSize = sizeof(info);
        info.fMask = CMIC_MASK_FLAG_NO_UI;
        info.hwnd = nullptr;
        info.lpVerb = MAKEINTRESOURCEA(cmd);
        info.nShow = SW_SHOWNORMAL;
        if (SUCCEEDED(menu->InvokeCommand(&info)))
            invoked = true;
        break;
    }
    DestroyMenu(h);
    return invoked;
}

// Pin the folder itself to Quick Access / Home.
bool pinToQuickAccess(const QString &nativePath)
{
    ComGuard guard;
    LPITEMIDLIST pidl = nullptr;
    if (FAILED(SHParseDisplayName(nativePath.toStdWString().c_str(), nullptr,
                                  &pidl, 0, nullptr)))
        return false;
    const ITEMIDLIST *child = nullptr;
    ComPtr<IShellFolder> parent;
    HRESULT hr = SHBindToParent(pidl, IID_IShellFolder,
                                reinterpret_cast<void **>(&parent),
                                &child);
    ILFree(pidl);
    if (FAILED(hr) || !parent.ok())
        return false;
    return invokeContextVerb(parent.get(), child, L"pintohome");
}

// Remove the Quick Access / Home entry pointing at nativePath.
bool unpinFromQuickAccess(const QString &nativePath)
{
    constexpr const wchar_t *kNsPrefix = L"shell:::{679F85CB-0220-4080-B29B-5540CC05AAB6}";
    ComGuard guard;

    ComPtr<IShellFolder> desktop;
    if (FAILED(SHGetDesktopFolder(
            reinterpret_cast<IShellFolder **>(&desktop))))
        return false;
    LPITEMIDLIST nsPidl = nullptr;
    if (FAILED(SHParseDisplayName(kNsPrefix, nullptr, &nsPidl, 0, nullptr)))
        return false;
    ComPtr<IShellFolder> qa;
    HRESULT hr = desktop->BindToObject(nsPidl, nullptr, IID_IShellFolder,
                                       reinterpret_cast<void **>(&qa));
    ILFree(nsPidl);
    if (FAILED(hr) || !qa.ok())
        return false;

    ComPtr<IEnumIDList> en;
    if (FAILED(qa->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS,
                               &en)))
        return false;

    const std::wstring want = nativePath.toStdWString();
    bool removed = false;
    LPITEMIDLIST child = nullptr;
    while (en->Next(1, &child, nullptr) == S_OK && !removed) {
        STRRET str;
        ZeroMemory(&str, sizeof(str));
        if (SUCCEEDED(qa->GetDisplayNameOf(child, SHGDN_FORPARSING, &str))) {
            LPWSTR name = nullptr;
            if (SUCCEEDED(StrRetToStrW(&str, child, &name)) && name
                && _wcsicmp(name, want.c_str()) == 0) {
                removed = invokeContextVerb(qa.get(), child, L"unpinfromhome");
            }
            if (name)
                CoTaskMemFree(name);
        }
        ILFree(child);
    }
    return removed;
}

#endif // _WIN32

QString gtkBookmarksFile()
{
    if (!g_testBookmarksFile.isEmpty())
        return g_testBookmarksFile;
    QString cfg = qgetenv("XDG_CONFIG_HOME");
    if (cfg.isEmpty())
        cfg = QDir::homePath() + QStringLiteral("/.config");
    return cfg + QStringLiteral("/gtk-3.0/bookmarks");
}

} // namespace

void QuickAccess::setBookmarksFileForTest(const QString &path)
{
    g_testBookmarksFile = path;
}

bool QuickAccess::isSupported()
{
#if defined(_WIN32) || defined(Q_OS_LINUX)
    return true;
#else
    return false;
#endif
}

bool QuickAccess::editBookmarksFile(const QString &bookmarksPath,
                                    const QString &uri, const QString &label,
                                    bool add)
{
    const QString wanted = uri + QLatin1String(" ") + label;

    QStringList lines;
    QByteArray original;
    QFile file(bookmarksPath);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;
        original = file.readAll();
        file.close();
        const QStringList raw = QString::fromUtf8(original).split(QLatin1Char('\n'));
        for (const QString &entry : raw) {
            const QString trimmed = entry.trimmed();
            if (trimmed.isEmpty())
                continue;
            const int sp = trimmed.indexOf(QLatin1Char(' '));
            const QString entryUri = (sp < 0) ? trimmed : trimmed.left(sp);
            if (entryUri == uri)
                continue;  // drop any existing entry for this URI
            lines.append(trimmed);
        }
    }

    if (add)
        lines.append(wanted);

    QString output;
    for (const QString &line : lines)
        output += line + QLatin1Char('\n');

    if (file.exists() && QString::fromUtf8(original) == output)
        return true;

    QSaveFile out(bookmarksPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    out.write(output.toUtf8());
    return out.commit();
}

bool QuickAccess::install(const QString &root)
{
    QDir rootDir(root);
    if (!rootDir.mkpath(QStringLiteral(".")))
        return false;

#ifdef _WIN32
    const QString native = QDir::toNativeSeparators(root);
    const QString guid = guidFor(root);
    const QString cls = clsKeyFor(guid);

    bool ok = true;
    ok &= regsetSz(cls, QString(), QStringLiteral("SvnSyncDrive"));
    ok &= regsetDword(cls, QStringLiteral("System.IsPinnedToNameSpaceTree"), 1);
    ok &= regsetDword(cls, QStringLiteral("SortOrderIndex"), 0x00000042);
    ok &= regsetSz(cls + QStringLiteral("\\DefaultIcon"), QString(),
                   QStringLiteral("%SystemRoot%\\System32\\imageres.dll,-111"));
    ok &= regsetSz(cls + QStringLiteral("\\InProcServer32"), QString(),
                   QStringLiteral("%SystemRoot%\\System32\\shell32.dll"));
    ok &= regsetSz(cls + QStringLiteral("\\Instance"), QStringLiteral("CLSID"),
                   QStringLiteral("{0E5AAE11-A475-4c5b-AB00-C66DE400274E}"));
    ok &= regsetDword(cls + QStringLiteral("\\Instance\\InitPropertyBag"),
                      QStringLiteral("Attributes"), 0x00000011);
    ok &= regsetSz(cls + QStringLiteral("\\Instance\\InitPropertyBag"),
                   QStringLiteral("TargetFolderPath"), native);
    ok &= regsetDword(cls + QStringLiteral("\\ShellFolder"),
                      QStringLiteral("Attributes"), 0xf080004d);
    ok &= regsetDword(cls + QStringLiteral("\\ShellFolder"),
                      QStringLiteral("FolderValueFlags"), 0x00000028);

    // Publish in the navigation pane (Desktop namespace) and under "This PC".
    const QString desktopNs =
        QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop\\NameSpace\\") + guid;
    const QString myComputerNs =
        QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MyComputer\\NameSpace\\") + guid;
    ok &= regsetSz(desktopNs, QString(), QStringLiteral("SvnSyncDrive"));
    ok &= regsetSz(myComputerNs, QString(), QStringLiteral("SvnSyncDrive"));

    // Best-effort Quick Access pin; report it so the UI can mention it.
    pinToQuickAccess(native);
    return ok;
#else
    const QString uri = QStringLiteral("file://") + root;
    return editBookmarksFile(gtkBookmarksFile(), uri,
                             QStringLiteral("SvnSyncDrive"), true);
#endif
}

bool QuickAccess::uninstall(const QString &root)
{
#ifdef _WIN32
    const QString guid = guidFor(root);
    regDeleteTreeKey(clsKeyFor(guid));
    regDeleteTreeKey(QStringLiteral(
        "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop\\NameSpace\\") + guid);
    regDeleteTreeKey(QStringLiteral(
        "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MyComputer\\NameSpace\\") + guid);
    unpinFromQuickAccess(QDir::toNativeSeparators(root));
    return true;
#else
    const QString uri = QStringLiteral("file://") + root;
    return editBookmarksFile(gtkBookmarksFile(), uri,
                             QStringLiteral("SvnSyncDrive"), false);
#endif
}

} // namespace svnsync