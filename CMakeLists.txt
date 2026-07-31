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

add_executable(svnsyncdrive
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
)

target_include_directories(svnsyncdrive PRIVATE
    "${SVNPLUS_INCLUDE_DIR}"
    "${LIBSVNPLUS_ROOT}/include/subversion-1"
)

target_compile_definitions(svnsyncdrive PRIVATE SVNPLUS_STATIC)

target_link_libraries(svnsyncdrive PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Sql
    "${SVNPLUS_LIBRARY}"
    "${LIBSVNPLUS_ROOT}/lib/libsvn_client-1.lib"
    "${LIBSVNPLUS_ROOT}/lib/libsvn_subr-1.lib"
    "${LIBSVNPLUS_ROOT}/lib/libapr-1.lib"
    "${LIBSVNPLUS_ROOT}/lib/libaprutil-1.lib"
)

file(GLOB LIBSVN_RUNTIME_DLLS "${LIBSVNPLUS_ROOT}/bin/*.dll")
add_custom_command(TARGET svnsyncdrive POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${LIBSVN_RUNTIME_DLLS}
        "$<TARGET_FILE_DIR:svnsyncdrive>"
    COMMENT "Copying libsvn runtime DLLs next to the executable"
)
