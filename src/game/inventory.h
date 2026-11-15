#pragma once
// Item slots: 0..8 are the hotbar. A slot is empty or a stack {id, count}.
// Blocks are items too (placing one uses it up). Also what the mouse holds in
// an open screen (the cursor) and worn armour. Ported from the Godot
// version's Inventory, including its save format (world.json, player.inventory):
//   [[id, count] or null, ... (36), {"armor": [head, chest, legs, feet ids]}]

#include "content/content.h"

#include <array>
#include <string>

namespace gc {

struct Stack {
	std::string id; // "" = empty
	int count = 0;
	bool empty() const { return id.empty() || count <= 0; }
	bool operator==(const Stack &) const = default;
};

class Inventory {
public:
	static constexpr int SIZE = 36;
	static constexpr int HOTBAR = 9;
	static constexpr int DEFAULT_MAX_STACK = 64;
	static constexpr const char *ARMOR_SLOTS[4] = {"head", "chest", "legs", "feet"};

	explicit Inventory(const Content &content) : content_(&content) {}

	std::array<Stack, SIZE> slots;
	Stack cursor;                // held by the mouse in an open screen
	std::array<Stack, 4> armor;  // ARMOR_SLOTS order

	int max_stack(const std::string &id) const;
	// Adds items, filling existing stacks first, then empty slots (hotbar first).
	// Returns how many didn't fit.
	int add(const std::string &id, int count);
	bool can_fit(const std::string &id) const; // at least one more
	int count_of(const std::string &id) const;
	// Removes `count` from wherever they are (the back first, keeping the
	// hotbar). False, and nothing removed, if there aren't enough.
	bool remove(const std::string &id, int count);
	// Uses up one item from a slot; returns its id ("" if it was empty).
	std::string take_one(int slot);
	// Mouse-style: puts `held` into a slot, returns what is held now (same item
	// stacks up to the limit, otherwise the two swap).
	Stack put(int slot, const Stack &held);
	// Click on a slot in a screen: put down or pick up.
	void click(int slot) { cursor = put(slot, cursor); }
	// Armour slot `slot` (index into ARMOR_SLOTS) and `held`: wear it if it
	// fits there, else keep holding it. Returns what is held afterwards.
	Stack put_armor(int slot, const Stack &held);
	int armor_points() const;
	// A screen closed: what the mouse held goes back in; returns what didn't fit.
	Stack close_cursor();

	Json serialize() const;
	void deserialize(const Json &data); // unknown items (their mod is gone) are dropped

private:
	const Json *item_def(const std::string &id) const; // the item's, else the block's definition
	const Content *content_;
};

} // namespace gc
