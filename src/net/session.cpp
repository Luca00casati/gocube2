#include "net/session.h"

#include "log.h"
#include "mods/mod_pack.h"
#include "net/wire.h"
#include "content/skin.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <tuple>

namespace gc {

using net::Reader;
using net::Writer;

namespace {

// Messages. Clients send C_*, the host H_*.
enum Msg : uint8_t {
	C_HELLO = 1, // protocol u16, name, mods (u16 count, entries)
	C_STATE,     // eye, yaw, pitch, item, flags u8, swings u32, selected u8 (unreliable)
	C_MINING,    // block
	C_BREAK,     // block, slot u8
	C_USE,       // eye, dir, slot u8, item, sneak
	C_EAT,       // slot u8
	C_HIT,       // creature id u32
	C_INVENTORY, // seq u32, action (data)
	C_OPEN,      // block (a container screen)
	C_CLOSE,     // block
	C_CHUNK,     // chunk position
	C_BED,       // block (the bed's head)
	C_SLEEP,     // bool
	C_RESPAWNED,
	C_MOD_PART,  // entry, part u32 (a player we offered our mods)
	C_SKIN,      // bytes (SKIN_BYTES of RGBA)
	C_CHAT,      // text (a line, or a "/" command)

	H_WELCOME = 64, // info (data), chunks (u32 count, position + bytes)
	H_REJECTED,     // reason
	H_PLAYERS,      // u16 count, {id u32, name, eye, yaw, pitch, item, flags u8, swings u32, light u8} (unreliable)
	H_BLOCK,        // block, id u16
	H_CHUNK,        // chunk position, bytes
	H_TIME,         // time f64, day_length f64
	H_CORRECT,      // eye
	H_INVENTORY,    // done u32, {inventory, cursor} (data)
	H_HEALTH,       // i32
	H_STRUCK,       // damage i32, push
	H_DROPS,        // u16 count, {item, count u16, pos, age f32, turn f32} (unreliable)
	H_CREATURES,    // u16 count, {id u32, kind, variant, pos, yaw, flags u8, flash f32, hurt f32, strikes u16} (unreliable)
	H_CONTAINER,    // block, entity (data; null: none)
	H_PICKED,       // item, count u16
	H_SOUND,        // set, kind, at, dB f32
	H_EFFECT,       // name, at
	H_WAKE,
	H_OFFER_MODS,   // u16 count, {entry, size u32} (our mods; size 0: the base game)
	H_MOD_PART,     // entry, part u32, parts u32, bytes
	H_SKIN,         // id u32, bytes (SKIN_BYTES of RGBA)
	H_CHAT,         // a line to show
	H_WORLD_SPAWN,  // feet (a command moved it)
	H_MODE,         // creative u8 (a command changed it)
};

// "id@version#fingerprint" -> its parts ("" if it isn't one).
std::string entry_id(const std::string &e) {
	return e.substr(0, std::min(e.find('@'), e.size()));
}

std::string entry_fingerprint(const std::string &e) {
	const size_t hash = e.rfind('#');
	return hash == std::string::npos ? "" : e.substr(hash + 1);
}

bool valid_entry(const std::string &e) {
	const std::string id = entry_id(e), fp = entry_fingerprint(e);
	if (id.empty() || id.size() > 64 || fp.size() != 12 || e.find('@') == std::string::npos) {
		return false;
	}
	for (char c : id) {
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
			return false;
		}
	}
	for (char c : fp) {
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	return true;
}

constexpr size_t MAX_CHUNK_BYTES = 256 * 1024;
constexpr int MAX_MODS = 256;

double now() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int floor_div(int v, int d) {
	return v >= 0 ? v / d : -((-v + d - 1) / d);
}

float lerp_angle(float from, float to, float w) {
	constexpr float PI = 3.14159265f;
	float d = std::fmod(to - from, 2 * PI);
	if (d > PI) {
		d -= 2 * PI;
	} else if (d < -PI) {
		d += 2 * PI;
	}
	return from + d * w;
}

Writer &block_pos(Writer &w, const BlockPos &p) {
	return w.i32(p.x).i32(p.y).i32(p.z);
}

BlockPos read_block(Reader &r) {
	BlockPos p;
	p.x = r.i32();
	p.y = r.i32();
	p.z = r.i32();
	return p;
}

float distance(const float a[3], const float b[3]) {
	return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

} // namespace

struct Session::Peer {
	Transport::Connection *conn = nullptr;
	int id = 0;
	bool joined = false;
	std::string name;
	// As it says it is (accepted by the movement check).
	float eye[3] = {0, 0, 0};
	float yaw = 0, pitch = 0;
	std::string item;
	int flags = 0, swings = 0, selected = 0, light = 0;
	// The movement check.
	float budget[3] = {0, 0, 0};
	double time = 0;
	std::optional<std::array<float, 3>> stuck_at;
	double corrected = -1e9;
	// Survival: its inventory and health as the host keeps them.
	std::unique_ptr<Inventory> inv;
	uint32_t done = 0; // the last inventory action of its we did
	bool inv_changed = false;
	Vitals vitals;
	std::optional<BlockPos> bed;
	bool sleeping = false;
	// Mining: what it started on, when; a break waiting for its mining time.
	std::optional<BlockPos> mining;
	double mining_since = 0, mined_at = 0;
	struct Break {
		BlockPos pos;
		int slot = 0;
		double at = 0;
	};
	std::optional<Break> waiting;
	std::map<std::string, std::array<double, 2>> rates; // kind -> tokens, last update
	double flood_logged = -1e9;
	std::map<std::tuple<int, int, int>, std::string> viewing; // open containers -> what was last sent
	bool offered = false; // it has other mods: it may download ours

	void feet(float out[3]) const {
		out[0] = eye[0];
		out[1] = eye[1] - Player::EYE_HEIGHT;
		out[2] = eye[2];
	}
};

const std::map<std::string, Session::Rate> &Session::rates() {
	static const std::map<std::string, Rate> table = {
			{"edit", {20, 40}}, // breaking and right clicks (creative mines fast)
			{"hit", {8, 12}},
			{"chunk", {30, 120}},
			{"container", {20, 40}},
			{"other", {4, 10}}, // bed, sleeping
			{"inventory", {30, 60}}, // clicks in screens, eating
			{"mine", {20, 40}},
			{"mod", {80, 160}}, // parts of our mods (offered, not joined)
			{"skin", {0.2f, 2}}, // a player's skin (sent once, maybe again)
			{"chat", {2, 6}},    // lines and commands typed
	};
	return table;
}

Session::Session() = default;

Session::~Session() {
	leave();
}

std::vector<std::string> Session::mod_list(const std::vector<ModInfo> &mods) {
	std::vector<std::string> out;
	for (const ModInfo &m : mods) {
		out.push_back(mod_entry(m));
	}
	std::sort(out.begin(), out.end());
	return out;
}

std::string Session::clean_name(const std::string &requested) {
	// Printable UTF-8 only: no control characters (C0, DEL, C1) and no
	// invisible or direction-changing ones.
	std::string out;
	size_t chars = 0;
	for (size_t i = 0; i < requested.size() && chars < NAME_LENGTH * 4;) {
		const unsigned char c = static_cast<unsigned char>(requested[i]);
		const int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
		if (len == 0 || i + len > requested.size()) {
			i++;
			continue; // not UTF-8
		}
		uint32_t u = len == 1 ? c : c & (0xFF >> (len + 1));
		bool valid = true;
		for (int k = 1; k < len; k++) {
			const unsigned char d = static_cast<unsigned char>(requested[i + k]);
			valid = valid && (d >> 6) == 2;
			u = (u << 6) | (d & 0x3F);
		}
		const bool printable = u >= 32 && u != 127 && !(u >= 0x80 && u < 0xA0) && !(u >= 0x200B && u <= 0x200F) &&
				!(u >= 0x202A && u <= 0x202E) && !(u >= 0x2066 && u <= 0x2069) && !(u >= 0xD800 && u <= 0xDFFF);
		if (valid && printable) {
			out.append(requested, i, len);
			chars++;
		}
		i += valid ? len : 1;
	}
	// Trim, then at most NAME_LENGTH characters, trimmed again.
	auto trim = [](std::string &s) {
		while (!s.empty() && s.back() == ' ') {
			s.pop_back();
		}
		size_t lead = 0;
		while (lead < s.size() && s[lead] == ' ') {
			lead++;
		}
		s.erase(0, lead);
	};
	trim(out);
	size_t n = 0, cut = 0;
	for (size_t i = 0; i < out.size(); n++) {
		const unsigned char c = static_cast<unsigned char>(out[i]);
		i += c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
		if (n + 1 == NAME_LENGTH) {
			cut = i;
		}
	}
	if (n > NAME_LENGTH) {
		out.resize(cut);
	}
	trim(out);
	return out;
}

// --- Setup and teardown -------------------------------------------------------------

std::string Session::host(int port, const std::string &name, std::vector<std::string> mods) {
	leave();
	net_ = std::make_unique<Transport>();
	net_->set_udp_enabled(udp_enabled_);
	if (std::string e = net_->listen(port, MAX_PLAYERS - 1); !e.empty()) {
		net_.reset();
		return e;
	}
	mode_ = Mode::HOST;
	name_ = clean_name(name).empty() ? "Host" : clean_name(name);
	mods_ = std::move(mods);
	my_id_ = HOST_ID;
	if (!my_skin_.empty()) {
		skins_[HOST_ID] = my_skin_;
	}
	log("Hosting on port %d", port);
	return "";
}

std::string Session::join(const std::string &address, int port, const std::string &name, std::vector<std::string> mods) {
	leave();
	net_ = std::make_unique<Transport>();
	net_->set_udp_enabled(udp_enabled_);
	if (std::string e = net_->connect(address, port); !e.empty()) {
		net_.reset();
		return e;
	}
	mode_ = Mode::CLIENT;
	name_ = clean_name(name).empty() ? "Player" : clean_name(name);
	mods_ = std::move(mods);
	started_ = now();
	return "";
}

void Session::leave() {
	detach();
	if (net_) {
		if (server_) {
			net_->disconnect_now(server_);
		}
		for (auto &p : peers_) {
			net_->disconnect_now(p->conn);
		}
		net_->close();
	}
	net_.reset();
	server_ = nullptr;
	peers_.clear();
	avatars_.clear();
	skins_.clear();
	skin_sent_ = false;
	skins_changed_++;
	mode_ = Mode::OFFLINE;
	connected_ = welcomed_ = false;
}

void Session::end(const std::string &why) {
	if (ended_.empty()) {
		ended_ = why;
		log("Multiplayer: %s", why.c_str());
	}
}

void Session::attach(const Attach &a) {
	detach();
	a_ = a;
	Game &game = *a_.game;
	if (!is_client()) {
		commands_ = std::make_unique<Commands>(command_world());
		if (a_.mods) {
			a_.mods->set_command_runner([this](const std::string &line) {
				CommandPlayer world;
				world.name = "world";
				world.op = true;
				const float *p = a_.player ? a_.player->position : nullptr;
				if (p) {
					std::copy(p, p + 3, world.feet);
				}
				return commands_->run(world, line);
			});
		}
	}
	if (is_client()) {
		game.set_remote(this);
		for (auto &[pos, bytes] : welcome_chunks_) {
			a_.world->set_chunk(pos, bytes.data(), bytes.size());
		}
		welcome_chunks_.clear();
		for (const std::vector<uint8_t> &m : std::exchange(held_, {})) {
			on_client_message(m.data(), m.size());
		}
		return;
	}
	if (!is_host()) {
		return;
	}
	// Every block change goes to every joined player.
	previous_block_hook_ = a_.world->on_block_changed;
	a_.world->on_block_changed = [this](const BlockPos &p, gocube::BlockId id) {
		if (previous_block_hook_) {
			previous_block_hook_(p, id);
		}
		Writer w(H_BLOCK);
		block_pos(w, p).u16(id);
		to_players(w.bytes);
	};
	game.on_damage_peer = [this](int peer, int amount, const std::string &source, const float push[3]) {
		Peer *p = peer_by_id(peer);
		if (!p) {
			return;
		}
		if (p->inv && source == "mob") { // armour takes off 4% a point, up to 60% (as Game::damage)
			const float kept = 1.0f - std::min(0.6f, float(p->inv->armor_points()) * 0.04f);
			amount = std::max(1, int(std::lround(float(amount) * kept)));
		}
		if (p->inv) {
			p->vitals.hurt(amount);
		}
		Writer w(H_STRUCK);
		w.i32(amount).vec3(push);
		send(p->conn, w.bytes);
	};
	game.on_pickup = [this](int peer, const Stack &s) {
		if (Peer *p = peer_by_id(peer)) {
			p->inv_changed = true;
			Writer w(H_PICKED);
			w.str(s.id).u16(uint16_t(std::clamp(s.count, 0, 65535)));
			send(p->conn, w.bytes);
		}
	};
	game.on_shared_sound = [this](const SoundEvent &e) {
		Writer w(H_SOUND);
		w.str(e.set).str(e.kind).vec3(e.at).f32(e.volume_db);
		to_players(w.bytes);
	};
	game.on_effect = [this](const Effect &e) {
		Writer w(H_EFFECT);
		w.str(e.name).vec3(e.at);
		to_players(w.bytes);
	};
}

void Session::detach() {
	if (!a_.game) {
		return;
	}
	if (a_.mods) {
		a_.mods->set_command_runner(nullptr);
	}
	commands_.reset();
	if (is_host()) {
		a_.world->on_block_changed = previous_block_hook_;
		a_.world->set_extra_viewers({});
		a_.game->on_damage_peer = nullptr;
		a_.game->on_pickup = nullptr;
		a_.game->on_shared_sound = nullptr;
		a_.game->on_effect = nullptr;
		a_.game->set_others({});
		saved_players();
	}
	a_.game->set_remote(nullptr);
	previous_block_hook_ = nullptr;
	a_ = {};
}

// --- Sending ----------------------------------------------------------------------

void Session::send(Transport::Connection *to, const std::vector<uint8_t> &bytes, bool reliable) {
	if (to && net_) {
		net_->send(to, bytes, reliable);
	}
}

bool Session::udp_ready() const {
	if (!net_) {
		return false;
	}
	if (is_client()) {
		return net_->udp_ready(server_);
	}
	for (const auto &p : peers_) {
		if (p->joined && !net_->udp_ready(p->conn)) {
			return false;
		}
	}
	return !peers_.empty();
}

void Session::send_host(const std::vector<uint8_t> &bytes, bool reliable) {
	if (is_client() && connected_) {
		send(server_, bytes, reliable);
	}
}

void Session::to_players(const std::vector<uint8_t> &bytes, bool reliable) {
	for (auto &p : peers_) {
		if (p->joined) {
			send(p->conn, bytes, reliable);
		}
	}
}

Session::Peer *Session::peer_by_id(int id) {
	for (auto &p : peers_) {
		if (p->id == id && p->joined) {
			return p.get();
		}
	}
	return nullptr;
}

// --- Receiving --------------------------------------------------------------------

void Session::poll() {
	if (!net_) {
		return;
	}
	Transport::Event ev;
	while (net_ && net_->poll(ev)) {
		// (The host: the player on that connection.)
		Peer *from = nullptr;
		for (auto &p : peers_) {
			if (p->conn == ev.connection) {
				from = p.get();
			}
		}
		switch (ev.type) {
			case Transport::Event::CONNECT:
				if (is_client()) {
					connected_ = true;
					server_ = ev.connection;
					Writer w(C_HELLO);
					w.u16(PROTOCOL).str(name_).u16(uint16_t(mods_.size()));
					for (const std::string &m : mods_) {
						w.str(m);
					}
					send(server_, w.bytes);
				} else {
					auto p = std::make_unique<Peer>();
					p->conn = ev.connection;
					p->id = next_id_++;
					peers_.push_back(std::move(p));
				}
				break;
			case Transport::Event::RECEIVE:
				if (is_client() && welcomed_ && !a_.game) {
					// Welcomed, the world still being built: kept for when it's there.
					held_.push_back(std::move(ev.data));
				} else if (is_client()) {
					on_client_message(ev.data.data(), ev.data.size());
				} else if (from) {
					on_host_message(*from, ev.data.data(), ev.data.size());
				}
				break;
			case Transport::Event::DISCONNECT:
				if (is_client()) {
					end(connected_ ? "The server closed the connection." : "Couldn't connect to the server.");
					connected_ = false;
					server_ = nullptr;
				} else if (from) {
					if (from->joined) {
						log("%s left", from->name.c_str());
						saved_players(); // (keeps its inventory and health)
						if (skins_.erase(from->id)) {
							skins_changed_++;
						}
					}
					std::erase_if(peers_, [&](const auto &p) { return p.get() == from; });
				}
				break;
		}
	}
	if (is_client() && !welcomed_ && ended_.empty() && host_mods_.empty() && now() - started_ > CONNECT_TIMEOUT) {
		end("Connection timed out.");
	}
}

void Session::hello(Peer &p, const uint8_t *data, size_t size) {
	Reader r(data, size);
	r.u8();
	const int protocol = r.u16();
	const std::string requested = r.str(256);
	const int count = r.u16();
	std::vector<std::string> mods;
	for (int i = 0; i < count && i < MAX_MODS && r.ok(); i++) {
		mods.push_back(r.str(256));
	}
	std::sort(mods.begin(), mods.end());
	std::string reason;
	if (!r.ok() || count > MAX_MODS) {
		reason = "Bad hello.";
	} else if (protocol != PROTOCOL) {
		reason = "Game version mismatch (server protocol " + std::to_string(PROTOCOL) + ", yours " +
				std::to_string(protocol) + ").";
	} else if (mods != mods_) {
		auto core = [](const std::vector<std::string> &list) {
			std::vector<std::string> out;
			for (const std::string &m : list) {
				if (m.rfind("core@", 0) == 0) {
					out.push_back(m);
				}
			}
			return out;
		};
		if (core(mods) != core(mods_)) {
			reason = "Game version mismatch (the base game's files differ from the server's).";
		} else if (mod_folders_.empty()) {
			reason = "The server plays with other mods.";
		} else {
			// Same game, other mods: it may download ours (then restarts with them).
			p.offered = true;
			Writer w(H_OFFER_MODS);
			w.u16(uint16_t(mods_.size()));
			for (const std::string &e : mods_) {
				w.str(e).u32(entry_id(e) == "core" ? 0 : uint32_t(pack_of(e).size()));
			}
			send(p.conn, w.bytes);
			log("A player has other mods; offering ours");
			return;
		}
	}
	std::string name = clean_name(requested);
	if (name.empty()) {
		name = "Player " + std::to_string(p.id);
	}
	if (reason.empty() && (name == name_ || std::any_of(peers_.begin(), peers_.end(), [&](const auto &o) {
			return o->joined && o->name == name;
		}))) {
		// Survival inventories are kept by name: one player per name.
		reason = "Someone called " + name + " is already playing here: pick another name.";
	}
	if (reason.empty() && !a_.game) {
		reason = "The server isn't ready yet.";
	}
	if (!reason.empty()) {
		log("Turned a player away: %s", reason.c_str());
		Writer w(H_REJECTED);
		w.str(reason);
		send(p.conn, w.bytes);
		net_->disconnect_later(p.conn); // (after the message)
		return;
	}
	p.name = name;
	p.joined = true;
	// It appears next to us.
	float eye[3];
	a_.player->eye(eye);
	p.eye[0] = eye[0] + 2;
	p.eye[1] = eye[1];
	p.eye[2] = eye[2] + 2;
	p.time = now();
	const float *speeds = a_.game->creative() ? MOVE_CREATIVE : MOVE_SURVIVAL;
	for (int k = 0; k < 3; k++) {
		p.budget[k] = speeds[k] * MOVE_BURST + MOVE_SLACK;
	}
	if (!a_.game->creative()) {
		start_survival(p);
	}
	Data info = world_info_;
	info["id"] = p.id;
	info["mode"] = a_.game->creative() ? "creative" : "survival";
	info["time"] = a_.day ? a_.day->time : 0.3;
	info["day_length"] = a_.day ? a_.day->day_length : DayCycle::DEFAULT_DAY_LENGTH;
	info["spawn"] = {p.eye[0], p.eye[1], p.eye[2]};
	if (const auto &ws = a_.game->world_spawn()) {
		info["world_spawn"] = {(*ws)[0], (*ws)[1], (*ws)[2]};
	}
	Writer w(H_WELCOME);
	w.data(info);
	const auto chunks = a_.world->edited_chunks();
	w.u32(uint32_t(chunks.size()));
	for (const auto &[pos, bytes] : chunks) {
		w.i32(pos.x).i32(pos.y).i32(pos.z).blob(bytes);
	}
	send(p.conn, w.bytes);
	for (const auto &[id, skin] : skins_) { // (everyone's skin, ours too)
		Writer s(H_SKIN);
		s.u32(uint32_t(id)).blob(skin);
		send(p.conn, s.bytes);
	}
	log("%s joined (player %d)", name.c_str(), p.id);
}

bool Session::allow(Peer &p, const std::string &kind) {
	const Rate rate = rates().at(kind);
	const double t = now();
	auto [it, added] = p.rates.try_emplace(kind, std::array<double, 2>{rate.burst, t});
	const double tokens = std::min(double(rate.burst), it->second[0] + (t - it->second[1]) * rate.per_second);
	if (tokens < 1.0) {
		it->second = {tokens, t};
		if (t - p.flood_logged > 5.0) {
			p.flood_logged = t;
			log("%s sends too many %s requests; ignoring some", p.name.c_str(), kind.c_str());
		}
		return false;
	}
	it->second = {tokens - 1.0, t};
	return true;
}

std::string Session::held_item(const Peer &p) const {
	return p.inv ? p.inv->slots[size_t(std::clamp(p.selected, 0, Inventory::HOTBAR - 1))].id : p.item;
}

bool Session::move_allowed(Peer &p, const float eye[3], int flags) {
	const double t = now();
	const float *speeds = a_.game->creative() ? MOVE_CREATIVE : MOVE_SURVIVAL;
	float budget[3], cap[3];
	for (int k = 0; k < 3; k++) {
		cap[k] = speeds[k] * MOVE_BURST + MOVE_SLACK;
		budget[k] = std::min(cap[k], p.budget[k] + speeds[k] * float(t - p.time));
	}
	p.time = t;
	const float d[3] = {eye[0] - p.eye[0], eye[1] - p.eye[1], eye[2] - p.eye[2]};
	const float cost[3] = {std::hypot(d[0], d[2]), std::max(d[1], 0.0f), std::max(-d[1], 0.0f)};
	if (cost[0] <= budget[0] && cost[1] <= budget[1] && cost[2] <= budget[2] &&
			(((p.flags | flags) & FLAG_SLEEP) != 0 || way_clear(p.eye, eye, p))) { // (into or out of bed: eyes low)
		for (int k = 0; k < 3; k++) {
			p.budget[k] = budget[k] - cost[k];
		}
		return true;
	}
	// Respawning: a jump to the world spawn or its bed is fine.
	auto close_to = [&](float x, float z) { return std::hypot(eye[0] - x, eye[2] - z) <= RESPAWN_RADIUS; };
	const auto &spawn = a_.game->world_spawn();
	if ((spawn && close_to((*spawn)[0], (*spawn)[2])) || (p.bed && close_to(p.bed->x + 0.5f, p.bed->z + 0.5f))) {
		std::copy(cap, cap + 3, p.budget);
		p.stuck_at.reset();
		return true;
	}
	std::copy(budget, budget + 3, p.budget);
	return false;
}

bool Session::way_clear(const float from[3], const float to[3], Peer &p) {
	// A body WALL_GRACE thinner on every side, followed in WALL_STEP steps: a
	// corner cut between two updates passes, going through a block doesn't.
	const float lift = Player::EYE_HEIGHT - WALL_GRACE;
	const float body[3] = {Player::RADIUS * 2 - WALL_GRACE * 2, Player::HEIGHT - WALL_GRACE * 2,
			Player::RADIUS * 2 - WALL_GRACE * 2};
	const float a[3] = {from[0], from[1] - lift, from[2]}, b[3] = {to[0], to[1] - lift, to[2]};
	// Where we haven't loaded the chunks we can't tell: no.
	const int c0[3] = {floor_div(int(std::floor(std::min(a[0], b[0]) - 1)), gocube::CHUNK),
			floor_div(int(std::floor(std::min(a[1], b[1]) - 1)), gocube::CHUNK),
			floor_div(int(std::floor(std::min(a[2], b[2]) - 1)), gocube::CHUNK)};
	const int c1[3] = {floor_div(int(std::floor(std::max(a[0], b[0]) + 1)), gocube::CHUNK),
			floor_div(int(std::floor(std::max(a[1], b[1]) + Player::HEIGHT + 1)), gocube::CHUNK),
			floor_div(int(std::floor(std::max(a[2], b[2]) + 1)), gocube::CHUNK)};
	if (c1[0] - c0[0] > 4 || c1[2] - c0[2] > 4) {
		return false;
	}
	for (int cx = c0[0]; cx <= c1[0]; cx++) {
		for (int cy = std::max(c0[1], World::CHUNK_Y_MIN); cy <= std::min(c1[1], World::CHUNK_Y_MAX); cy++) {
			for (int cz = c0[2]; cz <= c1[2]; cz++) {
				if (!a_.world->is_loaded(cx * gocube::CHUNK, cy * gocube::CHUNK, cz * gocube::CHUNK)) {
					return false;
				}
			}
		}
	}
	// A body in a block (one appeared there) may get out, within
	// ESCAPE_DISTANCE of where it got stuck; once out the rest must be clear.
	std::optional<std::array<float, 3>> stuck = p.stuck_at;
	if (a_.world->is_box_free(a, body)) {
		stuck.reset();
	} else if (!stuck) {
		stuck = std::array<float, 3>{from[0], from[1], from[2]};
	}
	bool escaping = stuck.has_value();
	const float length = std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
	const int steps = std::max(1, int(std::ceil(length / WALL_STEP)));
	for (int i = 1; i <= steps; i++) {
		const float f = float(i) / float(steps);
		const float at[3] = {a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f};
		const bool free = a_.world->is_box_free(at, body);
		if (escaping && free) {
			escaping = false; // out: from here on the way must be clear
		} else if (escaping) {
			const float eye_at[3] = {at[0], at[1] + lift, at[2]};
			if (distance(eye_at, stuck->data()) > ESCAPE_DISTANCE) {
				return false;
			}
		} else if (!free) {
			return false;
		}
	}
	p.stuck_at = escaping ? stuck : std::nullopt;
	return true;
}

void Session::do_break(Peer &p, const BlockPos &pos, int slot) {
	const float center[3] = {pos.x + 0.5f, pos.y + 0.5f, pos.z + 0.5f};
	if (p.vitals.dead && p.inv) {
		return;
	}
	if (distance(p.eye, center) > EDIT_REACH) {
		return;
	}
	const int old = a_.world->block_id_at(pos.x, pos.y, pos.z);
	if (old <= 0 || a_.rules->is_fluid(gocube::BlockId(old))) {
		return; // nothing there (or not loaded here)
	}
	std::string tool = p.item;
	if (p.inv) {
		tool = slot >= 0 && slot < Inventory::HOTBAR ? p.inv->slots[size_t(slot)].id : "";
	}
	a_.game->break_block(pos, tool);
	p.inv_changed = p.inv != nullptr;
}

void Session::on_host_message(Peer &p, const uint8_t *data, size_t size) {
	if (size == 0 || !a_.game) {
		return;
	}
	const uint8_t type = data[0];
	if (type == C_HELLO) {
		if (!p.joined) {
			hello(p, data, size);
		}
		return;
	}
	Reader r(data + 1, size - 1);
	if (type == C_MOD_PART) {
		const std::string entry = r.str(256);
		const uint32_t part = r.u32();
		if (!r.ok() || !p.offered || entry_id(entry) == "core" || !allow(p, "mod") ||
				std::find(mods_.begin(), mods_.end(), entry) == mods_.end()) {
			return;
		}
		const std::vector<uint8_t> &pack = pack_of(entry);
		const size_t parts = std::max<size_t>(1, (pack.size() + MOD_PART - 1) / MOD_PART);
		if (part >= parts) {
			return;
		}
		const size_t from = part * MOD_PART, to = std::min(pack.size(), from + MOD_PART);
		Writer w(H_MOD_PART);
		w.str(entry).u32(part).u32(uint32_t(parts)).blob(std::vector<uint8_t>(pack.begin() + from, pack.begin() + to));
		send(p.conn, w.bytes);
		return;
	}
	if (!p.joined) {
		return;
	}
	Game &game = *a_.game;
	const bool dead = p.inv && p.vitals.dead;
	switch (type) {
		case C_STATE: {
			float eye[3];
			r.vec3(eye);
			const float yaw = r.f32(), pitch = r.f32();
			const std::string item = r.str(128);
			const int flags = r.u8() & 7;
			const int swings = int(r.u32());
			const int selected = r.u8();
			if (!r.ok()) {
				return;
			}
			if (!move_allowed(p, eye, flags)) {
				if (now() - p.corrected >= CORRECT_EVERY) {
					p.corrected = now();
					Writer w(H_CORRECT);
					w.vec3(p.eye);
					send(p.conn, w.bytes);
				}
				return;
			}
			std::copy(eye, eye + 3, p.eye);
			p.yaw = std::remainder(yaw, 6.2831853f);
			p.pitch = std::clamp(pitch, -1.5708f, 1.5708f);
			p.selected = std::clamp(selected, 0, Inventory::HOTBAR - 1);
			p.flags = flags;
			p.swings = swings;
			// Only a real item (survival: the one in its hand in our copy); its
			// light comes from its definition, not from the client.
			const bool known = a_.content->items.count(item) || a_.rules->is_placeable(item);
			p.item = known ? item : "";
			p.item = held_item(p);
			auto it = a_.content->blocks.find(p.item);
			p.light = it != a_.content->blocks.end() ? std::clamp(int(it->second.value("light", 0.0)), 0, 15) : 0;
			if (p.inv) {
				float feet[3];
				p.feet(feet);
				p.vitals.moved(*a_.world, *a_.rules, *a_.mods, feet, (p.flags & (FLAG_FLY | FLAG_SLEEP)) != 0);
			}
			break;
		}
		case C_MINING: {
			const BlockPos pos = read_block(r);
			if (r.ok() && allow(p, "mine")) {
				// (not before a break still waiting is done: one block at a time)
				p.mining = pos;
				p.mining_since = std::max(now(), p.mined_at);
			}
			break;
		}
		case C_BREAK: {
			const BlockPos pos = read_block(r);
			const int slot = r.u8();
			if (!r.ok() || !allow(p, "edit") || dead) {
				return;
			}
			if (p.inv) { // survival: no sooner than its mining time after it started
				const std::optional<BlockPos> m = std::exchange(p.mining, std::nullopt);
				const int old = a_.world->block_id_at(pos.x, pos.y, pos.z);
				if (!m || !(*m == pos) || old <= 0) {
					return;
				}
				const std::string tool = slot < Inventory::HOTBAR ? p.inv->slots[size_t(slot)].id : "";
				const double at = p.mining_since +
						game.mine_seconds(a_.rules->id_of(gocube::BlockId(old)), tool) * MINE_SLACK - MINE_LAG;
				p.mined_at = std::max(at, now());
				if (at > now()) {
					p.waiting = Peer::Break{pos, slot, at}; // (done when the time comes)
					return;
				}
			}
			do_break(p, pos, slot);
			break;
		}
		case C_USE: {
			float eye[3], dir[3];
			r.vec3(eye);
			r.vec3(dir);
			const int slot = r.u8();
			std::string item = r.str(128);
			const bool sneak = r.boolean();
			const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
			if (!r.ok() || !allow(p, "edit") || dead || slot >= Inventory::HOTBAR || len < 0.9f || len > 1.1f ||
					distance(eye, p.eye) > USE_EYE_SLACK) {
				p.inv_changed = p.inv != nullptr; // (its copy as it is)
				return;
			}
			if (p.inv) {
				item = p.inv->slots[size_t(slot)].id;
			} else if (!a_.content->items.count(item) && !a_.rules->is_placeable(item)) {
				item.clear();
			}
			Player body; // where it is, looking where it clicked
			body.position[0] = eye[0];
			body.position[1] = eye[1] - Player::EYE_HEIGHT;
			body.position[2] = eye[2];
			body.yaw = std::atan2(-dir[0] / len, -dir[2] / len);
			body.pitch = std::asin(std::clamp(dir[1] / len, -1.0f, 1.0f));
			Actor who;
			who.peer = p.id;
			who.body = &body;
			who.inventory = p.inv.get();
			who.selected = slot;
			who.item = item;
			game.use(who, sneak);
			p.inv_changed = p.inv != nullptr;
			break;
		}
		case C_EAT: {
			const int slot = r.u8();
			if (!r.ok() || !p.inv || !allow(p, "inventory") || slot >= Inventory::HOTBAR) {
				return;
			}
			p.inv_changed = true;
			const std::string item = p.inv->slots[size_t(slot)].id;
			const std::optional<Data> food = item.empty() ? std::nullopt : a_.mods->rule("food", Data::array({item}));
			if (food && food->is_number() && food->get<double>() > 0 && !p.vitals.dead &&
					p.vitals.health < Vitals::MAX_HEALTH) {
				p.vitals.heal(int(food->get<double>()));
				p.inv->take_one(slot);
			}
			break;
		}
		case C_HIT: {
			const int id = int(r.u32());
			if (!r.ok() || !allow(p, "hit") || dead) {
				return;
			}
			const Creature *c = game.creatures().find(id);
			if (!c) {
				return;
			}
			const float at[3] = {c->pos[0], c->pos[1] + c->box[1] * 0.5f, c->pos[2]};
			if (distance(at, p.eye) > CREATURE_REACH) {
				return;
			}
			const std::string item = held_item(p);
			auto it = a_.content->items.find(item);
			const int damage = it != a_.content->items.end() ? int(it->second.value("damage", 2.0)) : 2;
			float feet[3];
			p.feet(feet);
			a_.mods->creature_hit(id, damage, feet, p.id);
			break;
		}
		case C_INVENTORY: {
			const uint32_t seq = r.u32();
			const Data action = r.data(512);
			if (!r.ok() || !p.inv) {
				return;
			}
			// Every numbered request counts as done, refused or not, so the
			// client knows when our copy has caught up with it.
			p.done = seq;
			p.inv_changed = true;
			if (!allow(p, "inventory") || dead) {
				return;
			}
			if (action.is_array() && action.size() == 5 && action[0] == "container") {
				// Only a container it opened.
				const auto key = std::make_tuple(action[1].is_number_integer() ? action[1].get<int>() : 0,
						action[2].is_number_integer() ? action[2].get<int>() : 0,
						action[3].is_number_integer() ? action[3].get<int>() : 0);
				if (!p.viewing.count(key) || !allow(p, "container")) {
					return;
				}
			}
			float feet[3];
			p.feet(feet);
			game.apply_inventory_action(*p.inv, action, feet);
			break;
		}
		case C_OPEN: {
			const BlockPos pos = read_block(r);
			const float center[3] = {pos.x + 0.5f, pos.y + 0.5f, pos.z + 0.5f};
			if (r.ok() && allow(p, "container") && game.block_entities().at(pos) &&
					distance(center, p.eye) <= CONTAINER_REACH) {
				p.viewing[{pos.x, pos.y, pos.z}] = ""; // (sent with the next update)
			}
			break;
		}
		case C_CLOSE: {
			const BlockPos pos = read_block(r);
			p.viewing.erase({pos.x, pos.y, pos.z});
			break;
		}
		case C_CHUNK: {
			const ChunkPos c{r.i32(), r.i32(), r.i32()};
			if (r.ok() && allow(p, "chunk")) {
				const std::vector<uint8_t> bytes = a_.world->chunk_bytes(c);
				if (!bytes.empty()) {
					Writer w(H_CHUNK);
					w.i32(c.x).i32(c.y).i32(c.z).blob(bytes);
					send(p.conn, w.bytes);
				}
			}
			break;
		}
		case C_BED: {
			const BlockPos pos = read_block(r);
			const float center[3] = {pos.x + 0.5f, pos.y + 0.5f, pos.z + 0.5f};
			if (r.ok() && allow(p, "other") && distance(center, p.eye) <= CONTAINER_REACH &&
					a_.rules->def(a_.world->block_at(pos.x, pos.y, pos.z)).value("bed", false)) {
				p.bed = pos; // respawning there is a jump we allow
			}
			break;
		}
		case C_SLEEP: {
			const bool on = r.boolean();
			if (r.ok() && allow(p, "other")) {
				p.sleeping = on;
			}
			break;
		}
		case C_RESPAWNED:
			if (p.inv && p.vitals.dead) {
				p.vitals.respawn();
			}
			break;
		case C_CHAT: {
			const std::string text = r.str(MAX_CHAT);
			if (r.ok() && !text.empty() && allow(p, "chat")) {
				handle_chat(p.id, text);
			}
			break;
		}
		case C_SKIN: {
			std::vector<uint8_t> skin = r.blob(SKIN_BYTES);
			if (!r.ok() || skin.size() != SKIN_BYTES || !allow(p, "skin")) {
				break;
			}
			Writer w(H_SKIN);
			w.u32(uint32_t(p.id)).blob(skin);
			for (auto &other : peers_) { // (to the others; it has its own)
				if (other->joined && other.get() != &p) {
					send(other->conn, w.bytes);
				}
			}
			skins_[p.id] = std::move(skin);
			skins_changed_++;
			break;
		}
		default:
			break;
	}
}

void Session::on_client_message(const uint8_t *data, size_t size) {
	if (size == 0) {
		return;
	}
	Reader r(data + 1, size - 1);
	const uint8_t type = data[0];
	if (type == H_REJECTED) {
		const std::string reason = r.str();
		end(reason.empty() ? "The server turned us away." : reason);
		return;
	}
	if (type == H_OFFER_MODS) {
		const int count = r.u16();
		std::vector<std::string> entries;
		std::map<std::string, size_t> missing;
		size_t total = 0;
		bool valid = count <= MAX_MODS;
		for (int i = 0; i < count && valid; i++) {
			const std::string e = r.str(256);
			const size_t bytes = r.u32();
			valid = r.ok() && valid_entry(e);
			entries.push_back(e);
			if (valid && entry_id(e) != "core" && !local_mods_.count(e)) {
				missing[e] = bytes;
				total += bytes;
			}
		}
		if (!valid) {
			end("The server sent a bad mod list.");
		} else if (!restarted_with_.empty() && entries == restarted_with_) {
			end("Still not the server's mods after restarting with them; check the log.");
		} else if (total > MAX_MOD_DOWNLOAD) {
			end("The server's mods are too big to download (" + std::to_string(total >> 20) + " MB).");
		} else {
			host_mods_ = std::move(entries);
			missing_ = std::move(missing);
		}
		return;
	}
	if (type == H_MOD_PART) {
		const std::string entry = r.str(256);
		const uint32_t part = r.u32(), parts = r.u32();
		const std::vector<uint8_t> bytes = r.blob(MOD_PART);
		auto it = downloads_.find(entry);
		if (!r.ok() || it == downloads_.end() || it->second.done) {
			return;
		}
		Download &d = it->second;
		if (d.parts == 0) {
			d.parts = parts;
		}
		if (part != d.data.size() / MOD_PART || parts != d.parts || size_t(parts) * MOD_PART > MAX_MOD_DOWNLOAD + MOD_PART ||
				(part + 1 < parts && bytes.size() != MOD_PART)) {
			end("A mod download from the server went wrong.");
			return;
		}
		d.data.insert(d.data.end(), bytes.begin(), bytes.end());
		if (part + 1 < parts) {
			Writer w(C_MOD_PART);
			w.str(entry).u32(part + 1);
			send(server_, w.bytes);
			return;
		}
		// Whole: unpacked into the cache, and it must be exactly that mod.
		const std::string id = entry_id(entry), fp = entry_fingerprint(entry);
		const std::filesystem::path dir = cache_ / (id + "-" + fp);
		std::error_code ec;
		std::filesystem::remove_all(dir, ec); // (a damaged copy from before)
		std::string problem = unpack_mod(d.data, dir);
		if (problem.empty()) {
			ModInfo info;
			Data m;
			std::string why;
			if (!read_lua_data(dir / "mod.lua", m, why) || !m.is_object()) {
				problem = "its mod.lua can't be read";
			} else {
				info.id = m.value("id", "");
				info.version = m.value("version", "");
				info.fingerprint = mod_fingerprint(dir);
				if (mod_entry(info) != entry) {
					problem = "its files don't match its fingerprint";
				}
			}
			if (!problem.empty()) {
				std::filesystem::remove_all(dir, ec);
			}
		}
		if (!problem.empty()) {
			end("The server's mod " + id + ": " + problem + ".");
			return;
		}
		d.done = true;
		local_mods_[entry] = dir;
		log("Downloaded the server's mod %s", entry.c_str());
		mods_ready_ = std::all_of(downloads_.begin(), downloads_.end(), [](const auto &kv) { return kv.second.done; });
		return;
	}
	if (type == H_CHAT) {
		const std::string line = r.str(MAX_CHAT * 2);
		if (r.ok() && chat_in_.size() < 200) {
			chat_in_.push_back(line);
		}
		return;
	}
	if (type == H_SKIN) { // (may come before we play)
		const int id = int(r.u32());
		std::vector<uint8_t> skin = r.blob(SKIN_BYTES);
		if (r.ok() && skin.size() == SKIN_BYTES && id != my_id_) {
			skins_[id] = std::move(skin);
			skins_changed_++;
		}
		return;
	}
	if (type == H_WELCOME) {
		if (welcomed_) {
			return;
		}
		Data info = r.data(4 << 20);
		const uint32_t count = r.u32();
		std::vector<std::pair<ChunkPos, std::vector<uint8_t>>> chunks;
		for (uint32_t i = 0; i < count && r.ok(); i++) {
			const ChunkPos c{r.i32(), r.i32(), r.i32()};
			chunks.push_back({c, r.blob(MAX_CHUNK_BYTES)});
		}
		if (!r.ok() || !info.is_object() || !info.contains("block_ids") || !info.contains("seed")) {
			end("The server sent a bad welcome.");
			return;
		}
		welcome_ = std::move(info);
		welcome_chunks_ = std::move(chunks);
		my_id_ = int(welcome_.value("id", 0.0));
		welcomed_ = true;
		return;
	}
	if (!a_.game) {
		return; // (not playing yet)
	}
	Game &game = *a_.game;
	switch (type) {
		case H_PLAYERS: {
			const int count = r.u16();
			std::vector<Avatar> states;
			for (int i = 0; i < count && i < 64 && r.ok(); i++) {
				Avatar a;
				a.id = int(r.u32());
				a.name = r.str(64);
				r.vec3(a.eye);
				a.yaw = r.f32();
				a.pitch = r.f32();
				a.item = r.str(128);
				a.flags = r.u8();
				a.swings = int(r.u32());
				a.light = std::min<int>(r.u8(), 15);
				states.push_back(std::move(a));
			}
			if (!r.ok()) {
				return;
			}
			player_count_ = int(states.size());
			std::erase_if(states, [&](const Avatar &a) { return a.id == my_id_; });
			merge_avatars(states);
			break;
		}
		case H_BLOCK: {
			const BlockPos pos = read_block(r);
			const gocube::BlockId id = r.u16();
			if (!r.ok()) {
				return;
			}
			const int old = a_.world->block_id_at(pos.x, pos.y, pos.z);
			if (old < 0 || !a_.world->set_block(pos, id)) {
				// Not loaded here: get the whole chunk (else it would come back
				// as the generator makes it).
				Writer w(C_CHUNK);
				w.i32(floor_div(pos.x, gocube::CHUNK)).i32(floor_div(pos.y, gocube::CHUNK)).i32(floor_div(pos.z, gocube::CHUNK));
				send_host(w.bytes);
				return;
			}
			if (old != int(id)) { // what it sounds like: breaking, or placing
				const float center[3] = {pos.x + 0.5f, pos.y + 0.5f, pos.z + 0.5f};
				if (id == 0) {
					game.play_block_sound(a_.rules->id_of(gocube::BlockId(old)), "break", center);
				} else if (!a_.rules->is_fluid(id)) {
					game.play_block_sound(a_.rules->id_of(id), "place", center);
				}
			}
			break;
		}
		case H_CHUNK: {
			const ChunkPos c{r.i32(), r.i32(), r.i32()};
			const std::vector<uint8_t> bytes = r.blob(MAX_CHUNK_BYTES);
			if (r.ok()) {
				a_.world->set_chunk(c, bytes.data(), bytes.size());
			}
			break;
		}
		case H_TIME: {
			const double t = r.f64(), length = r.f64();
			if (r.ok() && a_.day) {
				a_.day->day_length = std::max(10.0, length);
				const double drift = std::abs(std::remainder(t - a_.day->time, 1.0)) * a_.day->day_length;
				if (drift > 0.5) { // seconds
					a_.day->time = std::fmod(std::fmod(t, 1.0) + 1.0, 1.0);
				}
			}
			break;
		}
		case H_CORRECT: {
			std::array<float, 3> eye;
			r.vec3(eye.data());
			if (r.ok()) {
				correction_ = eye;
			}
			break;
		}
		case H_INVENTORY: {
			const uint32_t done = r.u32();
			const Data got = r.data();
			// Shown only when no action of ours is still on its way (it would
			// be undone for a moment).
			if (r.ok() && got.is_object() && done >= inventory_seq_) {
				Inventory &inv = game.inventory();
				inv.deserialize(got.value("inventory", Data::array()));
				const Data cursor = got.value("cursor", Data());
				if (cursor.is_array() && cursor.size() == 2 && cursor[0].is_string() && cursor[1].is_number()) {
					inv.cursor = {cursor[0].get<std::string>(), int(cursor[1].get<double>())};
				}
			}
			break;
		}
		case H_HEALTH: {
			const int h = r.i32();
			if (r.ok()) {
				game.set_health_from_host(h);
			}
			break;
		}
		case H_STRUCK: {
			r.i32();
			std::array<float, 3> push;
			r.vec3(push.data());
			if (r.ok()) {
				push_ = push;
			}
			break;
		}
		case H_DROPS: {
			const int count = r.u16();
			std::vector<ItemDrops::Drop> drops;
			for (int i = 0; i < count && r.ok(); i++) {
				ItemDrops::Drop d;
				d.item = r.str(128);
				d.count = r.u16();
				r.vec3(d.pos);
				d.age = r.f32();
				d.turn = r.f32();
				drops.push_back(std::move(d));
			}
			if (r.ok()) {
				game.set_drops(std::move(drops));
			}
			break;
		}
		case H_CREATURES: {
			const int count = r.u16();
			std::vector<CreatureState> states;
			for (int i = 0; i < count && i < Creatures::MAX && r.ok(); i++) {
				CreatureState s;
				s.id = int(r.u32());
				s.kind = r.str(128);
				s.variant = r.str(128);
				r.vec3(s.pos);
				s.yaw = r.f32();
				const int flags = r.u8();
				s.walking = flags & 1;
				s.on_floor = flags & 2;
				s.blocked = flags & 4;
				s.flash = r.f32();
				s.hurt_time = r.f32();
				s.strikes = r.u16();
				states.push_back(std::move(s));
			}
			if (r.ok()) {
				game.creatures().apply_states(*a_.content, states);
			}
			break;
		}
		case H_CONTAINER: {
			const BlockPos pos = read_block(r);
			const Data got = r.data();
			if (r.ok()) {
				game.block_entities().set_entity_data(pos, got);
			}
			break;
		}
		case H_PICKED: {
			r.str(128);
			r.u16();
			float eye[3];
			a_.player->eye(eye);
			game.play_sound("core:effects", "pickup", eye, -8.0f);
			break;
		}
		case H_SOUND: {
			const std::string set = r.str(128), kind = r.str(64);
			float at[3];
			r.vec3(at);
			const float db = r.f32();
			if (r.ok()) {
				game.play_sound(set, kind, at, std::clamp(db, -60.0f, 12.0f));
			}
			break;
		}
		case H_EFFECT: {
			Effect e;
			e.name = r.str(64);
			r.vec3(e.at);
			if (r.ok()) {
				game.add_effect(std::move(e));
			}
			break;
		}
		case H_WAKE:
			woken_ = true;
			break;
		case H_WORLD_SPAWN: {
			float at[3];
			r.vec3(at);
			if (r.ok()) {
				game.set_world_spawn(Data::array({at[0], at[1], at[2]}));
			}
			break;
		}
		case H_MODE: {
			const uint8_t on = r.u8();
			if (r.ok()) {
				game.set_creative(on != 0); // (survival: the inventory follows from the host)
			}
			break;
		}
		default:
			break;
	}
}

// --- Every frame ------------------------------------------------------------------

void Session::update(float dt, const LocalPose &pose) {
	last_pose_ = pose;
	if (is_host() && a_.game) {
		host_update(dt);
	} else if (is_client() && a_.game) {
		client_update(dt, pose);
	}
	update_avatars(dt);
	if (net_) {
		net_->flush();
	}
}

std::vector<uint8_t> Session::players_message(const LocalPose &pose) const {
	Writer w(H_PLAYERS);
	int count = 1;
	for (const auto &p : peers_) {
		count += p->joined ? 1 : 0;
	}
	w.u16(uint16_t(count));
	w.u32(HOST_ID).str(name_).vec3(pose.eye).f32(pose.yaw).f32(pose.pitch).str(pose.item).u8(uint8_t(pose.flags & 7))
			.u32(uint32_t(pose.swings)).u8(uint8_t(std::clamp(pose.light, 0, 15)));
	for (const auto &p : peers_) {
		if (p->joined) {
			w.u32(uint32_t(p->id)).str(p->name).vec3(p->eye).f32(p->yaw).f32(p->pitch).str(p->item)
					.u8(uint8_t(p->flags)).u32(uint32_t(p->swings)).u8(uint8_t(p->light));
		}
	}
	return w.bytes;
}

void Session::host_update(float dt) {
	Game &game = *a_.game;
	// Other players: chunks stay loaded around them; the game knows where
	// they are (creatures, drops, placing).
	std::vector<std::array<float, 3>> viewers;
	std::vector<OtherPlayer> others;
	for (auto &p : peers_) {
		if (!p->joined) {
			continue;
		}
		OtherPlayer o;
		o.peer = p->id;
		p->feet(o.feet);
		o.alive = !(p->inv && p->vitals.dead);
		o.inventory = p->inv.get();
		others.push_back(o);
		viewers.push_back({o.feet[0], o.feet[1], o.feet[2]});
	}
	a_.world->set_extra_viewers(std::move(viewers));
	game.set_others(std::move(others));

	const double t = now();
	for (auto &p : peers_) {
		if (!p->joined) {
			continue;
		}
		if (p->waiting && t >= p->waiting->at) { // a break that waited for its mining time
			const Peer::Break b = *p->waiting;
			p->waiting.reset();
			do_break(*p, b.pos, b.slot);
		}
		if (p->inv) {
			float feet[3];
			p->feet(feet);
			p->vitals.tick(*a_.world, *a_.rules, *a_.mods, feet, (p->flags & FLAG_SLEEP) != 0, dt);
			if (p->vitals.changed) {
				p->vitals.changed = false;
				Writer w(H_HEALTH);
				w.i32(p->vitals.health);
				send(p->conn, w.bytes);
			}
			if (p->inv_changed) {
				p->inv_changed = false;
				Data data = {{"inventory", p->inv->serialize()}};
				data["cursor"] = p->inv->cursor.empty() ? Data(false) : Data::array({p->inv->cursor.id, p->inv->cursor.count});
				Writer w(H_INVENTORY);
				w.u32(p->done).data(data);
				send(p->conn, w.bytes);
			}
		}
	}

	time_timer_ += dt;
	if (time_timer_ >= TIME_SYNC_SECONDS && a_.day) {
		time_timer_ = 0;
		Writer w(H_TIME);
		w.f64(a_.day->time).f64(a_.day->day_length);
		to_players(w.bytes);
	}
	world_timer_ += dt;
	if (world_timer_ >= WORLD_INTERVAL) {
		world_timer_ = 0;
		Writer d(H_DROPS);
		const auto &drops = game.drops().drops();
		const size_t n = std::min<size_t>(drops.size(), 512);
		d.u16(uint16_t(n));
		for (size_t i = 0; i < n; i++) {
			d.str(drops[i].item).u16(uint16_t(std::clamp(drops[i].count, 0, 65535))).vec3(drops[i].pos)
					.f32(drops[i].age).f32(drops[i].turn);
		}
		to_players(d.bytes, false);
		Writer c(H_CREATURES);
		const std::vector<CreatureState> states = game.creatures().states();
		c.u16(uint16_t(states.size()));
		for (const CreatureState &s : states) {
			c.u32(uint32_t(s.id)).str(s.kind).str(s.variant).vec3(s.pos).f32(s.yaw)
					.u8(uint8_t((s.walking ? 1 : 0) | (s.on_floor ? 2 : 0) | (s.blocked ? 4 : 0)))
					.f32(s.flash).f32(s.hurt_time).u16(uint16_t(s.strikes));
		}
		to_players(c.bytes, false);
		// Containers players look at: when they change.
		for (auto &p : peers_) {
			for (auto &[key, sent] : p->viewing) {
				const BlockPos pos{std::get<0>(key), std::get<1>(key), std::get<2>(key)};
				const Data data = game.block_entities().entity_data(pos);
				const std::string text = data.is_null() ? std::string() : write_lua_data(data);
				if (text != sent) {
					sent = text;
					Writer w(H_CONTAINER);
					block_pos(w, pos).data(data.is_null() ? Data(false) : data);
					send(p->conn, w.bytes);
				}
			}
		}
	}
	state_timer_ += dt;
	if (state_timer_ >= STATE_INTERVAL) {
		state_timer_ = 0;
		std::vector<Avatar> states;
		for (auto &p : peers_) {
			if (p->joined) {
				Avatar a;
				a.id = p->id;
				a.name = p->name;
				std::copy(p->eye, p->eye + 3, a.eye);
				a.yaw = p->yaw;
				a.pitch = p->pitch;
				a.item = p->item;
				a.flags = p->flags;
				a.swings = p->swings;
				a.light = p->light;
				states.push_back(std::move(a));
			}
		}
		merge_avatars(states);
		to_players(players_message(last_pose_), false);
	}
}

// --- Chat and commands -------------------------------------------------------------

namespace {

// A typed line, cleaned: no control characters, MAX_CHAT bytes at most.
std::string clean_chat(const std::string &text) {
	std::string out;
	for (char c : text) {
		if (static_cast<unsigned char>(c) >= 32 && c != 127) {
			out += c;
		}
	}
	const size_t start = out.find_first_not_of(' ');
	if (start == std::string::npos) {
		return "";
	}
	out.erase(0, start);
	if (out.size() > Session::MAX_CHAT) {
		size_t n = Session::MAX_CHAT;
		while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xc0) == 0x80) {
			n--; // (whole UTF-8 characters)
		}
		out.resize(n);
	}
	return out;
}

} // namespace

void Session::chat(const std::string &typed) {
	const std::string text = clean_chat(typed);
	if (text.empty()) {
		return;
	}
	if (is_client()) {
		Writer w(C_CHAT);
		w.str(text);
		send_host(w.bytes);
	} else {
		handle_chat(0, text);
	}
}

std::vector<std::string> Session::complete_command(const std::string &typed) const {
	CommandPlayer me;
	me.op = true;
	if (commands_) {
		return commands_->complete(me, typed);
	}
	static Commands builtin{CommandWorld{}}; // (a client: the game's own names)
	return builtin.complete(me, typed);
}

void Session::handle_chat(int peer, const std::string &typed) {
	const std::string text = clean_chat(typed);
	if (text.empty() || !commands_) {
		return;
	}
	CommandPlayer who;
	for (const CommandPlayer &p : command_world().players()) {
		if (p.peer == peer) {
			who = p;
		}
	}
	if (who.name.empty()) {
		return; // (gone)
	}
	if (text[0] == '/') {
		log("%s: %s", who.name.c_str(), text.c_str());
		for (const std::string &line : commands_->run(who, text)) {
			chat_to(peer, line);
		}
	} else {
		chat_all("<" + who.name + "> " + text);
	}
}

void Session::chat_to(int peer, const std::string &line) {
	if (peer == 0) {
		chat_in_.push_back(line);
		return;
	}
	if (Peer *p = peer_by_id(peer)) {
		Writer w(H_CHAT);
		w.str(line);
		send(p->conn, w.bytes);
	}
}

void Session::chat_all(const std::string &line) {
	chat_in_.push_back(line);
	if (is_host()) {
		Writer w(H_CHAT);
		w.str(line);
		to_players(w.bytes);
	}
}

// What commands act on: our game and world, and (hosting) the joined players.
CommandWorld Session::command_world() {
	CommandWorld w;
	w.players = [this] {
		std::vector<CommandPlayer> out;
		CommandPlayer me;
		me.name = is_host() ? name_ : local_name_;
		me.op = true;
		if (a_.player) {
			std::copy(a_.player->position, a_.player->position + 3, me.feet);
		}
		out.push_back(me);
		for (auto &p : peers_) {
			if (p->joined) {
				CommandPlayer c;
				c.peer = p->id;
				c.name = p->name;
				p->feet(c.feet);
				c.op = std::find(ops_.begin(), ops_.end(), p->name) != ops_.end();
				out.push_back(c);
			}
		}
		return out;
	};
	w.is_block = [this](const std::string &id) { return a_.content && a_.content->block_index(id) >= 0; };
	w.is_item = [this](const std::string &id) { return a_.content && a_.content->items.count(id) > 0; };
	w.is_creature = [this](const std::string &id) { return a_.content && a_.content->creatures.count(id) > 0; };
	w.teleport = [this](int peer, const float feet[3]) {
		if (peer == 0) {
			if (!a_.player) {
				return false;
			}
			std::copy(feet, feet + 3, a_.player->position);
			std::fill(a_.player->velocity, a_.player->velocity + 3, 0.0f);
			return true;
		}
		Peer *p = peer_by_id(peer);
		if (!p) {
			return false;
		}
		p->eye[0] = feet[0];
		p->eye[1] = feet[1] + Player::EYE_HEIGHT;
		p->eye[2] = feet[2];
		p->stuck_at.reset();
		Writer w(H_CORRECT); // (the client goes where we say)
		w.vec3(p->eye);
		send(p->conn, w.bytes);
		return true;
	};
	w.give = [this](int peer, const std::string &item, int count) {
		Inventory *inv = nullptr;
		if (peer == 0) {
			inv = a_.game ? &a_.game->inventory() : nullptr;
		} else if (Peer *p = peer_by_id(peer)) {
			inv = p->inv.get(); // (null in creative: its own palette)
			p->inv_changed = inv != nullptr;
		}
		return inv ? count - inv->add(item, count) : 0;
	};
	w.kill = [this](int peer) {
		if (peer == 0) {
			if (!a_.game || a_.game->creative() || a_.game->dead()) {
				return false;
			}
			a_.game->damage(1 << 20, "command");
			return true;
		}
		Peer *p = peer_by_id(peer);
		if (!p || !p->inv) {
			return false;
		}
		p->vitals.hurt(1 << 20);
		return true;
	};
	w.set_world_spawn = [this](const float feet[3]) {
		if (a_.game) {
			a_.game->set_world_spawn(Data::array({feet[0], feet[1], feet[2]}));
		}
		if (is_host()) {
			Writer w(H_WORLD_SPAWN);
			w.vec3(feet);
			to_players(w.bytes);
		}
	};
	w.creative = [this] { return a_.game && a_.game->creative(); };
	w.set_creative = [this](bool on) {
		if (!a_.game || a_.game->creative() == on) {
			return;
		}
		if (on) {
			saved_players(); // (survival players' inventories, for when it comes back)
		}
		a_.game->set_creative(on);
		if (!is_host()) {
			return;
		}
		for (auto &p : peers_) {
			if (!p->joined) {
				continue;
			}
			if (on) {
				p->inv.reset(); // (creative: its own palette)
				p->waiting.reset();
				p->vitals = Vitals();
			} else {
				start_survival(*p);
			}
		}
		Writer w(H_MODE);
		w.u8(on ? 1 : 0);
		to_players(w.bytes);
	};
	w.time = [this] { return a_.day ? a_.day->time : 0.0; };
	w.set_time = [this](double t) {
		if (a_.day) {
			a_.day->time = t;
			time_timer_ = TIME_SYNC_SECONDS; // (everyone gets it at once)
		}
	};
	w.set_block = [this](const BlockPos &pos, const std::string &id) {
		const int index = a_.content ? a_.content->block_index(id) : -1;
		return index >= 0 && a_.world && a_.world->set_block(pos, gocube::BlockId(index));
	};
	w.summon = [this](const std::string &kind, const float at[3]) {
		return a_.game && a_.content && a_.game->creatures().spawn(*a_.content, kind, at) > 0;
	};
	w.kick = [this](int peer, const std::string &why) {
		Peer *p = peer_by_id(peer);
		if (!p) {
			return false;
		}
		log("Kicked %s: %s", p->name.c_str(), why.c_str());
		Writer w(H_REJECTED);
		w.str(why);
		send(p->conn, w.bytes);
		net_->disconnect_later(p->conn); // (after the message)
		return true;
	};
	w.set_op = [this](const std::string &name, bool on) {
		std::erase(ops_, name);
		if (on) {
			ops_.push_back(name);
		}
	};
	w.tell = [this](int peer, const std::string &line) { chat_to(peer, line); };
	w.say_all = [this](const std::string &line) { chat_all(line); };
	w.seed = uint32_t(world_info_.value("seed", 0.0));
	w.mod_commands = [this] { return a_.mods ? a_.mods->commands() : std::vector<ModCommand>{}; };
	w.run_mod_command = [this](const std::string &name, const CommandPlayer &who, const std::vector<std::string> &args) {
		return a_.mods ? a_.mods->run_command(name, who, args) : std::nullopt;
	};
	w.function = [this](const std::string &name) { return a_.mods ? a_.mods->function(name) : nullptr; };
	return w;
}

void Session::set_skin(std::vector<uint8_t> rgba) {
	if (rgba.size() != SKIN_BYTES) {
		rgba.clear();
	}
	my_skin_ = std::move(rgba);
	skin_sent_ = false;
	if (is_host() && !my_skin_.empty()) {
		skins_[HOST_ID] = my_skin_;
		Writer w(H_SKIN);
		w.u32(uint32_t(HOST_ID)).blob(my_skin_);
		to_players(w.bytes);
	}
}

const std::vector<uint8_t> *Session::skin_of(int id) const {
	auto it = skins_.find(id);
	return it != skins_.end() ? &it->second : nullptr;
}

void Session::client_update(float dt, const LocalPose &pose) {
	if (welcomed_ && !skin_sent_ && !my_skin_.empty()) {
		skin_sent_ = true;
		Writer w(C_SKIN);
		w.blob(my_skin_);
		send_host(w.bytes);
	}
	state_timer_ += dt;
	if (state_timer_ >= STATE_INTERVAL && welcomed_) {
		state_timer_ = 0;
		Writer w(C_STATE);
		w.vec3(pose.eye).f32(pose.yaw).f32(pose.pitch).str(pose.item).u8(uint8_t(pose.flags & 7))
				.u32(uint32_t(pose.swings)).u8(uint8_t(std::clamp(pose.selected, 0, Inventory::HOTBAR - 1)));
		send_host(w.bytes, false);
	}
}

void Session::merge_avatars(const std::vector<Avatar> &states) {
	std::erase_if(avatars_, [&](const Avatar &a) {
		return std::none_of(states.begin(), states.end(), [&](const Avatar &s) { return s.id == a.id; });
	});
	for (const Avatar &s : states) {
		auto it = std::find_if(avatars_.begin(), avatars_.end(), [&](const Avatar &a) { return a.id == s.id; });
		if (it == avatars_.end()) {
			Avatar a = s;
			std::copy(s.eye, s.eye + 3, a.shown);
			a.shown_yaw = s.yaw;
			a.seen_swings = s.swings;
			avatars_.push_back(std::move(a));
			continue;
		}
		it->name = s.name;
		std::copy(s.eye, s.eye + 3, it->eye);
		it->yaw = s.yaw;
		it->pitch = s.pitch;
		it->item = s.item;
		it->flags = s.flags;
		it->swings = s.swings;
		it->light = s.light;
	}
}

void Session::update_avatars(float dt) {
	if (dt <= 0) {
		return;
	}
	for (Avatar &a : avatars_) {
		// Smooth between the 20 Hz updates; the speed comes from how far the
		// figure moves.
		const float t = 1.0f - std::exp(-15.0f * dt), v = 1.0f - std::exp(-10.0f * dt);
		for (int k = 0; k < 3; k++) {
			const float before = a.shown[k];
			a.shown[k] += (a.eye[k] - a.shown[k]) * t;
			a.velocity[k] += ((a.shown[k] - before) / dt - a.velocity[k]) * v;
		}
		a.shown_yaw = lerp_angle(a.shown_yaw, a.yaw, t);
		a.figure.move_speed = std::hypot(a.velocity[0], a.velocity[2]);
		a.figure.on_floor = (a.flags & FLAG_FLY) == 0 && std::abs(a.velocity[1]) < 1.5f;
		a.figure.sneaking = (a.flags & FLAG_SNEAK) != 0;
		a.figure.lying = (a.flags & FLAG_SLEEP) != 0;
		a.figure.pitch = a.pitch;
		if (a.swings != a.seen_swings) {
			a.seen_swings = a.swings;
			a.figure.swing();
		}
		a.figure.update(dt);
	}
}

int Session::player_count() const {
	if (is_host()) {
		int n = 1;
		for (const auto &p : peers_) {
			n += p->joined ? 1 : 0;
		}
		return n;
	}
	return is_client() ? player_count_ : 1;
}

std::vector<std::array<float, 4>> Session::held_lights() const {
	std::vector<std::array<float, 4>> out;
	for (const Avatar &a : avatars_) {
		if (a.light > 0) {
			out.push_back({a.shown[0], a.shown[1], a.shown[2], float(a.light)});
		}
	}
	return out;
}

// --- Beds and sleeping --------------------------------------------------------------

void Session::set_sleeping(bool on) {
	Writer w(C_SLEEP);
	w.boolean(on);
	send_host(w.bytes);
}

bool Session::peers_sleeping() const {
	return std::all_of(peers_.begin(), peers_.end(), [](const auto &p) { return !p->joined || p->sleeping; });
}

void Session::night_skipped() {
	if (!is_host()) {
		return;
	}
	for (auto &p : peers_) {
		p->sleeping = false;
	}
	if (a_.day) {
		Writer w(H_TIME);
		w.f64(a_.day->time).f64(a_.day->day_length);
		to_players(w.bytes);
	}
	to_players(Writer(H_WAKE).bytes);
}

// Survival: we keep its inventory and health (back from last time).
void Session::start_survival(Peer &p) {
	p.inv = std::make_unique<Inventory>(*a_.content);
	p.vitals = Vitals();
	const Data saved = saved_players_.value(p.name, Data::object());
	if (saved.is_object() && saved.contains("inventory")) {
		p.inv->deserialize(saved["inventory"]);
	} else {
		a_.game->give_starting_items(*p.inv);
	}
	if (saved.is_object() && saved.contains("health") && saved["health"].is_number()) {
		p.vitals.health = std::clamp(int(saved["health"].get<double>()), 1, Vitals::MAX_HEALTH);
	}
	p.vitals.changed = true;
	p.inv_changed = true;
}

// --- Saved players ------------------------------------------------------------------

void Session::set_saved_players(const Data &players) {
	saved_players_ = players.is_object() ? players : Data::object();
}

Data Session::saved_players() {
	for (const auto &p : peers_) {
		if (p->joined && p->inv) {
			// What the mouse holds goes back in first.
			Inventory copy = *p->inv;
			if (!copy.cursor.empty()) {
				copy.add(copy.cursor.id, copy.cursor.count);
				copy.cursor = {};
			}
			Data entry = {{"inventory", copy.serialize()}};
			if (!p->vitals.dead) {
				entry["health"] = p->vitals.health;
			}
			saved_players_[p->name] = entry;
		}
	}
	return saved_players_;
}

// --- Mods --------------------------------------------------------------------------

const std::vector<uint8_t> &Session::pack_of(const std::string &entry) {
	auto it = packs_.find(entry);
	if (it == packs_.end()) {
		auto dir = mod_folders_.find(entry);
		it = packs_.emplace(entry, dir == mod_folders_.end() ? std::vector<uint8_t>() : pack_mod(dir->second)).first;
	}
	return it->second;
}

void Session::download_mods(const std::filesystem::path &cache) {
	cache_ = cache;
	std::error_code ec;
	std::filesystem::create_directories(cache, ec);
	downloads_.clear();
	for (const auto &[entry, bytes] : missing_) {
		downloads_[entry] = {};
		Writer w(C_MOD_PART);
		w.str(entry).u32(0);
		send(server_, w.bytes);
	}
	mods_ready_ = downloads_.empty();
}

size_t Session::downloaded_bytes() const {
	size_t n = 0;
	for (const auto &[entry, d] : downloads_) {
		n += d.data.size();
	}
	return n;
}

// --- Remote: a client's game asks the host ----------------------------------------------

void Session::start_mining(const BlockPos &p) {
	Writer w(C_MINING);
	block_pos(w, p);
	send_host(w.bytes);
}

void Session::request_break(const BlockPos &p, int slot) {
	Writer w(C_BREAK);
	block_pos(w, p).u8(uint8_t(std::clamp(slot, 0, 255)));
	send_host(w.bytes);
}

void Session::request_use(const float eye[3], const float dir[3], int slot, const std::string &item, bool sneak) {
	Writer w(C_USE);
	w.vec3(eye).vec3(dir).u8(uint8_t(std::clamp(slot, 0, 255))).str(item).boolean(sneak);
	send_host(w.bytes);
}

void Session::request_eat(int slot) {
	Writer w(C_EAT);
	w.u8(uint8_t(std::clamp(slot, 0, 255)));
	send_host(w.bytes);
}

void Session::hit_creature(int id) {
	Writer w(C_HIT);
	w.u32(uint32_t(id));
	send_host(w.bytes);
}

void Session::inventory_action(const Data &action) {
	Writer w(C_INVENTORY);
	w.u32(++inventory_seq_).data(action);
	send_host(w.bytes);
}

void Session::set_bed(const BlockPos &head) {
	Writer w(C_BED);
	block_pos(w, head);
	send_host(w.bytes);
}

void Session::respawned() {
	send_host(Writer(C_RESPAWNED).bytes);
}

void Session::open_container(const BlockPos &p) {
	Writer w(C_OPEN);
	block_pos(w, p);
	send_host(w.bytes);
}

void Session::close_container(const BlockPos &p) {
	Writer w(C_CLOSE);
	block_pos(w, p);
	send_host(w.bytes);
}

} // namespace gc
