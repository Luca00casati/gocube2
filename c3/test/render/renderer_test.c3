// The Direct3D 11 bindings on the software adapter (WARP): a hidden window,
// a frame cleared to a colour, the screenshot reads it back.
module gocube::test::renderer_test;
import gocube::renderer, gocube::window, gocube::content::image, gocube::chunk_renderer, gocube::test::play, gocube::world, gocube::clock;
import gocube::game::creature_models, gocube::util::vec, gocube::mat, gocube::gui, gocube::gui_d3d, std::io, std::io::file;

fn void clears_presents_and_screenshots() @test
{
	Window w;
	defer w.free();
	assert(w.create("GoCube test", 64, 48, false));
	Renderer r;
	defer r.free();
	assert(r.init(w.hwnd, w.width, w.height, { .warp = true, .vsync = false }));
	String path = "../build/c3/renderer_test.png";
	r.begin_frame({ 1.0f, 0.0f, 0.0f, 1.0f });
	assert(r.end_frame(null, null, path));
	Image img;
	defer img.free();
	assert(image::read_png(path, &img));
	assert(img.width == w.width && img.height == w.height);
	assert(img.rgba[0] == 255 && img.rgba[1] == 0 && img.rgba[2] == 0, "got %d %d %d", img.rgba[0], img.rgba[1], img.rgba[2]);
	// Smaller world target, stretched: the same colour.
	r.set_render_scale(0.5f);
	r.begin_frame({ 0.0f, 0.0f, 1.0f, 1.0f });
	assert(r.end_frame(null, null, path));
	Image img2;
	defer img2.free();
	assert(image::read_png(path, &img2));
	assert(img2.rgba[2] == 255 && img2.rgba[0] == 0);
	(void)file::delete(path);
}

// A generated world drawn from above (terrain, sky, a creature, an outline):
// the ground isn't the clear colour, the sky above the horizon is the sky's.
fn void draws_the_world() @test
{
	Play* p = play::new();
	defer p.free();
	Window w;
	defer w.free();
	assert(w.create("GoCube test", 480, 320, false));
	Renderer r;
	defer r.free();
	assert(r.init(w.hwnd, w.width, w.height, { .warp = true }));
	ChunkRenderer c;
	defer c.free();
	assert(c.init(r.device, r.context, &p.table.atlas));
	// Every chunk meshed again, the meshes uploaded.
	p.world.remesh_all();
	double deadline = clock::now() + 60;
	int uploaded = 0;
	double last = clock::now();
	while (clock::now() < deadline && clock::now() - last < 0.5)
	{
		p.world.update(0.5f, 10.0f, 0.5f);
		Vec{ChunkMesh} meshes = p.world.take_meshes();
		foreach (&m : meshes)
		{
			c.upload(m);
			m.free();
			uploaded++;
			last = clock::now();
		}
		meshes.free();
		clock::sleep_ms(5);
	}
	assert(uploaded > 0);
	int ground = p.generator.height_at(0, 0);
	Camera cam = chunk_renderer::camera();
	cam.position = { 0.5f, (float)ground + 20.0f, 0.5f };
	cam.pitch = -0.5f; // looking down a little: ground below, sky above
	SkyParams sky = chunk_renderer::sky_params();
	r.begin_frame({ 1.0f, 0.0f, 1.0f, 1.0f }); // magenta: what nothing drew
	CreatureVertices creature;
	defer creature.free();
	creature_models::build_cube({ 0.5f, (float)ground + 18.0f, -3.0f }, 1.0f, { 1, 0, 0 }, &creature);
	c.draw(&cam, (float)r.width / (float)r.height, &sky, fn void(void* x)
	{
		Object* o = x;
		o.c.draw_sky(o.cam, o.aspect, o.sky);
	}, &&(Object){ &c, &cam, (float)r.width / (float)r.height, &sky });
	Matrix vp = cam.view() * cam.projection((float)r.width / (float)r.height);
	c.draw_creatures(creature.array_view(), vp, cam.position, &sky);
	String path = "../build/c3/world_test.png";
	assert(r.end_frame(null, null, path));
	assert(c.stats.drawn > 0 && c.stats.triangles > 0);
	Image img;
	defer img.free();
	assert(image::read_png(path, &img));
	int magenta = 0;
	for (sz i = 0; i < img.rgba.len; i += 4) magenta += img.rgba[i] == 255 && img.rgba[i + 1] == 0 && img.rgba[i + 2] == 255 ? 1 : 0;
	assert(magenta == 0, "%d pixels nothing drew", magenta);
	char* top = &img.rgba[((sz)2 * img.width + img.width / 2) * 4];       // sky
	char* bottom = &img.rgba[((sz)(img.height - 3) * img.width + img.width / 2) * 4]; // ground
	assert(top[2] > top[0], "the sky is blue: %d %d %d", top[0], top[1], top[2]);
	assert(top[0] != bottom[0] || top[1] != bottom[1] || top[2] != bottom[2], "the ground isn't the sky");
	// (Left in build/c3 to look at.)
}

struct Object
{
	ChunkRenderer* c;
	Camera* cam;
	float aspect;
	SkyParams* sky;
}

// The UI over a cleared frame: a window, text (GDI glyphs), a button, an
// item icon. (Left in build/c3/ui_test.png to look at.)
fn void draws_the_ui() @test
{
	Play* p = play::new();
	defer p.free();
	Window w;
	defer w.free();
	assert(w.create("GoCube test", 480, 320, false));
	Renderer r;
	defer r.free();
	assert(r.init(w.hwnd, w.width, w.height, { .warp = true }));
	GuiD3D d;
	defer d.free();
	assert(d.init(r.device, r.context));
	d.set_icons(&p.table.atlas);
	Gui g;
	defer g.free();
	g.init(&gui_d3d::measure, &d);
	DrawCmds cmds;
	defer
	{
		gui::free_cmds(&cmds);
		cmds.free();
	}
	for (int frame = 0; frame < 2; frame++) // (a window that fits its content shows from its second frame)
	{
		Input in = gui::input();
		in.width = (float)r.width;
		in.height = (float)r.height;
		g.begin_frame(in);
		g.begin_window("menu", { .pos = { 240, 160 }, .pivot = { 0.5f, 0.5f } });
		g.text("GoCube — in C3", 1.6f);
		g.text_dim("Wrapped and dim text under the title");
		g.button("Singleplayer", { 220, 0 });
		g.draw_list().image({ g.cursor().x, g.cursor().y, g.cursor().x + 32, g.cursor().y + 32 }, p.table.icons.get_or("core:grass", 0));
		g.dummy({ 32, 32 });
		g.end_window();
		gui::free_cmds(&cmds);
		g.end_frame(&cmds);
	}
	r.begin_frame({ 0.05f, 0.1f, 0.2f, 1.0f });
	r.end_frame(fn void(void* x)
	{
		UiDraw* u = x;
		u.d.draw(u.width, u.height, u.cmds.array_view());
	}, &&(UiDraw){ &d, r.width, r.height, &cmds }, "../build/c3/ui_test.png");
	assert(cmds.len() > 5);
}

struct UiDraw
{
	GuiD3D* d;
	int width, height;
	DrawCmds* cmds;
}
