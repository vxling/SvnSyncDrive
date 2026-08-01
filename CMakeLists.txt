cmake_minimum_required(VERSION 3.24)

project(SvnSyncDrive VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Widgets Network)

if(NOT DEFINED LIBSVNPLUS_ROOT)
    set(LIBSVNPLUS_ROOT "C:/Users/xuser/Documents/LibSVNPlus/build/_stage"
        CACHE PATH "Root of the staged libsvnplus install (include/lib/bin layout)")
endif()

find_path(SVNPLUS_INCLUDE_DIR svnplus/SvnClient.h HINTS "${LIBSVNPLUS_ROOT}/include")
find_library(SVNPLUS_LIBRARY NAMES svnplus HINTS "${LIBSVNPLUS_ROOT}/lib")

if(NOT SVNPLUS_INCLUDE_DIR OR NOT SVNPLUS_LIBRARY)
    message(FATAL_ERROR "libsvnplus not found under LIBSVNPLUS_ROOT=${LIBSVNPLUS_ROOT}")
endif()

set(SVNPLUS_LIBS
    "${SVNPLUS_LIBRARY}"
    "${LIBSVNPLUS_ROOT}/lib/libsvn_client-1.lib"
    "${LIBSVNPLUS_ROOT}/lib/libsvn_subr-1.lib"
    "${LIBSVNPLUS_ROOT}/lib/libapr-1.lib"
    "${LIBSVNPLUS_ROOT}/lib/libaprutil-1.lib"
)

# ── core library (no GUI dependency) ──────────────────────────────────────
add_library(svnsync_core STATIC
    src/core/SvnCommand.cpp
    src/core/SvnWorker.cpp
    src/core/RepoWatcher.cpp
    src/core/SyncEngine.cpp
    src/core/RepoManager.cpp
    src/core/ConfigStore.cpp
    src/core/Repository.h
    src/core/SvnCommand.h
    src/core/ICommandRunner.h
    src/core/SvnWorker.h
    src/core/RepoWatcher.h
    src/core/SyncEngine.h
    src/core/RepoManager.h
    src/core/ConfigStore.h
)

target_include_directories(svnsync_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(svnsync_core PUBLIC
    "${SVNPLUS_INCLUDE_DIR}"
    "${LIBSVNPLUS_ROOT}/include/subversion-1"
)
target_compile_definitions(svnsync_core PUBLIC SVNPLUS_STATIC)
target_link_libraries(svnsync_core PUBLIC Qt6::Core PRIVATE ${SVNPLUS_LIBS})

# Copies the libsvn runtime DLLs next to a target so it can start standalone.
function(copy_libsvn_runtime_dlls TARGET)
    file(GLOB LIBSVN_RUNTIME_DLLS "${LIBSVNPLUS_ROOT}/bin/*.dll")
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${LIBSVN_RUNTIME_DLLS}
            "$<TARGET_FILE_DIR:${TARGET}>"
        COMMENT "Copying libsvn runtime DLLs next to ${TARGET}")
endfunction()

# Runs windeployqt next to a target so the executable and all Qt DLLs and
# plugins (platforms, styles, imageformats, tls, sqldrivers, ...) are staged
# for direct launch. Windows-only for now; Linux/macOS packaging can use the
# same pattern with their platform deploy tools later.
function(deploy_qt TARGET)
    if(NOT WIN32)
        return()
    endif()
    get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    if(NOT QT_QMAKE_EXECUTABLE)
        message(WARNING "deploy_qt(${TARGET}): Qt6::qmake not found, skipping")
        return()
    endif()
    get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
    set(WINDEPLOYQT "${QT_BIN_DIR}/windeployqt${CMAKE_EXECUTABLE_SUFFIX}")
    if(NOT EXISTS "${WINDEPLOYQT}")
        message(WARNING "deploy_qt(${TARGET}): windeployqt not found, skipping")
        return()
    endif()
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND "${WINDEPLOYQT}"
            --no-translations
            --no-system-d3d-compiler
            --no-opengl-sw
            "$<IF:$<CONFIG:Debug>,--debug,--release>"
            "$<TARGET_FILE:${TARGET}>"
        COMMENT "Running windeployqt for ${TARGET}")
endfunction()

# ── GUI application ───────────────────────────────────────────────────────
add_executable(svnsyncdrive
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/RepoListPanel.cpp
    src/ui/RepoListPanel.h
    src/ui/RepoDetailPage.cpp
    src/ui/RepoDetailPage.h
    src/ui/FileBrowser.cpp
    src/ui/FileBrowser.h
    src/ui/AddRepoDialog.cpp
    src/ui/AddRepoDialog.h
)

target_link_libraries(svnsyncdrive PRIVATE
    svnsync_core
    Qt6::Widgets
    Qt6::Network
)
copy_libsvn_runtime_dlls(svnsyncdrive)
deploy_qt(svnsyncdrive)

# ── console test harness ─────────────────────────────────────────────────
add_executable(synccoretest tests/coretest/main.cpp)
target_link_libraries(synccoretest PRIVATE svnsync_core)
copy_libsvn_runtime_dlls(synccoretest)
deploy_qt(synccoretest)

# ── temporary diagnostics ─────────────────────────────────────────────────
add_executable(diagmanager tests/diagmanager/main.cpp)
target_link_libraries(diagmanager PRIVATE svnsync_core)
copy_libsvn_runtime_dlls(diagmanager)
