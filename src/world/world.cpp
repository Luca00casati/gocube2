#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace gc {

using namespace gocube;

namespace {

int floor_div(int v, int d) {
	return v >= 0 ? v / d : -((-v + d - 1) / d);
}

} // namespace

World::World(const std::vector<BlockInfo> &blocks, const AtlasLayout &atlas, const Generator &generator,
		int worker_threads) :
		blocks_(blocks), atlas_(atlas), generator_(generator) {
	if (worker_threads <= 0) {
		worker_threads = std::max(1, int(std::thread::hardware_concurrency()) - 1); // one core for the game
	}
	for (int i = 0; i < worker_threads; i++) {
		workers_.emplace_back([this] { worker_loop(); });
	}
}

World::~World() {
	{
		std::lock_guard<std::mutex> lock(job_mutex_);
		stop_ = true;
	}
	job_cv_.notify_all();
	for (std::thread &t : workers_) {
		t.join();
	}
}

int64_t World::key(int x, int y, int z) {
	return (static_cast<int64_t>(x & 0x1FFFFF) << 42) | (static_cast<int64_t>(y & 0x1FFFFF) << 21) |
			static_cast<int64_t>(z & 0x1FFFFF);
}

ChunkPos World::pos_of(int64_t k) {
	auto field = [](int64_t v) { // 21-bit two's complement
		v &= 0x1FFFFF;
		return int(v >= 0x100000 ? v - 0x200000 : v);
	};
	return {field(k >> 42), field(k >> 21), field(k)};
}

// --- Workers --------------------------------------------------------------------

void World::push_job(Job &&job) {
	{
		std::lock_guard<std::mutex> lock(job_mutex_);
		jobs_.push_back(std::move(job));
	}
	job_cv_.notify_one();
}

void World::worker_loop() {
	while (true) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(job_mutex_);
			job_cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
			if (stop_) {
				return;
			}
			// Nearest first (the queue is small enough for a linear scan).
			auto best = std::min_element(
					jobs_.begin(), jobs_.end(), [](const Job &a, const Job &b) { return a.priority < b.priority; });
			job = std::move(*best);
			*best = std::move(jobs_.back());
			jobs_.pop_back();
			running_++;
		}
		Result r;
		run(job, r);
		{
			std::lock_guard<std::mutex> lock(result_mutex_);
			results_.push_back(std::move(r));
		}
		std::lock_guard<std::mutex> lock(job_mutex_);
		running_--;
	}
}

void World::run(Job &job, Result &r) const {
	r.kind = job.kind;
	r.pos = job.pos;
	r.serial = job.serial;
	if (job.kind == JOB_GENERATE) {
		auto data = std::make_shared<ChunkData>();
		generator_.generate(*data, job.pos.x * CHUNK, job.pos.y * CHUNK, job.pos.z * CHUNK);
		chunk_local_tops(*data, blocks_, r.local_tops);
		r.data = std::move(data);
		return;
	}
	const ChunkData *hood[27];
	for (int i = 0; i < 27; i++) {
		hood[i] = job.hood[i].get();
	}
	const ColumnTops *tops[9];
	for (int i = 0; i < 9; i++) {
		tops[i] = job.tops[i].get();
	}
	LightRegion light;
	compute_light(light, hood, tops, job.pos.y, blocks_);
	const ChunkData *faces[FACE_COUNT] = {hood[neighbourhood_index(1, 0, 0)], hood[neighbourhood_index(-1, 0, 0)],
			hood[neighbourhood_index(0, 1, 0)], hood[neighbourhood_index(0, -1, 0)],
			hood[neighbourhood_index(0, 0, 1)], hood[neighbourhood_index(0, 0, -1)]};
	MeshOptions options;
	options.smooth_lighting = true;
	options.tops = tops;
	options.origin_x = job.pos.x * CHUNK;
	options.origin_y = job.pos.y * CHUNK;
	options.origin_z = job.pos.z * CHUNK;
	r.mesh = std::make_unique<ChunkMesh>();
	r.mesh->pos = job.pos;
	mesh_chunk(*hood[neighbourhood_index(0, 0, 0)], faces, hood, blocks_, atlas_, &light, options, r.mesh->opaque,
			r.mesh->cutout, r.mesh->fluid);
}

// --- Main thread ---------------------------------------------------------------

void World::update(float viewer_x, float /*viewer_y*/, float viewer_z) {
	const ChunkPos center{floor_div(int(std::floor(viewer_x)), CHUNK), 0, floor_div(int(std::floor(viewer_z)), CHUNK)};
	const int r = (view_distance_ + CHUNK - 1) / CHUNK;
	center_ = center;
	radius_ = r;

	// Finished work.
	std::vector<Result> results;
	{
		std::lock_guard<std::mutex> lock(result_mutex_);
		results.swap(results_);
	}
	for (Result &res : results) {
		auto it = chunks_.find(key(res.pos));
		if (it == chunks_.end() || it->second.serial != res.serial) {
			continue; // the chunk went away meanwhile
		}
		Chunk &c = it->second;
		if (res.kind == JOB_GENERATE) {
			c.data = std::move(res.data);
			c.local_tops = res.local_tops;
			update_column_tops(res.pos.x, res.pos.z);
			// It and every neighbour may now have all they need to be meshed.
			for (int dy = -1; dy <= 1; dy++) {
				for (int dz = -1; dz <= 1; dz++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int64_t k = key(res.pos.x + dx, res.pos.y + dy, res.pos.z + dz);
						auto n = chunks_.find(k);
						if (n != chunks_.end() && n->second.data && !n->second.meshed && !n->second.mesh_queued) {
							mesh_candidates_.insert(k);
						}
					}
				}
			}
		} else {
			c.mesh_queued = false;
			c.meshed = true;
			meshes_.push_back(std::move(*res.mesh));
		}
	}

	// Chunks within view distance (a circle of columns, the world's full
	// height), and every neighbour of those: a chunk is only meshed once its
	// neighbours are there.
	const int load = r + 1;
	for (int dz = -load; dz <= load; dz++) {
		for (int dx = -load; dx <= load; dx++) {
			const int nx = std::max(0, std::abs(dx) - 1), nz = std::max(0, std::abs(dz) - 1); // nearest column in view
			if (nx * nx + nz * nz > r * r) {
				continue;
			}
			for (int cy = CHUNK_Y_MIN; cy <= CHUNK_Y_MAX; cy++) {
				const ChunkPos p{center.x + dx, cy, center.z + dz};
				auto [it, added] = chunks_.try_emplace(key(p));
				if (!added) {
					continue;
				}
				it->second.serial = next_serial_++;
				Job job;
				job.kind = JOB_GENERATE;
				job.pos = p;
				job.serial = it->second.serial;
				job.priority = float(dx * dx + dz * dz) + std::abs(cy) * 0.5f;
				push_job(std::move(job));
			}
		}
	}

	// Forget chunks well outside it (a margin, so walking back and forth
	// doesn't regenerate them).
	const int keep = r + 3;
	for (auto it = chunks_.begin(); it != chunks_.end();) {
		const ChunkPos p = pos_of(it->first);
		const int dx = p.x - center.x, dz = p.z - center.z;
		if (dx * dx + dz * dz > keep * keep) {
			if (it->second.meshed || it->second.mesh_queued) {
				unloaded_.push_back(p);
			}
			column_tops_.erase(key(p.x, 0, p.z));
			mesh_candidates_.erase(it->first);
			it = chunks_.erase(it);
		} else {
			++it;
		}
	}
	{
		// Jobs for chunks that are gone are of no use any more.
		std::lock_guard<std::mutex> lock(job_mutex_);
		std::erase_if(jobs_, [&](const Job &j) {
			auto it = chunks_.find(key(j.pos));
			return it == chunks_.end() || it->second.serial != j.serial;
		});
	}

	schedule_meshing(center, r);
}

void World::update_column_tops(int cx, int cz) {
	const int64_t ckey = key(cx, 0, cz);
	auto tops = std::make_shared<ColumnTops>();
	tops->fill(NO_TOP);
	for (int cy = CHUNK_Y_MAX; cy >= CHUNK_Y_MIN; cy--) {
		auto it = chunks_.find(key(cx, cy, cz));
		if (it == chunks_.end() || !it->second.data) {
			column_tops_.erase(ckey); // column incomplete: no sky information yet
			return;
		}
		for (int i = 0; i < CHUNK_AREA; i++) {
			if ((*tops)[i] == NO_TOP && it->second.local_tops[i] >= 0) {
				(*tops)[i] = static_cast<int16_t>(cy * CHUNK + it->second.local_tops[i]);
			}
		}
	}
	column_tops_[ckey] = tops;
}

void World::schedule_meshing(const ChunkPos &center, int r) {
	for (auto it = mesh_candidates_.begin(); it != mesh_candidates_.end();) {
		auto cit = chunks_.find(*it);
		if (cit == chunks_.end() || !cit->second.data || cit->second.mesh_queued || cit->second.meshed) {
			it = mesh_candidates_.erase(it);
			continue;
		}
		Chunk &c = cit->second;
		const ChunkPos p = pos_of(*it);
		Job job;
		job.kind = JOB_MESH;
		job.pos = p;
		job.serial = c.serial;
		const int dx0 = p.x - center.x, dz0 = p.z - center.z;
		if (dx0 * dx0 + dz0 * dz0 > r * r) {
			++it; // not in view yet; stays a candidate
			continue;
		}
		job.priority = float(dx0 * dx0 + dz0 * dz0) + std::abs(p.y) * 0.5f;
		bool ready = true;
		for (int dy = -1; dy <= 1 && ready; dy++) {
			for (int dz = -1; dz <= 1 && ready; dz++) {
				for (int dx = -1; dx <= 1 && ready; dx++) {
					const int ny = p.y + dy;
					if (ny > CHUNK_Y_MAX || ny < CHUNK_Y_MIN) {
						continue; // outside the world: air above, solid below
					}
					auto n = chunks_.find(key(p.x + dx, ny, p.z + dz));
					if (n == chunks_.end() || !n->second.data) {
						ready = false;
						break;
					}
					job.hood[neighbourhood_index(dx, dy, dz)] = n->second.data;
				}
			}
		}
		for (int dz = -1; dz <= 1 && ready; dz++) {
			for (int dx = -1; dx <= 1 && ready; dx++) {
				auto t = column_tops_.find(key(p.x + dx, 0, p.z + dz));
				if (t == column_tops_.end()) {
					ready = false; // a column isn't complete: sky light unknown
					break;
				}
				job.tops[(dz + 1) * 3 + (dx + 1)] = t->second;
			}
		}
		if (!ready) {
			++it; // a neighbour arriving makes it a candidate again
			continue;
		}
		c.mesh_queued = true;
		push_job(std::move(job));
		it = mesh_candidates_.erase(it);
	}
}

std::vector<ChunkMesh> World::take_meshes() {
	return std::exchange(meshes_, {});
}

std::vector<ChunkPos> World::take_unloaded() {
	return std::exchange(unloaded_, {});
}

BlockId World::block_at(int x, int y, int z) const {
	const int cx = floor_div(x, CHUNK), cy = floor_div(y, CHUNK), cz = floor_div(z, CHUNK);
	auto it = chunks_.find(key(cx, cy, cz));
	if (it == chunks_.end() || !it->second.data) {
		return AIR;
	}
	return it->second.data->get(x - cx * CHUNK, y - cy * CHUNK, z - cz * CHUNK);
}

WorldStats World::stats() const {
	WorldStats s;
	for (const auto &[k, c] : chunks_) {
		s.loaded += c.data ? 1 : 0;
		s.meshed += c.meshed ? 1 : 0;
	}
	{
		std::lock_guard<std::mutex> lock(job_mutex_);
		s.queued = int(jobs_.size()) + running_;
	}
	s.settled = s.queued == 0;
	for (int dz = -radius_; dz <= radius_ && s.settled; dz++) {
		for (int dx = -radius_; dx <= radius_ && s.settled; dx++) {
			if (dx * dx + dz * dz > radius_ * radius_) {
				continue;
			}
			for (int cy = CHUNK_Y_MIN; cy <= CHUNK_Y_MAX; cy++) {
				auto it = chunks_.find(key(center_.x + dx, cy, center_.z + dz));
				if (it == chunks_.end() || !it->second.meshed) {
					s.settled = false;
					break;
				}
			}
		}
	}
	return s;
}

} // namespace gc
