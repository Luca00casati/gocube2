#pragma once
// The 3D model of an item: a
// small cube for solid blocks (each face its own texture, shrunk to a slab's or
// a door's single box), a whole bed for beds, else the item's icon given
// depth (each opaque pixel a little block, walls where it borders a clear
// one). Used for dropped items and the item in the first-person hand.
//
// Models are centred on the origin (a cube stands on the bottom of its
// 0.25 cell: a slab's top is at 0), in metres, with atlas uvs; built once per
// item.

#include "content/content.h"

#include <map>
#include <string>
#include <vector>

namespace gc {

struct ModelVertex {
	float pos[3];
	float normal[3]; // the model's own (shading looks at it, not the turned one)
	float uv[2];     // in the block atlas
};

class ItemModels {
public:
	static constexpr float CUBE_SIZE = 0.25f;
	static constexpr float ITEM_SIZE = 0.42f;      // an icon spans this many metres
	static constexpr float ITEM_THICKNESS = 0.035f;

	// `content` and `table` must outlive this.
	ItemModels(const Content &content, const BlockTable &table) : content_(content), table_(table) {}

	// Triangles (three vertices each), front faces counter-clockwise.
	const std::vector<ModelVertex> &model(const std::string &item);
	// Whether the model is a block's boxes (held differently from a flat item).
	bool is_block(const std::string &item) const;

private:
	void add_box(std::vector<ModelVertex> &out, int block, const float mn[3], const float mx[3], int skip) const;
	void add_extruded(std::vector<ModelVertex> &out, int tile) const;

	const Content &content_;
	const BlockTable &table_;
	std::map<std::string, std::vector<ModelVertex>> models_;
};

} // namespace gc
