cmake_minimum_required(VERSION 3.28)
project(gocube VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
	message(FATAL_ERROR "GoCube targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static runtime, matching the x64-windows-static vcpkg triplet: one .exe, no DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(imgui CONFIG REQUIRED)

add_executable(gocube
	src/main.cpp
	src/log.h
	src/platform/window.cpp
	src/platform/window.h
	src/render/renderer.cpp
	src/render/renderer.h
)
target_include_directories(gocube PRIVATE src)
target_link_libraries(gocube PRIVATE imgui::imgui d3d11 dxgi dxguid)
target_compile_definitions(gocube PRIVATE
	UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
	GOCUBE_VERSION="${PROJECT_VERSION}"
	$<$<CONFIG:Debug>:GOCUBE_DEBUG>
)
target_compile_options(gocube PRIVATE /W4 /permissive- /utf-8 /EHsc)
# Debug builds keep a console for logs; release builds are a plain window app.
set_target_properties(gocube PROPERTIES WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
target_link_options(gocube PRIVATE $<$<NOT:$<CONFIG:Debug>>:/ENTRY:mainCRTStartup>)

enable_testing()
# Opens a hidden window, renders 60 frames on WARP (the software adapter, so it
# runs without a GPU) and fails if Direct3D 11's debug layer reported anything.
add_test(NAME smoke COMMAND gocube --warp --hidden --frames=60)
