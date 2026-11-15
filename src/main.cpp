// GoCube: a voxel world streamed around a flying camera (milestone 2).
//
// Controls: click to look around with the mouse (Esc lets go), W A S D to fly,
// Space up, Shift down, Ctrl faster.
//
// Options:
//   --seed=N         world seed (default 777)
//   --generator=N    world generator version (default: the newest)
//   --view=N         view distance in blocks (default 128)
//   --pos=X,Y,Z      start position (default: above the ground at 0, 0)
//   --look=YAW,PITCH start view direction in degrees (0, 0: towards -z)
//   --warp           render on WARP, Windows' software adapter (no GPU needed)
//   --hidden         don't show the window (tests)
//   --frames=N       quit after N frames; the exit code is 1 if the debug layer
//                    reported errors or warnings. With --screenshot, counted
//                    from when the world around has finished loading.
//   --screenshot=F   save the last frame as a PNG
//   --no-vsync       present as fast as possible
//   --debug / --no-debug   force the Direct3D 11 debug layer on or off
//                    (default: on in debug builds)

#include "content/content.h"
#include "log.h"
#include "platform/window.h"
#include "render/chunk_renderer.h"
#include "render/renderer.h"
#include "world/world.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

namespace {

struct Options {
	gc::RendererOptions renderer;
	bool hidden = false;
	long frames = 0; // 0: until the window is closed
	std::string screenshot;
	uint32_t seed = 777;
	int generator = gc::GENERATOR_VERSION;
	int view = 128;
	bool has_pos = false;
	float pos[3] = {0, 0, 0};
	float look[2] = {0, 0};
};

Options parse(int argc, char **argv) {
	Options o;
#ifdef GOCUBE_DEBUG
	o.renderer.debug = true;
#endif
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		auto value = [&](const char *name) -> const char * {
			const size_t n = strlen(name);
			return strncmp(a, name, n) == 0 ? a + n : nullptr;
		};
		if (!strcmp(a, "--warp")) {
			o.renderer.warp = true;
		} else if (!strcmp(a, "--hidden")) {
			o.hidden = true;
		} else if (const char *v = value("--frames=")) {
			o.frames = strtol(v, nullptr, 10);
		} else if (const char *v2 = value("--screenshot=")) {
			o.screenshot = v2;
		} else if (const char *v3 = value("--seed=")) {
			o.seed = uint32_t(strtoll(v3, nullptr, 10));
		} else if (const char *v4 = value("--generator=")) {
			o.generator = int(strtol(v4, nullptr, 10));
		} else if (const char *v5 = value("--view=")) {
			o.view = int(strtol(v5, nullptr, 10));
		} else if (const char *v6 = value("--pos=")) {
			o.has_pos = sscanf_s(v6, "%f,%f,%f", &o.pos[0], &o.pos[1], &o.pos[2]) == 3;
		} else if (const char *v7 = value("--look=")) {
			sscanf_s(v7, "%f,%f", &o.look[0], &o.look[1]);
		} else if (!strcmp(a, "--no-vsync")) {
			o.renderer.vsync = false;
		} else if (!strcmp(a, "--debug")) {
			o.renderer.debug = true;
		} else if (!strcmp(a, "--no-debug")) {
			o.renderer.debug = false;
		} else {
			gc::log("Unknown option %s (ignored)", a);
		}
	}
	return o;
}

// The folder holding mods/: next to the game, or a parent of it (a build tree).
std::filesystem::path find_mods() {
	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	for (std::filesystem::path dir = std::filesystem::path(exe).parent_path(); !dir.empty(); dir = dir.parent_path()) {
		if (std::filesystem::exists(dir / "mods" / "core" / "mod.json")) {
			return dir / "mods";
		}
		if (dir == dir.parent_path()) {
			break;
		}
	}
	return {};
}

void fly(gc::Camera &camera, gc::Window &window, float dt) {
	float dx, dy;
	window.take_mouse_delta(dx, dy);
	const float sensitivity = 0.0025f; // radians per mouse count
	camera.yaw -= dx * sensitivity;
	camera.pitch = std::clamp(camera.pitch - dy * sensitivity, -1.55f, 1.55f);

	float move[3] = {0, 0, 0};
	const float fx = -std::sin(camera.yaw), fz = -std::cos(camera.yaw); // forward on the ground
	auto add = [&](float x, float y, float z) {
		move[0] += x;
		move[1] += y;
		move[2] += z;
	};
	if (window.key_down('W')) add(fx, 0, fz);
	if (window.key_down('S')) add(-fx, 0, -fz);
	if (window.key_down('A')) add(fz, 0, -fx);
	if (window.key_down('D')) add(-fz, 0, fx);
	if (window.key_down(VK_SPACE)) add(0, 1, 0);
	if (window.key_down(VK_SHIFT)) add(0, -1, 0);
	const float len = std::sqrt(move[0] * move[0] + move[1] * move[1] + move[2] * move[2]);
	if (len > 0) {
		const float speed = (window.key_down(VK_CONTROL) ? 40.0f : 10.0f) * dt / len; // blocks per second
		camera.position.x += move[0] * speed;
		camera.position.y += move[1] * speed;
		camera.position.z += move[2] * speed;
	}
}

void draw_ui(const gc::Renderer &renderer, const gc::Camera &camera, const gc::World &world,
		const gc::ChunkRenderer &chunks, uint32_t seed, bool captured) {
	const ImGuiIO &io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.6f);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoInputs;
	if (ImGui::Begin("Info", nullptr, flags)) {
		const gc::WorldStats ws = world.stats();
		const gc::ChunkRenderStats &cs = chunks.stats();
		ImGui::Text("GoCube %s  -  %s", GOCUBE_VERSION, renderer.adapter_name().c_str());
		ImGui::Text("%.0f fps (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);
		ImGui::Text("Position %.1f, %.1f, %.1f  -  seed %u", camera.position.x, camera.position.y, camera.position.z, seed);
		ImGui::Text("Chunks: %d loaded, %d meshed, %d drawn, %d jobs waiting", ws.loaded, ws.meshed, cs.drawn, ws.queued);
		ImGui::Text("%.2f M triangles", double(cs.triangles) / 1e6);
		if (!captured) {
			ImGui::TextDisabled("Click to look around; W A S D, Space, Shift to fly (Ctrl: faster)");
		}
	}
	ImGui::End();
}

} // namespace

int main(int argc, char **argv) {
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	const Options options = parse(argc, argv);

	// Content: the base game (more mods come with the Lua milestone).
	const std::filesystem::path mods = find_mods();
	gc::Content content;
	if (std::string e = mods.empty() ? "no mods folder found" : content.load_mod(mods / "core"); !e.empty()) {
		gc::log("Couldn't load the base game: %s", e.c_str());
		return 1;
	}
	content.freeze();
	const gc::BlockTable table = gc::build_block_table(content);
	gocube::Generator generator;
	gc::configure_generator(generator, content, options.seed, options.generator);
	gc::log("%zu blocks, %zu atlas tiles; seed %u, generator %d", table.blocks.size(), table.atlas.tiles.size(),
			options.seed, options.generator);

	gc::Window window;
	if (!window.create(L"GoCube", 1280, 720, !options.hidden)) {
		gc::log("Couldn't create the window");
		return 1;
	}
	gc::Renderer renderer;
	if (!renderer.init(window.hwnd(), window.width(), window.height(), options.renderer)) {
		gc::log("Couldn't start Direct3D 11");
		return 1;
	}
	gc::ChunkRenderer chunks;
	if (!chunks.init(renderer.device(), table.atlas)) {
		gc::log("Couldn't set up the terrain renderer");
		return 1;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr; // no imgui.ini next to the game
	ImGui::StyleColorsDark();
	ImGui_ImplWin32_Init(window.hwnd());
	if (!renderer.init_imgui()) {
		gc::log("Couldn't start Dear ImGui's Direct3D 11 backend");
		return 1;
	}

	auto world = std::make_unique<gc::World>(table.blocks, table.atlas.layout, generator);
	world->set_view_distance(options.view);

	gc::Camera camera;
	camera.far_z = float(options.view) * 2.0f;
	if (options.has_pos) {
		camera.position = {options.pos[0], options.pos[1], options.pos[2]};
	} else {
		camera.position = {0.5f, float(generator.height_at(0, 0)) + 3.0f, 0.5f};
	}
	camera.yaw = DirectX::XMConvertToRadians(options.look[0]);
	camera.pitch = DirectX::XMConvertToRadians(options.look[1]);

	gc::SkyParams sky;
	sky.fog_end = float(options.view);
	sky.fog_start = sky.fog_end * 0.7f;
	const float clear[4] = {sky.fog_color.x, sky.fog_color.y, sky.fog_color.z, 1.0f};

	using clock = std::chrono::steady_clock;
	const auto start = clock::now();
	auto last = start;
	long frame = 0;
	bool ok = true;
	while (window.pump()) {
		const auto now = clock::now();
		const float dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
		last = now;
		if (window.take_resized()) {
			renderer.resize(window.width(), window.height());
		}
		if (window.take_click() && !window.mouse_captured() && !ImGui::GetIO().WantCaptureMouse) {
			window.set_mouse_captured(true);
		}
		if (window.mouse_captured() && window.key_down(VK_ESCAPE)) {
			window.set_mouse_captured(false);
		}
		if (window.minimized()) {
			Sleep(16); // nothing to present to
			continue;
		}
		if (window.mouse_captured()) {
			fly(camera, window, dt);
		}

		world->update(camera.position.x, camera.position.y, camera.position.z);
		for (const gc::ChunkPos &p : world->take_unloaded()) {
			chunks.remove(p);
		}
		for (const gc::ChunkMesh &m : world->take_meshes()) {
			chunks.upload(renderer.device(), m);
		}

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		draw_ui(renderer, camera, *world, chunks, options.seed, window.mouse_captured());
		ImGui::Render();

		sky.time = std::chrono::duration<float>(now - start).count();
		renderer.begin_frame(clear);
		chunks.draw(renderer.context(), camera, float(renderer.width()) / float(std::max(1, renderer.height())), sky);

		// With a screenshot, count frames once the world around has loaded.
		const gc::WorldStats ws = world->stats();
		bool counting = options.screenshot.empty() || ws.settled;
		if (!counting && sky.time > 60.0f) {
			static bool warned = false;
			if (!warned) {
				gc::log("The world hasn't finished loading after 60 s (%d jobs left); taking the screenshot anyway", ws.queued);
				warned = true;
			}
			counting = true;
		}
		const bool last_frame = options.frames > 0 && counting && ++frame >= options.frames;
		if (!renderer.end_frame(last_frame && !options.screenshot.empty() ? options.screenshot.c_str() : nullptr)) {
			ok = false;
			break;
		}
		if (last_frame) {
			break;
		}
	}

	world.reset(); // stops the workers
	const int messages = renderer.debug_messages();
	renderer.shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	if (messages > 0) {
		gc::log("The Direct3D 11 debug layer reported %d errors or warnings", messages);
	}
	return ok && messages == 0 ? 0 : 1;
}
