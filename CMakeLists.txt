cmake_minimum_required(VERSION 3.16)
project(my-vigem VERSION 1.3.0 LANGUAGES C CXX RC)

add_definitions(-DWIN32 -DUNICODE -D_UNICODE -D_CRT_NON_CONFORMING_SWPRINTFS)

# Windows resource script (icon + version info), generated into the build dir
set(APP_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Bitmaps/MiPad.ico")
set(APP_VERSION_COMMA "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
set(APP_VERSION_DOT "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.0")
string(CONFIGURE [=[
#include <winver.h>

APP_ICON            ICON "@APP_ICON_PATH@"

VS_VERSION_INFO     VERSIONINFO
FILEVERSION         @APP_VERSION_COMMA@
PRODUCTVERSION      @APP_VERSION_COMMA@
FILETYPE            VFT_APP
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904E4"
        BEGIN
            VALUE "FileDescription",  "My-ViGEm"
            VALUE "FileVersion",      "@APP_VERSION_DOT@"
            VALUE "ProductName",      "My-ViGEm"
            VALUE "ProductVersion",   "@APP_VERSION_DOT@"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1252
    END
END
]=] APP_RC_CONTENT @ONLY)
set(APP_RC_FILE "${CMAKE_CURRENT_BINARY_DIR}/res.rc")
file(GENERATE OUTPUT "${APP_RC_FILE}" CONTENT "${APP_RC_CONTENT}")

add_executable(my-vigem WIN32
    Sources/main.c Sources/utils.c Sources/tray.c Sources/hid.c Sources/hid_hide.c Sources/mi.c
    Resources/ViGEmClient/src/ViGEmClient.cpp
    "${APP_RC_FILE}")

target_include_directories(my-vigem PRIVATE Resources/ViGEmClient/include)
# Silence false positives from the vendored ViGEm headers and Clazy
target_compile_options(my-vigem PRIVATE)
target_link_libraries(my-vigem PRIVATE
    setupapi hid dbghelp cfgmgr32 user32 shell32 advapi32 ole32)
target_link_options(my-vigem PRIVATE -static -static-libgcc -static-libstdc++)
