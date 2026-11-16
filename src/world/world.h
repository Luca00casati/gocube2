#pragma once
// The voxel world around the player: chunks are loaded from the world's
// chunk files or generated, on worker threads, then lit and meshed once their
// whole 3x3x3 neighbourhood and the 9 columns around them are there (sky
// light needs complete columns). Edits change a copy of the chunk, are saved
// to its file, and remesh everything their light can reach.
// The main thread calls update() once per frame and uploads finished meshes.
//
// Scheduling, editing, the raycast and collision follow the Godot version's
// GoCubeWorld, without Godot.

#include "voxel/generator.h"
#include "voxel/light.h"
#include "voxel/mesher.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gc {

struct ChunkPos {
	int x = 0, y = 0, z = 0;
	bool operator==(const ChunkPos &) const = default;
};

struct BlockPos {
	int x = 0, y = 0, z = 0;
	bool operator==(const BlockPos &) const = default;
	BlockPos operator+(const BlockPos &o) const { return {x + o.x, y + o.y, z + o.z}; }
};

// A chunk's finished meshes (chunk-local positions; the chunk's minimum corner
// is pos * CHUNK in blocks).
struct ChunkMesh {
	ChunkPos pos;
	gocube::Surface opaque, cutout, fluid;
};

// A vertex of the world's meshes (chunk.hlsl's input): position, normal, uv,
// colour (sky light, block light, occlusion, marks).
struct WorldVertex {
	float pos[3];
	float normal[3];
	float uv[2];
	float color[4];
};

struct WorldStats {
	int loaded = 0;  // chunks with block data
	int meshed = 0;  // chunks with a mesh
	int queued = 0;  // jobs waiting for a worker or running
	int dirty = 0;   // edited chunks not saved yet
	bool settled = false; // everything within view distance is meshed and up to date
};

struct RayHit {
	bool hit = false;
	BlockPos position;       // the block hit
	BlockPos previous;       // the cell in front of the face hit (where a block would go)
	BlockPos normal;         // the face hit, pointing out of the block
	gocube::BlockId block = 0;
	float distance = 0;
};

struct MoveResult {
	float position[3] = {0, 0, 0}; // feet, after the move
	bool on_floor = false, hit_ceiling = false, hit_wall = false;
};

class World {
public:
	static constexpr int CHUNK_Y_MIN = -3, CHUNK_Y_MAX = 3; // the world is 7 chunks (224 blocks) tall

	// `blocks`, `atlas` and `generator` must outlive the world. `save_dir`: the
	// world's chunks folder ("": nothing is loaded or saved).
	World(const std::vector<gocube::BlockInfo> &blocks, const gocube::AtlasLayout &atlas,
			const gocube::Generator &generator, std::string save_dir = "", int worker_threads = 0);
	~World(); // (doesn't save: call save_modified() first)
	World(const World &) = delete;
	World &operator=(const World &) = delete;

	void set_view_distance(int blocks) { view_distance_ = blocks > 0 ? blocks : 1; }
	int view_distance() const { return view_distance_; }

	// Block shadows (the Godot version's "blocks" sun shadows): each face is in
	// the sun or in a block's shadow, worked out by the mesher from where the
	// sun is (radians above the eastern horizon). They follow the sun in steps
	// of SUN_STEP_DEGREES; each step (or turning them on or off) remeshes every
	// chunk, nearest first. At night the last step stays, for the morning.
	static constexpr int SUN_STEP_DEGREES = 2;
	void set_block_shadows(bool enabled);
	bool block_shadows() const { return block_shadows_.load(); }
	void set_sun_angle(float radians);

	// Main thread, once per frame: takes finished work, asks for chunks around
	// the viewer, saves and forgets far ones, schedules meshing.
	void update(float viewer_x, float viewer_y, float viewer_z);
	// Meshes finished since the last call, and chunks that went away.
	std::vector<ChunkMesh> take_meshes();
	std::vector<ChunkPos> take_unloaded();

	// The block at a world position: AIR above and below the world, -1 where
	// the chunk isn't loaded.
	int block_id_at(int x, int y, int z) const;
	// The same with AIR for unloaded chunks.
	gocube::BlockId block_at(int x, int y, int z) const;
	bool is_loaded(int x, int y, int z) const;
	// Sky and block light (0..15 each) at a block, as of its chunk's last
	// mesh; open sky where it isn't known.
	void light_at(int x, int y, int z, int &sky, int &block) const;
	// Light held in hand (a torch: x, y, z, level 0..15) spread like placed
	// light, one level less per block and stopped by opaque blocks, in the cube
	// of `size` blocks from `origin`: a level per block, indexed
	// (y * size + z) * size + x. (The Godot version's GoCubeWorld.held_light.)
	std::vector<uint8_t> held_light(const BlockPos &origin, int size, const std::vector<std::array<float, 4>> &lights) const;
	// Told about every block change (position, the new block), after it happened.
	std::function<void(const BlockPos &, gocube::BlockId)> on_block_changed;
	// Changes a block; false if its chunk isn't loaded.
	bool set_block(const BlockPos &p, gocube::BlockId id);
	// Goes up with every block changed, and every chunk loaded or forgotten.
	uint64_t revision() const { return revision_; }
	// Writes every edited chunk to its file; returns how many.
	int save_modified();

	// The first block a ray meets (fluids are seen through), within max_distance.
	RayHit raycast(const float origin[3], const float direction[3], float max_distance) const;
	// Moves a box (feet at `position`, `size` wide/tall/deep, centred on x and z)
	// by `motion`, stopped by blocks' collision boxes; unloaded chunks are solid.
	// step_height > 0 lets it walk up that much.
	MoveResult move_box(const float position[3], const float size[3], const float motion[3], float step_height = 0) const;
	bool is_box_free(const float position[3], const float size[3]) const;
	// A block's boxes (world coordinates): collision boxes, or the ones it's
	// drawn and picked with.
	void block_boxes(int x, int y, int z, int id, bool collision, std::vector<gocube::Box> &out) const;

	WorldStats stats() const;

	// --- Multiplayer ---
	// Chunks as sent over the network: the chunk file format (voxel/storage.h).
	// A world without a folder (a client's, or one not saved) keeps edited
	// chunks in memory when it forgets them, so walking back finds them.
	// The chunk at `p` (loaded, kept or saved); empty if it was never edited
	// and isn't loaded.
	std::vector<uint8_t> chunk_bytes(const ChunkPos &p) const;
	// Puts a chunk from the host in place (loaded or for when it is); false if
	// the data is bad (unknown block ids included).
	bool set_chunk(const ChunkPos &p, const uint8_t *data, size_t size);
	// Every edited chunk (loaded, kept in memory or saved): what differs from
	// what the generator makes.
	std::vector<std::pair<ChunkPos, std::vector<uint8_t>>> edited_chunks() const;
	// The host: other players' feet. Chunks within EXTRA_RADIUS chunks of each
	// stay loaded (block data only, not meshed), so their moves, edits and
	// health are checked against real blocks.
	static constexpr int EXTRA_RADIUS = 2;
	void set_extra_viewers(std::vector<std::array<float, 3>> feet) { extra_viewers_ = std::move(feet); }

private:
	enum JobKind { JOB_GENERATE, JOB_MESH };
	struct Job {
		JobKind kind = JOB_GENERATE;
		ChunkPos pos;
		float priority = 0; // lower first
		uint64_t serial = 0; // the chunk entry it was made for
		uint64_t version = 0; // JOB_MESH: the chunk's mesh_version
		std::shared_ptr<const gocube::ChunkData> kept; // JOB_GENERATE: kept in memory (no file)
		std::shared_ptr<const gocube::ChunkData> hood[27];
		std::shared_ptr<const gocube::ColumnTops> tops[9];
	};
	struct Result {
		JobKind kind = JOB_GENERATE;
		ChunkPos pos;
		uint64_t serial = 0;
		uint64_t version = 0;
		std::shared_ptr<const gocube::ChunkData> data;
		std::array<int8_t, gocube::CHUNK_AREA> local_tops{};
		std::unique_ptr<ChunkMesh> mesh;
		std::shared_ptr<const std::vector<uint8_t>> light; // sky << 4 | block, per block
	};
	struct Chunk {
		uint64_t serial = 0; // tells a reloaded chunk from the one a job was made for
		std::shared_ptr<const gocube::ChunkData> data; // null until generated or loaded
		std::array<int8_t, gocube::CHUNK_AREA> local_tops{};
		uint64_t mesh_version = 1;  // goes up when the chunk (or its light) changes
		uint64_t meshed_version = 0; // of the mesh handed out (0: none yet)
		bool mesh_queued = false;
		bool dirty = false; // edited since it was last saved
		std::shared_ptr<const std::vector<uint8_t>> light; // of the last mesh
	};

	static int64_t key(int x, int y, int z);
	static int64_t key(const ChunkPos &p) { return key(p.x, p.y, p.z); }
	static ChunkPos pos_of(int64_t k);

	void worker_loop();
	void run(Job &job, Result &r) const;
	void push_job(Job &&job);
	void update_column_tops(int cx, int cz);
	void schedule_meshing(const ChunkPos &center, int radius);
	void request_remesh(int cx, int cy, int cz);
	void remesh_all();
	void relight_around(const BlockPos &p, int y_low, int y_high);
	bool save_chunk(const ChunkPos &p, Chunk &c);
	const gocube::ChunkData *chunk_data_at(int x, int y, int z, int &lx, int &ly, int &lz) const;
	bool fence_connects(int x, int y, int z) const;
	void collect_collision(const float mn[3], const float mx[3], std::vector<gocube::Box> &out) const;

	const std::vector<gocube::BlockInfo> &blocks_;
	const gocube::AtlasLayout &atlas_;
	const gocube::Generator &generator_;
	const std::string save_dir_;
	int view_distance_ = 128;
	uint64_t next_serial_ = 1;
	uint64_t revision_ = 0;
	ChunkPos center_;  // at the last update
	int radius_ = 0;   // in chunks, at the last update

	// Main thread only.
	std::unordered_map<int64_t, Chunk> chunks_;
	std::unordered_map<int64_t, std::shared_ptr<const gocube::ColumnTops>> column_tops_; // key(cx, 0, cz)
	std::unordered_set<int64_t> mesh_candidates_;
	std::unordered_set<int64_t> urgent_; // remeshed after an edit: before anything else
	std::vector<ChunkMesh> meshes_;
	std::vector<ChunkPos> unloaded_;
	std::unordered_map<int64_t, std::shared_ptr<const gocube::ChunkData>> kept_; // edited, not loaded (no folder)
	std::vector<std::array<float, 3>> extra_viewers_;
	void load_column_chunks(const ChunkPos &center, int dx, int dz, float priority_bias);

	// Shared with the workers.
	mutable std::mutex job_mutex_;
	std::condition_variable job_cv_;
	std::vector<Job> jobs_;
	int running_ = 0; // jobs being worked on
	std::atomic<bool> block_shadows_{false};
	std::atomic<int> sun_step_{45}; // SUN_STEP_DEGREES steps above the eastern horizon
	bool stop_ = false;
	std::mutex result_mutex_;
	std::vector<Result> results_;
	std::vector<std::thread> workers_;
};

} // namespace gc
