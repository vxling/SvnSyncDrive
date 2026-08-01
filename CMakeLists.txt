cmake_minimum_required(VERSION 3.24)

project(SvnSyncDrive VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Widgets Network Sql)

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
    src/core/ConfigStore.cpp
    src/core/Repository.h
    src/core/SvnCommand.h
    src/core/ICommandRunner.h
    src/core/SvnWorker.h
    src/core/RepoWatcher.h
    src/core/SyncEngine.h
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

# ── GUI application ───────────────────────────────────────────────────────
add_executable(svnsyncdrive
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
)

target_link_libraries(svnsyncdrive PRIVATE
    svnsync_core
    Qt6::Widgets
    Qt6::Network
    Qt6::Sql
)
copy_libsvn_runtime_dlls(svnsyncdrive)

# ── console test harness ─────────────────────────────────────────────────
add_executable(synccoretest tests/coretest/main.cpp)
target_link_libraries(synccoretest PRIVATE svnsync_core)
copy_libsvn_runtime_dlls(synccoretest)
