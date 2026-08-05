# SvnSyncDrive

A cross-platform desktop client that keeps local folders in **two-way live sync** with Subversion repositories: it watches working copies, auto-adds unversioned files, commits changes, and pulls remote updates — with conflict detection and encrypted credential storage.

Built with C++17 + Qt 6 (Widgets), on top of [LibSVNPlus](https://github.com/vxling/LibSVNPlus), a thin C++ wrapper around the Subversion C API.

## Features

- **Upward sync (local → server)**: watches the working copy, auto-runs `svn add` on unversioned files and commits versioned changes (grouped by directory, deepest first).
- **Downward sync (server → local)**: polls the server HEAD, discovers remote changes via `svn status -u`, and updates the working copy in chunks (deepest directory first).
- **Full sync on a timer**: periodic whole-tree `svn update` + full upward scan as a safety net for edge cases.
- **Conflict handling**: conflicts are detected and surfaced in a dialog; never auto-resolved.
- **Per-repository engine**: every repo gets its own watcher, worker thread, and timers — one slow repo never blocks another.
- **Resilient networking**: per-command network timeouts with a worker watchdog, and a configurable disconnect threshold that flags dead servers quickly.
- **System tray integration**: run minimized to the tray so background sync keeps going.
- **Credentials never stored in app config**: delegated to libsvn's own encrypted auth cache.
- **Single instance**: a second launch just focuses the already-running window.
- **Self-contained bundles**: every artifact ships the exact libsvn it was built against.

## Download

Grab the latest release from [Releases](https://github.com/vxling/SvnSyncDrive/releases):

| Platform | Artifacts |
|---|---|
| Windows x64 | portable zip · per-user MSI installer |
| Linux x86_64 | tar.gz bundle · `.deb` package |
| macOS arm64 | `.dmg` (built by CI) |

## Installation

### Windows

- **MSI** (recommended): double-click `SvnSyncDrive-<version>-win64.msi`. It installs **per-user** — no administrator rights required. Note the per-user installation is tied to the account that installs it.
- **Portable zip**: unpack anywhere and run `SvnSyncDrive.exe`.

### Linux

Prefer the `.deb` for Debian/Ubuntu-family systems:

```bash
sudo apt install ./svnsyncdrive_<version>_amd64.deb
```

> **Note for Ubuntu 25.10 and newer:** double-clicking the `.deb` in GNOME Software is blocked with a signature/authentication error — since 25.10 the Software app verifies third-party `.deb` files and refuses unsigned packages. Install from the terminal with `sudo apt install ./...` (or `sudo dpkg -i` followed by `sudo apt -f install`), which does not apply the sideload signature check. On older Ubuntu versions double-clicking still works.

The portable **tar.gz** bundle needs no installation:

```bash
tar -xzf svnsyncdrive-<version>-linux64.tar.gz
cd svnsyncdrive-<version>-linux64
./svnsyncdrive.sh
```

### macOS

Open the `.dmg` and drag the app to *Applications*. The app is only ad-hoc signed (no Apple Developer ID), so on first launch right-click it in Finder and choose **Open** to bypass Gatekeeper.

## File status icons in the OS file manager

SvnSyncDrive shows SVN status icons inside its own file browser, but it does **not** add status overlays to the OS file manager (Explorer / Finder / Nautilus). Shell icon overlays are platform-specific, tightly coupled to each OS, and hard-limited by the OS (Windows exposes only 15 overlay slots system-wide; Nautilus removed its emblem API on modern GNOME; Finder badges require a separately-signed Swift extension). Building and maintaining that per platform is not worth it for a desktop client.

On **Windows**, install [TortoiseSVN](https://tortoisesvn.net/) to get SVN status overlays directly in Explorer — it reads the same `svn` working copies and requires no extra configuration:

1. Install TortoiseSVN (use "Show overlay icons" from its settings if overlays are hidden — overlay slots are shared with Dropbox/OneDrive/other tools).
2. Its Explorer overlays and column providers work on any SVN working copy, including ones managed by SvnSyncDrive.

On **macOS** and **Linux**, Explorer-style overlays are not practically available; rely on the status icons inside SvnSyncDrive's file browser instead.

## Quick start

1. Launch SvnSyncDrive.
2. Click **+ 添加仓库** (Add repository).
3. Enter a name, the local working-copy path (created automatically if missing), and the repository URL — e.g. `https://svn.example.com/repo/project`.
4. Optionally enter credentials and click **测试连接** (Test connection) to verify.
5. The app checks out a fresh working copy if the directory is empty, then starts syncing.

Once running, each repository keeps syncing in the background. Close the window (or start with **启动时最小化到系统托盘**) to keep it in the system tray.

## How it works

- Every repository has an independent **SyncEngine** (on the GUI thread) owning a **RepoWatcher** (filesystem events) and an **SvnWorker** (a single dedicated thread that runs all SVN calls, so the libsvn client is never used concurrently).
- **Upward**: watcher batches file changes → a local `svn status` scan → `svn add` unversioned files (optional) → commit by directory.
- **Downward**: a poll timer compares the working-copy revision with the server HEAD → `GetServerUpdatePaths` (`svn status -u`) → chunked `svn update`.
- Results are marshalled back to the GUI thread via queued signals.

The full architecture, data layout, and the decisions/pitfalls behind it live in [docs/DESIGN.md](docs/DESIGN.md).

## Data storage

Everything is kept under `~/.svnsyncdrive`:

| File | Content |
|---|---|
| `config.db` | SQLite: repository list + global settings |
| `logs.db` | SQLite: per-repository sync history |
| `svnsyncdrive.log` | Program log (rotating) |

Settings (all changeable in **设置** / Settings): poll interval, full-sync interval, auto-add unversioned files, trust self-signed certificates, minimize to tray, logs kept per repo, disconnect threshold, and network timeout.

## Building from source

Requirements: CMake 3.24+, a C++17 compiler (MSVC / GCC / Clang), Qt 6.5+ (Widgets, Network, Sql), and a staged [LibSVNPlus](https://github.com/vxling/LibSVNPlus) install.

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
    -DLIBSVNPLUS_ROOT=/path/to/libsvnplus/stage
cmake --build build
```

- `LIBSVNPLUS_ROOT` points at a staged libsvnplus install with an `include/` and `lib/` layout (on Windows also the runtime DLLs under `bin/`).
- On Windows, OpenSSL is located automatically via the LibSVNPlus vcpkg tree unless `OPENSSL_ROOT_DIR` is set.
- On Windows the target auto-runs `windeployqt` and copies the libsvn runtime DLLs next to the executable.

### Packaging

- Windows: `scripts/publish-win.ps1` (portable zip + WiX per-user MSI).
- Linux: `scripts/publish-linux.sh` (tar.gz + deb).
- macOS: `scripts/publish-macos.sh`; the CI workflow [macos.yml](.github/workflows/macos.yml) builds and attaches the `.dmg` on `v*` tag pushes.

## Testing

`synccoretest` runs unit tests plus optional live integration tests against a real repo:

```bash
# Unit tests only
./build/synccoretest

# Two-way live regression (builds a repo + two working copies)
powershell -ExecutionPolicy Bypass -File scripts/live-sync-test.ps1
```

Also try `SvnSyncDrive --repo <wc> <url> [user] [pass]` for a single-repo headless-ish live run.

## License

The license is not yet declared — no `LICENSE` file exists in this repository. Add one before distributing.
