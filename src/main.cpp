// GoCube: a window with Direct3D 11 and Dear ImGui (milestone 1).
//
// Options:
//   --warp        render on WARP, Windows' software adapter (no GPU needed)
//   --hidden      don't show the window (tests)
//   --frames=N    quit after N frames; the exit code is 1 if the debug layer
//                 reported errors or warnings
//   --no-vsync    present as fast as possible
//   --debug / --no-debug   force the Direct3D 11 debug layer on or off
//                 (default: on in debug builds)

#include "log.h"
#include "platform/window.h"
#include "render/renderer.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <cstdlib>
#include <cstring>

namespace {

struct Options {
	gc::RendererOptions renderer;
	bool hidden = false;
	long frames = 0; // 0: until the window is closed
};

Options parse(int argc, char **argv) {
	Options o;
#ifdef GOCUBE_DEBUG
	o.renderer.debug = true;
#endif
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--warp")) {
			o.renderer.warp = true;
		} else if (!strcmp(a, "--hidden")) {
			o.hidden = true;
		} else if (!strncmp(a, "--frames=", 9)) {
			o.frames = strtol(a + 9, nullptr, 10);
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

void draw_ui(const gc::Renderer &renderer) {
	const ImGuiIO &io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.6f);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	if (ImGui::Begin("Info", nullptr, flags)) {
		ImGui::Text("GoCube %s", GOCUBE_VERSION);
		ImGui::Text("%s", renderer.adapter_name().c_str());
		ImGui::Text("%.0f fps (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);
	}
	ImGui::End();
	ImGui::ShowDemoWindow();
}

} // namespace

int main(int argc, char **argv) {
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	const Options options = parse(argc, argv);

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

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr; // no imgui.ini next to the game
	ImGui::StyleColorsDark();
	ImGui_ImplWin32_Init(window.hwnd());
	if (!renderer.init_imgui()) {
		gc::log("Couldn't start Dear ImGui's Direct3D 11 backend");
		return 1;
	}

	const float clear[4] = {0.45f, 0.62f, 0.86f, 1.0f}; // sky blue
	long frame = 0;
	bool ok = true;
	while (window.pump()) {
		if (window.take_resized()) {
			renderer.resize(window.width(), window.height());
		}
		if (window.minimized()) {
			Sleep(16); // nothing to present to
			continue;
		}
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		draw_ui(renderer);
		ImGui::Render();
		if (!renderer.render_frame(clear)) {
			ok = false;
			break;
		}
		if (options.frames > 0 && ++frame >= options.frames) {
			break;
		}
	}

	const int messages = renderer.debug_messages();
	renderer.shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	if (messages > 0) {
		gc::log("The Direct3D 11 debug layer reported %d errors or warnings", messages);
	}
	return ok && messages == 0 ? 0 : 1;
}
