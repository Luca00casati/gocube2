#include "game/blocks.h"

#include <cmath>

namespace gc {

namespace {

const std::string EMPTY_STRING;

std::string str(const Json &j, const char *key, const std::string &fallback = "") {
	auto it = j.find(key);
	return it != j.end() && it->is_string() ? it->get<std::string>() : fallback;
}

bool flag(const Json &j, const char *key, bool fallback = false) {
	auto it = j.find(key);
	return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

BlockPos facing_dir(const std::string &f) { // towards -z, +x, +z, -x
	if (f == "n") return {0, 0, -1};
	if (f == "e") return {1, 0, 0};
	if (f == "s") return {0, 0, 1};
	if (f == "w") return {-1, 0, 0};
	return {0, -1, 0}; // "floor"
}

// The wall a block sits against when placed on a face with normal n.
std::string wall_facing(const BlockPos &n) {
	if (n.z == 1) return "n";
	if (n.z == -1) return "s";
	if (n.x == 1) return "w";
	return "e";
}

std::string look_facing(const float look[3]) {
	if (std::fabs(look[0]) > std::fabs(look[2])) {
		return look[0] > 0 ? "e" : "w";
	}
	return look[2] > 0 ? "s" : "n";
}

} // namespace

BlockRules::BlockRules(const Content &content) : content_(content) {
	for (const std::string &id : content.block_ids()) { // (sorted after air)
		if (is_placeable(id)) {
			placeable_.push_back(id);
		}
	}
}

const std::string &BlockRules::id_of(int block) const {
	const auto &ids = content_.block_ids();
	return block >= 0 && block < int(ids.size()) ? ids[block] : EMPTY_STRING;
}

const Json &BlockRules::def(const std::string &id) const {
	static const Json empty = Json::object();
	auto it = content_.blocks.find(id);
	return it != content_.blocks.end() ? it->second : empty;
}

bool BlockRules::is_placeable(const std::string &id) const {
	auto it = content_.blocks.find(id);
	return it != content_.blocks.end() && !it->second.contains("fluid") && id != "core:air" &&
			!it->second.contains("variant_of") && !flag(it->second, "hidden");
}

std::string BlockRules::base_of(const std::string &id) const {
	return str(def(id), "variant_of", id);
}

bool BlockRules::replaceable(int block) const {
	return flag(def(block), "replaceable");
}

bool BlockRules::climbable(int block) const {
	return flag(def(block), "climbable");
}

bool BlockRules::is_fluid(int block) const {
	return def(block).contains("fluid");
}

std::string BlockRules::variant(const std::string &base, const std::string &facing) const {
	return facing == "n" ? base : base + "_" + facing;
}

std::string BlockRules::door_id(const std::string &base, const std::string &facing, const std::string &half, bool open) const {
	if (facing == "n" && half == "lower" && !open) {
		return base;
	}
	return base + "_" + facing + "_" + half + (open ? "_open" : "");
}

BlockRules::Placement BlockRules::placement(const std::string &base, const RayHit &hit, const float look[3], float hit_y) const {
	const Json &d = def(base);
	const BlockPos pos = hit.previous, n = hit.normal;
	if (flag(d, "leaves")) {
		return {{pos, base + "_placed"}}; // placed leaves never decay
	}
	const std::string shape = str(d, "shape", "cube");
	if (shape == "slab") {
		const float frac = hit_y - std::floor(hit_y);
		const bool top = n.y == -1 || (n.y == 0 && frac > 0.5f);
		return {{pos, base + (top ? "_top" : "")}};
	}
	if (shape == "stairs") {
		return {{pos, variant(base, look_facing(look))}};
	}
	if (shape == "torch") {
		if (n.y == 1) {
			return {{pos, base}};
		}
		if (n.y == -1) {
			return {};
		}
		return {{pos, base + "_wall_" + wall_facing(n)}};
	}
	if (shape == "ladder") {
		if (n.y != 0) {
			return {};
		}
		return {{pos, variant(base, wall_facing(n))}};
	}
	if (shape == "door") {
		const std::string f = look_facing(look);
		return {{pos, door_id(base, f, "lower", false)}, {pos + BlockPos{0, 1, 0}, door_id(base, f, "upper", false)}};
	}
	if (shape == "bed") {
		// The foot where it's placed, the head one block further the way you look.
		const std::string f = look_facing(look);
		const std::string foot = f == "n" ? base : base + "_" + f + "_foot";
		return {{pos, foot}, {pos + facing_dir(f), base + "_" + f + "_head"}};
	}
	return {{pos, base}};
}

std::optional<BlockPos> BlockRules::other_part(const std::string &id, const BlockPos &pos) const {
	const Json &d = def(id);
	const Json st = d.value("state", Json::object());
	const std::string shape = str(d, "shape");
	if (shape == "door") {
		return pos + BlockPos{0, str(st, "half") == "lower" ? 1 : -1, 0};
	}
	if (shape == "bed") {
		const BlockPos f = facing_dir(str(st, "facing"));
		return str(st, "part") == "foot" ? pos + f : pos + BlockPos{-f.x, -f.y, -f.z};
	}
	return std::nullopt;
}

BlockRules::Placement BlockRules::toggle_door(const std::string &id, const BlockPos &pos) const {
	const Json &d = def(id);
	if (str(d, "shape") != "door") {
		return {};
	}
	const std::string base = base_of(id);
	const Json st = d.value("state", Json::object());
	const bool open = !flag(st, "open");
	const std::string half = str(st, "half"), facing = str(st, "facing");
	const std::string other_half = half == "lower" ? "upper" : "lower";
	const BlockPos other = pos + BlockPos{0, half == "lower" ? 1 : -1, 0};
	return {{pos, door_id(base, facing, half, open)}, {other, door_id(base, facing, other_half, open)}};
}

bool BlockRules::solid_support(const World &world, const BlockPos &pos) const {
	const std::string &id = id_of(world.block_at(pos.x, pos.y, pos.z));
	const Json &d = def(id);
	const std::string shape = str(d, "shape", "cube");
	return !d.empty() && flag(d, "solid", true) && !d.contains("fluid") && id != "core:air" &&
			(shape == "cube" || shape == "slab" || shape == "stairs");
}

bool BlockRules::supported(const World &world, const std::string &id, const BlockPos &pos) const {
	const Json &d = def(id);
	const std::string &below = id_of(world.block_at(pos.x, pos.y - 1, pos.z));
	if (auto it = d.find("plant_on"); it != d.end() && it->is_array()) {
		for (const Json &b : *it) {
			if (b.is_string() && b.get<std::string>() == below) {
				return true;
			}
		}
		return false;
	}
	if (!flag(d, "needs_support")) {
		return true;
	}
	const Json st = d.value("state", Json::object());
	if (str(d, "shape") == "door") { // stands on the floor whichever way it faces
		if (str(st, "half") == "upper") {
			return base_of(below) == base_of(id);
		}
		return solid_support(world, pos + BlockPos{0, -1, 0});
	}
	return solid_support(world, pos + facing_dir(str(st, "facing", "floor")));
}

} // namespace gc
