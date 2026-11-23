// The base game loaded and a world (seed 4242) around the origin, played
// without a window: for the tests.
module gocube::test::play;
import gocube::content, gocube::game, gocube::game::blocks, gocube::game::player, gocube::world, gocube::voxel, gocube::clock, gocube::util::vec;
import std::io, std::math;

// Where the game's content is, from where the tests run (c3/).
const String CONTENT_DIR = "../content";

struct Setup
{
	bool creative;
	uint seed;
	float x, z; // loaded around here
	int view;
	String[] block_ids; // (a client: the host's)
}

fn Setup setup(bool creative = false) => { .creative = creative, .seed = 4242, .x = 0.5f, .z = 0.5f, .view = 32 };

struct Play
{
	Content content;
	BlockTable table;
	BlockRules rules;
	Generator generator;
	World world;
	Player player;
	Game game;
}

// A new one (on the heap: everything in it points at the rest). free() it.
fn Play* new(Setup s = setup())
{
	Play* p = mem::new(Play);
	DString why;
	defer why.free();
	assert(p.content.load(CONTENT_DIR, &why), "%s", why.str_view());
	p.content.freeze(s.block_ids);
	p.table = p.content.build_block_table();
	p.rules.init(&p.content);
	p.content.configure_generator(&p.generator, s.seed);
	p.world.init(p.table.blocks.array_view(), &p.table.atlas.layout, &p.generator);
	p.world.set_view_distance(s.view);
	p.wait_loaded(s.x, s.z);
	p.game.init(&p.content, &p.table, &p.rules, &p.world, &p.player, s.creative);
	p.game.set_random_seed(s.seed); // (what happens by chance is the same every run)
	p.game.generator = &p.generator;
	p.game.start_new_player();
	return p;
}

fn void Play.free(&self)
{
	self.game.free();
	self.world.free();
	self.generator.free();
	self.rules.free();
	self.table.free();
	self.content.free();
	free(self);
}

// Until the chunks around (x, z) are loaded.
fn void Play.wait_loaded(&self, float x, float z)
{
	double deadline = clock::now() + 60;
	while (!(self.world.is_loaded((int)math::floor(x), 0, (int)math::floor(z)) && self.world.stats().queued == 0))
	{
		self.world.update(x, 10.0f, z);
		self.drain_meshes();
		assert(clock::now() < deadline, "the world didn't load");
		clock::sleep_ms(5);
	}
}

// (Meshes no one draws.)
fn void Play.drain_meshes(&self)
{
	Vec{ChunkMesh} meshes = self.world.take_meshes();
	foreach (&m : meshes) m.free();
	meshes.free();
	Vec{ChunkPos} gone = self.world.take_unloaded();
	gone.free();
}

fn BlockId Play.id(&self, String name) => (BlockId)self.content.block_index(name);
fn String Play.block(&self, int x, int y, int z) => self.content.block_ids()[self.world.block_at(x, y, z)];
fn String Play.block_at(&self, BlockPos p) => self.block(p.x, p.y, p.z);

fn void Play.set(&self, int x, int y, int z, String name)
{
	assert(self.world.set_block({ x, y, z }, self.id(name)), "set_block (%d, %d, %d) %s", x, y, z, name);
}

// Lives `seconds`: physics steps (standing still) and their health.
fn void Play.live(&self, float seconds)
{
	PlayerInput none;
	for (int i = 0; i < (int)(seconds * 60); i++)
	{
		if (!self.game.frozen())
		{
			self.player.step(none, &self.world, &self.rules);
			self.game.after_move(player::STEP);
		}
	}
}

// A pool of `fluid` (or air) 3 deep around x 0..2, z 0..2 at y 40..42, on stone.
fn void Play.pool(&self, String fluid)
{
	for (int x = -1; x <= 3; x++)
	{
		for (int z = -1; z <= 3; z++)
		{
			for (int y = 39; y <= 46; y++)
			{
				bool wall = x < 0 || x > 2 || z < 0 || z > 2 || y == 39;
				self.set(x, y, z, wall ? "core:stone" : (y <= 42 ? fluid : "core:air"));
			}
		}
	}
}
