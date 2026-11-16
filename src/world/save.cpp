#include "world/save.h"

#include "data/lua_data.h"
#include "log.h"

#include <windows.h>

#include <ctime>
#include <stdexcept>
#include <fstream>
#include <sstream>

namespace gc {

namespace {

// "YYYY-MM-DDTHH:MM:SS", local time.
std::string now_string() {
	const std::time_t t = std::time(nullptr);
	std::tm tm;
	localtime_s(&tm, &t);
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
	return buf;
}

double number(const Json &j, const char *key, double fallback) {
	auto it = j.find(key);
	return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}

} // namespace

std::string WorldSave::open(const std::filesystem::path &dir, uint32_t new_seed, const std::string &new_mode) {
	dir_ = dir;
	const std::filesystem::path path = dir / "world.lua";
	std::error_code ec;
	if (std::filesystem::exists(path, ec)) {
		std::string why;
		// A world that can't be read safely is left alone.
		if (!read_lua_data(path, meta_, why) || !meta_.is_object() || !meta_.contains("seed") ||
				!meta_["seed"].is_number()) {
			return "its world.lua is damaged, so it isn't opened (to keep it safe).";
		}
		if (number(meta_, "format", FORMAT_VERSION) > FORMAT_VERSION ||
				number(meta_, "generator", 1) > GENERATOR_VERSION) {
			return "it was saved by a newer version of GoCube. Update the game to play it.";
		}
		if (!meta_.contains("mode")) {
			meta_["mode"] = "creative";
		}
	} else if (std::filesystem::exists(dir / "world.json", ec)) {
		return "it was saved as world.json (by the Godot version, or before worlds were Lua), which this "
			   "version doesn't read.";
	} else {
		meta_ = Json::object();
		meta_["seed"] = new_seed;
		meta_["mode"] = new_mode;
		meta_["first_night"] = "ahead"; // no ninjas until it's over
		meta_["created"] = now_string();
		meta_["generator"] = GENERATOR_VERSION;
	}
	meta_["format"] = FORMAT_VERSION;
	std::filesystem::create_directories(chunks_dir(), ec);
	if (ec) {
		return "its folder can't be written (" + ec.message() + ")";
	}
	return "";
}

uint32_t WorldSave::seed() const {
	return uint32_t(int64_t(number(meta_, "seed", 0)));
}

int WorldSave::generator() const {
	return int(number(meta_, "generator", 1)); // worlds from before biomes have none
}

std::string WorldSave::mode() const {
	auto it = meta_.find("mode");
	return it != meta_.end() && it->is_string() ? it->get<std::string>() : "creative";
}

std::vector<std::string> WorldSave::block_ids() const {
	std::vector<std::string> ids;
	for (const Json &id : meta_.value("block_ids", Json::array())) {
		if (id.is_string()) {
			ids.push_back(id.get<std::string>());
		}
	}
	return ids;
}

void WorldSave::set_block_ids(const std::vector<std::string> &ids) {
	meta_["block_ids"] = ids;
	write_meta(); // before any chunk can be saved with these numbers
}

const Json &WorldSave::player() const {
	static const Json empty = Json::object();
	auto it = meta_.find("player");
	return it != meta_.end() && it->is_object() ? *it : empty;
}

const Json &WorldSave::field(const std::string &key) const {
	static const Json none = nullptr;
	auto it = meta_.find(key);
	return it != meta_.end() ? *it : none;
}

bool WorldSave::save(const Json &player) {
	if (!player.empty()) {
		Json &saved = meta_["player"];
		if (!saved.is_object()) {
			saved = Json::object();
		}
		for (const auto &[k, v] : player.items()) {
			saved[k] = v;
		}
	}
	meta_["saved"] = now_string();
	return write_meta();
}

// To a temporary file, then renamed over the old one: a crash mid-write can't
// leave a broken world.lua.
bool WorldSave::write_meta() {
	const std::filesystem::path path = dir_ / "world.lua";
	const std::filesystem::path tmp = dir_ / "world.lua.tmp";
	std::string text;
	try {
		text = write_lua_data(meta_);
	} catch (const std::invalid_argument &e) {
		log("Can't save %s: %s", path.string().c_str(), e.what());
		return false;
	}
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f) {
			log("Can't write %s", tmp.string().c_str());
			return false;
		}
		f << text;
		if (!f.good()) {
			return false;
		}
	}
	if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		log("Can't replace %s (error %lu)", path.string().c_str(), GetLastError());
		return false;
	}
	return true;
}

} // namespace gc
