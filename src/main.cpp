// GoCube (milestone 4): walk, fly, break and place blocks in a world that is
// saved in the same format as the Godot version; content and code come from
// mods (Lua, sandboxed), the base game ("core") first.
//
// Controls: click to look around with the mouse (Esc lets go). W A S D walk,
// Space jumps (swims up, flies up), Shift sneaks (flies down), Ctrl sprints,
// F flies. Left click breaks, right click places (or opens a door; sneak to
// place against it), the wheel picks the block to place, middle click picks
// the block aimed at.
//
// Options:
//   --world=NAME     play the saved world NAME (in %APPDATA%\GoCube\worlds;
//                    a new one is made if it doesn't exist)
// Mods are found in mods\ next to the game (built-in, the base game first) and
// in %APPDATA%\GoCube\mods (one folder per mod).
//
//   --world-dir=DIR  play the world in this folder (for example one the Godot
//                    version saved, in %APPDATA%\Godot\app_userdata\GoCube\worlds)
//                    Without either, the world isn't saved.
//   --seed=N         seed for a new world (default 777)
//   --generator=N    world generator version for an unsaved world (default: the newest)
//   --view=N         view distance in blocks (default 128)
//   --pos=X,Y,Z      start flying at this position (feet)
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
#include "game/blocks.h"
#include "game/player.h"
#include "log.h"
#include "mods/mods.h"
#include "platform/window.h"
#include "render/chunk_renderer.h"
#include "render/renderer.h"
#include "world/save.h"
#include "world/world.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace {

struct Options {
	gc::RendererOptions renderer;
	bool hidden = false;
	long frames = 0; // 0: until the window is closed
	std::string screenshot;
	std::string world_name;
	std::filesystem::path world_dir;
	uint32_t seed = 777;
	int generator = gc::GENERATOR_VERSION;
	int view = 128;
	bool has_pos = false;
	float pos[3] = {0, 0, 0};
	bool has_look = false;
	float look[2] = {0, 0};
};

std::filesystem::path utf8_path(const char *s) {
	return std::filesystem::path(reinterpret_cast<const char8_t *>(s));
}

std::string utf8_string(const std::filesystem::path &p) {
	const std::u8string u = p.u8string();
	return std::string(u.begin(), u.end());
}

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
		const char *v = nullptr;
		if (!strcmp(a, "--warp")) {
			o.renderer.warp = true;
		} else if (!strcmp(a, "--hidden")) {
			o.hidden = true;
		} else if ((v = value("--frames="))) {
			o.frames = strtol(v, nullptr, 10);
		} else if ((v = value("--screenshot="))) {
			o.screenshot = v;
		} else if ((v = value("--world="))) {
			o.world_name = v;
		} else if ((v = value("--world-dir="))) {
			o.world_dir = utf8_path(v);
		} else if ((v = value("--seed="))) {
			o.seed = uint32_t(strtoll(v, nullptr, 10));
		} else if ((v = value("--generator="))) {
			o.generator = int(strtol(v, nullptr, 10));
		} else if ((v = value("--view="))) {
			o.view = int(strtol(v, nullptr, 10));
		} else if ((v = value("--pos="))) {
			o.has_pos = sscanf_s(v, "%f,%f,%f", &o.pos[0], &o.pos[1], &o.pos[2]) == 3;
		} else if ((v = value("--look="))) {
			o.has_look = sscanf_s(v, "%f,%f", &o.look[0], &o.look[1]) == 2;
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

// %APPDATA%\GoCube
std::filesystem::path user_dir() {
	PWSTR appdata = nullptr;
	std::filesystem::path out;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
		out = std::filesystem::path(appdata) / "GoCube";
	}
	CoTaskMemFree(appdata);
	return out;
}

struct Controls {
	bool fly_key_was_down = false;
	int selected = 0; // index into the placeable blocks
};

gc::PlayerInput read_input(const gc::Window &window) {
	gc::PlayerInput in;
	if (!window.mouse_captured()) {
		return in;
	}
	in.forward = float(window.key_down('W')) - float(window.key_down('S'));
	in.right = float(window.key_down('D')) - float(window.key_down('A'));
	in.jump = window.key_down(VK_SPACE);
	in.sneak = window.key_down(VK_SHIFT);
	in.sprint = window.key_down(VK_CONTROL);
	return in;
}

void draw_ui(const gc::Renderer &renderer, const gc::Player &player, const gc::World &world,
		const gc::ChunkRenderer &chunks, const gc::WorldSave *save, uint32_t seed, const std::string &block,
		const gc::Mods &mods, bool captured) {
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
		ImGui::Text("Position %.1f, %.1f, %.1f%s  -  seed %u", player.position[0], player.position[1],
				player.position[2], player.flying ? "  (flying)" : "", seed);
		ImGui::Text("Chunks: %d loaded, %d meshed, %d drawn, %d jobs waiting", ws.loaded, ws.meshed, cs.drawn, ws.queued);
		if (save) {
			ImGui::Text("World: %s  (%d edited chunks not saved yet)", utf8_string(save->dir().filename()).c_str(), ws.dirty);
		} else {
			ImGui::TextDisabled("World not saved (use --world=NAME)");
		}
		ImGui::Text("Block: %s  (wheel to change, middle click to pick)", block.c_str());
		std::string names;
		for (const gc::ModInfo &m : mods.loaded()) {
			names += (names.empty() ? "" : ", ") + m.id + " " + m.version;
		}
		ImGui::Text("Mods: %s", names.c_str());
		for (const auto &[id, why] : mods.problems()) {
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "Mod %s: %s", id.c_str(), why.c_str());
		}
		if (!captured) {
			ImGui::TextDisabled("Click to play. W A S D, Space, Shift, Ctrl, F to fly; left click breaks, right click places");
		}
	}
	ImGui::End();
	if (captured) { // crosshair
		ImDrawList *dl = ImGui::GetForegroundDrawList();
		const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
		const ImU32 col = IM_COL32(255, 255, 255, 200);
		dl->AddLine(ImVec2(c.x - 8, c.y), ImVec2(c.x + 9, c.y), col, 2.0f);
		dl->AddLine(ImVec2(c.x, c.y - 8), ImVec2(c.x, c.y + 9), col, 2.0f);
	}
}

gc::Json player_state(const gc::Player &p) {
	gc::Json s = gc::Json::object();
	s["position"] = {p.position[0], p.position[1], p.position[2]};
	s["rotation"] = {p.pitch, p.yaw};
	s["flying"] = p.flying;
	return s;
}

void restore_player(gc::Player &p, const gc::Json &s) {
	auto pos = s.find("position");
	if (pos != s.end() && pos->is_array() && pos->size() == 3) {
		for (int i = 0; i < 3; i++) {
			p.position[i] = (*pos)[i].get<float>();
		}
	}
	auto rot = s.find("rotation");
	if (rot != s.end() && rot->is_array() && rot->size() == 2) {
		p.pitch = (*rot)[0].get<float>();
		p.yaw = (*rot)[1].get<float>();
	}
	auto fly = s.find("flying");
	p.flying = fly != s.end() && fly->is_boolean() && fly->get<bool>();
}

} // namespace

int main(int argc, char **argv) {
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	const Options options = parse(argc, argv);

	// Content: the mods (the base game first).
	const std::filesystem::path builtin_mods = find_mods();
	if (builtin_mods.empty()) {
		gc::log("Couldn't find the mods folder (with the base game, mods\\core)");
		return 1;
	}
	gc::Content content;
	gc::Mods mods(content);
	mods.load_all({builtin_mods, user_dir() / "mods"});
	if (mods.loaded().empty() || mods.loaded().front().id != "core") {
		auto it = mods.problems().find("core");
		gc::log("Couldn't load the base game: %s", it != mods.problems().end() ? it->second.c_str() : "it's missing");
		return 1;
	}

	// The world: its folder decides the seed, generator and block ids.
	std::optional<gc::WorldSave> save;
	std::filesystem::path dir = options.world_dir;
	if (dir.empty() && !options.world_name.empty()) {
		dir = user_dir() / "worlds" / utf8_path(options.world_name.c_str());
	}
	uint32_t seed = options.seed;
	int generator_version = options.generator;
	if (!dir.empty()) {
		save.emplace();
		if (std::string e = save->open(dir, options.seed); !e.empty()) {
			gc::log("Can't open the world in %s: %s", utf8_string(dir).c_str(), e.c_str());
			return 1;
		}
		seed = save->seed();
		generator_version = save->generator();
		content.freeze(save->block_ids());
		save->set_block_ids(content.block_ids());
		gc::log("World %s: seed %u, generator %d, %s", utf8_string(dir).c_str(), seed, generator_version,
				save->mode().c_str());
	} else {
		content.freeze();
	}
	const gc::BlockTable table = gc::build_block_table(content);
	const gc::BlockRules rules(content);
	gocube::Generator generator;
	gc::configure_generator(generator, content, seed, generator_version);
	gc::log("%zu blocks, %zu atlas tiles", table.blocks.size(), table.atlas.tiles.size());

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

	auto world = std::make_unique<gc::World>(table.blocks, table.atlas.layout, generator,
			save ? utf8_string(save->chunks_dir()) : std::string());
	world->set_view_distance(options.view);
	mods.attach(world.get(), &rules);

	gc::Player player;
	player.position[0] = 0.5f;
	player.position[1] = float(generator.height_at(0, 0)) + 1.0f; // on the generated ground
	player.position[2] = 0.5f;
	if (save) {
		restore_player(player, save->player());
	}
	if (options.has_pos) {
		std::copy(options.pos, options.pos + 3, player.position);
		player.flying = true; // stays where it was put
	}
	if (options.has_look) {
		player.yaw = DirectX::XMConvertToRadians(options.look[0]);
		player.pitch = DirectX::XMConvertToRadians(options.look[1]);
	}
	float previous_position[3];
	std::copy(player.position, player.position + 3, previous_position);
	Controls controls;
	if (const auto &list = rules.placeable(); !list.empty()) {
		auto it = std::find(list.begin(), list.end(), "core:stone");
		controls.selected = it != list.end() ? int(it - list.begin()) : 0;
	}

	gc::Camera camera;
	camera.far_z = float(options.view) * 2.0f;
	gc::SkyParams sky;
	sky.fog_end = float(options.view);
	sky.fog_start = sky.fog_end * 0.7f;
	const float clear[4] = {sky.fog_color.x, sky.fog_color.y, sky.fog_color.z, 1.0f};

	auto save_all = [&] {
		if (save) {
			const int chunks_saved = world->save_modified();
			save->save(player_state(player));
			gc::log("Saved the world (%d chunks)", chunks_saved);
		}
	};

	using clock = std::chrono::steady_clock;
	const auto start = clock::now();
	auto last = start, last_save = start;
	float accumulator = 0;
	double tick_accumulator = 0;
	constexpr double TICK = 0.05; // mods' tick event: 20 a second
	long frame = 0;
	bool ok = true;
	while (window.pump()) {
		const auto now = clock::now();
		const float dt = std::min(0.25f, std::chrono::duration<float>(now - last).count());
		last = now;
		if (window.take_resized()) {
			renderer.resize(window.width(), window.height());
		}
		const bool left = window.take_pressed(gc::Window::LEFT);
		const bool right = window.take_pressed(gc::Window::RIGHT);
		const bool middle = window.take_pressed(gc::Window::MIDDLE);
		const float wheel = window.take_wheel();
		const bool captured = window.mouse_captured();
		if (left && !captured && !ImGui::GetIO().WantCaptureMouse) {
			window.set_mouse_captured(true); // (that click only captures)
		}
		if (captured && window.key_down(VK_ESCAPE)) {
			window.set_mouse_captured(false);
		}
		if (window.minimized()) {
			Sleep(16); // nothing to present to
			continue;
		}

		// Looking around, then fixed physics steps; the camera is drawn between
		// the last two steps, so movement is smooth at any frame rate.
		float mx, my;
		window.take_mouse_delta(mx, my);
		const float sensitivity = 0.003f; // radians per mouse count (the Godot version's default)
		player.yaw -= mx * sensitivity;
		player.pitch = std::clamp(player.pitch - my * sensitivity, -1.55f, 1.55f);
		const bool fly_key = captured && window.key_down('F');
		if (fly_key && !controls.fly_key_was_down) {
			player.flying = !player.flying;
			player.velocity[1] = 0;
		}
		controls.fly_key_was_down = fly_key;
		const gc::PlayerInput input = read_input(window);
		accumulator += dt;
		while (accumulator >= gc::Player::STEP) {
			std::copy(player.position, player.position + 3, previous_position);
			player.step(input, *world, rules);
			accumulator -= gc::Player::STEP;
		}
		const float alpha = accumulator / gc::Player::STEP;

		// Breaking, placing and picking.
		const auto &placeable = rules.placeable();
		if (!placeable.empty() && wheel != 0 && captured) {
			const int n = int(placeable.size());
			controls.selected = ((controls.selected - int(wheel)) % n + n) % n;
		}
		const std::string block = placeable.empty() ? "" : placeable[controls.selected];
		std::vector<gc::BlockChange> changes;
		if (captured && left) {
			player.break_target(*world, rules, &changes);
		}
		if (captured && right) {
			player.use_or_place(*world, rules, block, window.key_down(VK_SHIFT), &changes);
		}
		for (const gc::BlockChange &c : changes) {
			if (c.placed) {
				mods.block_placed(c.pos, c.id);
			} else {
				mods.block_broken(c.pos, c.id);
			}
		}
		tick_accumulator += dt;
		while (tick_accumulator >= TICK) {
			mods.tick(TICK);
			tick_accumulator -= TICK;
		}
		if (captured && middle) {
			const gc::RayHit hit = player.target(*world);
			if (hit.hit) {
				const std::string base = rules.base_of(rules.id_of(hit.block));
				auto it = std::find(placeable.begin(), placeable.end(), base);
				if (it != placeable.end()) {
					controls.selected = int(it - placeable.begin());
				}
			}
		}

		camera.position = {previous_position[0] + (player.position[0] - previous_position[0]) * alpha,
				previous_position[1] + (player.position[1] - previous_position[1]) * alpha + gc::Player::EYE_HEIGHT,
				previous_position[2] + (player.position[2] - previous_position[2]) * alpha};
		camera.yaw = player.yaw;
		camera.pitch = player.pitch;

		world->update(player.position[0], player.position[1], player.position[2]);
		for (const gc::ChunkPos &p : world->take_unloaded()) {
			chunks.remove(p);
		}
		for (const gc::ChunkMesh &m : world->take_meshes()) {
			chunks.upload(renderer.device(), m);
		}
		if (save && std::chrono::duration<float>(now - last_save).count() >= 60.0f) {
			save_all(); // autosave
			last_save = now;
		}

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		draw_ui(renderer, player, *world, chunks, save ? &*save : nullptr, seed,
				placeable.empty() ? "" : placeable[controls.selected], mods, window.mouse_captured());
		ImGui::Render();

		sky.time = std::chrono::duration<float>(now - start).count();
		renderer.begin_frame(clear);
		chunks.draw(renderer.context(), camera, float(renderer.width()) / float(std::max(1, renderer.height())), sky);
		if (const gc::RayHit hit = player.target(*world); hit.hit) {
			std::vector<gocube::Box> boxes;
			world->block_boxes(hit.position.x, hit.position.y, hit.position.z, hit.block, false, boxes);
			chunks.draw_outline(renderer.context(), boxes);
		}

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

	save_all();
	mods.attach(nullptr, nullptr);
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
