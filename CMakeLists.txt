cmake_minimum_required(VERSION 3.24)
project(SkeinDownloader VERSION 0.7 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(wxBUILD_SHARED OFF CACHE BOOL "" FORCE)
set(wxBUILD_USE_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(wxBUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(wxBUILD_TESTS OFF CACHE BOOL "" FORCE)
set(wxBUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(wxBUILD_LOCALES OFF CACHE BOOL "" FORCE)
set(wxBUILD_INSTALL OFF CACHE BOOL "" FORCE)
# LIBWEBP: wx 3.3 builds or links libwebp by default; nothing here shows WebP images.
foreach(feature LIBJPEG LIBTIFF LIBWEBP NANOSVG EXPAT REGEX)
  set(wxUSE_${feature} OFF CACHE STRING "" FORCE)
endforeach()
# PNG (with the zlib it needs) for the Ko-fi logo. Windows uses wx's own copies. Linux uses
# the system's, which every desktop has (GTK's image loading needs them) and which the
# Flatpak runtime provides; wx's bundled zlib does not build with GCC 14 (implicit lseek).
if(WIN32)
  set(wx_image_libs builtin)
else()
  set(wx_image_libs sys)
endif()
foreach(feature LIBPNG ZLIB)
  set(wxUSE_${feature} ${wx_image_libs} CACHE STRING "" FORCE)
endforeach()
# DETECT_SM, UIACTIONSIMULATOR and XTEST would link libSM, libICE and libXtst, which a
# Wayland-only system may lack and the AppImage would then fail to start without.
foreach(feature WEBVIEW WEBVIEW_EDGE WEBVIEW_IE WEBREQUEST MEDIACTRL HTML XML XRC RICHTEXT STC AUI RIBBON PROPGRID GL SVG ARCHIVE_STREAMS ZIPSTREAM TARSTREAM JOYSTICK SOCKETS PROTOCOL URL PROTOCOL_FTP PROTOCOL_HTTP PROTOCOL_FILE FS_INET FS_ARCHIVE FS_ZIP SECRETSTORE LIBLZMA GIF PCX TGA IFF PNM ARTPROVIDER_TANGO IPC HELP WXHTML_HELP MS_HTML_HELP DETECT_SM UIACTIONSIMULATOR XTEST)
  set(wxUSE_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
if(NOT WIN32)
  set(wxBUILD_TOOLKIT gtk3 CACHE STRING "" FORCE)
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/wxWidgets/CMakeLists.txt")
  message(FATAL_ERROR "wxWidgets is missing from vendor/wxWidgets. It is a git submodule: run\n"
    "  git submodule update --init --recursive\nor clone with --recursive.")
endif()
add_subdirectory(vendor/wxWidgets EXCLUDE_FROM_ALL)
if(WIN32)
  add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  add_library(archive_core src/core.cpp src/http.cpp src/hosts.cpp src/lockers.cpp src/platform/files_windows.cpp)
  target_link_libraries(archive_core PUBLIC winhttp bcrypt)
else()
  find_package(Threads REQUIRED)
  find_package(CURL REQUIRED)
  find_package(OpenSSL REQUIRED)
  add_library(archive_core src/core.cpp src/http.cpp src/hosts.cpp src/lockers.cpp src/platform/files_linux.cpp)
  target_link_libraries(archive_core PUBLIC CURL::libcurl OpenSSL::Crypto Threads::Threads)
endif()
target_include_directories(archive_core PUBLIC src third_party)
# src/util/sys_log.* is a copy of AAE's aae/system/util/sys_log.*; keep it in sync with that master.
target_sources(archive_core PRIVATE src/util/sys_log.cpp)
target_compile_definitions(archive_core PRIVATE LOG_WITH_TIMESTAMP)
add_library(video_core src/video.cpp)
target_link_libraries(video_core PUBLIC archive_core)
add_library(download_queue src/download_queue.cpp src/job_runner.cpp)
target_link_libraries(download_queue PUBLIC archive_core video_core)
add_library(ui_logic src/wx/logic.cpp)
target_link_libraries(ui_logic PUBLIC download_queue)
# Embed gettext catalogs so every package and portable executable is self-contained.
# Regenerate the checked-in .mo files with: python tools/translations.py
set(translation_header "// Generated from locale/*.mo by CMake.\n")
set(translation_rows "")
foreach(language zh_CN ru ja fr it es ko de zh_TW vi pt hi)
  foreach(domain skein wxstd)
    set(catalog "${CMAKE_CURRENT_SOURCE_DIR}/locale/${language}.${domain}.mo")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${catalog}")
    file(READ "${catalog}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    set(symbol "catalog_${language}_${domain}")
    string(APPEND translation_header "static const unsigned char ${symbol}[]={${bytes}};\n")
    string(APPEND translation_rows "{\"${language}\",\"${domain}\",${symbol},sizeof ${symbol}},\n")
  endforeach()
endforeach()
string(APPEND translation_header "struct EmbeddedCatalog {const char* language;const char* domain;const unsigned char* data;size_t size;};\nstatic const EmbeddedCatalog embeddedCatalogs[]={${translation_rows}};\n")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/translations.h" CONTENT "${translation_header}")
add_library(ui_translations src/wx/localization.cpp)
target_link_libraries(ui_translations PUBLIC wx::base)
target_include_directories(ui_translations PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(MSVC)
  target_compile_options(ui_translations PRIVATE /utf-8 /W4)
endif()
add_executable(skein-downloader WIN32 src/wx/app.cpp src/wx/manager.cpp src/wx/file_picker.cpp src/wx/video_dialog.cpp src/wx/language_button.cpp)
target_link_libraries(skein-downloader PRIVATE ui_logic ui_translations wx::core wx::base)
target_compile_definitions(skein-downloader PRIVATE SKEIN_VERSION="${PROJECT_VERSION}")
target_include_directories(skein-downloader PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
# The Ko-fi cup logo for the toolbar's Ko-fi button, compiled in (src/wx/res/kofi-cup.png,
# cut from Ko-fi's badge, src/wx/res/kofi-badge.png). The rest of the button is drawn.
set(png "${CMAKE_CURRENT_SOURCE_DIR}/src/wx/res/kofi-cup.png")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${png}")
file(READ "${png}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/kofi_cup.h"
  CONTENT "// Generated from src/wx/res/kofi-cup.png by CMakeLists.txt.\nstatic const unsigned char kofiCup[] = {${hex}};\n")
# Compile the language selector flags into the executable so the menu never rasterizes artwork.
set(language_flags_header "// Generated from src/wx/res/flags/*.png by CMakeLists.txt.\n")
set(language_flags_rows "")
foreach(language system en zh_CN zh_TW de vi ru ja ko fr it es pt hi)
  foreach(size 20 30 40 60)
    set(flag_png "${CMAKE_CURRENT_SOURCE_DIR}/src/wx/res/flags/${language}-${size}.png")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${flag_png}")
    file(READ "${flag_png}" flag_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," flag_bytes "${flag_hex}")
    set(flag_symbol "languageFlag_${language}_${size}")
    string(APPEND language_flags_header "static const unsigned char ${flag_symbol}[] = {${flag_bytes}};\n")
    string(APPEND language_flags_rows "{\"${language}\",${size},${flag_symbol},sizeof ${flag_symbol}},\n")
  endforeach()
endforeach()
string(APPEND language_flags_header "struct EmbeddedLanguageFlag {const char* language;int size;const unsigned char* data;size_t length;};\nstatic const EmbeddedLanguageFlag embeddedLanguageFlags[] = {${language_flags_rows}};\n")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/language_flags.h" CONTENT "${language_flags_header}")
# Program icon, built by tools/make-icons.py into packaging/icons.
set(SKEIN_ICONS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons")
if(WIN32)
  # Explorer shows the exe's first icon group; app.cpp gives windows the same icons for the
  # title bar, taskbar and Alt+Tab, in every size the .ico holds (16-256 px for each DPI).
  # The version block fills the exe's Properties > Details tab.
  set(rc_version "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},0,0")
  file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/skein-downloader.rc" CONTENT
"appicon ICON \"${SKEIN_ICONS}/skein-downloader.ico\"
1 VERSIONINFO
FILEVERSION ${rc_version}
PRODUCTVERSION ${rc_version}
BEGIN
  BLOCK \"StringFileInfo\"
  BEGIN
    BLOCK \"040904B0\"
    BEGIN
      VALUE \"CompanyName\", \"Tim Cottrill\"
      VALUE \"FileDescription\", \"Skein Downloader\"
      VALUE \"FileVersion\", \"${PROJECT_VERSION}\"
      VALUE \"InternalName\", \"skein-downloader\"
      VALUE \"LegalCopyright\", \"Copyright (C) 2026 Tim Cottrill. GPL-3.0-or-later.\"
      VALUE \"OriginalFilename\", \"skein-downloader.exe\"
      VALUE \"ProductName\", \"Skein Downloader\"
      VALUE \"ProductVersion\", \"${PROJECT_VERSION}\"
    END
  END
  BLOCK \"VarFileInfo\"
  BEGIN
    VALUE \"Translation\", 0x409, 1200
  END
END
")
  target_sources(skein-downloader PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/skein-downloader.rc")
  # The resource compiler reports no dependencies, so a new .ico would not relink the exe.
  set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/skein-downloader.rc"
    PROPERTIES OBJECT_DEPENDS "${SKEIN_ICONS}/skein-downloader.ico")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
  target_link_libraries(skein-downloader PRIVATE PkgConfig::GTK3)
  install(TARGETS skein-downloader RUNTIME DESTINATION bin)
  install(DIRECTORY "${SKEIN_ICONS}/hicolor" DESTINATION share/icons)
  # The same PNGs compiled in, for X11 window icons where no installed theme icon is found
  # (an AppImage that was never integrated); Wayland compositors use the .desktop icon.
  set(header "// Generated from packaging/icons by CMakeLists.txt.\n")
  set(list "")
  foreach(size 16 24 32 48 64 128 256)
    set(png "${SKEIN_ICONS}/hicolor/${size}x${size}/apps/io.github.tcottrill.SkeinDownloader.png")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${png}")
    file(READ "${png}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
    string(APPEND header "static const unsigned char skeinIcon${size}[] = {${hex}};\n")
    string(APPEND list "{skeinIcon${size}, sizeof skeinIcon${size}},")
  endforeach()
  string(APPEND header "struct SkeinIcon {const unsigned char* data; unsigned long size;};\n"
    "static const SkeinIcon skeinIcons[] = {${list}};\n")
  file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/skein_icons.h" CONTENT "${header}")
endif()
if(MSVC)
  target_compile_options(skein-downloader PRIVATE /utf-8 /W4)
  target_link_options(skein-downloader PRIVATE /MANIFEST:EMBED "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
endif()
# Packaging builds with a read-only source tree (tools/build-linux-appimage.sh) set their own folder.
set(SKEIN_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist" CACHE PATH "Folder for the built executable")
# Release in that folder, Debug in its debug/ subfolder (Visual Studio builds both; the
# generator expression also stops it adding a Release/ or Debug/ folder of its own).
set_target_properties(skein-downloader PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SKEIN_OUTPUT_DIR}$<$<CONFIG:Debug>:/debug>")
# Visual Studio (CMakePresets.json, preset vs2022): F5 starts the program, not ALL_BUILD.
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT skein-downloader)
if(WIN32)
  add_custom_command(TARGET skein-downloader POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/install-video-tools.ps1" "$<TARGET_FILE_DIR:skein-downloader>/install-video-tools.ps1")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/tests.cmake")
  include(tests/tests.cmake)
endif()
