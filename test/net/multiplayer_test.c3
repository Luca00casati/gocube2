// Multiplayer: a host and a client in one process, over TCP and UDP on
// localhost. The client is driven as a person would (aim, click); the host
// checks what it asks and does it as that player. Both worlds must agree
// after.
module gocube::test::multiplayer_test;
import gocube::test::netplay, gocube::test::play, gocube::net::session, gocube::game, gocube::game::inventory, gocube::game::block_entities;
import gocube::content, gocube::data, gocube::world, gocube::voxel, gocube::win, gocube::util::vec;
import std::math, std::io;

fn int test_port(int n) => 26000 + (int)(win::get_current_process_id() % 1000) * 8 + n;

// What the conditions look at.
struct Sides
{
	NetPlay* h;
	NetPlay* c;
	BlockPos p;
	String id;
	int n;
}

fn NetPlay*[2] both_sides(Sides* s) => { s.h, s.c };

fn bool both(Sides* s) => s.h.play.block_at(s.p) == s.id && s.c.play.block_at(s.p) == s.id;
fn bool both_cond(void* x) => both((Sides*)x);

fn void host_frame(void* h) => ((NetPlay*)h).frame();

fn void joining_and_block_changes_creative() @test
{
	NetPlay* host = netplay::host(test_port(0), true);
	defer host.free();
	BlockPos before = { 5, 100, 5 };
	BlockPos after = { 6, 100, 5 };
	host.world().set_block(before, host.play.id("core:lamp"));
	NetPlay* client = netplay::join(test_port(0), "Client", &host_frame, host);
	assert(client);
	Sides s = { host, client, before, "core:lamp", 0 };
	NetPlay*[2] sides = both_sides(&s);
	assert(client.game().creative);
	assert(netplay::run_until(&sides, fn bool(void* x) => ((Sides*)x).c.world().is_loaded(5, 100, 5), &s, 20));
	assert(client.play.block_at(before) == "core:lamp"); // an edit made before we joined
	host.world().set_block(after, host.play.id("core:sandstone"));
	s.p = after;
	s.id = "core:sandstone";
	assert(netplay::run_until(&sides, &both_cond, &s, 5)); // and after
	// Each sees the other, with its name.
	assert(netplay::run_until(&sides, fn bool(void* x)
	{
		Sides* s = x;
		return s.c.session.avatars.len() == 1 && s.h.session.avatars.len() == 1;
	}, &s, 5));
	assert(client.session.avatars[0].name == "Host");
	assert(host.session.avatars[0].name == "Client");
	assert(client.session.player_count() == 2);
	// Positions go by UDP (checked both ways first).
	assert(netplay::run_until(&sides, fn bool(void* x) => ((Sides*)x).c.session.udp_ready() && ((Sides*)x).h.session.udp_ready(), &s, 5));
	// We place a block: the host does it, both worlds have it.
	BlockPos cell = { 4, 100, 1 };
	client.take_from_palette("core:planks", 0);
	client.input.number_key = 1;
	client.frame();
	client.aim_at_floor(cell.x, cell.z);
	client.input.use_pressed = true;
	s.p = cell;
	s.id = "core:planks";
	assert(netplay::run_until(&sides, &both_cond, &s, 5));
	// And break it.
	client.aim(cell.x + 0.5f, cell.y + 0.5f, cell.z + 0.5f);
	client.input.break_pressed = true;
	s.id = "core:air";
	assert(netplay::run_until(&sides, &both_cond, &s, 5));
	// Out of reach, nothing happens.
	host.world().set_block({ 14, 100, -14 }, host.play.id("core:dirt")); // (20 blocks from us)
	client.session.request_break({ 14, 100, -14 }, 0);
	netplay::run_for(&sides, 0.5f);
	assert(host.play.block(14, 100, -14) == "core:dirt");
	// When the client leaves, the host sees it go.
	client.free();
	NetPlay*[1] alone = { host };
	assert(netplay::run_until(&alone, fn bool(void* x) => ((NetPlay*)x).session.avatars.is_empty(), host, 5));
}

fn void udp_blocked_everything_over_tcp() @test
{
	NetPlay* host = netplay::host(test_port(6), true);
	defer host.free();
	NetPlay* client = netplay::join(test_port(6), "Client", &host_frame, host, null, false);
	assert(client);
	defer client.free();
	Sides s = { host, client, { 4, 100, 1 }, "core:planks", 0 };
	NetPlay*[2] sides = both_sides(&s);
	assert(netplay::run_until(&sides, fn bool(void* x) => ((Sides*)x).c.world().is_loaded(4, 100, 1), &s, 20));
	// Each sees the other move.
	assert(netplay::run_until(&sides, fn bool(void* x) => ((Sides*)x).c.session.avatars.len() == 1 && ((Sides*)x).h.session.avatars.len() == 1, &s, 5));
	host.player().position[0] = 3.5f;
	assert(netplay::run_until(&sides, fn bool(void* x) => math::abs(((Sides*)x).c.session.avatars[0].eye[0] - 3.5f) < 0.01f, &s, 5));
	assert(!client.session.udp_ready() && !host.session.udp_ready());
	// A change goes through the host.
	client.take_from_palette("core:planks", 0);
	client.input.number_key = 1;
	client.frame();
	client.aim_at_floor(4, 1);
	client.input.use_pressed = true;
	assert(netplay::run_until(&sides, &both_cond, &s, 5));
}

fn int count_of(NetPlay* n, String item) => n.game().inventory.count_of(item);

fn void survival_client_through_host() @test
{
	NetPlay* host = netplay::host(test_port(1));
	defer host.free();
	// A survival inventory as the host keeps it for a player who played before.
	Inventory kept = inventory::new(&host.play.content);
	kept.slots[0] = inventory::stack("core:planks", 4);
	kept.slots[1] = inventory::stack("core:glass", 1);
	kept.slots[2] = inventory::stack("core:sand", 3);
	Data entry = data::object();
	entry.set("inventory", kept.serialize());
	Data players = data::object();
	defer players.free();
	players.set("Client", entry);
	host.session.set_saved_players(&players);
	NetPlay* client = netplay::join(test_port(1), "Client", &host_frame, host);
	assert(client);
	defer client.free();
	Sides s = { host, client, { 4, 100, 1 }, "core:planks", 0 };
	NetPlay*[2] sides = both_sides(&s);
	assert(netplay::run_until(&sides, fn bool(void* x) => count_of(((Sides*)x).c, "core:planks") == 4 && count_of(((Sides*)x).c, "core:sand") == 3, &s, 5));
	assert(netplay::run_until(&sides, fn bool(void* x) => ((Sides*)x).c.world().is_loaded(4, 100, 1), &s, 20));
	// Placing takes the item from the host's copy of our inventory.
	assert(client.hold("core:planks"));
	client.frame();
	client.aim_at_floor(4, 1);
	client.input.use_pressed = true;
	assert(netplay::run_until(&sides, fn bool(void* x) => both((Sides*)x) && count_of(((Sides*)x).c, "core:planks") == 3, &s, 5));
	// Breaking at once, without mining, is refused.
	client.session.request_break(s.p, client.game().selected);
	netplay::run_for(&sides, 0.5f);
	assert(both(&s));
	// Mining it: the break waits for the mining time; the host drops it, it
	// comes to us, into its copy of our inventory.
	client.aim(4.5f, 100.5f, 1.5f);
	client.input.break_held = true;
	s.id = "core:air";
	assert(netplay::run_until(&sides, &both_cond, &s, 10));
	client.input.break_held = false;
	// (Where it lands is random: we step onto the cell, as a player would.)
	client.player().position = { 4.5f, netplay::FLOOR_Y, 1.5f };
	assert(netplay::run_until(&sides, fn bool(void* x) => count_of(((Sides*)x).c, "core:planks") == 4 && ((Sides*)x).h.game().drops.drops.is_empty(), &s, 5));
	// A teleport is put back.
	float start = client.player().position[0];
	client.player().position[0] += 200;
	s.n = (int)start;
	assert(netplay::run_until(&sides, fn bool(void* x) => math::abs(((Sides*)x).c.player().position[0] - (float)((Sides*)x).n) < 2.0f, &s, 5));
	// Health is the host's: lava doesn't burn; a command's damage, the host says so.
	BlockPos feet = { (int)math::floor(client.player().position[0]), 100, (int)math::floor(client.player().position[2]) };
	host.world().set_block(feet, host.play.id("core:lava"));
	netplay::run_for(&sides, 1.5f);
	assert(client.game().health == game::MAX_HEALTH);
	host.world().set_block(feet, 0);
	assert(host.session.kill(host.session.peers[0].id));
	assert(netplay::run_until(&sides, fn bool(void* x) => ((Sides*)x).c.game().health < game::MAX_HEALTH, &s, 5));
	// The host keeps what we have when we leave.
	Data saved = host.session.saved_players();
	defer saved.free();
	Inventory back = inventory::new(&host.play.content);
	back.deserialize(saved.get("Client").get("inventory"));
	assert(back.count_of("core:planks") == 4); // (placed one, mined it back)
}

fn void chat_and_commands_through_host() @test
{
	NetPlay* host = netplay::host(test_port(3));
	defer host.free();
	NetPlay* client = netplay::join(test_port(3), "Client", &host_frame, host);
	assert(client);
	defer client.free();
	Sides s = { host, client, {}, "", 0 };
	NetPlay*[2] sides = both_sides(&s);
	client.session.chat("hello all");
	assert(netplay::run_until(&sides, fn bool(void* x)
	{
		Sides* s = x;
		foreach (l : s.h.session.chat_in) if (l == "<Client> hello all") return true;
		return false;
	}, &s, 5));
	// Not an operator: commands that change the world are refused, to it only.
	client.session.chat("/time set noon");
	assert(netplay::run_until(&sides, fn bool(void* x)
	{
		Sides* s = x;
		foreach (l : s.c.session.chat_in) if (l.contains("Only operators")) return true;
		return false;
	}, &s, 5));
	Vec{String} lines = host.session.take_chat();
	foreach (l : lines) free(l.ptr);
	lines.free();
	lines = client.session.take_chat();
	foreach (l : lines) free(l.ptr);
	lines.free();
	// The host makes it one.
	host.session.chat("/op Client");
	client.session.chat("/time set noon");
	assert(netplay::run_until(&sides, fn bool(void* x) => math::abs(((Sides*)x).h.day.time - 0.5) < 0.01, &s, 5));
}

fn void turned_away_name_taken() @test
{
	NetPlay* host = netplay::host(test_port(4));
	defer host.free();
	DString why;
	defer why.free();
	why.init(mem);
	NetPlay* client = netplay::join(test_port(4), "Host", &host_frame, host, &why);
	assert(!client);
	assert(why.str_view().contains("already playing"), "%s", why.str_view());
}
