cmake_minimum_required(VERSION 3.21)
project(UltraLiteDesktopStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

add_executable(UltraLiteDesktopStudio WIN32
  src/main.cpp
  src/WidgetConfig.cpp
  src/SystemMonitor.cpp
  src/AudioEngine.cpp
  src/Platform.cpp
  src/WeatherEngine.cpp
  src/MediaHelperClient.cpp
  src/WidgetWindow.cpp
  src/Widgets.cpp
  src/JsHtmlPackage.cpp
  src/JsHtmlWidget.cpp
  src/EffectsOverlay.cpp
  src/EffectsEditor.cpp
  src/Preview3D.cpp
  src/DesktopIcons.cpp
  assets/resources.qrc
)
target_include_directories(UltraLiteDesktopStudio PRIVATE src)
target_link_libraries(UltraLiteDesktopStudio PRIVATE Qt6::Widgets Qt6::Network)

# zlib powers the JSHTML package ZIP import (deflate members). When
# it is missing the import still handles stored members and reports
# deflated ones honestly instead of failing the whole build.
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
  target_compile_definitions(UltraLiteDesktopStudio PRIVATE LDS_HAVE_ZLIB)
  target_link_libraries(UltraLiteDesktopStudio PRIVATE ZLIB::ZLIB)
endif()

if(WIN32)
  # SystemMonitor: iphlpapi (GetIfTable2) + ws2_32 (winsock2 headers).
  # AudioEngine: ole32/avrt. Platform: ole32 (Core Audio volume).
  target_link_libraries(UltraLiteDesktopStudio PRIVATE iphlpapi ws2_32 ole32 avrt comctl32 shell32)
  # _WIN32_WINNT >= Vista is required or netioapi.h hides GetIfTable2 /
  # MIB_IF_TABLE2 / FreeMibTable in MinGW headers (this was the v0.1
  # MSYS2 build error).
  target_compile_definitions(UltraLiteDesktopStudio PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601 WINVER=0x0601)
  # Application icon (assets/icon.ico, from the user's artwork).
  target_sources(UltraLiteDesktopStudio PRIVATE assets/app.rc)

  # ---- Runtime deployment (so the built exe runs outside the MSYS2 shell) ----
  # 1) windeployqt copies the Qt DLLs and plugins (platforms/qwindows.dll,
  #    tls/qschannelbackend.dll for HTTPS, styles, ...).
  #    MSYS2 names it windeployqt6.exe or windeployqt-qt6.exe.
  # 2) cmake/DeployRuntime.cmake then copies the transitive closure of the
  #    MinGW runtime DLLs (libstdc++/libgcc/libwinpthread and whatever the
  #    Qt DLLs themselves need, e.g. freetype/harfbuzz/ICU in MSYS2 builds)
  #    by scanning PE imports with objdump.
  get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  find_program(WINDEPLOYQT_EXE
    NAMES windeployqt6 windeployqt-qt6 windeployqt
    HINTS "${_mingw_bin}" "${CMAKE_PREFIX_PATH}/bin")
  if(WINDEPLOYQT_EXE)
    add_custom_command(TARGET UltraLiteDesktopStudio POST_BUILD
      COMMAND "${WINDEPLOYQT_EXE}" --release --no-translations
              --no-opengl-sw --no-system-d3d-compiler
              "$<TARGET_FILE:UltraLiteDesktopStudio>"
      COMMENT "Deploying Qt runtime with windeployqt")
  else()
    # Fallback: copy the Qt DLLs and the essential plugins by hand.
    message(WARNING "windeployqt not found - falling back to manual Qt deployment")
    get_target_property(_qmake_exe Qt6::qmake IMPORTED_LOCATION)
    execute_process(COMMAND "${_qmake_exe}" -query QT_INSTALL_PLUGINS
      OUTPUT_VARIABLE _qt_plugins OUTPUT_STRIP_TRAILING_WHITESPACE)
    foreach(_qt_lib Core Gui Widgets Network)
      add_custom_command(TARGET UltraLiteDesktopStudio POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:Qt6::${_qt_lib}>"
                "$<TARGET_FILE_DIR:UltraLiteDesktopStudio>")
    endforeach()
    foreach(_plugdir platforms tls styles imageformats iconengines)
      if(EXISTS "${_qt_plugins}/${_plugdir}")
        add_custom_command(TARGET UltraLiteDesktopStudio POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_directory
                  "${_qt_plugins}/${_plugdir}"
                  "$<TARGET_FILE_DIR:UltraLiteDesktopStudio>/${_plugdir}")
      endif()
    endforeach()
  endif()
  add_custom_command(TARGET UltraLiteDesktopStudio POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            "-DEXE=$<TARGET_FILE:UltraLiteDesktopStudio>"
            "-DBIN_DIR=${_mingw_bin}"
            "-DDEST_DIR=$<TARGET_FILE_DIR:UltraLiteDesktopStudio>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployRuntime.cmake"
    COMMENT "Copying MinGW runtime DLL closure")

  # Media helper (Python bridge for WinRT media metadata, onedir build).
  # If a prebuilt helper/MediaHelper folder exists (see
  # helper/README_media_helper.md), ship it next to the exe so the app can
  # start it on demand; PackageDist.cmake includes it in the zip as well.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/helper/MediaHelper")
    add_custom_command(TARGET UltraLiteDesktopStudio POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/helper/MediaHelper"
              "$<TARGET_FILE_DIR:UltraLiteDesktopStudio>/MediaHelper"
      COMMENT "Copying MediaHelper next to the exe")
  endif()

  # Same treatment for the one-shot transport-control helper.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/helper/MediaControlHelper")
    add_custom_command(TARGET UltraLiteDesktopStudio POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/helper/MediaControlHelper"
              "$<TARGET_FILE_DIR:UltraLiteDesktopStudio>/MediaControlHelper"
      COMMENT "Copying MediaControlHelper next to the exe")
  endif()

  # `cmake --build build --target dist` -> build/dist/UltraLiteDesktopStudio-win64.zip
  add_custom_target(dist
    COMMAND "${CMAKE_COMMAND}"
            "-DSRC_DIR=$<TARGET_FILE_DIR:UltraLiteDesktopStudio>"
            "-DOUT_DIR=${CMAKE_BINARY_DIR}/dist"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageDist.cmake"
    DEPENDS UltraLiteDesktopStudio
    COMMENT "Packaging UltraLiteDesktopStudio-win64.zip")
endif()

# ---- Optional JSHTML plugin, v0.14.0 / WebView2 backend v0.14.1 ----
# A separate module the exe QLibrary-loads on first use
# (arbitrary component LDS_ENABLE_JSHTML); the
# exe itself never links a browser engine. Backends:
#  - Windows: Microsoft Edge WebView2, driven through its COM API.
#    MSYS2 ships no Qt WebEngine for the MinGW GCC environments
#    (Chromium cannot be built with MinGW GCC), while the WebView2
#    runtime is preinstalled and evergreen on Windows 11. The SDK
#    headers + loader DLL are vendored under third_party/webview2
#    (Microsoft.Web.WebView2 1.0.4314.35); the loader is resolved
#    dynamically because its import lib is MSVC-only.
#  - Other platforms: Qt WebEngine, when the toolchain provides it.
# Without a backend the plugin is skipped and html_js widgets
# degrade to a placeholder.
option(LDS_ENABLE_JSHTML "Build the optional JSHTML plugin" ON)
if(LDS_ENABLE_JSHTML AND WIN32)
  add_library(LdsJsHtml MODULE src/jshtml_plugin/JsHtmlWebView2.cpp)
  target_include_directories(LdsJsHtml PRIVATE src
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/include")
  target_link_libraries(LdsJsHtml PRIVATE Qt6::Widgets ole32 oleaut32 uuid)
  target_compile_definitions(LdsJsHtml PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601 WINVER=0x0601)
  set_target_properties(LdsJsHtml PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:UltraLiteDesktopStudio>"
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:UltraLiteDesktopStudio>")
  # WebView2Loader.dll ships next to the plugin (the plugin loads it
  # dynamically); PackageDist picks up every *.dll in the exe dir.
  add_custom_command(TARGET LdsJsHtml POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/x64/WebView2Loader.dll"
            "$<TARGET_FILE_DIR:LdsJsHtml>/WebView2Loader.dll"
    COMMENT "Copying WebView2Loader.dll next to the JSHTML plugin")
  add_custom_command(TARGET LdsJsHtml POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            "-DEXE=$<TARGET_FILE:LdsJsHtml>"
            "-DBIN_DIR=${_mingw_bin}"
            "-DDEST_DIR=$<TARGET_FILE_DIR:LdsJsHtml>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployRuntime.cmake"
    COMMENT "Copying MinGW runtime DLL closure for the JSHTML plugin")
  if(TARGET dist)
    add_dependencies(dist LdsJsHtml)
  endif()
  message(STATUS "JSHTML plugin: enabled (WebView2 backend)")
elseif(LDS_ENABLE_JSHTML)
  find_package(Qt6 COMPONENTS WebEngineWidgets WebChannel QUIET)
  if(Qt6WebEngineWidgets_FOUND AND Qt6WebChannel_FOUND)
    add_library(LdsJsHtml MODULE src/jshtml_plugin/JsHtmlPlugin.cpp)
    target_include_directories(LdsJsHtml PRIVATE src)
    target_link_libraries(LdsJsHtml PRIVATE Qt6::WebEngineWidgets Qt6::WebChannel)
    set_target_properties(LdsJsHtml PROPERTIES
      PREFIX ""
      LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:UltraLiteDesktopStudio>"
      RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:UltraLiteDesktopStudio>")
    message(STATUS "JSHTML plugin: enabled (Qt WebEngine backend)")
  else()
    message(STATUS "JSHTML plugin: skipped (Qt6 WebEngineWidgets/WebChannel not found)")
  endif()
endif()

if(MSVC)
  target_compile_options(UltraLiteDesktopStudio PRIVATE /O2)
else()
  target_compile_options(UltraLiteDesktopStudio PRIVATE -O2)
endif()
