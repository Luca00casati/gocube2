// The voxel world around the player: chunks are loaded from the world's
// chunk files or generated, on worker threads, then lit and meshed once their
// whole 3x3x3 neighbourhood and the 9 columns around them are there (sky
// light needs complete columns). Edits change a copy of the chunk, are saved
// to its file, and remesh everything their light can reach.
// The main thread calls update() once per frame and uploads finished meshes.
// (src/world/world.*)
module gocube::world;
import gocube::voxel, gocube::util::vec, gocube::util::map, gocube::util::shared, gocube::maths, gocube::clock, gocube::log;
import std::thread, std::atomic, std::math;
import std::core::mem::allocators @public;

struct ChunkPos
{
	int x, y, z;
}

struct BlockPos
{
	int x, y, z;
}

fn BlockPos BlockPos.add(self, BlockPos o) @operator(+) => { self.x + o.x, self.y + o.y, self.z + o.z };
fn bool BlockPos.eq(self, BlockPos o) @operator(==) => self.x == o.x && self.y == o.y && self.z == o.z;

// A chunk's finished meshes (chunk-local positions; the chunk's minimum corner
// is pos * CHUNK in blocks).
struct ChunkMesh
{
	ChunkPos pos;
	Surface opaque, cutout, fluid;
}

fn void ChunkMesh.free(&self)
{
	self.opaque.free();
	self.cutout.free();
	self.fluid.free();
}

// A vertex of the world's meshes (chunk.hlsl's input): position, normal, uv,
// colour (sky light, block light, occlusion, marks), dye (rgb 0..1, a = 1 if
// dyed).
struct WorldVertex
{
	float[3] pos;
	float[3] normal;
	float[2] uv;
	float[4] color;
	float[4] dye;
}

fn void WorldVertex.set_dye(&self, Dye d)
{
	self.dye = { (float)((d >> 16) & 255) / 255.0f, (float)((d >> 8) & 255) / 255.0f, (float)(d & 255) / 255.0f,
		d != voxel::NO_DYE ? 1.0f : 0.0f };
}

struct WorldStats
{
	int loaded;  // chunks with block data
	int meshed;  // chunks with a mesh
	int queued;  // jobs waiting for a worker or running
	int dirty;   // edited chunks not saved yet
	bool settled; // everything within view distance is meshed and up to date
	// The worker threads' work so far: chunks generated (not read from a file)
	// and meshed, and the ms they took in all.
	int workers;
	long generated, meshed_jobs;
	double generate_ms, light_ms, mesh_ms;
}

struct RayHit
{
	bool hit;
	BlockPos position; // the block hit
	BlockPos previous; // the cell in front of the face hit (where a block would go)
	BlockPos normal;   // the face hit, pointing out of the block
	BlockId block;
	float distance;
}

struct MoveResult
{
	float[3] position; // feet, after the move
	bool on_floor, hit_ceiling, hit_wall;
}

struct EditedChunk
{
	ChunkPos pos;
	Bytes bytes;
}

const int CHUNK = voxel::CHUNK;
const int CHUNK_AREA = voxel::CHUNK_AREA;
const int CHUNK_VOLUME = voxel::CHUNK_VOLUME;
const int FACE_COUNT = voxel::FACE_COUNT;

alias SharedChunk = Shared{ChunkData};
alias SharedTops = Shared{ColumnTops};
alias SharedLight = Shared{char[CHUNK_VOLUME]};
alias BlockChangedFn = fn void(void* context, BlockPos p, BlockId id);

const int CHUNK_Y_MIN = -3; // the world is 7 chunks (224 blocks) tall
const int CHUNK_Y_MAX = 3;
const int BOTTOM_Y = CHUNK_Y_MIN * CHUNK; // nothing below: falling past it hurts
// Block shadows follow the sun in steps of SUN_STEP_DEGREES.
const int SUN_STEP_DEGREES = 2;
// The host: chunks within EXTRA_RADIUS chunks of other players stay loaded
// (block data only, not meshed).
const int EXTRA_RADIUS = 2;

enum JobKind : char
{
	GENERATE,
	MESH,
}

struct Job
{
	JobKind kind;
	ChunkPos pos;
	float priority;     // lower first
	ulong serial;       // the chunk entry it was made for
	ulong version;      // MESH: the chunk's mesh_version
	SharedChunk* kept;  // GENERATE: kept in memory (no file)
	SharedChunk*[27] hood;
	SharedTops*[9] tops;
}

fn void Job.release(&self)
{
	if (self.kept) self.kept.release();
	foreach (h : self.hood)
	{
		if (h) h.release();
	}
	foreach (t : self.tops)
	{
		if (t) t.release();
	}
	*self = {};
}

struct Result
{
	JobKind kind;
	ChunkPos pos;
	ulong serial;
	ulong version;
	SharedChunk* data;
	ichar[CHUNK_AREA] local_tops;
	ChunkMesh* mesh;
	SharedLight* light; // sky << 4 | block, per block
}

fn void Result.release(&self)
{
	if (self.data) self.data.release();
	if (self.light) self.light.release();
	if (self.mesh)
	{
		self.mesh.free();
		free(self.mesh);
	}
	*self = {};
}

struct Chunk
{
	ulong serial;        // tells a reloaded chunk from the one a job was made for
	SharedChunk* data;   // null until generated or loaded
	ichar[CHUNK_AREA] local_tops;
	ulong mesh_version;  // goes up when the chunk (or its light) changes
	ulong meshed_version; // of the mesh handed out (0: none yet)
	bool mesh_queued;
	bool dirty;          // edited since it was last saved
	SharedLight* light;  // of the last mesh
}

fn void Chunk.release(&self)
{
	if (self.data) self.data.release();
	if (self.light) self.light.release();
	*self = {};
}

struct World
{
	BlockInfo[] blocks;
	AtlasLayout* atlas;
	Generator* generator;
	String save_dir; // owned ("": nothing is loaded or saved)
	int view_distance;
	ulong next_serial;
	ulong revision_;
	ChunkPos center;  // at the last update
	int radius;       // in chunks, at the last update
	BlockChangedFn on_block_changed; // told about every block change, after it
	void* on_block_changed_context;

	// Main thread only.
	Map{long, Chunk*} chunks;
	Map{long, SharedTops*} column_tops; // key(cx, 0, cz)
	Map{long, bool} mesh_candidates;
	Map{long, bool} urgent; // remeshed after an edit: before anything else
	Vec{ChunkMesh} meshes;
	Vec{ChunkPos} unloaded;
	Map{long, SharedChunk*} kept; // edited, not loaded (no folder)
	Vec{float[3]} extra_viewers;

	// Shared with the workers.
	Mutex job_mutex;
	ConditionVariable job_cv;
	Vec{Job} jobs;
	int running; // jobs being worked on
	Atomic{int} block_shadows;
	Atomic{int} smooth_lighting;
	Atomic{int} sun_step; // SUN_STEP_DEGREES steps above the eastern horizon
	bool stop;
	Mutex result_mutex;
	Vec{Result} results;
	Vec{Thread} workers;
	// What the workers allocate and free with: the creating thread's (snapshots
	// made on one thread are often freed on another).
	Allocator allocator;
	// Work done by the workers (microseconds; counts).
	Atomic{long} generate_us, light_us, mesh_us, generated, meshed_jobs;
}

extern fn uint get_active_processor_count(ushort group) @cname("GetActiveProcessorCount");

// `blocks`, `atlas` and `generator` must outlive the world. `save_dir`: the
// world's chunks folder ("": nothing is loaded or saved).
fn void World.init(&self, BlockInfo[] blocks, AtlasLayout* atlas, Generator* generator, String save_dir = "", int worker_threads = 0)
{
	*self = { .blocks = blocks, .atlas = atlas, .generator = generator, .save_dir = save_dir.copy(mem), .view_distance = 128, .next_serial = 1 };
	self.smooth_lighting.store(1);
	self.sun_step.store(45);
	self.job_mutex.init()!!;
	self.job_cv.init()!!;
	self.result_mutex.init()!!;
	self.allocator = allocators::thread_allocator;
	if (worker_threads <= 0) worker_threads = math::max(1, (int)get_active_processor_count(0xFFFF) - 1); // one core for the game
	for (int i = 0; i < worker_threads; i++)
	{
		Thread t;
		t.create(&worker_main, self)!!;
		self.workers.push(t);
	}
}

// (Doesn't save: call save_modified() first.)
fn void World.free(&self)
{
	self.job_mutex.lock();
	self.stop = true;
	self.job_mutex.unlock();
	self.job_cv.broadcast();
	foreach (t : self.workers) (void)t.join();
	self.workers.free();
	foreach (&j : self.jobs) j.release();
	self.jobs.free();
	foreach (&r : self.results) r.release();
	self.results.free();
	self.chunks.@each(; long k, Chunk* c)
	{
		c.release();
		free(c);
	};
	self.chunks.free();
	self.column_tops.@each(; long k, SharedTops* t) { t.release(); };
	self.column_tops.free();
	self.kept.@each(; long k, SharedChunk* d) { d.release(); };
	self.kept.free();
	self.mesh_candidates.free();
	self.urgent.free();
	foreach (&m : self.meshes) m.free();
	self.meshes.free();
	self.unloaded.free();
	self.extra_viewers.free();
	self.job_mutex.destroy();
	(void)self.job_cv.destroy();
	self.result_mutex.destroy();
	free(self.save_dir.ptr);
	*self = {};
}

fn void World.set_view_distance(&self, int blocks) => self.view_distance = blocks > 0 ? blocks : 1;
fn ulong World.revision(&self) => self.revision_;
fn bool World.has_folder(&self) => self.save_dir.len > 0;

fn long key(int x, int y, int z) => ((long)(x & 0x1FFFFF) << 42) | ((long)(y & 0x1FFFFF) << 21) | (long)(z & 0x1FFFFF);
fn long key_of(ChunkPos p) @inline => key(p.x, p.y, p.z);

fn int field21(long v) @private
{
	v &= 0x1FFFFF; // 21-bit two's complement
	return (int)(v >= 0x100000 ? v - 0x200000 : v);
}

fn ChunkPos pos_of(long k) => { field21(k >> 42), field21(k >> 21), field21(k) };

fn int floor_div(int v, int d) @inline => maths::floor_div(v, d);

// --- Workers ---------------------------------------------------------------------

fn void World.push_job(&self, Job job)
{
	self.job_mutex.lock();
	self.jobs.push(job);
	self.job_mutex.unlock();
	self.job_cv.signal();
}

fn int worker_main(void* arg) @private
{
	World* self = arg;
	allocators::thread_allocator = self.allocator;
	while (true)
	{
		self.job_mutex.lock();
		while (!self.stop && self.jobs.is_empty()) self.job_cv.wait(&self.job_mutex);
		if (self.stop)
		{
			self.job_mutex.unlock();
			return 0;
		}
		// Nearest first (the queue is small enough for a linear scan).
		sz best = 0;
		for (sz i = 1; i < self.jobs.len(); i++)
		{
			if (self.jobs[i].priority < self.jobs[best].priority) best = i;
		}
		Job job = self.jobs[best];
		self.jobs.swap_remove(best);
		self.running++;
		self.job_mutex.unlock();
		Result r;
		self.run(&job, &r);
		job.release();
		self.result_mutex.lock();
		self.results.push(r);
		self.result_mutex.unlock();
		self.job_mutex.lock();
		self.running--;
		self.job_mutex.unlock();
	}
}

fn void World.run(&self, Job* job, Result* r)
{
	r.kind = job.kind;
	r.pos = job.pos;
	r.serial = job.serial;
	r.version = job.version;
	if (job.kind == GENERATE)
	{
		SharedChunk* data = shared::new{ChunkData}();
		// An edited chunk comes from its file (or memory); the rest is generated again.
		bool loaded = false;
		if (job.kept)
		{
			copy_chunk(&data.value, &job.kept.value);
			loaded = true;
		}
		else if (self.save_dir.len)
		{
			@pool()
			{
				loaded = voxel::load_chunk_file(voxel::chunk_file_path(tmem, self.save_dir, job.pos.x, job.pos.y, job.pos.z), &data.value);
			};
		}
		if (!loaded)
		{
			long t0 = clock::now_us();
			self.generator.generate(&data.value, job.pos.x * CHUNK, job.pos.y * CHUNK, job.pos.z * CHUNK);
			self.generate_us.add(clock::now_us() - t0);
			self.generated.add(1);
		}
		voxel::chunk_local_tops(&data.value, self.blocks, &r.local_tops);
		r.data = data;
		return;
	}
	ChunkData*[27] hood;
	for (int i = 0; i < 27; i++) hood[i] = job.hood[i] ? &job.hood[i].value : null;
	ColumnTops*[9] tops;
	for (int i = 0; i < 9; i++) tops[i] = job.tops[i] ? &job.tops[i].value : null;
	long t0 = clock::now_us();
	LightRegion light;
	defer light.free();
	voxel::compute_light(&light, &hood, &tops, job.pos.y, self.blocks);
	long t1 = clock::now_us();
	self.light_us.add(t1 - t0);
	ChunkData*[FACE_COUNT] faces = { hood[voxel::neighbourhood_index(1, 0, 0)], hood[voxel::neighbourhood_index(-1, 0, 0)],
		hood[voxel::neighbourhood_index(0, 1, 0)], hood[voxel::neighbourhood_index(0, -1, 0)],
		hood[voxel::neighbourhood_index(0, 0, 1)], hood[voxel::neighbourhood_index(0, 0, -1)] };
	MeshOptions options;
	options.smooth_lighting = self.smooth_lighting.load() != 0;
	options.sun_shadows = self.block_shadows.load() != 0;
	float sun = (float)(self.sun_step.load() * SUN_STEP_DEGREES) * 3.14159265f / 180.0f;
	options.sun_x = math::cos(sun);
	options.sun_y = math::sin(sun);
	options.tops = &tops;
	options.origin_x = job.pos.x * CHUNK;
	options.origin_y = job.pos.y * CHUNK;
	options.origin_z = job.pos.z * CHUNK;
	r.mesh = mem::new(ChunkMesh);
	r.mesh.pos = job.pos;
	voxel::mesh_chunk(hood[voxel::neighbourhood_index(0, 0, 0)], faces, &hood, self.blocks, self.atlas, &light, &options,
		&r.mesh.opaque, &r.mesh.cutout, &r.mesh.fluid);
	SharedLight* packed = shared::new{char[CHUNK_VOLUME]}();
	for (int y = 0; y < CHUNK; y++)
	{
		for (int z = 0; z < CHUNK; z++)
		{
			for (int x = 0; x < CHUNK; x++)
			{
				packed.value[(y * CHUNK + z) * CHUNK + x] = light.sky_at(x, y, z) << 4 | light.block_at(x, y, z);
			}
		}
	}
	r.light = packed;
	self.mesh_us.add(clock::now_us() - t1);
	self.meshed_jobs.add(1);
}

// A copy of a chunk's data (its own memory).
fn void copy_chunk(ChunkData* to, ChunkData* from)
{
	to.free();
	to.mixed = from.mixed;
	to.uniform_value = from.uniform_value;
	if (from.mixed)
	{
		to.blocks = mem::alloc_array(BlockId, CHUNK_VOLUME);
		to.blocks[..] = from.blocks[..];
	}
	from.dyes.@each(; ushort i, Dye d) { to.dyes[i] = d; };
}

// --- Main thread ---------------------------------------------------------------

fn Chunk* World.chunk(&self, long k) @inline => self.chunks.get_or(k, null);

// Once per frame: takes finished work, asks for chunks around the viewer,
// saves and forgets far ones, schedules meshing.
fn void World.update(&self, float viewer_x, float viewer_y, float viewer_z)
{
	ChunkPos center = { floor_div((int)math::floor(viewer_x), CHUNK), 0, floor_div((int)math::floor(viewer_z), CHUNK) };
	int r = (self.view_distance + CHUNK - 1) / CHUNK;
	self.center = center;
	self.radius = r;

	// Finished work.
	Vec{Result} results;
	defer results.free();
	self.result_mutex.lock();
	results = self.results;
	self.results = {};
	self.result_mutex.unlock();
	foreach (&res : results)
	{
		Chunk* c = self.chunk(key_of(res.pos));
		if (!c || c.serial != res.serial)
		{
			res.release(); // the chunk went away meanwhile
			continue;
		}
		if (res.kind == GENERATE)
		{
			if (c.data) c.data.release();
			c.data = res.data;
			res.data = null;
			self.revision_++;
			c.local_tops = res.local_tops;
			self.update_column_tops(res.pos.x, res.pos.z);
			// It and every neighbour may now have all they need to be meshed.
			for (int dy = -1; dy <= 1; dy++)
			{
				for (int dz = -1; dz <= 1; dz++)
				{
					for (int dx = -1; dx <= 1; dx++)
					{
						long k = key(res.pos.x + dx, res.pos.y + dy, res.pos.z + dz);
						Chunk* n = self.chunk(k);
						if (n && n.data && n.meshed_version != n.mesh_version) self.mesh_candidates[k] = true;
					}
				}
			}
		}
		else
		{
			if (res.version == c.mesh_version) // (else changed again since: a newer mesh is on its way)
			{
				c.mesh_queued = false;
				c.meshed_version = res.version;
				if (c.light) c.light.release();
				c.light = res.light;
				res.light = null;
				self.meshes.push(*res.mesh);
				free(res.mesh);
				res.mesh = null;
			}
		}
		res.release();
	}

	// Chunks within view distance (a circle of columns, the world's full
	// height), and every neighbour of those: a chunk is only meshed once its
	// neighbours are there.
	int load = r + 1;
	for (int dz = -load; dz <= load; dz++)
	{
		for (int dx = -load; dx <= load; dx++)
		{
			int nx = math::max(0, math::abs(dx) - 1); // nearest column in view
			int nz = math::max(0, math::abs(dz) - 1);
			if (nx * nx + nz * nz > r * r) continue;
			self.load_column_chunks(center, dx, dz, 0.0f);
		}
	}
	// Around other players (the host): block data only, after our own view.
	foreach (feet : self.extra_viewers)
	{
		ChunkPos c = { floor_div((int)math::floor(feet[0]), CHUNK), 0, floor_div((int)math::floor(feet[2]), CHUNK) };
		for (int dz = -EXTRA_RADIUS; dz <= EXTRA_RADIUS; dz++)
		{
			for (int dx = -EXTRA_RADIUS; dx <= EXTRA_RADIUS; dx++) self.load_column_chunks(c, dx, dz, 1000.0f);
		}
	}

	// Save and forget chunks well outside it (a margin, so walking back and
	// forth doesn't load them again).
	int keep = r + 3;
	Vec{long} gone;
	defer gone.free();
	self.chunks.@each(; long k, Chunk* c)
	{
		ChunkPos p = pos_of(k);
		int dx = p.x - center.x;
		int dz = p.z - center.z;
		bool near_other = false;
		foreach (feet : self.extra_viewers)
		{
			int ex = floor_div((int)math::floor(feet[0]), CHUNK) - p.x;
			int ez = floor_div((int)math::floor(feet[2]), CHUNK) - p.z;
			near_other = near_other || (math::abs(ex) <= EXTRA_RADIUS + 1 && math::abs(ez) <= EXTRA_RADIUS + 1);
		}
		if (dx * dx + dz * dz > keep * keep && !near_other) gone.push(k);
	};
	foreach (k : gone)
	{
		Chunk* c = self.chunk(k);
		ChunkPos p = pos_of(k);
		self.save_chunk(p, c);
		if (!self.save_dir.len && c.dirty && c.data) self.keep_in_memory(k, c.data); // (no file to keep it in)
		if (c.meshed_version || c.mesh_queued)
		{
			self.unloaded.push(p);
			self.revision_++;
		}
		long ck = key(p.x, 0, p.z);
		if (SharedTops** t = self.column_tops.get_ref(ck))
		{
			(*t).release();
			self.column_tops.remove(ck);
		}
		self.mesh_candidates.remove(k);
		self.urgent.remove(k);
		c.release();
		free(c);
		self.chunks.remove(k);
	}
	// Jobs for chunks that are gone are of no use any more.
	self.job_mutex.lock();
	for (sz i = 0; i < self.jobs.len();)
	{
		Job* j = self.jobs.get_ref(i);
		Chunk* c = self.chunk(key_of(j.pos));
		if (!c || c.serial != j.serial)
		{
			j.release();
			self.jobs.swap_remove(i);
		}
		else
		{
			i++;
		}
	}
	self.job_mutex.unlock();

	self.schedule_meshing(center, r);
}

fn void World.keep_in_memory(&self, long k, SharedChunk* data)
{
	if (SharedChunk** old = self.kept.get_ref(k))
	{
		if (*old == data) return;
		(*old).release();
	}
	self.kept[k] = data.retain();
}

fn void World.load_column_chunks(&self, ChunkPos center, int dx, int dz, float priority_bias)
{
	for (int cy = CHUNK_Y_MIN; cy <= CHUNK_Y_MAX; cy++)
	{
		ChunkPos p = { center.x + dx, cy, center.z + dz };
		long k = key_of(p);
		if (self.chunks.has(k)) continue;
		Chunk* c = mem::new(Chunk);
		c.serial = self.next_serial++;
		c.mesh_version = 1;
		self.chunks[k] = c;
		Job job = { .kind = GENERATE, .pos = p, .serial = c.serial,
			.priority = (float)(dx * dx + dz * dz) + (float)math::abs(cy) * 0.5f + priority_bias };
		if (try kept = self.kept.get(k))
		{
			job.kept = kept.retain();
			c.dirty = true; // (still differs from the generator's)
		}
		self.push_job(job);
	}
}

// The chunk at `p` (loaded, kept or saved), appended to `out` as the chunk
// file format; false if it was never edited and isn't loaded.
fn bool World.chunk_bytes(&self, ChunkPos p, Bytes* out)
{
	Chunk* c = self.chunk(key_of(p));
	if (c && c.data)
	{
		voxel::encode_chunk(&c.data.value, out);
		return true;
	}
	if (try kept = self.kept.get(key_of(p)))
	{
		voxel::encode_chunk(&kept.value, out);
		return true;
	}
	if (!self.save_dir.len) return false;
	ChunkData data;
	defer data.free();
	bool ok;
	@pool()
	{
		ok = voxel::load_chunk_file(voxel::chunk_file_path(tmem, self.save_dir, p.x, p.y, p.z), &data);
	};
	if (!ok) return false;
	voxel::encode_chunk(&data, out);
	return true;
}

// Puts a chunk from the host in place (loaded or for when it is); false if
// the data is bad (unknown block ids included).
fn bool World.set_chunk(&self, ChunkPos p, char[] bytes)
{
	if (p.y < CHUNK_Y_MIN || p.y > CHUNK_Y_MAX) return false;
	SharedChunk* data = shared::new{ChunkData}();
	if (!voxel::decode_chunk(bytes, &data.value))
	{
		data.release();
		return false;
	}
	bool bad = data.value.mixed ? false : data.value.uniform_value >= self.blocks.len;
	if (data.value.mixed)
	{
		foreach (b : data.value.blocks)
		{
			if (b >= self.blocks.len)
			{
				bad = true;
				break;
			}
		}
	}
	if (bad)
	{
		data.release();
		return false; // a block we don't know
	}
	long k = key_of(p);
	if (!self.save_dir.len) self.keep_in_memory(k, data);
	Chunk* c = self.chunk(k);
	if (!c || !c.data)
	{
		if (c)
		{
			// Being generated: forget that job's result, load it from here instead.
			c.release();
			free(c);
			self.chunks.remove(k);
		}
		if (self.save_dir.len)
		{
			@pool()
			{
				voxel::save_chunk_file(voxel::chunk_file_path(tmem, self.save_dir, p.x, p.y, p.z), &data.value);
			};
		}
		data.release();
		return true; // (loaded from kept or its file when it's needed)
	}
	c.data.release();
	c.data = data;
	c.dirty = true;
	self.revision_++;
	voxel::chunk_local_tops(&data.value, self.blocks, &c.local_tops);
	self.update_column_tops(p.x, p.z);
	for (int cy = CHUNK_Y_MIN; cy <= CHUNK_Y_MAX; cy++) // its light may reach the neighbours, and down the column
	{
		for (int dz = -1; dz <= 1; dz++)
		{
			for (int dx = -1; dx <= 1; dx++) self.request_remesh(p.x + dx, cy, p.z + dz);
		}
	}
	return true;
}

// Every edited chunk (loaded, kept in memory or saved): what differs from
// what the generator makes. (free() each one's bytes, and the list.)
fn void World.edited_chunks(&self, Vec{EditedChunk}* out)
{
	Map{long, bool} seen;
	defer seen.free();
	self.chunks.@each(; long k, Chunk* c)
	{
		if (c.data && c.dirty)
		{
			EditedChunk e = { .pos = pos_of(k) };
			voxel::encode_chunk(&c.data.value, &e.bytes);
			out.push(e);
			seen[k] = true;
		}
	};
	self.kept.@each(; long k, SharedChunk* d)
	{
		if (!seen.has(k))
		{
			EditedChunk e = { .pos = pos_of(k) };
			voxel::encode_chunk(&d.value, &e.bytes);
			out.push(e);
			seen[k] = true;
		}
	};
	if (self.save_dir.len)
	{
		Vec{int[3]} files;
		defer files.free();
		voxel::list_chunk_files(self.save_dir, &files);
		foreach (f : files)
		{
			ChunkPos p = { f[0], f[1], f[2] };
			if (seen.has(key_of(p))) continue;
			seen[key_of(p)] = true;
			EditedChunk e = { .pos = p };
			if (self.chunk_bytes(p, &e.bytes))
			{
				out.push(e);
			}
			else
			{
				e.bytes.free();
			}
		}
	}
}

fn void World.update_column_tops(&self, int cx, int cz)
{
	long ckey = key(cx, 0, cz);
	SharedTops* tops = shared::new{ColumnTops}();
	tops.value[..] = voxel::NO_TOP;
	for (int cy = CHUNK_Y_MAX; cy >= CHUNK_Y_MIN; cy--)
	{
		Chunk* c = self.chunk(key(cx, cy, cz));
		if (!c || !c.data)
		{
			tops.release();
			if (SharedTops** t = self.column_tops.get_ref(ckey)) // column incomplete: no sky information yet
			{
				(*t).release();
				self.column_tops.remove(ckey);
			}
			return;
		}
		for (int i = 0; i < CHUNK_AREA; i++)
		{
			if (tops.value[i] == voxel::NO_TOP && c.local_tops[i] >= 0) tops.value[i] = (short)(cy * CHUNK + c.local_tops[i]);
		}
	}
	if (SharedTops** t = self.column_tops.get_ref(ckey))
	{
		(*t).release();
		*t = tops;
		return;
	}
	self.column_tops[ckey] = tops;
}

fn void World.schedule_meshing(&self, ChunkPos center, int r)
{
	Vec{long} candidates;
	defer candidates.free();
	self.mesh_candidates.@each(; long k, bool v) { candidates.push(k); };
	foreach (k : candidates)
	{
		Chunk* c = self.chunk(k);
		if (!c || !c.data || c.mesh_queued || c.meshed_version == c.mesh_version)
		{
			self.mesh_candidates.remove(k);
			continue;
		}
		ChunkPos p = pos_of(k);
		int dx0 = p.x - center.x;
		int dz0 = p.z - center.z;
		bool urgent = self.urgent.has(k);
		if (!urgent && dx0 * dx0 + dz0 * dz0 > r * r) continue; // not in view yet; stays a candidate
		Job job = { .kind = MESH, .pos = p, .serial = c.serial, .version = c.mesh_version,
			.priority = (float)(dx0 * dx0 + dz0 * dz0) + (float)math::abs(p.y) * 0.5f - (urgent ? 1e6f : 0.0f) };
		bool ready = true;
		for (int dy = -1; dy <= 1 && ready; dy++)
		{
			for (int dz = -1; dz <= 1 && ready; dz++)
			{
				for (int dx = -1; dx <= 1 && ready; dx++)
				{
					int ny = p.y + dy;
					if (ny > CHUNK_Y_MAX || ny < CHUNK_Y_MIN) continue; // outside the world: air above, solid below
					Chunk* n = self.chunk(key(p.x + dx, ny, p.z + dz));
					if (!n || !n.data)
					{
						ready = false;
						break;
					}
					job.hood[voxel::neighbourhood_index(dx, dy, dz)] = n.data.retain();
				}
			}
		}
		for (int dz = -1; dz <= 1 && ready; dz++)
		{
			for (int dx = -1; dx <= 1 && ready; dx++)
			{
				SharedTops* t = self.column_tops.get_or(key(p.x + dx, 0, p.z + dz), null);
				if (!t)
				{
					ready = false; // a column isn't complete: sky light unknown
					break;
				}
				job.tops[(dz + 1) * 3 + (dx + 1)] = t.retain();
			}
		}
		if (!ready)
		{
			job.release(); // a neighbour arriving makes it a candidate again
			continue;
		}
		c.mesh_queued = true;
		self.urgent.remove(k);
		self.push_job(job);
		self.mesh_candidates.remove(k);
	}
}

// Meshes finished since the last call (the caller owns them: free() each, and
// the list), and chunks that went away.
fn Vec{ChunkMesh} World.take_meshes(&self)
{
	Vec{ChunkMesh} out = self.meshes;
	self.meshes = {};
	return out;
}

fn Vec{ChunkPos} World.take_unloaded(&self)
{
	Vec{ChunkPos} out = self.unloaded;
	self.unloaded = {};
	return out;
}

// --- Blocks and editing -------------------------------------------------------------

fn ChunkData* World.chunk_data_at(&self, int x, int y, int z, int* lx, int* ly, int* lz)
{
	int cx = floor_div(x, CHUNK);
	int cy = floor_div(y, CHUNK);
	int cz = floor_div(z, CHUNK);
	Chunk* c = self.chunk(key(cx, cy, cz));
	if (!c || !c.data) return null;
	*lx = x - cx * CHUNK;
	*ly = y - cy * CHUNK;
	*lz = z - cz * CHUNK;
	return &c.data.value;
}

// The block at a world position: AIR above and below the world, -1 where
// the chunk isn't loaded.
fn int World.block_id_at(&self, int x, int y, int z)
{
	int cy = floor_div(y, CHUNK);
	if (cy > CHUNK_Y_MAX || cy < CHUNK_Y_MIN) return voxel::AIR;
	int lx;
	int ly;
	int lz;
	ChunkData* d = self.chunk_data_at(x, y, z, &lx, &ly, &lz);
	return d ? d.get(lx, ly, lz) : -1;
}

// The same with AIR for unloaded chunks.
fn BlockId World.block_at(&self, int x, int y, int z)
{
	int id = self.block_id_at(x, y, z);
	return id < 0 ? voxel::AIR : (BlockId)id;
}

// A block's dye (NO_DYE: undyed, or not loaded).
fn Dye World.dye_at(&self, int x, int y, int z)
{
	int lx;
	int ly;
	int lz;
	ChunkData* d = self.chunk_data_at(x, y, z, &lx, &ly, &lz);
	return d ? d.dye(lx, ly, lz) : voxel::NO_DYE;
}

fn bool World.is_loaded(&self, int x, int y, int z)
{
	int lx;
	int ly;
	int lz;
	return self.chunk_data_at(x, y, z, &lx, &ly, &lz) != null;
}

// Sky and block light (0..15 each) at a block, as of its chunk's last mesh;
// open sky where it isn't known.
fn void World.light_at(&self, int x, int y, int z, int* sky, int* block)
{
	int cx = floor_div(x, CHUNK);
	int cy = floor_div(y, CHUNK);
	int cz = floor_div(z, CHUNK);
	Chunk* c = self.chunk(key(cx, cy, cz));
	if (!c || !c.light)
	{
		*sky = voxel::LIGHT_MAX; // not lit yet (or above the world): open sky
		*block = 0;
		return;
	}
	char v = c.light.value[((y - cy * CHUNK) * CHUNK + (z - cz * CHUNK)) * CHUNK + (x - cx * CHUNK)];
	*sky = v >> 4;
	*block = v & 15;
}

// Light held in hand (a torch: x, y, z, level 0..15) spread like placed
// light, one level less per block and stopped by opaque blocks, in the cube
// of `size` blocks from `origin`: a level per block, indexed
// (y * size + z) * size + x, into `vol` (resized).
fn void World.held_light(&self, BlockPos origin, int size, float[4][] lights, Vec{char}* vol)
{
	size = math::clamp(size, 1, 128);
	vol.clear();
	vol.resize((sz)size * size * size);
	Vec{int} queue;
	defer queue.free();
	foreach (l : lights)
	{
		int x = (int)math::floor(l[0]) - origin.x;
		int y = (int)math::floor(l[1]) - origin.y;
		int z = (int)math::floor(l[2]) - origin.z;
		int level = math::clamp((int)l[3], 0, voxel::LIGHT_MAX);
		if (x < 0 || y < 0 || z < 0 || x >= size || y >= size || z >= size) continue;
		int i = (y * size + z) * size + x;
		if (level <= (*vol)[i]) continue;
		(*vol)[i] = (char)level;
		queue.push(i);
	}
	int[3][6] dirs = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	for (sz q = 0; q < queue.len(); q++)
	{
		int i = queue[q];
		int x = i % size;
		int z = (i / size) % size;
		int y = i / (size * size);
		int next = (*vol)[i] - 1;
		if (next <= 0) continue;
		foreach (d : dirs)
		{
			int nx = x + d[0];
			int ny = y + d[1];
			int nz = z + d[2];
			if (nx < 0 || ny < 0 || nz < 0 || nx >= size || ny >= size || nz >= size) continue;
			int n = (ny * size + nz) * size + nx;
			if ((*vol)[n] >= next || !self.light_passes(origin, nx, ny, nz)) continue;
			(*vol)[n] = (char)next;
			queue.push(n);
		}
	}
}

// Whether light passes through the block at a volume position (unloaded: no;
// above the world: open air).
fn bool World.light_passes(&self, BlockPos origin, int x, int y, int z)
{
	int wy = origin.y + y;
	if (floor_div(wy, CHUNK) > CHUNK_Y_MAX) return true;
	int id = self.block_id_at(origin.x + x, wy, origin.z + z);
	return id >= 0 && (id >= self.blocks.len || !self.blocks[id].opaque);
}

// Changes a block (and its dye: a new block is undyed unless given one);
// false if its chunk isn't loaded.
fn bool World.set_block(&self, BlockPos p, BlockId id, Dye dye = voxel::NO_DYE)
{
	if (id >= self.blocks.len) return false;
	int cx = floor_div(p.x, CHUNK);
	int cy = floor_div(p.y, CHUNK);
	int cz = floor_div(p.z, CHUNK);
	Chunk* chunk = self.chunk(key(cx, cy, cz));
	if (!chunk || !chunk.data) return false;
	int lx = p.x - cx * CHUNK;
	int ly = p.y - cy * CHUNK;
	int lz = p.z - cz * CHUNK;
	if (id == voxel::AIR) dye = voxel::NO_DYE;
	if (chunk.data.value.get(lx, ly, lz) == id)
	{
		if (chunk.data.value.dye(lx, ly, lz) == dye) return true;
		// Only the dye: this chunk's mesh, nothing else (light doesn't change).
		SharedChunk* copy = shared::new{ChunkData}();
		copy_chunk(&copy.value, &chunk.data.value);
		copy.value.set_dye(lx, ly, lz, dye);
		chunk.data.release();
		chunk.data = copy;
		chunk.dirty = true;
		self.revision_++;
		self.request_remesh(cx, cy, cz);
		if (self.on_block_changed) self.on_block_changed(self.on_block_changed_context, p, id);
		return true;
	}
	// Copy-on-write: mesh jobs may still be reading the old snapshot.
	SharedChunk* copy = shared::new{ChunkData}();
	copy_chunk(&copy.value, &chunk.data.value);
	copy.value.set(lx, ly, lz, id);
	copy.value.set_dye(lx, ly, lz, dye);
	copy.value.compact();
	chunk.data.release();
	chunk.data = copy;
	chunk.dirty = true;
	self.revision_++;

	// Column top within this chunk, then for the whole column.
	int column = lz * CHUNK + lx;
	ichar local_top = -1;
	for (int y = CHUNK - 1; y >= 0; y--)
	{
		BlockId b = copy.value.get(lx, y, lz);
		if (b < self.blocks.len && self.blocks[b].opaque)
		{
			local_top = (ichar)y;
			break;
		}
	}
	long ckey = key(cx, 0, cz);
	SharedTops* t = self.column_tops.get_or(ckey, null);
	int old_top = t ? t.value[column] : voxel::NO_TOP;
	chunk.local_tops[column] = local_top;
	self.update_column_tops(cx, cz);
	t = self.column_tops.get_or(ckey, null);
	int new_top = t ? t.value[column] : voxel::NO_TOP;

	// Everything light can reach from here, plus everything below a column
	// whose sky exposure changed.
	int y_low = p.y;
	int y_high = p.y;
	if (old_top != new_top)
	{
		y_low = math::min(y_low, old_top, new_top);
		y_high = math::max(y_high, old_top, new_top);
		y_low = math::max(y_low, CHUNK_Y_MIN * CHUNK);
	}
	self.relight_around(p, y_low, y_high);
	if (self.on_block_changed) self.on_block_changed(self.on_block_changed_context, p, id);
	return true;
}

fn void World.relight_around(&self, BlockPos p, int y_low, int y_high)
{
	int ax = floor_div(p.x - voxel::LIGHT_MAX, CHUNK);
	int ay = floor_div(y_low - voxel::LIGHT_MAX, CHUNK);
	int az = floor_div(p.z - voxel::LIGHT_MAX, CHUNK);
	int bx = floor_div(p.x + voxel::LIGHT_MAX, CHUNK);
	int by = floor_div(y_high + voxel::LIGHT_MAX, CHUNK);
	int bz = floor_div(p.z + voxel::LIGHT_MAX, CHUNK);
	for (int cy = math::max(ay, CHUNK_Y_MIN); cy <= math::min(by, CHUNK_Y_MAX); cy++)
	{
		for (int cz = az; cz <= bz; cz++)
		{
			for (int cx = ax; cx <= bx; cx++) self.request_remesh(cx, cy, cz);
		}
	}
}

// Block shadows: each face is in the sun or in a block's shadow, worked out
// by the mesher from where the sun is; turning them on or off remeshes every
// chunk, nearest first.
fn void World.set_block_shadows(&self, bool enabled)
{
	int on = enabled ? 1 : 0;
	if (self.block_shadows.load() != on)
	{
		self.block_shadows.store(on);
		self.remesh_all();
	}
}

fn bool World.block_shadows_on(&self) => self.block_shadows.load() != 0;

// Smooth lighting (corner shading and soft light, the default); turning it
// on or off remeshes every chunk.
fn void World.set_smooth_lighting(&self, bool enabled)
{
	int on = enabled ? 1 : 0;
	if (self.smooth_lighting.load() != on)
	{
		self.smooth_lighting.store(on);
		self.remesh_all();
	}
}

// Where the sun is (radians above the eastern horizon); shadows follow it in
// SUN_STEP_DEGREES steps. At night the last step stays, for the morning.
fn void World.set_sun_angle(&self, float radians)
{
	float degrees = radians * 180.0f / 3.14159265f;
	if (!(degrees > 0.0f && degrees < 180.0f)) return; // night: keep the last shadows for the morning's first frames
	int last = 180 / SUN_STEP_DEGREES - 1;
	int step = math::clamp((int)math::round(degrees / SUN_STEP_DEGREES), 1, last);
	if (self.sun_step.load() != step)
	{
		self.sun_step.store(step);
		if (self.block_shadows.load()) self.remesh_all();
	}
}

// Every meshed chunk again (not urgent: nearest first); the old meshes stay
// until the new ones arrive.
fn void World.remesh_all(&self)
{
	self.chunks.@each(; long k, Chunk* c)
	{
		if (c.data && c.meshed_version > 0)
		{
			c.mesh_version++;
			c.mesh_queued = false;
			self.mesh_candidates[k] = true;
		}
	};
}

fn void World.request_remesh(&self, int cx, int cy, int cz)
{
	long k = key(cx, cy, cz);
	Chunk* c = self.chunk(k);
	if (!c || !c.data) return;
	c.mesh_version++;
	c.mesh_queued = false; // a running job's result is now stale
	self.mesh_candidates[k] = true;
	int dx = cx - self.center.x;
	int dz = cz - self.center.z;
	if (dx * dx + dz * dz <= (self.radius + 1) * (self.radius + 1)) self.urgent[k] = true; // (not chunks only loaded around other players)
}

fn bool World.save_chunk(&self, ChunkPos p, Chunk* c)
{
	if (!self.save_dir.len || !c.dirty || !c.data) return true;
	bool ok;
	@pool()
	{
		ok = voxel::save_chunk_file(voxel::chunk_file_path(tmem, self.save_dir, p.x, p.y, p.z), &c.data.value);
	};
	if (!ok)
	{
		log::log("Couldn't save chunk (%d, %d, %d) in %s", p.x, p.y, p.z, self.save_dir);
		return false;
	}
	c.dirty = false;
	return true;
}

// Writes every edited chunk to its file; returns how many.
fn int World.save_modified(&self)
{
	int saved = 0;
	self.chunks.@each(; long k, Chunk* c)
	{
		if (c.dirty && self.save_chunk(pos_of(k), c)) saved++;
	};
	return saved;
}

// --- Raycast and collision -------------------------------------------------------------

// The first block a ray meets (fluids are seen through), within max_distance.
fn RayHit World.raycast(&self, float[3] origin, float[3] direction, float max_distance)
{
	RayHit result;
	float len = math::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
	if (len < 1e-6f) return result;
	float[3] dir = { direction[0] / len, direction[1] / len, direction[2] / len };
	// Voxel traversal: step to whichever cell boundary the ray reaches first.
	int[3] cell = { (int)math::floor(origin[0]), (int)math::floor(origin[1]), (int)math::floor(origin[2]) };
	int[3] previous = cell;
	int[3] normal;
	int[3] step;
	float[3] t_max;
	float[3] t_delta;
	for (int a = 0; a < 3; a++)
	{
		float d = dir[a];
		step[a] = d > 0 ? 1 : (d < 0 ? -1 : 0);
		t_delta[a] = d != 0 ? math::abs(1.0f / d) : float::inf;
		float boundary = d > 0 ? math::floor(origin[a]) + 1.0f : math::floor(origin[a]);
		t_max[a] = d != 0 ? (boundary - origin[a]) / d : float::inf;
	}
	float t = 0.0f;
	BoxList boxes;
	defer boxes.free();
	while (t <= max_distance)
	{
		int id = self.block_id_at(cell[0], cell[1], cell[2]);
		if (id < 0) return result; // left the loaded world
		if (id != voxel::AIR && id < self.blocks.len && !self.blocks[id].fluid)
		{
			if (self.blocks[id].shape == CUBE)
			{
				return { true, { cell[0], cell[1], cell[2] }, { previous[0], previous[1], previous[2] },
					{ normal[0], normal[1], normal[2] }, (BlockId)id, t };
			}
			// Shaped block: the nearest of its boxes along the ray, if any.
			boxes.clear();
			self.block_boxes(cell[0], cell[1], cell[2], id, false, &boxes);
			float best = float::inf;
			int[3] best_normal;
			foreach (&bx : boxes)
			{
				float t0 = 0.0f;
				float t1 = max_distance;
				int axis = -1;
				bool miss = false;
				for (int k = 0; k < 3 && !miss; k++)
				{
					if (dir[k] == 0.0f)
					{
						miss = origin[k] < bx.mn[k] || origin[k] > bx.mx[k];
						continue;
					}
					float ta = (bx.mn[k] - origin[k]) / dir[k];
					float tb = (bx.mx[k] - origin[k]) / dir[k];
					if (ta > tb) @swap(ta, tb);
					if (ta > t0)
					{
						t0 = ta;
						axis = k;
					}
					t1 = math::min(t1, tb);
					miss = t0 > t1;
				}
				if (!miss && t0 < best)
				{
					best = t0;
					best_normal = { 0, 0, 0 };
					if (axis >= 0) best_normal[axis] = dir[axis] > 0 ? -1 : 1;
				}
			}
			if (best <= max_distance)
			{
				return { true, { cell[0], cell[1], cell[2] },
					{ cell[0] + best_normal[0], cell[1] + best_normal[1], cell[2] + best_normal[2] },
					{ best_normal[0], best_normal[1], best_normal[2] }, (BlockId)id, best };
			}
		}
		int a = (t_max[0] < t_max[1]) ? (t_max[0] < t_max[2] ? 0 : 2) : (t_max[1] < t_max[2] ? 1 : 2);
		previous = cell;
		cell[a] += step[a];
		t = t_max[a];
		t_max[a] += t_delta[a];
		normal = { 0, 0, 0 };
		normal[a] = -step[a];
	}
	return result;
}

fn bool World.fence_connects(&self, int x, int y, int z)
{
	int id = self.block_id_at(x, y, z);
	return id > 0 && id < self.blocks.len && (self.blocks[id].shape == FENCE || self.blocks[id].opaque);
}

// A block's boxes (world coordinates): collision boxes, or the ones it's
// drawn and picked with. (id: as block_id_at gives it.)
fn void World.block_boxes(&self, int x, int y, int z, int id, bool collision, BoxList* out)
{
	sz first = out.len();
	if (id < 0)
	{
		out.push({ .mn = { 0, 0, 0 }, .mx = { 1, 1, 1 } }); // unloaded: solid
	}
	else if (id == voxel::AIR || id >= self.blocks.len)
	{
		return;
	}
	else
	{
		BlockInfo* b = &self.blocks[id];
		if (b.shape == FENCE)
		{
			voxel::fence_boxes(self.fence_connects(x, y, z - 1), self.fence_connects(x + 1, y, z), self.fence_connects(x, y, z + 1),
				self.fence_connects(x - 1, y, z), collision, out);
		}
		else if (collision)
		{
			out.push_all(b.collision.array_view());
		}
		else if (b.fluid)
		{
			return;
		}
		else if (b.shape == BOXES && !b.model.is_empty())
		{
			out.push_all(b.model.array_view());
		}
		else if (b.shape == CROSS)
		{
			out.push({ .mn = { 0.15f, 0.0f, 0.15f }, .mx = { 0.85f, 0.8f, 0.85f } });
		}
		else
		{
			out.push({ .mn = { 0, 0, 0 }, .mx = { 1, 1, 1 } });
		}
	}
	float[3] o = { (float)x, (float)y, (float)z };
	for (sz i = first; i < out.len(); i++)
	{
		Box* b = out.get_ref(i);
		for (int k = 0; k < 3; k++)
		{
			b.mn[k] += o[k];
			b.mx[k] += o[k];
		}
	}
}

const float EPS @private = 1e-3f;

// Collision boxes of every block that could touch [mn, mx] (one cell lower
// too: fences reach 1.5 blocks up).
fn void World.collect_collision(&self, float[3] mn, float[3] mx, BoxList* out)
{
	int x0 = (int)math::floor(mn[0] + EPS);
	int x1 = (int)math::floor(mx[0] - EPS);
	int y0 = (int)math::floor(mn[1] + EPS) - 1;
	int y1 = (int)math::floor(mx[1] - EPS);
	int z0 = (int)math::floor(mn[2] + EPS);
	int z1 = (int)math::floor(mx[2] - EPS);
	for (int y = y0; y <= y1; y++)
	{
		for (int z = z0; z <= z1; z++)
		{
			for (int x = x0; x <= x1; x++)
			{
				sz first = out.len();
				// Unloaded chunks are solid, so nothing falls out of the loaded
				// world; above and below the world it's open.
				self.block_boxes(x, y, z, self.block_id_at(x, y, z), true, out);
				// Keep only boxes that reach into the region.
				for (sz i = out.len(); i-- > first;)
				{
					Box* b = out.get_ref(i);
					bool touches = true;
					for (int k = 0; k < 3; k++) touches = touches && b.mx[k] > mn[k] + EPS && b.mn[k] < mx[k] - EPS;
					if (!touches) out.remove_at(i);
				}
			}
		}
	}
}

fn void World.simulate_move(&self, BoxList* boxes, float[3] size, float[3] start, float[3] m, bool* on_floor, bool* ceiling,
	bool* wall, float[3]* out)
{
	float[3] p = start;
	// Everything within reach of this whole move.
	float[3] rmn = { p[0] - size[0] * 0.5f, p[1], p[2] - size[2] * 0.5f };
	float[3] rmx = { p[0] + size[0] * 0.5f, p[1] + size[1], p[2] + size[2] * 0.5f };
	for (int k = 0; k < 3; k++)
	{
		rmn[k] += math::min(0.0f, m[k]) - 1.0f;
		rmx[k] += math::max(0.0f, m[k]) + 1.0f;
	}
	boxes.clear();
	self.collect_collision(rmn, rmx, boxes);
	foreach (a : (int[3]){ 1, 0, 2 })
	{
		float[3] mn = { p[0] - size[0] * 0.5f, p[1], p[2] - size[2] * 0.5f };
		float[3] mx = { p[0] + size[0] * 0.5f, p[1] + size[1], p[2] + size[2] * 0.5f };
		int u = (a + 1) % 3;
		int v = (a + 2) % 3;
		float allowed = m[a];
		foreach (&b : *boxes)
		{
			if (b.mx[u] <= mn[u] + EPS || b.mn[u] >= mx[u] - EPS || b.mx[v] <= mn[v] + EPS || b.mn[v] >= mx[v] - EPS)
			{
				continue; // not in the way on the other two axes
			}
			if (allowed > 0.0f && b.mn[a] >= mx[a] - EPS)
			{
				allowed = math::min(allowed, math::max(0.0f, b.mn[a] - mx[a] - EPS));
			}
			else if (allowed < 0.0f && b.mx[a] <= mn[a] + EPS)
			{
				allowed = math::max(allowed, math::min(0.0f, b.mx[a] - mn[a] + EPS));
			}
		}
		if (allowed != m[a])
		{
			if (a == 1)
			{
				if (m[a] < 0)
				{
					*on_floor = true;
				}
				else
				{
					*ceiling = true;
				}
			}
			else
			{
				*wall = true;
			}
		}
		p[a] += allowed;
	}
	*out = p;
}

// Moves a box (feet at `position`, `size` wide/tall/deep, centred on x and z)
// by `motion`, stopped by blocks' collision boxes; unloaded chunks are solid.
// step_height > 0 lets it walk up that much.
fn MoveResult World.move_box(&self, float[3] position, float[3] size, float[3] motion, float step_height = 0)
{
	// Moves y, then x, then z, each clipped against the blocks' collision boxes.
	BoxList boxes;
	defer boxes.free();
	MoveResult r;
	self.simulate_move(&boxes, size, position, motion, &r.on_floor, &r.hit_ceiling, &r.hit_wall, &r.position);

	// Step up: if walking on the ground into something, try going over it.
	if (step_height > 0.0f && r.hit_wall)
	{
		bool f2;
		bool c2;
		bool w2;
		float[3] below = { position[0], position[1] - 0.05f, position[2] };
		if (!self.is_box_free(below, size) || r.on_floor)
		{
			float[3] up;
			float[3] across;
			float[3] down;
			self.simulate_move(&boxes, size, position, { 0, step_height, 0 }, &f2, &c2, &w2, &up);
			self.simulate_move(&boxes, size, up, { motion[0], 0, motion[2] }, &f2, &c2, &w2, &across);
			bool f3;
			bool c3;
			bool w3;
			self.simulate_move(&boxes, size, across, { 0, -step_height + math::min(0.0f, motion[1]), 0 }, &f3, &c3, &w3, &down);
			float flat = math::hypot(r.position[0] - position[0], r.position[2] - position[2]);
			float stepped = math::hypot(down[0] - position[0], down[2] - position[2]);
			if (stepped > flat + 1e-3f)
			{
				r.position = down;
				r.on_floor = f3;
				r.hit_wall = w2;
			}
		}
	}
	return r;
}

fn bool World.is_box_free(&self, float[3] position, float[3] size)
{
	float[3] mn = { position[0] - size[0] * 0.5f, position[1], position[2] - size[2] * 0.5f };
	float[3] mx = { position[0] + size[0] * 0.5f, position[1] + size[1], position[2] + size[2] * 0.5f };
	BoxList boxes;
	defer boxes.free();
	self.collect_collision(mn, mx, &boxes); // only boxes overlapping the region are kept
	return boxes.is_empty();
}

fn WorldStats World.stats(&self)
{
	WorldStats s;
	self.chunks.@each(; long k, Chunk* c)
	{
		s.loaded += c.data ? 1 : 0;
		s.meshed += c.meshed_version ? 1 : 0;
		s.dirty += c.dirty ? 1 : 0;
	};
	self.job_mutex.lock();
	s.queued = (int)self.jobs.len() + self.running;
	self.job_mutex.unlock();
	s.settled = s.queued == 0 && self.urgent.is_empty();
	s.workers = (int)self.workers.len();
	s.generated = self.generated.load();
	s.meshed_jobs = self.meshed_jobs.load();
	s.generate_ms = (double)self.generate_us.load() / 1000.0;
	s.light_ms = (double)self.light_us.load() / 1000.0;
	s.mesh_ms = (double)self.mesh_us.load() / 1000.0;
	for (int dz = -self.radius; dz <= self.radius && s.settled; dz++)
	{
		for (int dx = -self.radius; dx <= self.radius && s.settled; dx++)
		{
			if (dx * dx + dz * dz > self.radius * self.radius) continue;
			for (int cy = CHUNK_Y_MIN; cy <= CHUNK_Y_MAX; cy++)
			{
				Chunk* c = self.chunk(key(self.center.x + dx, cy, self.center.z + dz));
				if (!c || c.meshed_version != c.mesh_version)
				{
					s.settled = false;
					break;
				}
			}
		}
	}
	return s;
}
