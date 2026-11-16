cmake_minimum_required(VERSION 3.28)
project(gocube VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
	message(FATAL_ERROR "GoCube targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static runtime: one .exe, no DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Every dependency is in thirdparty/, as source pinned to a release (see
# thirdparty/README.md), built here: nothing is downloaded at build time.
# Their warnings are theirs: /W0.
file(GLOB ZLIB_SOURCES ${CMAKE_SOURCE_DIR}/thirdparty/zlib/*.c)
add_library(zlib STATIC ${ZLIB_SOURCES})
target_include_directories(zlib PUBLIC thirdparty/zlib)
target_compile_options(zlib PRIVATE /W0 /MP)
target_compile_definitions(zlib PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)

file(GLOB PNG_SOURCES ${CMAKE_SOURCE_DIR}/thirdparty/libpng/*.c ${CMAKE_SOURCE_DIR}/thirdparty/libpng/intel/*.c)
add_library(png STATIC ${PNG_SOURCES})
target_include_directories(png PUBLIC thirdparty/libpng)
target_link_libraries(png PUBLIC zlib)
target_compile_options(png PRIVATE /W0 /MP)
target_compile_definitions(png PRIVATE _CRT_SECURE_NO_WARNINGS)

add_library(imgui STATIC
	thirdparty/imgui/imgui.cpp
	thirdparty/imgui/imgui_demo.cpp
	thirdparty/imgui/imgui_draw.cpp
	thirdparty/imgui/imgui_tables.cpp
	thirdparty/imgui/imgui_widgets.cpp
	thirdparty/imgui/backends/imgui_impl_dx11.cpp
	thirdparty/imgui/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC thirdparty/imgui thirdparty/imgui/backends)
target_compile_options(imgui PRIVATE /W0 /MP)

add_library(miniaudio STATIC thirdparty/miniaudio/miniaudio.c)
target_include_directories(miniaudio PUBLIC thirdparty/miniaudio)
target_compile_definitions(miniaudio PUBLIC MA_NO_MP3 MA_NO_FLAC MA_NO_ENCODING MA_NO_GENERATION) # (WAV only)
target_compile_options(miniaudio PRIVATE /W0)

file(GLOB ENET_SOURCES thirdparty/enet/*.c)
add_library(enet STATIC ${ENET_SOURCES})
target_include_directories(enet PUBLIC thirdparty/enet/include)
target_compile_definitions(enet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
target_compile_options(enet PRIVATE /W0)
target_link_libraries(enet PUBLIC ws2_32 winmm)

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE thirdparty/nlohmann_json)
add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE thirdparty/doctest)

# Lua 5.4, from source (thirdparty/lua), compiled as C++ so its errors unwind
# C++ code properly, with a fixed string-hash seed (gocube_lua_config.h).
file(GLOB LUA_SOURCES ${CMAKE_SOURCE_DIR}/thirdparty/lua/*.c)
add_library(lua STATIC ${LUA_SOURCES})
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
target_include_directories(lua PUBLIC thirdparty/lua)
target_compile_options(lua PRIVATE /W0 /EHsc /MP /FIgocube_lua_config.h)
target_compile_definitions(lua PRIVATE _CRT_SECURE_NO_WARNINGS)

set(GOCUBE_DEFINES UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
set(GOCUBE_OPTIONS /W4 /permissive- /utf-8 /EHsc /MP)

# Everything that doesn't touch the window or the GPU: the voxel engine (from
# the Godot version's GDExtension) and mods' content. Shared with the tests.
add_library(gocube_core STATIC
	src/content/content.cpp
	src/content/content.h
	src/content/item_models.cpp
	src/content/item_models.h
	src/data/lua_data.cpp
	src/data/lua_data.h
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
	src/game/block_entities.cpp
	src/game/block_entities.h
	src/game/creature_models.cpp
	src/game/creature_models.h
	src/game/creatures.cpp
	src/game/creatures.h
	src/game/figure.cpp
	src/game/figure.h
	src/game/blocks.cpp
	src/game/blocks.h
	src/game/drops.cpp
	src/game/drops.h
	src/game/game.cpp
	src/game/game_health.cpp
	src/game/game.h
	src/game/hand.cpp
	src/game/hand.h
	src/game/inventory.cpp
	src/game/inventory.h
	src/game/player.cpp
	src/game/player.h
	src/mods/mod_pack.cpp
	src/mods/mod_pack.h
	src/mods/mods.cpp
	src/mods/mods.h
	src/world/save.cpp
	src/world/save.h
	src/world/world.cpp
	src/world/world.h
	src/game/vitals.cpp
	src/game/vitals.h
	src/net/session.cpp
	src/net/session.h
	src/net/wire.h
)
target_include_directories(gocube_core PUBLIC src)
target_link_libraries(gocube_core PUBLIC nlohmann_json PRIVATE lua png zlib bcrypt enet)
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
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${file} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/godot_color.hlsli
		COMMENT "Compiling ${file} (${entry})")
	set(SHADER_HEADERS ${SHADER_HEADERS} ${out} PARENT_SCOPE)
endfunction()
add_shader(shaders/chunk.hlsl vs_main vs_5_0 chunk_vs)
add_shader(shaders/chunk.hlsl ps_opaque ps_5_0 chunk_ps_opaque)
add_shader(shaders/chunk.hlsl ps_cutout ps_5_0 chunk_ps_cutout)
add_shader(shaders/chunk.hlsl ps_fluid ps_5_0 chunk_ps_fluid)
add_shader(shaders/lines.hlsl vs_main vs_5_0 lines_vs)
add_shader(shaders/lines.hlsl ps_main ps_5_0 lines_ps)
add_shader(shaders/sky.hlsl vs_main vs_5_0 sky_vs)
add_shader(shaders/sky.hlsl ps_main ps_5_0 sky_ps)
add_shader(shaders/items.hlsl vs_main vs_5_0 items_vs)
add_shader(shaders/items.hlsl ps_main ps_5_0 items_ps)
add_shader(shaders/creatures.hlsl vs_main vs_5_0 creatures_vs)
add_shader(shaders/creatures.hlsl ps_main ps_5_0 creatures_ps)
add_shader(shaders/blit.hlsl vs_main vs_5_0 blit_vs)
add_shader(shaders/blit.hlsl ps_main ps_5_0 blit_ps)

add_executable(gocube
	src/main.cpp
	src/log.h
	src/audio/audio.cpp
	src/audio/audio.h
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
	shaders/sky.hlsl
	shaders/items.hlsl
	shaders/blit.hlsl
	shaders/creatures.hlsl
	${SHADER_HEADERS}
)
set_source_files_properties(shaders/chunk.hlsl shaders/lines.hlsl shaders/sky.hlsl shaders/items.hlsl shaders/blit.hlsl
	shaders/creatures.hlsl
	PROPERTIES HEADER_FILE_ONLY TRUE) # (built by FXC above)
target_include_directories(gocube PRIVATE ${SHADER_DIR})
target_link_libraries(gocube PRIVATE gocube_core imgui png miniaudio d3d11 dxgi dxguid shell32 ole32)
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
	tests/net_play.h
	tests/play.h
	tests/test_mods.cpp
	tests/test_multiplayer.cpp
	tests/test_player.cpp
	tests/test_rendering.cpp
	tests/test_save.cpp
	tests/test_survival.cpp
	tests/test_worldgen.cpp
)
target_link_libraries(gocube_tests PRIVATE gocube_core doctest zlib bcrypt)
target_compile_definitions(gocube_tests PRIVATE GOCUBE_SOURCE_DIR="${CMAKE_SOURCE_DIR}" _CRT_SECURE_NO_WARNINGS)
target_compile_options(gocube_tests PRIVATE ${GOCUBE_OPTIONS})

# Multiplayer across two real processes (host and client over localhost).
add_executable(gocube_mp_smoke tests/mp_smoke.cpp tests/net_play.h tests/play.h)
target_link_libraries(gocube_mp_smoke PRIVATE gocube_core)
target_compile_definitions(gocube_mp_smoke PRIVATE GOCUBE_SOURCE_DIR="${CMAKE_SOURCE_DIR}" _CRT_SECURE_NO_WARNINGS)
target_compile_options(gocube_mp_smoke PRIVATE ${GOCUBE_OPTIONS})

enable_testing()
add_test(NAME unit COMMAND gocube_tests)
add_test(NAME multiplayer_processes COMMAND gocube_mp_smoke)
# Opens a hidden window, renders 60 frames on WARP (the software adapter, so it
# runs without a GPU) and fails if Direct3D 11's debug layer reported anything.
add_test(NAME smoke COMMAND gocube --warp --hidden --frames=60)
