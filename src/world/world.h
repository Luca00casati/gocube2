#pragma once
// The voxel world around the player: chunks are generated (or, later, loaded)
// on worker threads, then lit and meshed once their whole 3x3x3 neighbourhood
// and the 9 columns around them are there (sky light needs complete columns).
// The main thread calls update() once per frame and uploads finished meshes.
//
// Scheduling follows the Godot version's GoCubeWorld, without Godot.

#include "voxel/generator.h"
#include "voxel/light.h"
#include "voxel/mesher.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gc {

struct ChunkPos {
	int x = 0, y = 0, z = 0;
	bool operator==(const ChunkPos &) const = default;
};

// A chunk's finished meshes (chunk-local positions; the chunk's minimum corner
// is pos * CHUNK in blocks).
struct ChunkMesh {
	ChunkPos pos;
	gocube::Surface opaque, cutout, fluid;
};

struct WorldStats {
	int loaded = 0;  // chunks with block data
	int meshed = 0;  // chunks with a mesh
	int queued = 0;  // jobs waiting for a worker or running
	bool settled = false; // everything within view distance is meshed
};

class World {
public:
	static constexpr int CHUNK_Y_MIN = -3, CHUNK_Y_MAX = 3; // the world is 7 chunks (224 blocks) tall

	// `blocks`, `atlas` and `generator` must outlive the world.
	World(const std::vector<gocube::BlockInfo> &blocks, const gocube::AtlasLayout &atlas,
			const gocube::Generator &generator, int worker_threads = 0);
	~World();
	World(const World &) = delete;
	World &operator=(const World &) = delete;

	void set_view_distance(int blocks) { view_distance_ = blocks > 0 ? blocks : 1; }
	int view_distance() const { return view_distance_; }

	// Main thread, once per frame: takes finished work, asks for chunks around
	// the viewer, forgets far ones and schedules meshing.
	void update(float viewer_x, float viewer_y, float viewer_z);
	// Meshes finished since the last call, and chunks that went away.
	std::vector<ChunkMesh> take_meshes();
	std::vector<ChunkPos> take_unloaded();

	// The block at a world position (0, air, where nothing is loaded).
	gocube::BlockId block_at(int x, int y, int z) const;
	WorldStats stats() const;

private:
	enum JobKind { JOB_GENERATE, JOB_MESH };
	struct Job {
		JobKind kind = JOB_GENERATE;
		ChunkPos pos;
		float priority = 0; // lower first
		uint64_t serial = 0; // the chunk entry it was made for
		std::shared_ptr<const gocube::ChunkData> hood[27];
		std::shared_ptr<const gocube::ColumnTops> tops[9];
	};
	struct Result {
		JobKind kind = JOB_GENERATE;
		ChunkPos pos;
		uint64_t serial = 0;
		std::shared_ptr<const gocube::ChunkData> data;
		std::array<int8_t, gocube::CHUNK_AREA> local_tops{};
		std::unique_ptr<ChunkMesh> mesh;
	};
	struct Chunk {
		uint64_t serial = 0; // tells a reloaded chunk from the one a job was made for
		std::shared_ptr<const gocube::ChunkData> data; // null until generated
		std::array<int8_t, gocube::CHUNK_AREA> local_tops{};
		bool mesh_queued = false;
		bool meshed = false;
	};

	static int64_t key(int x, int y, int z);
	static int64_t key(const ChunkPos &p) { return key(p.x, p.y, p.z); }
	static ChunkPos pos_of(int64_t k);

	void worker_loop();
	void run(Job &job, Result &r) const;
	void push_job(Job &&job);
	void update_column_tops(int cx, int cz);
	void schedule_meshing(const ChunkPos &center, int radius);

	const std::vector<gocube::BlockInfo> &blocks_;
	const gocube::AtlasLayout &atlas_;
	const gocube::Generator &generator_;
	int view_distance_ = 128;
	uint64_t next_serial_ = 1;
	ChunkPos center_;  // at the last update
	int radius_ = 0;   // in chunks, at the last update

	// Main thread only.
	std::unordered_map<int64_t, Chunk> chunks_;
	std::unordered_map<int64_t, std::shared_ptr<const gocube::ColumnTops>> column_tops_; // key(cx, 0, cz)
	std::unordered_set<int64_t> mesh_candidates_;
	std::vector<ChunkMesh> meshes_;
	std::vector<ChunkPos> unloaded_;

	// Shared with the workers.
	mutable std::mutex job_mutex_;
	std::condition_variable job_cv_;
	std::vector<Job> jobs_;
	int running_ = 0; // jobs being worked on
	bool stop_ = false;
	std::mutex result_mutex_;
	std::vector<Result> results_;
	std::vector<std::thread> workers_;
};

} // namespace gc
