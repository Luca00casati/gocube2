#include "world/world.h"

#include "log.h"
#include "voxel/storage.h"

#include <algorithm>
#include <chrono>
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
		std::string save_dir, int worker_threads) :
		blocks_(blocks), atlas_(atlas), generator_(generator), save_dir_(std::move(save_dir)) {
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
	r.version = job.version;
	if (job.kind == JOB_GENERATE) {
		auto data = std::make_shared<ChunkData>();
		// An edited chunk comes from its file (or memory); the rest is generated again.
		bool loaded = false;
		if (job.kept) {
			*data = *job.kept;
			loaded = true;
		} else if (!save_dir_.empty()) {
			loaded = load_chunk_file(chunk_file_path(save_dir_, job.pos.x, job.pos.y, job.pos.z), *data);
		}
		if (!loaded) {
			const auto t0 = std::chrono::steady_clock::now();
			generator_.generate(*data, job.pos.x * CHUNK, job.pos.y * CHUNK, job.pos.z * CHUNK);
			generate_us_ += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
			generated_++;
		}
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
	const auto t0 = std::chrono::steady_clock::now();
	LightRegion light;
	compute_light(light, hood, tops, job.pos.y, blocks_);
	const auto t1 = std::chrono::steady_clock::now();
	light_us_ += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
	const ChunkData *faces[FACE_COUNT] = {hood[neighbourhood_index(1, 0, 0)], hood[neighbourhood_index(-1, 0, 0)],
			hood[neighbourhood_index(0, 1, 0)], hood[neighbourhood_index(0, -1, 0)],
			hood[neighbourhood_index(0, 0, 1)], hood[neighbourhood_index(0, 0, -1)]};
	MeshOptions options;
	options.smooth_lighting = smooth_lighting_.load();
	options.sun_shadows = block_shadows_.load();
	const float sun = float(sun_step_.load() * SUN_STEP_DEGREES) * 3.14159265f / 180.0f;
	options.sun_x = std::cos(sun);
	options.sun_y = std::sin(sun);
	options.tops = tops;
	options.origin_x = job.pos.x * CHUNK;
	options.origin_y = job.pos.y * CHUNK;
	options.origin_z = job.pos.z * CHUNK;
	r.mesh = std::make_unique<ChunkMesh>();
	r.mesh->pos = job.pos;
	mesh_chunk(*hood[neighbourhood_index(0, 0, 0)], faces, hood, blocks_, atlas_, &light, options, r.mesh->opaque,
			r.mesh->cutout, r.mesh->fluid);
	auto packed = std::make_shared<std::vector<uint8_t>>(CHUNK_VOLUME);
	for (int y = 0; y < CHUNK; y++) {
		for (int z = 0; z < CHUNK; z++) {
			for (int x = 0; x < CHUNK; x++) {
				(*packed)[(y * CHUNK + z) * CHUNK + x] = uint8_t(light.sky_at(x, y, z) << 4 | light.block_at(x, y, z));
			}
		}
	}
	r.light = std::move(packed);
	mesh_us_ += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t1).count();
	meshed_jobs_++;
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
			revision_++;
			c.local_tops = res.local_tops;
			update_column_tops(res.pos.x, res.pos.z);
			// It and every neighbour may now have all they need to be meshed.
			for (int dy = -1; dy <= 1; dy++) {
				for (int dz = -1; dz <= 1; dz++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int64_t k = key(res.pos.x + dx, res.pos.y + dy, res.pos.z + dz);
						auto n = chunks_.find(k);
						if (n != chunks_.end() && n->second.data && n->second.meshed_version != n->second.mesh_version) {
							mesh_candidates_.insert(k);
						}
					}
				}
			}
		} else {
			if (res.version != c.mesh_version) {
				continue; // changed again since: a newer mesh is on its way
			}
			c.mesh_queued = false;
			c.meshed_version = res.version;
			c.light = std::move(res.light);
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
			load_column_chunks(center, dx, dz, 0.0f);
		}
	}
	// Around other players (the host): block data only, after our own view.
	for (const auto &feet : extra_viewers_) {
		const ChunkPos c{floor_div(int(std::floor(feet[0])), CHUNK), 0, floor_div(int(std::floor(feet[2])), CHUNK)};
		for (int dz = -EXTRA_RADIUS; dz <= EXTRA_RADIUS; dz++) {
			for (int dx = -EXTRA_RADIUS; dx <= EXTRA_RADIUS; dx++) {
				load_column_chunks(c, dx, dz, 1000.0f);
			}
		}
	}

	// Save and forget chunks well outside it (a margin, so walking back and
	// forth doesn't load them again).
	const int keep = r + 3;
	for (auto it = chunks_.begin(); it != chunks_.end();) {
		const ChunkPos p = pos_of(it->first);
		const int dx = p.x - center.x, dz = p.z - center.z;
		bool near_other = false;
		for (const auto &feet : extra_viewers_) {
			const int ex = floor_div(int(std::floor(feet[0])), CHUNK) - p.x, ez = floor_div(int(std::floor(feet[2])), CHUNK) - p.z;
			near_other = near_other || (std::abs(ex) <= EXTRA_RADIUS + 1 && std::abs(ez) <= EXTRA_RADIUS + 1);
		}
		if (dx * dx + dz * dz > keep * keep && !near_other) {
			save_chunk(p, it->second);
			if (save_dir_.empty() && it->second.dirty && it->second.data) {
				kept_[it->first] = it->second.data; // (no file to keep it in)
			}
			if (it->second.meshed_version || it->second.mesh_queued) {
				unloaded_.push_back(p);
				revision_++;
			}
			column_tops_.erase(key(p.x, 0, p.z));
			mesh_candidates_.erase(it->first);
			urgent_.erase(it->first);
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

void World::load_column_chunks(const ChunkPos &center, int dx, int dz, float priority_bias) {
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
		job.priority = float(dx * dx + dz * dz) + std::abs(cy) * 0.5f + priority_bias;
		if (auto k = kept_.find(key(p)); k != kept_.end()) {
			job.kept = k->second;
			it->second.dirty = true; // (still differs from the generator's)
		}
		push_job(std::move(job));
	}
}

std::vector<uint8_t> World::chunk_bytes(const ChunkPos &p) const {
	if (auto it = chunks_.find(key(p)); it != chunks_.end() && it->second.data) {
		return encode_chunk(*it->second.data);
	}
	if (auto k = kept_.find(key(p)); k != kept_.end()) {
		return encode_chunk(*k->second);
	}
	ChunkData data;
	if (!save_dir_.empty() && load_chunk_file(chunk_file_path(save_dir_, p.x, p.y, p.z), data)) {
		return encode_chunk(data);
	}
	return {};
}

bool World::set_chunk(const ChunkPos &p, const uint8_t *bytes, size_t size) {
	if (p.y < CHUNK_Y_MIN || p.y > CHUNK_Y_MAX) {
		return false;
	}
	auto data = std::make_shared<ChunkData>();
	if (!decode_chunk(bytes, size, *data)) {
		return false;
	}
	if (data->uniform ? data->uniform_value >= blocks_.size()
					  : std::any_of(data->blocks.begin(), data->blocks.end(), [&](BlockId b) { return b >= blocks_.size(); })) {
		return false; // a block we don't know
	}
	const int64_t k = key(p);
	if (save_dir_.empty()) {
		kept_[k] = data;
	}
	auto it = chunks_.find(k);
	if (it == chunks_.end() || !it->second.data) {
		if (it != chunks_.end()) {
			// Being generated: forget that job's result, load it from here instead.
			chunks_.erase(it);
		}
		if (!save_dir_.empty()) {
			save_chunk_file(chunk_file_path(save_dir_, p.x, p.y, p.z), *data);
		}
		return true; // (loaded from kept_ or its file when it's needed)
	}
	Chunk &c = it->second;
	c.data = data;
	c.dirty = true;
	revision_++;
	chunk_local_tops(*data, blocks_, c.local_tops);
	update_column_tops(p.x, p.z);
	for (int cy = CHUNK_Y_MIN; cy <= CHUNK_Y_MAX; cy++) { // its light may reach the neighbours, and down the column
		for (int dz = -1; dz <= 1; dz++) {
			for (int dx = -1; dx <= 1; dx++) {
				request_remesh(p.x + dx, cy, p.z + dz);
			}
		}
	}
	return true;
}

std::vector<std::pair<ChunkPos, std::vector<uint8_t>>> World::edited_chunks() const {
	std::vector<std::pair<ChunkPos, std::vector<uint8_t>>> out;
	std::unordered_set<int64_t> seen;
	for (const auto &[k, c] : chunks_) {
		if (c.data && c.dirty) {
			out.push_back({pos_of(k), encode_chunk(*c.data)});
			seen.insert(k);
		}
	}
	for (const auto &[k, data] : kept_) {
		if (seen.insert(k).second) {
			out.push_back({pos_of(k), encode_chunk(*data)});
		}
	}
	if (!save_dir_.empty()) {
		for (const auto &f : list_chunk_files(save_dir_)) {
			const ChunkPos p{f[0], f[1], f[2]};
			if (seen.insert(key(p)).second) {
				if (std::vector<uint8_t> bytes = chunk_bytes(p); !bytes.empty()) {
					out.push_back({p, std::move(bytes)});
				}
			}
		}
	}
	return out;
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
		if (cit == chunks_.end() || !cit->second.data || cit->second.mesh_queued ||
				cit->second.meshed_version == cit->second.mesh_version) {
			it = mesh_candidates_.erase(it);
			continue;
		}
		Chunk &c = cit->second;
		const ChunkPos p = pos_of(*it);
		const int dx0 = p.x - center.x, dz0 = p.z - center.z;
		const bool urgent = urgent_.count(*it) > 0;
		if (!urgent && dx0 * dx0 + dz0 * dz0 > r * r) {
			++it; // not in view yet; stays a candidate
			continue;
		}
		Job job;
		job.kind = JOB_MESH;
		job.pos = p;
		job.serial = c.serial;
		job.version = c.mesh_version;
		job.priority = float(dx0 * dx0 + dz0 * dz0) + std::abs(p.y) * 0.5f - (urgent ? 1e6f : 0.0f);
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
		urgent_.erase(*it);
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

// --- Blocks and editing ----------------------------------------------------------

const ChunkData *World::chunk_data_at(int x, int y, int z, int &lx, int &ly, int &lz) const {
	const int cx = floor_div(x, CHUNK), cy = floor_div(y, CHUNK), cz = floor_div(z, CHUNK);
	auto it = chunks_.find(key(cx, cy, cz));
	if (it == chunks_.end() || !it->second.data) {
		return nullptr;
	}
	lx = x - cx * CHUNK;
	ly = y - cy * CHUNK;
	lz = z - cz * CHUNK;
	return it->second.data.get();
}

int World::block_id_at(int x, int y, int z) const {
	const int cy = floor_div(y, CHUNK);
	if (cy > CHUNK_Y_MAX || cy < CHUNK_Y_MIN) {
		return AIR;
	}
	int lx, ly, lz;
	const ChunkData *d = chunk_data_at(x, y, z, lx, ly, lz);
	return d ? d->get(lx, ly, lz) : -1;
}

BlockId World::block_at(int x, int y, int z) const {
	const int id = block_id_at(x, y, z);
	return id < 0 ? AIR : BlockId(id);
}

void World::light_at(int x, int y, int z, int &sky, int &block) const {
	const int cx = floor_div(x, CHUNK), cy = floor_div(y, CHUNK), cz = floor_div(z, CHUNK);
	auto it = chunks_.find(key(cx, cy, cz));
	if (it == chunks_.end() || !it->second.light) {
		sky = LIGHT_MAX; // not lit yet (or above the world): open sky
		block = 0;
		return;
	}
	const uint8_t v = (*it->second.light)[((y - cy * CHUNK) * CHUNK + (z - cz * CHUNK)) * CHUNK + (x - cx * CHUNK)];
	sky = v >> 4;
	block = v & 15;
}

std::vector<uint8_t> World::held_light(const BlockPos &origin, int size,
		const std::vector<std::array<float, 4>> &lights) const {
	size = std::clamp(size, 1, 128);
	std::vector<uint8_t> vol(size_t(size) * size * size, 0);
	// Whether light passes through the block at a volume position (unloaded: no;
	// above the world: open air).
	auto passes = [&](int x, int y, int z) {
		const int wy = origin.y + y;
		if (floor_div(wy, CHUNK) > CHUNK_Y_MAX) {
			return true;
		}
		const int id = block_id_at(origin.x + x, wy, origin.z + z);
		return id >= 0 && (size_t(id) >= blocks_.size() || !blocks_[size_t(id)].opaque);
	};
	auto index = [size](int x, int y, int z) { return size_t((y * size + z) * size + x); };
	std::vector<size_t> queue;
	for (const auto &l : lights) {
		const int x = int(std::floor(l[0])) - origin.x, y = int(std::floor(l[1])) - origin.y,
				  z = int(std::floor(l[2])) - origin.z;
		const int level = std::clamp(int(l[3]), 0, int(LIGHT_MAX));
		if (x < 0 || y < 0 || z < 0 || x >= size || y >= size || z >= size || level <= vol[index(x, y, z)]) {
			continue;
		}
		vol[index(x, y, z)] = uint8_t(level);
		queue.push_back(index(x, y, z));
	}
	static const int DIRS[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	for (size_t q = 0; q < queue.size(); q++) {
		const size_t i = queue[q];
		const int x = int(i % size_t(size)), z = int((i / size_t(size)) % size_t(size)), y = int(i / size_t(size * size));
		const int next = vol[i] - 1;
		if (next <= 0) {
			continue;
		}
		for (const auto &d : DIRS) {
			const int nx = x + d[0], ny = y + d[1], nz = z + d[2];
			if (nx < 0 || ny < 0 || nz < 0 || nx >= size || ny >= size || nz >= size) {
				continue;
			}
			const size_t n = index(nx, ny, nz);
			if (vol[n] >= next || !passes(nx, ny, nz)) {
				continue;
			}
			vol[n] = uint8_t(next);
			queue.push_back(n);
		}
	}
	return vol;
}

bool World::is_loaded(int x, int y, int z) const {
	int lx, ly, lz;
	return chunk_data_at(x, y, z, lx, ly, lz) != nullptr;
}

bool World::set_block(const BlockPos &p, BlockId id) {
	if (id >= blocks_.size()) {
		return false;
	}
	const int cx = floor_div(p.x, CHUNK), cy = floor_div(p.y, CHUNK), cz = floor_div(p.z, CHUNK);
	auto it = chunks_.find(key(cx, cy, cz));
	if (it == chunks_.end() || !it->second.data) {
		return false;
	}
	Chunk &chunk = it->second;
	const int lx = p.x - cx * CHUNK, ly = p.y - cy * CHUNK, lz = p.z - cz * CHUNK;
	if (chunk.data->get(lx, ly, lz) == id) {
		return true;
	}
	// Copy-on-write: mesh jobs may still be reading the old snapshot.
	auto copy = std::make_shared<ChunkData>(*chunk.data);
	copy->set(lx, ly, lz, id);
	copy->compact();
	chunk.data = copy;
	chunk.dirty = true;
	revision_++;

	// Column top within this chunk, then for the whole column.
	const int column = lz * CHUNK + lx;
	int8_t local_top = -1;
	for (int y = CHUNK - 1; y >= 0; y--) {
		const BlockId b = copy->get(lx, y, lz);
		if (b < blocks_.size() && blocks_[b].opaque) {
			local_top = static_cast<int8_t>(y);
			break;
		}
	}
	const int64_t ckey = key(cx, 0, cz);
	auto tit = column_tops_.find(ckey);
	const int old_top = tit != column_tops_.end() ? (*tit->second)[column] : NO_TOP;
	chunk.local_tops[column] = local_top;
	update_column_tops(cx, cz);
	tit = column_tops_.find(ckey);
	const int new_top = tit != column_tops_.end() ? (*tit->second)[column] : NO_TOP;

	// Everything light can reach from here, plus everything below a column
	// whose sky exposure changed.
	int y_low = p.y, y_high = p.y;
	if (old_top != new_top) {
		y_low = std::min({y_low, old_top, new_top});
		y_high = std::max({y_high, old_top, new_top});
		y_low = std::max(y_low, CHUNK_Y_MIN * CHUNK);
	}
	relight_around(p, y_low, y_high);
	if (on_block_changed) {
		on_block_changed(p, id);
	}
	return true;
}

void World::relight_around(const BlockPos &p, int y_low, int y_high) {
	const int ax = floor_div(p.x - LIGHT_MAX, CHUNK), ay = floor_div(y_low - LIGHT_MAX, CHUNK),
			  az = floor_div(p.z - LIGHT_MAX, CHUNK);
	const int bx = floor_div(p.x + LIGHT_MAX, CHUNK), by = floor_div(y_high + LIGHT_MAX, CHUNK),
			  bz = floor_div(p.z + LIGHT_MAX, CHUNK);
	for (int cy = std::max(ay, CHUNK_Y_MIN); cy <= std::min(by, CHUNK_Y_MAX); cy++) {
		for (int cz = az; cz <= bz; cz++) {
			for (int cx = ax; cx <= bx; cx++) {
				request_remesh(cx, cy, cz);
			}
		}
	}
}

void World::set_block_shadows(bool enabled) {
	if (block_shadows_.exchange(enabled) != enabled) {
		remesh_all();
	}
}

void World::set_smooth_lighting(bool enabled) {
	if (smooth_lighting_.exchange(enabled) != enabled) {
		remesh_all();
	}
}

void World::set_sun_angle(float radians) {
	const float degrees = radians * 180.0f / 3.14159265f;
	if (!(degrees > 0.0f && degrees < 180.0f)) {
		return; // night: keep the last shadows for the morning's first frames
	}
	const int last = 180 / SUN_STEP_DEGREES - 1;
	const int step = std::clamp(int(std::lround(degrees / SUN_STEP_DEGREES)), 1, last);
	if (sun_step_.exchange(step) != step && block_shadows_.load()) {
		remesh_all();
	}
}

// Every meshed chunk again (not urgent: nearest first); the old meshes stay
// until the new ones arrive.
void World::remesh_all() {
	for (auto &[k, c] : chunks_) {
		if (c.data && c.meshed_version > 0) {
			c.mesh_version++;
			c.mesh_queued = false;
			mesh_candidates_.insert(k);
		}
	}
}

void World::request_remesh(int cx, int cy, int cz) {
	const int64_t k = key(cx, cy, cz);
	auto it = chunks_.find(k);
	if (it == chunks_.end() || !it->second.data) {
		return;
	}
	it->second.mesh_version++;
	it->second.mesh_queued = false; // a running job's result is now stale
	mesh_candidates_.insert(k);
	const int dx = cx - center_.x, dz = cz - center_.z;
	if (dx * dx + dz * dz <= (radius_ + 1) * (radius_ + 1)) {
		urgent_.insert(k); // (not chunks only loaded around other players)
	}
}

bool World::save_chunk(const ChunkPos &p, Chunk &c) {
	if (save_dir_.empty() || !c.dirty || !c.data) {
		return true;
	}
	if (!save_chunk_file(chunk_file_path(save_dir_, p.x, p.y, p.z), *c.data)) {
		log("Couldn't save chunk (%d, %d, %d) in %s", p.x, p.y, p.z, save_dir_.c_str());
		return false;
	}
	c.dirty = false;
	return true;
}

int World::save_modified() {
	int saved = 0;
	for (auto &[k, c] : chunks_) {
		if (c.dirty && save_chunk(pos_of(k), c)) {
			saved++;
		}
	}
	return saved;
}

// --- Raycast and collision ---------------------------------------------------------

RayHit World::raycast(const float origin[3], const float direction[3], float max_distance) const {
	RayHit result;
	const float len = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
	if (len < 1e-6f) {
		return result;
	}
	const float dir[3] = {direction[0] / len, direction[1] / len, direction[2] / len};
	// Voxel traversal: step to whichever cell boundary the ray reaches first.
	int cell[3] = {int(std::floor(origin[0])), int(std::floor(origin[1])), int(std::floor(origin[2]))};
	int previous[3] = {cell[0], cell[1], cell[2]};
	int normal[3] = {0, 0, 0};
	int step[3];
	float t_max[3], t_delta[3];
	for (int a = 0; a < 3; a++) {
		const float d = dir[a];
		step[a] = d > 0 ? 1 : (d < 0 ? -1 : 0);
		t_delta[a] = d != 0 ? std::fabs(1.0f / d) : INFINITY;
		const float boundary = d > 0 ? std::floor(origin[a]) + 1.0f : std::floor(origin[a]);
		t_max[a] = d != 0 ? (boundary - origin[a]) / d : INFINITY;
	}
	float t = 0.0f;
	while (t <= max_distance) {
		const int id = block_id_at(cell[0], cell[1], cell[2]);
		if (id < 0) {
			return result; // left the loaded world
		}
		if (id != AIR && id < int(blocks_.size()) && !blocks_[id].fluid) {
			if (blocks_[id].shape == SHAPE_CUBE) {
				result.hit = true;
				result.position = {cell[0], cell[1], cell[2]};
				result.previous = {previous[0], previous[1], previous[2]};
				result.normal = {normal[0], normal[1], normal[2]};
				result.block = BlockId(id);
				result.distance = t;
				return result;
			}
			// Shaped block: the nearest of its boxes along the ray, if any.
			std::vector<Box> boxes;
			block_boxes(cell[0], cell[1], cell[2], id, false, boxes);
			float best = INFINITY;
			int best_normal[3] = {0, 0, 0};
			for (const Box &bx : boxes) {
				float t0 = 0.0f, t1 = max_distance;
				int axis = -1;
				bool miss = false;
				for (int k = 0; k < 3 && !miss; k++) {
					if (dir[k] == 0.0f) {
						miss = origin[k] < bx.mn[k] || origin[k] > bx.mx[k];
						continue;
					}
					float ta = (bx.mn[k] - origin[k]) / dir[k], tb = (bx.mx[k] - origin[k]) / dir[k];
					if (ta > tb) {
						std::swap(ta, tb);
					}
					if (ta > t0) {
						t0 = ta;
						axis = k;
					}
					t1 = std::min(t1, tb);
					miss = t0 > t1;
				}
				if (!miss && t0 < best) {
					best = t0;
					best_normal[0] = best_normal[1] = best_normal[2] = 0;
					if (axis >= 0) {
						best_normal[axis] = dir[axis] > 0 ? -1 : 1;
					}
				}
			}
			if (best <= max_distance) {
				result.hit = true;
				result.position = {cell[0], cell[1], cell[2]};
				result.previous = {cell[0] + best_normal[0], cell[1] + best_normal[1], cell[2] + best_normal[2]};
				result.normal = {best_normal[0], best_normal[1], best_normal[2]};
				result.block = BlockId(id);
				result.distance = best;
				return result;
			}
		}
		const int a = (t_max[0] < t_max[1]) ? (t_max[0] < t_max[2] ? 0 : 2) : (t_max[1] < t_max[2] ? 1 : 2);
		previous[0] = cell[0];
		previous[1] = cell[1];
		previous[2] = cell[2];
		cell[a] += step[a];
		t = t_max[a];
		t_max[a] += t_delta[a];
		normal[0] = normal[1] = normal[2] = 0;
		normal[a] = -step[a];
	}
	return result;
}

bool World::fence_connects(int x, int y, int z) const {
	const int id = block_id_at(x, y, z);
	return id > 0 && id < int(blocks_.size()) && (blocks_[id].shape == SHAPE_FENCE || blocks_[id].opaque);
}

void World::block_boxes(int x, int y, int z, int id, bool collision, std::vector<Box> &out) const {
	const size_t first = out.size();
	if (id < 0) {
		out.push_back({{0, 0, 0}, {1, 1, 1}}); // unloaded: solid
	} else if (id == AIR || id >= int(blocks_.size())) {
		return;
	} else {
		const BlockInfo &b = blocks_[id];
		if (b.shape == SHAPE_FENCE) {
			fence_boxes(fence_connects(x, y, z - 1), fence_connects(x + 1, y, z), fence_connects(x, y, z + 1),
					fence_connects(x - 1, y, z), collision, out);
		} else if (collision) {
			out.insert(out.end(), b.collision.begin(), b.collision.end());
		} else if (b.fluid) {
			return;
		} else if (b.shape == SHAPE_BOXES && !b.model.empty()) {
			out.insert(out.end(), b.model.begin(), b.model.end());
		} else if (b.shape == SHAPE_CROSS) {
			out.push_back({{0.15f, 0.0f, 0.15f}, {0.85f, 0.8f, 0.85f}});
		} else {
			out.push_back({{0, 0, 0}, {1, 1, 1}});
		}
	}
	for (size_t i = first; i < out.size(); i++) {
		const float o[3] = {float(x), float(y), float(z)};
		for (int k = 0; k < 3; k++) {
			out[i].mn[k] += o[k];
			out[i].mx[k] += o[k];
		}
	}
}

// Collision boxes of every block that could touch [mn, mx] (one cell lower
// too: fences reach 1.5 blocks up).
void World::collect_collision(const float mn[3], const float mx[3], std::vector<Box> &out) const {
	constexpr float EPS = 1e-3f;
	const int x0 = int(std::floor(mn[0] + EPS)), x1 = int(std::floor(mx[0] - EPS));
	const int y0 = int(std::floor(mn[1] + EPS)) - 1, y1 = int(std::floor(mx[1] - EPS));
	const int z0 = int(std::floor(mn[2] + EPS)), z1 = int(std::floor(mx[2] - EPS));
	for (int y = y0; y <= y1; y++) {
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const size_t first = out.size();
				// Unloaded chunks are solid, so nothing falls out of the loaded
				// world; above and below the world it's open.
				block_boxes(x, y, z, block_id_at(x, y, z), true, out);
				// Keep only boxes that reach into the region.
				for (size_t i = out.size(); i-- > first;) {
					const Box &b = out[i];
					bool touches = true;
					for (int k = 0; k < 3; k++) {
						touches = touches && b.mx[k] > mn[k] + EPS && b.mn[k] < mx[k] - EPS;
					}
					if (!touches) {
						out.erase(out.begin() + std::ptrdiff_t(i));
					}
				}
			}
		}
	}
}

MoveResult World::move_box(const float position[3], const float size[3], const float motion[3], float step_height) const {
	// Moves y, then x, then z, each clipped against the blocks' collision boxes.
	constexpr float EPS = 1e-3f;
	std::vector<Box> boxes;
	auto simulate = [&](const float start[3], const float m[3], bool &on_floor, bool &ceiling, bool &wall,
							float out[3]) {
		float p[3] = {start[0], start[1], start[2]};
		// Everything within reach of this whole move.
		float rmn[3] = {p[0] - size[0] * 0.5f, p[1], p[2] - size[2] * 0.5f};
		float rmx[3] = {p[0] + size[0] * 0.5f, p[1] + size[1], p[2] + size[2] * 0.5f};
		for (int k = 0; k < 3; k++) {
			rmn[k] += std::min(0.0f, m[k]) - 1.0f;
			rmx[k] += std::max(0.0f, m[k]) + 1.0f;
		}
		boxes.clear();
		collect_collision(rmn, rmx, boxes);
		const int order[3] = {1, 0, 2};
		for (int a : order) {
			const float mn[3] = {p[0] - size[0] * 0.5f, p[1], p[2] - size[2] * 0.5f};
			const float mx[3] = {p[0] + size[0] * 0.5f, p[1] + size[1], p[2] + size[2] * 0.5f};
			const int u = (a + 1) % 3, v = (a + 2) % 3;
			float allowed = m[a];
			for (const Box &b : boxes) {
				if (b.mx[u] <= mn[u] + EPS || b.mn[u] >= mx[u] - EPS || b.mx[v] <= mn[v] + EPS || b.mn[v] >= mx[v] - EPS) {
					continue; // not in the way on the other two axes
				}
				if (allowed > 0.0f && b.mn[a] >= mx[a] - EPS) {
					allowed = std::min(allowed, std::max(0.0f, b.mn[a] - mx[a] - EPS));
				} else if (allowed < 0.0f && b.mx[a] <= mn[a] + EPS) {
					allowed = std::max(allowed, std::min(0.0f, b.mx[a] - mn[a] + EPS));
				}
			}
			if (allowed != m[a]) {
				if (a == 1) {
					(m[a] < 0 ? on_floor : ceiling) = true;
				} else {
					wall = true;
				}
			}
			p[a] += allowed;
		}
		out[0] = p[0];
		out[1] = p[1];
		out[2] = p[2];
	};
	MoveResult r;
	simulate(position, motion, r.on_floor, r.hit_ceiling, r.hit_wall, r.position);

	// Step up: if walking on the ground into something, try going over it.
	if (step_height > 0.0f && r.hit_wall) {
		bool f2 = false, c2 = false, w2 = false;
		const float below[3] = {position[0], position[1] - 0.05f, position[2]};
		if (!is_box_free(below, size) || r.on_floor) {
			float up[3], across[3], down[3];
			const float m_up[3] = {0, step_height, 0};
			simulate(position, m_up, f2, c2, w2, up);
			const float m_across[3] = {motion[0], 0, motion[2]};
			simulate(up, m_across, f2, c2, w2, across);
			bool f3 = false, c3 = false, w3 = false;
			const float m_down[3] = {0, -step_height + std::min(0.0f, motion[1]), 0};
			simulate(across, m_down, f3, c3, w3, down);
			const float flat = std::hypot(r.position[0] - position[0], r.position[2] - position[2]);
			const float stepped = std::hypot(down[0] - position[0], down[2] - position[2]);
			if (stepped > flat + 1e-3f) {
				std::copy(down, down + 3, r.position);
				r.on_floor = f3;
				r.hit_wall = w2;
			}
		}
	}
	return r;
}

bool World::is_box_free(const float position[3], const float size[3]) const {
	const float mn[3] = {position[0] - size[0] * 0.5f, position[1], position[2] - size[2] * 0.5f};
	const float mx[3] = {position[0] + size[0] * 0.5f, position[1] + size[1], position[2] + size[2] * 0.5f};
	std::vector<Box> boxes;
	collect_collision(mn, mx, boxes); // only boxes overlapping the region are kept
	return boxes.empty();
}

WorldStats World::stats() const {
	WorldStats s;
	for (const auto &[k, c] : chunks_) {
		s.loaded += c.data ? 1 : 0;
		s.meshed += c.meshed_version ? 1 : 0;
		s.dirty += c.dirty ? 1 : 0;
	}
	{
		std::lock_guard<std::mutex> lock(job_mutex_);
		s.queued = int(jobs_.size()) + running_;
	}
	s.settled = s.queued == 0 && urgent_.empty();
	s.workers = int(workers_.size());
	s.generated = generated_.load();
	s.meshed_jobs = meshed_jobs_.load();
	s.generate_ms = double(generate_us_.load()) / 1000.0;
	s.light_ms = double(light_us_.load()) / 1000.0;
	s.mesh_ms = double(mesh_us_.load()) / 1000.0;
	for (int dz = -radius_; dz <= radius_ && s.settled; dz++) {
		for (int dx = -radius_; dx <= radius_ && s.settled; dx++) {
			if (dx * dx + dz * dz > radius_ * radius_) {
				continue;
			}
			for (int cy = CHUNK_Y_MIN; cy <= CHUNK_Y_MAX; cy++) {
				auto it = chunks_.find(key(center_.x + dx, cy, center_.z + dz));
				if (it == chunks_.end() || it->second.meshed_version != it->second.mesh_version) {
					s.settled = false;
					break;
				}
			}
		}
	}
	return s;
}

} // namespace gc
