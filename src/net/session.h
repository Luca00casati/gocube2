#pragma once
// Multiplayer over TCP and UDP (net/transport.h).
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
// Mods: both sides must play with the same mods (id, version and files). A
// player whose mods differ (the same base game) is offered the host's: after
// they agree, they download the ones they lack (in parts, packed by
// mods/mod_pack.h, checked against the fingerprint) and the game starts again
// with exactly the host's mods (Mods::load_all's `only`). Mods are sandboxed;
// a different base game (another game version) is still refused.

#include "game/day_cycle.h"
#include "game/figure.h"
#include "game/commands.h"
#include "game/game.h"
#include "game/vitals.h"
#include "net/transport.h"

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
	static constexpr int PROTOCOL = 5; // (any change to the messages: one more)
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
	// mode, time, day_length, spawn, world_spawn, id).
	bool welcomed() const { return welcomed_; }
	const Data &welcome() const { return welcome_; }
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
	void set_world_info(Data info) { world_info_ = std::move(info); }
	void detach();
	// Each frame, after the game's update: states out, the host's checks,
	// avatars smoothed.
	void update(float dt, const LocalPose &pose);

	const std::vector<Avatar> &avatars() const { return avatars_; }
	// Chat. What the local player typed: a client sends it to the host, the
	// host (or a world played alone) handles it: a line starting with "/" is
	// a command (game/commands.h) run as that player, its answer to that
	// player only; anything else goes to everyone as "<name> text". The
	// lines to show come out of take_chat(). MAX_CHAT bytes a line; the host
	// limits how fast each player chats.
	static constexpr size_t MAX_CHAT = 256;
	void chat(const std::string &text);
	std::vector<std::string> take_chat() { return std::exchange(chat_in_, {}); }
	// Command names starting with `typed` (Tab).
	std::vector<std::string> complete_command(const std::string &typed) const;
	// Our name when playing alone (in multiplayer: the one host/join got).
	void set_local_name(const std::string &name) { local_name_ = name; }
	// Operators (they may change the world with commands), by name. The
	// host, or whoever plays alone, always is one. Saved with the world.
	void set_ops(std::vector<std::string> names) { ops_ = std::move(names); }
	const std::vector<std::string> &ops() const { return ops_; }

	// Players' skins (content/skin.h: SKIN_BYTES of RGBA). Ours (before or
	// while playing; any other size: none): a client sends it to the host
	// once welcomed, the host gives it to everyone who joins. The host passes
	// each player's on to the others. skin_of: a player's, or null;
	// skins_changed: counts changes (one came, a player left).
	void set_skin(std::vector<uint8_t> rgba);
	const std::vector<uint8_t> *skin_of(int id) const;
	int skins_changed() const { return skins_changed_; }
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
	void set_saved_players(const Data &players);
	Data saved_players();

	// Remote (a client's game asks through these).
	void start_mining(const BlockPos &p) override;
	void request_break(const BlockPos &p, int slot) override;
	void request_use(const float eye[3], const float dir[3], int slot, const std::string &item, bool sneak) override;
	void request_eat(int slot) override;
	void hit_creature(int id) override;
	void inventory_action(const Data &action) override;
	void set_bed(const BlockPos &head) override;
	void respawned() override;
	// A client: a container screen opened or closed (its contents come from the host).
	void open_container(const BlockPos &p);
	void close_container(const BlockPos &p);

	// --- Mods: everyone plays with the host's ---
	// The host: where its mods are (to send them to players who lack them).
	void set_mod_folders(std::map<std::string, std::filesystem::path> folders) { mod_folders_ = std::move(folders); }
	// A client: mod folders it has (mod_copies: the user's, downloaded ones).
	void set_local_mods(std::map<std::string, std::filesystem::path> copies) { local_mods_ = std::move(copies); }
	// A client restarted with the host's mods (a second offer is then an error).
	void set_restarted_with(std::vector<std::string> entries) { restarted_with_ = std::move(entries); }
	// A client: the host plays with other mods (same base game): its list,
	// and those we lack (entry -> bytes to download). Answer with
	// download_mods() or leave().
	bool mods_offered() const { return !host_mods_.empty(); }
	const std::vector<std::string> &host_mods() const { return host_mods_; }
	const std::map<std::string, size_t> &missing_mods() const { return missing_; }
	// Fetches the missing ones into `cache` (a folder of mod folders), each
	// checked against its fingerprint; then mods_ready().
	void download_mods(const std::filesystem::path &cache);
	bool mods_ready() const { return mods_ready_; }
	size_t downloaded_bytes() const;
	static constexpr size_t MOD_PART = 60000;           // bytes a message
	static constexpr size_t MAX_MOD_DOWNLOAD = 64u << 20; // all mods together

	// For the tests: requests of each kind a peer may make per second, and in a burst.
	struct Rate {
		float per_second, burst;
	};
	static const std::map<std::string, Rate> &rates();
	// For the tests: play without UDP (as behind a firewall that blocks it; set
	// before host() or join()), and whether positions go by UDP now.
	void set_udp_enabled(bool on) { udp_enabled_ = on; }
	bool udp_ready() const;

	struct Peer; // (session.cpp)

private:
	void send(Transport::Connection *to, const std::vector<uint8_t> &bytes, bool reliable = true);
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
	std::unique_ptr<Transport> net_;
	Transport::Connection *server_ = nullptr; // a client's connection to the host
	bool udp_enabled_ = true;
	std::string name_;
	std::vector<std::string> mods_;
	int my_id_ = 0;
	bool connected_ = false, welcomed_ = false;
	Data welcome_;
	std::string ended_;
	double started_ = 0;
	Attach a_;
	std::vector<std::unique_ptr<Peer>> peers_;
	int next_id_ = HOST_ID + 1;
	std::vector<Avatar> avatars_;
	std::unique_ptr<Commands> commands_; // (the host's, or alone: who runs them)
	std::vector<std::string> chat_in_, ops_;
	std::string local_name_ = "Player";
	void handle_chat(int peer, const std::string &text); // the host: peer 0 is us
	void start_survival(Peer &p); // the host: a joined player starts (or goes back to) survival
	void chat_to(int peer, const std::string &line);
	void chat_all(const std::string &line);
	CommandWorld command_world();
	std::vector<uint8_t> my_skin_;
	bool skin_sent_ = false;
	std::map<int, std::vector<uint8_t>> skins_; // by player id (the host's own too)
	int skins_changed_ = 0;
	int player_count_ = 1;
	float state_timer_ = 0, world_timer_ = 0, time_timer_ = 0;
	LocalPose last_pose_;
	Data saved_players_ = Data::object();
	bool woken_ = false;
	std::optional<std::array<float, 3>> correction_, push_;
	uint32_t inventory_seq_ = 0; // a client: inventory actions sent
	Data world_info_ = Data::object();
	std::map<std::string, std::filesystem::path> mod_folders_, local_mods_;
	std::map<std::string, std::vector<uint8_t>> packs_; // the host: mods packed to send (made once)
	std::vector<std::string> host_mods_, restarted_with_;
	std::map<std::string, size_t> missing_;
	struct Download {
		std::vector<uint8_t> data;
		size_t parts = 0;
		bool done = false;
	};
	std::map<std::string, Download> downloads_;
	std::filesystem::path cache_;
	bool mods_ready_ = false;
	const std::vector<uint8_t> &pack_of(const std::string &entry);
	std::vector<std::pair<ChunkPos, std::vector<uint8_t>>> welcome_chunks_;
	std::vector<std::vector<uint8_t>> held_; // a client: what came before the world was built
	std::function<void(const BlockPos &, gocube::BlockId)> previous_block_hook_;
};

} // namespace gc
