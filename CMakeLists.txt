cmake_minimum_required(VERSION 3.20)
project(Magnifier VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Magnifier is Windows-only (Desktop Duplication + DirectComposition).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT: the exe runs without installing the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.8
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(nlohmann_json imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

set(MAGNIFIER_DEFS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
set(MAGNIFIER_FLAGS /W4 /permissive- /utf-8 /EHsc)

# Platform-independent core, unit tested.
add_library(magnifier_core STATIC src/app/Settings.cpp)
target_include_directories(magnifier_core PUBLIC src)
target_link_libraries(magnifier_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(magnifier_core PRIVATE ${MAGNIFIER_FLAGS})

add_executable(Magnifier WIN32
    src/main.cpp
    src/app/Log.cpp
    src/capture/DesktopDup.cpp
    src/render/Overlay.cpp
    src/render/LensRenderer.cpp
    src/ui/SettingsWindow.cpp
    src/ui/HotkeyName.cpp
    src/input/BindingWatcher.cpp)
target_compile_definitions(Magnifier PRIVATE ${MAGNIFIER_DEFS})
target_compile_options(Magnifier PRIVATE ${MAGNIFIER_FLAGS})
target_link_libraries(Magnifier PRIVATE magnifier_core imgui
    d3d11 dxgi dcomp d3dcompiler shell32 user32)

enable_testing()
add_executable(magnifier_tests tests/test_main.cpp)
target_link_libraries(magnifier_tests PRIVATE magnifier_core)
target_compile_options(magnifier_tests PRIVATE ${MAGNIFIER_FLAGS})
add_test(NAME magnifier_tests COMMAND magnifier_tests)
