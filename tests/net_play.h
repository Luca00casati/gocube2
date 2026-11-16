#pragma once
// A player in a multiplayer game without a window: its world and game (Play),
// its Session, and frames of play as the game's loop runs them (in real time:
// the host times mining, movement and request rates by the clock). Players
// are driven like a person would: they stand where they're put (no physics:
// moving is the test's), aim, and click (GameInput).

#include "play.h"

#include "game/day_cycle.h"
#include "net/session.h"

#include <cmath>
#include <functional>
#include <thread>

struct NetPlay {
	std::unique_ptr<Play> play;
	gc::Session session;
	gc::DayCycle day;
	gc::GameInput input; // held for every frame (clicks: one frame, see click())
	double tick_time = 0;
	std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();

	static constexpr float FLOOR_Y = 100; // the host's stone floor, where everyone stands

	gc::Game &game() { return *play->game; }
	gc::World &world() { return *play->world; }
	gc::Player &player() { return play->player; }

	// The host: a world (seed 4242) with a stone floor at FLOOR_Y around the
	// origin, open on `port`; its player stands on it at (0.5, 0.5).
	static std::unique_ptr<NetPlay> host(int port, bool creative = false, const std::string &name = "Host") {
		auto n = std::make_unique<NetPlay>();
		n->play = std::make_unique<Play>(Play::Setup{creative});
		Play &p = *n->play;
		for (int x = -14; x <= 14; x++) {
			for (int z = -14; z <= 14; z++) {
				p.world->set_block({x, int(FLOOR_Y) - 1, z}, p.id("core:stone"));
				for (int y = int(FLOOR_Y); y < int(FLOOR_Y) + 4; y++) {
					p.world->set_block({x, y, z}, 0);
				}
			}
		}
		p.player.position[0] = 0.5f;
		p.player.position[1] = FLOOR_Y;
		p.player.position[2] = 0.5f;
		p.game->set_world_spawn(gc::Json::array({0.5, FLOOR_Y, 0.5}));
		n->day.time = 0.4; // day
		const std::string e = n->session.host(port, name, gc::Session::mod_list(p.mods.loaded()));
		PLAY_REQUIRE(e.empty());
		n->session.set_world_info({{"seed", 4242}, {"generator", gc::GENERATOR_VERSION}, {"block_ids", p.content.block_ids()}});
		n->attach();
		return n;
	}

	// A client: connects, waits for the host's welcome (while `others` run
	// their frames), then builds the world from it. Null if it was turned away
	// (`why` says why).
	static std::unique_ptr<NetPlay> join(int port, const std::string &name, const std::function<void()> &others,
			std::string *why = nullptr, std::vector<std::string> mods = {}) {
		auto n = std::make_unique<NetPlay>();
		if (mods.empty()) {
			gc::Content c;
			gc::Mods m(c);
			m.load_all({std::filesystem::path(GOCUBE_SOURCE_DIR) / "mods"});
			mods = gc::Session::mod_list(m.loaded());
		}
		PLAY_REQUIRE(n->session.join("127.0.0.1", port, name, mods).empty());
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (!n->session.welcomed() && n->session.ended().empty() && std::chrono::steady_clock::now() < deadline) {
			n->session.poll();
			others();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		if (!n->session.welcomed()) {
			if (why) {
				*why = n->session.ended();
			}
			return nullptr;
		}
		const gc::Json &w = n->session.welcome();
		Play::Setup s;
		s.creative = w.value("mode", "survival") == "creative";
		s.seed = uint32_t(w.value("seed", 0.0));
		s.generator = int(w.value("generator", 1.0));
		for (const gc::Json &id : w["block_ids"]) {
			s.block_ids.push_back(id.get<std::string>());
		}
		const gc::Json spawn = w["spawn"];
		s.x = spawn[0].get<float>();
		s.z = spawn[2].get<float>();
		// (The host keeps running while this world loads.)
		std::thread loader([&] { n->play = std::make_unique<Play>(s); });
		while (!n->play) {
			n->session.poll();
			others();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		loader.join();
		Play &p = *n->play;
		p.player.position[0] = spawn[0].get<float>();
		p.player.position[1] = spawn[1].get<float>() - gc::Player::EYE_HEIGHT;
		p.player.position[2] = spawn[2].get<float>();
		p.game->set_world_spawn(w.value("world_spawn", gc::Json()));
		n->day.time = w.value("time", 0.4);
		n->attach();
		return n;
	}

	void attach() {
		Play &p = *play;
		session.attach({p.game.get(), p.world.get(), &p.player, &p.content, p.rules.get(), &p.mods, &day});
	}

	// One frame: the network, the world, the game, the mods (not a
	// client's), what others see of us.
	void frame() {
		const auto now = std::chrono::steady_clock::now();
		const float dt = std::min(0.05f, std::chrono::duration<float>(now - last).count());
		last = now;
		Play &p = *play;
		session.poll();
		if (auto eye = session.take_correction()) {
			p.player.position[0] = (*eye)[0];
			p.player.position[1] = (*eye)[1] - gc::Player::EYE_HEIGHT;
			p.player.position[2] = (*eye)[2];
		}
		p.world->update(p.player.position[0], p.player.position[1], p.player.position[2]);
		p.world->take_meshes();
		p.world->take_unloaded();
		p.game->physics_step(dt);
		input.captured = true;
		p.game->update(dt, input);
		input.break_pressed = input.use_pressed = input.pick_pressed = false; // (clicks last one frame)
		input.number_key = 0;
		p.game->tick(dt);
		day.update(dt);
		p.game->set_day(day.day_amount());
		tick_time += dt;
		while (tick_time >= 0.05) {
			tick_time -= 0.05;
			if (!session.is_client()) {
				p.mods.tick(0.05);
			}
		}
		gc::LocalPose pose;
		p.player.eye(pose.eye);
		pose.yaw = p.player.yaw;
		pose.pitch = p.player.pitch;
		pose.item = p.game->selected_item();
		pose.selected = p.game->selected();
		pose.swings = p.game->swings();
		if (auto it = p.content.blocks.find(pose.item); it != p.content.blocks.end()) {
			pose.light = int(it->second.value("light", 0.0));
		}
		session.update(dt, pose);
	}

	// Looks at a point (from the eyes).
	void aim(float x, float y, float z) {
		float eye[3];
		player().eye(eye);
		const float dx = x - eye[0], dy = y - eye[1], dz = z - eye[2];
		player().yaw = std::atan2(-dx, -dz);
		player().pitch = std::atan2(dy, std::hypot(dx, dz));
	}

	// Puts this item in the hotbar slot it's in (the first), selected. False if none.
	bool hold(const std::string &item) {
		for (int i = 0; i < gc::Inventory::HOTBAR; i++) {
			if (game().inventory().slots[i].id == item) {
				input.number_key = i + 1;
				return true;
			}
		}
		return false;
	}
};

// Runs frames of every player until `cond` holds (true) or `seconds` pass.
inline bool run_until(const std::vector<NetPlay *> &players, const std::function<bool()> &cond, float seconds) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(seconds * 1000));
	while (!cond()) {
		if (std::chrono::steady_clock::now() > deadline) {
			return false;
		}
		for (NetPlay *p : players) {
			p->frame();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}
	return true;
}

inline void run_for(const std::vector<NetPlay *> &players, float seconds) {
	run_until(players, [] { return false; }, seconds);
}

// Walks a player to (x, z) on the floor at walking speed (4.3 m/s), aiming
// ahead, the others' frames running meanwhile.
inline void walk(NetPlay &who, const std::vector<NetPlay *> &players, float x, float z) {
	auto last = std::chrono::steady_clock::now();
	run_until(players, [&] {
		const auto now = std::chrono::steady_clock::now();
		const float dt = std::chrono::duration<float>(now - last).count();
		last = now;
		float *pos = who.player().position;
		const float dx = x - pos[0], dz = z - pos[2], d = std::hypot(dx, dz);
		if (d < 0.05f) {
			return true;
		}
		const float step = std::min(d, 4.3f * dt);
		pos[0] += dx / d * step;
		pos[2] += dz / d * step;
		return false;
	}, 20.0f);
}
