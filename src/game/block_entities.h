#pragma once
// Data attached to some blocks: chests (27 slots), furnaces (input, fuel,
// output; burning and smelting) and dye vats (the gems in its dye, one slot
// per kind, and how many blocks it can still dye: "charge"). A block gets one
// when it appears, if its definition has "block_entity": "chest", "furnace"
// or "vat"; when it goes, its contents spill. Saved in world.lua's
// block_entities:
//   {["x,y,z"] = {type, slots = {{id, count} or false, ...}, burn, burn_total, progress, charge}}
// Furnaces ask the mods' smelting and fuel_seconds rules.

#include "content/content.h"
#include "game/inventory.h"
#include "world/world.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace gc {

class Mods;

struct BlockEntity {
	std::string type; // "chest", "furnace", "vat"
	std::vector<Stack> slots;
	double burn = 0, burn_total = 0, progress = 0; // furnaces: seconds
	int charge = 0; // vats: blocks it can still dye
};

class BlockEntities {
public:
	static constexpr int CHEST_SLOTS = 27;
	static constexpr int FURNACE_INPUT = 0, FURNACE_FUEL = 1, FURNACE_OUTPUT = 2;
	static constexpr int VAT_KINDS = 8; // kinds of gem in one dye
	static constexpr double SMELT_SECONDS = 5.0;
	static constexpr double TICK = 0.25;

	BlockEntities(const Content &content, Mods &mods) : content_(content), mods_(mods) {}

	BlockEntity *at(const BlockPos &p);
	// A block at `p` became `block`: an entity is made, or the old one goes
	// and its contents are returned (to drop).
	std::vector<Stack> block_changed(const BlockPos &p, const std::string &block);
	// Mouse-style click on a slot while holding `held`; returns what is held
	// now. Furnace output can only be taken; fuel only takes fuel.
	Stack click(const BlockPos &p, int slot, const Stack &held);
	// Furnaces burn and smelt (call every frame; they tick every TICK seconds).
	void update(double dt);
	// 0..1: how much fuel is left, how far the smelting is.
	static void furnace_status(const BlockEntity &e, float &burn, float &progress);

	Data serialize() const;
	void deserialize(const Data &data);
	// One entity in the same form (null: none there); setting null removes it.
	// (A client's copies of the host's.)
	Data entity_data(const BlockPos &p) const;
	void set_entity_data(const BlockPos &p, const Data &data);
	size_t size() const { return entities_.size(); }

	double fuel_seconds(const std::string &item);
	std::optional<Stack> smelting(const std::string &input);

private:
	using Key = std::tuple<int, int, int>;
	static BlockEntity make(const std::string &type);
	void tick_furnace(BlockEntity &e, double dt);

	const Content &content_;
	Mods &mods_;
	std::map<Key, BlockEntity> entities_;
	double timer_ = 0;
};

} // namespace gc
