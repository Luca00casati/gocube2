// Multiplayer across two real processes over the network (the Godot version's
// test_mp_processes and mp_smoke). Run without arguments (CTest), it starts
// itself twice: a host (`host PORT`) and, once the host says it's ready, a
// client (`client PORT`). The client checks that edits made before and after
// it joined reach it, that the host keeps its inventory, that its requests are
// done (or refused) by the host, and plays a short round (placing, mining, a
// door, a bed, a chest), printing a PASS or FAIL line for each. Both print a
// fingerprint of the play area at the end; they must match.

#include <cstdio>
#include <cstdlib>

#define PLAY_REQUIRE(x)                                                           \
	do {                                                                          \
		if (!(x)) {                                                               \
			std::fprintf(stderr, "ERROR %s:%d: %s\n", __FILE__, __LINE__, #x); \
			std::exit(2);                                                         \
		}                                                                         \
	} while (0)

#include "net_play.h"

#include <windows.h>

#include <atomic>
#include <map>
#include <mutex>
#include <process.h>
#include <string>
#include <thread>
#include <vector>

namespace {

using gc::BlockPos;

const BlockPos LAMP_BEFORE{5, 100, 5}, ORE_AFTER{6, 100, 5}, CHEST{-4, 100, -2};

void say(const std::string &line) {
	std::printf("%s\n", line.c_str());
	std::fflush(stdout);
}

// The play area's blocks, hashed (FNV-1a over their ids).
std::string area_hash(const Play &p) {
	uint64_t h = 1469598103934665603ull;
	for (int x = -14; x <= 14; x++) {
		for (int z = -14; z <= 14; z++) {
			for (int y = 99; y <= 103; y++) {
				for (char c : p.block(x, y, z) + ";") {
					h = (h ^ uint8_t(c)) * 1099511628211ull;
				}
			}
		}
	}
	char out[32];
	std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(h));
	return out;
}

int run_host(int port) {
	auto host = NetPlay::host(port);
	NetPlay &h = *host;
	h.world().set_block(LAMP_BEFORE, h.play->id("core:lamp"));
	h.world().set_block(CHEST, h.play->id("core:chest"));
	h.game().block_entities().click(CHEST, 0, {"core:coal", 4});
	h.game().inventory().slots[h.game().selected()] = {"core:torch", 1};
	const float at[3] = {9.5f, 100.0f, -9.5f};
	h.game().creatures().spawn(h.play->content, "core:block_eater", at);
	// What the host keeps for the client (as if it had played here before).
	gc::Inventory inv(h.play->content);
	inv.slots[0] = {"core:planks", 4};
	inv.slots[1] = {"core:wood_door", 1};
	inv.slots[2] = {"core:bed", 3};
	h.session.set_saved_players({{"Client", {{"inventory", inv.serialize()}}}});
	say("HOST ready");
	if (!run_until({&h}, [&] { return !h.session.avatars().empty(); }, 60)) {
		say("HOST nobody came");
		return 1;
	}
	run_for({&h}, 1.5f);
	h.world().set_block(ORE_AFTER, h.play->id("core:gold_ore"));
	run_until({&h}, [&] { return h.session.avatars().empty(); }, 150);
	say("HOST AREA " + area_hash(*h.play));
	return 0;
}

int run_client(int port) {
	std::string why;
	auto client = NetPlay::join(port, "Client", [] {}, &why);
	std::map<std::string, bool> results;
	auto check = [&](const std::string &what, bool ok) { results[what] = ok; };
	check("joined" + (why.empty() ? "" : ": " + why), client != nullptr);
	if (client) {
		NetPlay &c = *client;
		const std::vector<NetPlay *> me = {&c};
		gc::Inventory &inv = c.game().inventory();
		auto at = [&](const BlockPos &p) { return c.play->block(p); };
		run_until(me, [&] { return c.world().is_loaded(-14, 100, -14) && c.world().is_loaded(14, 100, 14); }, 30);
		check("an edit made before we joined", at(LAMP_BEFORE) == "core:lamp");
		check("an edit made after", run_until(me, [&] { return at(ORE_AFTER) == "core:gold_ore"; }, 10));
		check("the host kept our inventory", run_until(me, [&] {
			return inv.count_of("core:planks") == 4 && inv.count_of("core:bed") == 3 && inv.count_of("core:torch") == 0;
		}, 5));
		check("we see the host's torch light", run_until(me, [&] {
			auto l = c.session.held_lights();
			return !l.empty() && l[0][3] == 13.0f;
		}, 5));
		check("we see the host's creatures", run_until(me, [&] { return !c.game().creatures().all().empty(); }, 5));

		// Placing takes the item from the host's copy.
		const BlockPos cell{4, 100, 1};
		c.hold("core:planks");
		c.frame();
		c.aim(cell.x + 0.5f, NetPlay::FLOOR_Y - 0.001f, cell.z + 0.5f);
		c.input.use_pressed = true;
		check("place a block, using the host's item", run_until(me, [&] {
			return at(cell) == "core:planks" && inv.count_of("core:planks") == 3;
		}, 5));
		// Breaking at once is refused; mining it, the host drops it to us.
		c.session.request_break(cell, c.game().selected());
		run_for(me, 0.5f);
		check("no break without mining", at(cell) == "core:planks");
		c.aim(cell.x + 0.5f, cell.y + 0.5f, cell.z + 0.5f);
		c.input.break_held = true;
		check("mine it, get it back", run_until(me, [&] {
			return at(cell) == "core:air" && inv.count_of("core:planks") == 4;
		}, 10));
		c.input.break_held = false;
		// A teleport is put back.
		const float x0 = c.player().position[0];
		c.player().position[0] += 200;
		check("a teleport is put back", run_until(me, [&] { return std::abs(c.player().position[0] - x0) < 2.0f; }, 5));
		run_until(me, [&] { return c.world().is_loaded(-14, 100, -14) && c.world().is_loaded(14, 100, 14); }, 30);
		// A door, opened.
		const BlockPos door{1, 100, 4};
		c.hold("core:wood_door");
		c.frame();
		c.aim(door.x + 0.5f, NetPlay::FLOOR_Y - 0.001f, door.z + 0.5f);
		c.input.use_pressed = true;
		check("place a door", run_until(me, [&] { return at(door + BlockPos{0, 1, 0}).ends_with("_upper"); }, 5));
		c.aim(door.x + 0.5f, 100.5f, door.z + 0.5f);
		c.input.use_pressed = true;
		check("open it", run_until(me, [&] { return at(door).ends_with("_open"); }, 5));
		// A bed, whole (one used).
		c.hold("core:bed");
		c.frame();
		c.aim(0.5f, NetPlay::FLOOR_Y - 0.001f, 2.5f);
		c.input.use_pressed = true;
		check("place a bed (one used)", run_until(me, [&] {
			int parts = 0;
			for (int x = -1; x <= 1; x++) {
				for (int z = 1; z <= 3; z++) {
					parts += at({x, 100, z}).starts_with("core:bed") ? 1 : 0;
				}
			}
			return parts == 2 && inv.count_of("core:bed") == 2;
		}, 5));
		// The host's chest: walk to it, open it, take the coal, put it back
		// in the next slot.
		walk(c, me, 2.5f, -1.5f); // (around the bed: the host puts back moves through blocks)
		walk(c, me, CHEST.x + 0.5f, CHEST.z + 2.5f);
		c.aim(CHEST.x + 0.5f, CHEST.y + 0.5f, CHEST.z + 0.5f);
		c.input.use_pressed = true;
		c.frame();
		const bool opened = c.game().take_opened().has_value();
		c.session.open_container(CHEST);
		auto slot = [&](int i) {
			const gc::BlockEntity *e = c.game().block_entities().at(CHEST);
			return e ? e->slots[size_t(i)] : gc::Stack{};
		};
		check("the chest's contents come from the host",
				opened && run_until(me, [&] { return slot(0) == gc::Stack{"core:coal", 4}; }, 5));
		c.game().inventory_action(gc::Json::array({"container", CHEST.x, CHEST.y, CHEST.z, 0}));
		c.game().inventory_action(gc::Json::array({"container", CHEST.x, CHEST.y, CHEST.z, 1}));
		check("move the coal in it", run_until(me, [&] { return slot(1) == gc::Stack{"core:coal", 4} && slot(0).empty(); }, 5));
		c.session.close_container(CHEST);
		run_for(me, 1.0f);
		say("AREA " + area_hash(*c.play));
	}
	client.reset(); // (leaves)
	bool all = true;
	for (const auto &[what, ok] : results) {
		say(std::string(ok ? "PASS " : "FAIL ") + what);
		all = all && ok;
	}
	return all ? 0 : 1;
}

// A child process (this program again), its output read line by line as it comes.
struct Child {
	PROCESS_INFORMATION pi{};
	HANDLE out = nullptr;
	std::thread reader;
	std::mutex mutex;
	std::vector<std::string> lines;

	bool start(const std::string &args) {
		SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
		HANDLE write = nullptr;
		if (!CreatePipe(&out, &write, &sa, 0)) {
			return false;
		}
		SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0);
		wchar_t exe[MAX_PATH];
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		std::wstring cmd = L"\"" + std::wstring(exe) + L"\" " + std::wstring(args.begin(), args.end());
		STARTUPINFOW si{};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdOutput = write;
		si.hStdError = write;
		si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		const bool ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
		CloseHandle(write);
		if (!ok) {
			return false;
		}
		reader = std::thread([this] {
			std::string partial;
			char buf[4096];
			DWORD n;
			while (ReadFile(out, buf, sizeof(buf), &n, nullptr) && n > 0) {
				partial.append(buf, n);
				size_t nl;
				while ((nl = partial.find('\n')) != std::string::npos) {
					std::string line = partial.substr(0, nl);
					if (!line.empty() && line.back() == '\r') {
						line.pop_back();
					}
					std::lock_guard<std::mutex> lock(mutex);
					lines.push_back(line);
					partial.erase(0, nl + 1);
				}
			}
		});
		return true;
	}
	bool running() const { return pi.hProcess && WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT; }
	bool wait(int ms) { return pi.hProcess && WaitForSingleObject(pi.hProcess, ms) == WAIT_OBJECT_0; }
	std::string line_starting(const std::string &prefix) {
		std::lock_guard<std::mutex> lock(mutex);
		for (const std::string &l : lines) {
			if (l.rfind(prefix, 0) == 0) {
				return l;
			}
		}
		return "";
	}
	void finish() {
		if (running()) {
			TerminateProcess(pi.hProcess, 3);
		}
		if (reader.joinable()) {
			reader.join();
		}
		CloseHandle(out);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
};

int run_both() {
	const int port = 25000 + _getpid() % 5000;
	Child host, client;
	if (!host.start("host " + std::to_string(port))) {
		say("FAIL couldn't start the host");
		return 1;
	}
	int failures = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (host.line_starting("HOST ready").empty() && host.running() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	if (host.line_starting("HOST ready").empty()) {
		say("FAIL the host didn't get ready");
		failures++;
	} else if (!client.start("client " + std::to_string(port))) {
		say("FAIL couldn't start the client");
		failures++;
	} else {
		// The host quits once the client has left.
		if (!client.wait(180000) || !host.wait(30000)) {
			say("FAIL both finished within the time");
			failures++;
		}
	}
	host.finish();
	if (client.pi.hProcess) {
		client.finish();
	}
	int results = 0;
	for (const std::string &l : client.lines) {
		if (l.rfind("PASS ", 0) == 0 || l.rfind("FAIL ", 0) == 0) {
			say("client: " + l);
			results++;
			failures += l[0] == 'F' ? 1 : 0;
		}
	}
	if (results == 0) {
		say("FAIL the client reported nothing");
		failures++;
	}
	const std::string mine = client.line_starting("AREA "), theirs = host.line_starting("HOST AREA ");
	const bool same = !mine.empty() && !theirs.empty() && mine.substr(5) == theirs.substr(10);
	say(std::string(same ? "PASS" : "FAIL") + " host and client worlds match (client " + (mine.empty() ? "none" : mine.substr(5)) +
			", host " + (theirs.empty() ? "none" : theirs.substr(10)) + ")");
	failures += same ? 0 : 1;
	for (Child *c : {&host, &client}) {
		for (const std::string &l : c->lines) {
			if (l.rfind("ERROR", 0) == 0) {
				say("FAIL " + l);
				failures++;
			}
		}
	}
	if (failures) {
		say("host output:");
		for (size_t i = host.lines.size() > 15 ? host.lines.size() - 15 : 0; i < host.lines.size(); i++) {
			say("  " + host.lines[i]);
		}
		say("client output:");
		for (size_t i = client.lines.size() > 40 ? client.lines.size() - 40 : 0; i < client.lines.size(); i++) {
			say("  " + client.lines[i]);
		}
	}
	return failures ? 1 : 0;
}

} // namespace

int main(int argc, char **argv) {
	const std::string role = argc > 1 ? argv[1] : "";
	const int port = argc > 2 ? std::atoi(argv[2]) : 24711;
	if (role == "host") {
		return run_host(port);
	}
	if (role == "client") {
		return run_client(port);
	}
	return run_both();
}
