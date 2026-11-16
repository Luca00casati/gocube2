#pragma once
// Multiplayer over ENet: the Godot version's Session.
//
// The host is an authoritative server that also plays. World generation is
// deterministic, so clients generate terrain themselves from the seed and only
// edits travel over the network:
//   - handshake: protocol and exact mod list check, then the seed, the block
//     table and every edited chunk (the chunk file format)
//   - block changes: a client asks (breaking, a right click), the host checks
//     it and does it as that player (Game::break_block, Game::use), and every
//     change goes to everyone; a client that hasn't loaded that chunk asks for
//     the whole chunk instead
//   - player positions (clients -> host -> everyone, 20 a second) and how
//     each looks (the held item, sneaking, asleep, flying, arm swings)
//   - survival: the host keeps every joined player's inventory (saved in its
//     world by name) and health (falls, lava and drowning from where it says
//     it is, creatures, healing, food: Vitals), and sends them. A client does
//     each inventory click at once and tells the host, which does the same to
//     its copy. A break comes no sooner than the block's mining time after
//     the client said it started mining it
//   - the host's creatures, dropped items, chests and furnaces, time of day,
//     sounds and sights mods make; beds and skipping the night
//   - the host checks what clients say: movement (speed, walls), reach,
//     request rates, what they hold
//
// Mods: both sides must play with the same mods (id, version and files).

#include "game/day_cycle.h"
#include "game/figure.h"
#include "game/game.h"
#include "game/vitals.h"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

typedef struct _ENetHost ENetHost;
typedef struct _ENetPeer ENetPeer;

namespace gc {

// How the local player looks to others (set every frame).
struct LocalPose {
	float eye[3] = {0, 0, 0};
	float yaw = 0, pitch = 0;
	std::string item;    // in hand
	int selected = 0;    // hotbar slot
	int flags = 0;       // Session::FLAG_*
	int swings = 0;      // Game::swings
	int light = 0;       // the held item's light (a torch: 13)
};

// Another player as we see them: where the host last said, and smoothed
// between its updates for drawing.
struct Avatar {
	int id = 0;
	std::string name;
	float eye[3] = {0, 0, 0}; // as sent
	float yaw = 0, pitch = 0;
	std::string item;
	int flags = 0, swings = 0, light = 0;
	// Drawn: eye position and facing, speed (from how it moves), the figure.
	float shown[3] = {0, 0, 0};
	float shown_yaw = 0;
	float velocity[3] = {0, 0, 0};
	Figure figure;
	int seen_swings = 0;
	bool placed = false;
};

class Session : public Remote {
public:
	static constexpr int PROTOCOL = 1;
	static constexpr int DEFAULT_PORT = 24680;
	static constexpr int MAX_PLAYERS = 8;
	static constexpr int HOST_ID = 1;
	static constexpr int FLAG_SNEAK = 1, FLAG_SLEEP = 2, FLAG_FLY = 4;
	static constexpr float STATE_INTERVAL = 0.05f; // positions: 20 a second
	static constexpr float WORLD_INTERVAL = 0.1f;  // creatures, drops, containers: 10 a second
	static constexpr float TIME_SYNC_SECONDS = 5.0f;
	static constexpr float EDIT_REACH = 16.0f;     // from a player's eyes to a block it changes
	static constexpr float USE_EYE_SLACK = 2.5f;   // a click's eye from where we last had it
	static constexpr float CONTAINER_REACH = 8.0f;
	static constexpr float CREATURE_REACH = 6.0f;
	static constexpr double CONNECT_TIMEOUT = 8.0;
	static constexpr size_t NAME_LENGTH = 16;
	// Host-side movement check: the fastest a player can go (horizontal, up,
	// down), in blocks a second; survival covers sprinting and a creature's
	// knock back, creative sprint-flying.
	static constexpr float MOVE_SURVIVAL[3] = {14, 13, 62}, MOVE_CREATIVE[3] = {28, 28, 62};
	static constexpr float MOVE_BURST = 0.75f; // seconds of movement that may arrive at once (lag)
	static constexpr float MOVE_SLACK = 1.0f;  // blocks always allowed
	static constexpr float RESPAWN_RADIUS = 2.0f;
	static constexpr float CORRECT_EVERY = 0.5f;
	static constexpr float WALL_STEP = 0.1f, WALL_GRACE = 0.15f, ESCAPE_DISTANCE = 1.5f;
	// A survival break comes no sooner than this share of the mining time
	// after mining started, less MINE_LAG.
	static constexpr float MINE_SLACK = 0.8f, MINE_LAG = 0.1f;

	enum class Mode { OFFLINE, HOST, CLIENT };

	Session();
	~Session() override;
	Session(const Session &) = delete;
	Session &operator=(const Session &) = delete;

	// "id@version#fingerprint" of every loaded mod, sorted.
	static std::vector<std::string> mod_list(const std::vector<ModInfo> &mods);
	// A player name as others see it: printable, trimmed, NAME_LENGTH at most.
	static std::string clean_name(const std::string &requested);

	// The host: listens on `port` ("" or what went wrong).
	std::string host(int port, const std::string &name, std::vector<std::string> mods);
	// A client: connects to the host (then poll() until welcomed() or ended()).
	std::string join(const std::string &address, int port, const std::string &name, std::vector<std::string> mods);
	void leave();
	Mode mode() const { return mode_; }
	bool is_host() const { return mode_ == Mode::HOST; }
	bool is_client() const { return mode_ == Mode::CLIENT; }
	int my_id() const { return my_id_; }

	// Network traffic in and out: every frame (a client also before it plays).
	void poll();
	// A client: the host took us in; the world to build (block_ids, seed,
	// generator, mode, time, day_length, spawn, world_spawn, first_night, id).
	bool welcomed() const { return welcomed_; }
	const Json &welcome() const { return welcome_; }
	// Why it ended (a client: refused, lost, timed out); "" while it goes on.
	const std::string &ended() const { return ended_; }

	// Playing: the game this session plays in (the host: its own world; a
	// client: the one built from the welcome, which this makes remote).
	struct Attach {
		Game *game = nullptr;
		World *world = nullptr;
		Player *player = nullptr;
		Content *content = nullptr;
		const BlockRules *rules = nullptr;
		Mods *mods = nullptr;
		DayCycle *day = nullptr;
	};
	void attach(const Attach &a);
	// The host: what a client needs to build the world: seed, generator, block_ids.
	void set_world_info(Json info) { world_info_ = std::move(info); }
	void detach();
	// Each frame, after the game's update: states out, the host's checks,
	// avatars smoothed.
	void update(float dt, const LocalPose &pose);

	const std::vector<Avatar> &avatars() const { return avatars_; }
	int player_count() const;
	// Other players' held lights (x, y, z, level) for the world's light.
	std::vector<std::array<float, 4>> held_lights() const;

	// Beds and sleeping: the host skips the night when everyone is in bed.
	void set_sleeping(bool on);     // a client: we lay down or got up
	bool peers_sleeping() const;    // the host: every joined player is (true alone)
	void night_skipped();           // the host: the new time, and everyone up
	bool take_woken() { return std::exchange(woken_, false); } // a client: get up
	// A client: the host put us back (a move it refused); the eye position.
	std::optional<std::array<float, 3>> take_correction() { return std::exchange(correction_, std::nullopt); }
	// A client: knocked back by a creature (velocity to add).
	std::optional<std::array<float, 3>> take_push() { return std::exchange(push_, std::nullopt); }

	// The host: joined players' inventories and health, kept by name in the
	// world (world.lua's "players"): {name = {inventory, health}}.
	void set_saved_players(const Json &players);
	Json saved_players();

	// Remote (a client's game asks through these).
	void start_mining(const BlockPos &p) override;
	void request_break(const BlockPos &p, int slot) override;
	void request_use(const float eye[3], const float dir[3], int slot, const std::string &item, bool sneak) override;
	void request_eat(int slot) override;
	void hit_creature(int id) override;
	void inventory_action(const Json &action) override;
	void set_bed(const BlockPos &head) override;
	void respawned() override;
	// A client: a container screen opened or closed (its contents come from the host).
	void open_container(const BlockPos &p);
	void close_container(const BlockPos &p);

	// For the tests: requests of each kind a peer may make per second, and in a burst.
	struct Rate {
		float per_second, burst;
	};
	static const std::map<std::string, Rate> &rates();

	struct Peer; // (session.cpp)

private:
	void send(ENetPeer *to, const std::vector<uint8_t> &bytes, bool reliable = true);
	void send_host(const std::vector<uint8_t> &bytes, bool reliable = true);
	void to_players(const std::vector<uint8_t> &bytes, bool reliable = true);
	void end(const std::string &why);
	void on_host_message(Peer &p, const uint8_t *data, size_t size);
	void on_client_message(const uint8_t *data, size_t size);
	void hello(Peer &p, const uint8_t *data, size_t size);
	bool allow(Peer &p, const std::string &kind);
	bool move_allowed(Peer &p, const float eye[3], int flags);
	bool way_clear(const float from[3], const float to[3], Peer &p);
	void do_break(Peer &p, const BlockPos &pos, int slot);
	Peer *peer_by_id(int id);
	void host_update(float dt);
	void client_update(float dt, const LocalPose &pose);
	void update_avatars(float dt);
	std::vector<uint8_t> players_message(const LocalPose &pose) const;
	void merge_avatars(const std::vector<Avatar> &states);
	std::string held_item(const Peer &p) const;

	Mode mode_ = Mode::OFFLINE;
	ENetHost *enet_ = nullptr;
	ENetPeer *server_ = nullptr; // a client's connection to the host
	std::string name_;
	std::vector<std::string> mods_;
	int my_id_ = 0;
	bool connected_ = false, welcomed_ = false;
	Json welcome_;
	std::string ended_;
	double started_ = 0;
	Attach a_;
	std::vector<std::unique_ptr<Peer>> peers_;
	int next_id_ = HOST_ID + 1;
	std::vector<Avatar> avatars_;
	int player_count_ = 1;
	float state_timer_ = 0, world_timer_ = 0, time_timer_ = 0;
	LocalPose last_pose_;
	Json saved_players_ = Json::object();
	std::string last_first_night_;
	bool woken_ = false;
	std::optional<std::array<float, 3>> correction_, push_;
	uint32_t inventory_seq_ = 0; // a client: inventory actions sent
	Json world_info_ = Json::object();
	std::vector<std::pair<ChunkPos, std::vector<uint8_t>>> welcome_chunks_;
	std::vector<std::vector<uint8_t>> held_; // a client: what came before the world was built
	std::function<void(const BlockPos &, gocube::BlockId)> previous_block_hook_;
};

} // namespace gc
