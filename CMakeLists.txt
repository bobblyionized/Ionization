cmake_minimum_required(VERSION 3.21)
project(Ionization LANGUAGES CXX RC)

set(IONIZATION_VERSION "0.0.0-dev" CACHE STRING "Version stamped into the exe (CI sets 1.0.<run>)")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Numeric version for the VERSIONINFO resource (dev builds -> 0.0.0.0).
string(REGEX MATCHALL "[0-9]+" _vparts "${IONIZATION_VERSION}")
list(LENGTH _vparts _vlen)
set(VER_MAJOR 0)
set(VER_MINOR 0)
set(VER_PATCH 0)
if(_vlen GREATER 2 AND NOT IONIZATION_VERSION MATCHES "dev")
  list(GET _vparts 0 VER_MAJOR)
  list(GET _vparts 1 VER_MINOR)
  list(GET _vparts 2 VER_PATCH)
endif()

# Embed every file under ui/ as an RCDATA resource named after its path
# (fonts/InterTight.woff2 -> FONTS_INTERTIGHT_WOFF2).
file(GLOB_RECURSE UI_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/ui/*")
set(UI_RC "${CMAKE_BINARY_DIR}/ui_resources.rc")
set(_rc "")
foreach(f ${UI_FILES})
  file(RELATIVE_PATH rel "${CMAKE_SOURCE_DIR}/ui" "${f}")
  if(rel MATCHES "\\.txt$")
    continue()
  endif()
  string(TOUPPER "${rel}" name)
  string(REGEX REPLACE "[^A-Z0-9]" "_" name "${name}")
  string(APPEND _rc "${name} RCDATA \"${f}\"\n")
endforeach()
file(GENERATE OUTPUT "${UI_RC}" CONTENT "${_rc}")

configure_file("${CMAKE_SOURCE_DIR}/src/app.rc.in" "${CMAKE_BINARY_DIR}/app.rc" @ONLY)

add_executable(Ionization WIN32
  src/main.cpp
  src/common.cpp
  src/renderer.cpp
  src/clock.cpp
  src/lut.cpp
  src/updater.cpp
  src/gamemode.cpp
  src/gpuload.cpp
  "${CMAKE_BINARY_DIR}/app.rc"
  "${UI_RC}"
)
set_source_files_properties("${UI_RC}" PROPERTIES OBJECT_DEPENDS "${UI_FILES}")

target_include_directories(Ionization PRIVATE src third_party/webview2/include)
target_compile_definitions(Ionization PRIVATE
  IONIZATION_VERSION="${IONIZATION_VERSION}"
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A00000C)
target_compile_options(Ionization PRIVATE /W3 /permissive- /utf-8 /MP)
target_link_options(Ionization PRIVATE /MANIFEST:EMBED)
target_link_libraries(Ionization PRIVATE
  "${CMAKE_SOURCE_DIR}/third_party/webview2/lib/WebView2LoaderStatic.lib"
  d3d11 dxgi d3dcompiler dcomp d2d1 dwrite windowscodecs winhttp bcrypt
  shlwapi shell32 ole32 oleaut32 user32 gdi32 dwmapi shcore advapi32 version pdh)

# Measurement tool (not shipped): a fullscreen GPU load that behaves like a game.
add_executable(fpsbench tools/fpsbench/main.cpp)
target_compile_definitions(fpsbench PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(fpsbench PRIVATE d3d11 dxgi d3dcompiler user32)
