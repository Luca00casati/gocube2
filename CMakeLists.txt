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
find_package(nlohmann_json CONFIG REQUIRED)
find_package(doctest CONFIG REQUIRED)
find_package(Stb REQUIRED)

# Lua 5.4, from source (thirdparty/lua), compiled as C++ so its errors unwind
# C++ code properly, with a fixed string-hash seed (gocube_lua_config.h).
file(GLOB LUA_SOURCES ${CMAKE_SOURCE_DIR}/thirdparty/lua/*.c)
add_library(lua STATIC ${LUA_SOURCES})
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
target_include_directories(lua PUBLIC thirdparty/lua)
target_compile_options(lua PRIVATE /W0 /EHsc /FIgocube_lua_config.h)
target_compile_definitions(lua PRIVATE _CRT_SECURE_NO_WARNINGS)

set(GOCUBE_DEFINES UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
set(GOCUBE_OPTIONS /W4 /permissive- /utf-8 /EHsc /MP)

# Everything that doesn't touch the window or the GPU: the voxel engine (from
# the Godot version's GDExtension) and mods' content. Shared with the tests.
add_library(gocube_core STATIC
	src/content/content.cpp
	src/content/content.h
	src/voxel/chunk.h
	src/voxel/generator.cpp
	src/voxel/generator.h
	src/voxel/light.cpp
	src/voxel/light.h
	src/voxel/mesher.cpp
	src/voxel/mesher.h
	src/voxel/noise.h
	src/voxel/storage.cpp
	src/voxel/storage.h
)
target_sources(gocube_core PRIVATE
	src/game/blocks.cpp
	src/game/blocks.h
	src/game/drops.cpp
	src/game/drops.h
	src/game/game.cpp
	src/game/game.h
	src/game/inventory.cpp
	src/game/inventory.h
	src/game/player.cpp
	src/game/player.h
	src/mods/mods.cpp
	src/mods/mods.h
	src/world/save.cpp
	src/world/save.h
	src/world/world.cpp
	src/world/world.h
)
target_include_directories(gocube_core PUBLIC src PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(gocube_core PUBLIC nlohmann_json::nlohmann_json PRIVATE lua bcrypt)
target_compile_definitions(gocube_core PUBLIC ${GOCUBE_DEFINES})
target_compile_options(gocube_core PRIVATE ${GOCUBE_OPTIONS})
# The voxel engine is the Godot version's, unchanged: quiet its warnings here
# rather than diverge from it.
set_source_files_properties(src/voxel/generator.cpp src/voxel/light.cpp src/voxel/mesher.cpp src/voxel/storage.cpp
	PROPERTIES COMPILE_OPTIONS "/wd4100;/wd4244;/wd4005"
	COMPILE_DEFINITIONS "_CRT_SECURE_NO_WARNINGS;_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING")

# Shaders: compiled by FXC (shader model 5.0) into headers at build time.
find_program(FXC fxc REQUIRED DOC "The Windows SDK's shader compiler (in the Visual Studio developer environment)")
set(SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(SHADER_HEADERS)
function(add_shader file entry profile name)
	set(out ${SHADER_DIR}/${name}.h)
	add_custom_command(OUTPUT ${out}
		COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
		COMMAND ${FXC} /nologo /T ${profile} /E ${entry} /O3 /WX /Vn g_${name} /Fh ${out} ${CMAKE_CURRENT_SOURCE_DIR}/${file}
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${file}
		COMMENT "Compiling ${file} (${entry})")
	set(SHADER_HEADERS ${SHADER_HEADERS} ${out} PARENT_SCOPE)
endfunction()
add_shader(shaders/chunk.hlsl vs_main vs_5_0 chunk_vs)
add_shader(shaders/chunk.hlsl ps_opaque ps_5_0 chunk_ps_opaque)
add_shader(shaders/chunk.hlsl ps_cutout ps_5_0 chunk_ps_cutout)
add_shader(shaders/chunk.hlsl ps_fluid ps_5_0 chunk_ps_fluid)
add_shader(shaders/lines.hlsl vs_main vs_5_0 lines_vs)
add_shader(shaders/lines.hlsl ps_main ps_5_0 lines_ps)

add_executable(gocube
	src/main.cpp
	src/log.h
	src/platform/window.cpp
	src/platform/window.h
	src/render/chunk_renderer.cpp
	src/render/chunk_renderer.h
	src/render/renderer.cpp
	src/render/renderer.h
	src/ui/screens.cpp
	src/ui/screens.h
	shaders/chunk.hlsl
	shaders/lines.hlsl
	${SHADER_HEADERS}
)
set_source_files_properties(shaders/chunk.hlsl shaders/lines.hlsl PROPERTIES HEADER_FILE_ONLY TRUE) # (built by FXC above)
target_include_directories(gocube PRIVATE ${SHADER_DIR} ${Stb_INCLUDE_DIR})
target_link_libraries(gocube PRIVATE gocube_core imgui::imgui d3d11 dxgi dxguid shell32 ole32)
target_compile_definitions(gocube PRIVATE
	GOCUBE_VERSION="${PROJECT_VERSION}"
	$<$<CONFIG:Debug>:GOCUBE_DEBUG>
)
target_compile_options(gocube PRIVATE ${GOCUBE_OPTIONS})
# Debug builds keep a console for logs; release builds are a plain window app.
set_target_properties(gocube PROPERTIES WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
target_link_options(gocube PRIVATE $<$<NOT:$<CONFIG:Debug>>:/ENTRY:mainCRTStartup>)

add_executable(gocube_tests
	tests/helpers.h
	tests/main.cpp
	tests/test_mods.cpp
	tests/test_player.cpp
	tests/test_save.cpp
	tests/test_survival.cpp
	tests/test_worldgen.cpp
)
target_link_libraries(gocube_tests PRIVATE gocube_core doctest::doctest bcrypt)
target_compile_definitions(gocube_tests PRIVATE GOCUBE_SOURCE_DIR="${CMAKE_SOURCE_DIR}" _CRT_SECURE_NO_WARNINGS)
target_compile_options(gocube_tests PRIVATE ${GOCUBE_OPTIONS})

enable_testing()
add_test(NAME unit COMMAND gocube_tests)
# Opens a hidden window, renders 60 frames on WARP (the software adapter, so it
# runs without a GPU) and fails if Direct3D 11's debug layer reported anything.
add_test(NAME smoke COMMAND gocube --warp --hidden --frames=60)
