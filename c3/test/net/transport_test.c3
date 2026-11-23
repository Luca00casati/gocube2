// The transport under multiplayer, on localhost: TCP for what must arrive,
// UDP for what may be lost once it's known to work, TCP for that too when it
// doesn't; closing after the last message; refusals.
module gocube::test::transport_test;
import gocube::net::transport, gocube::util::vec, gocube::clock, gocube::win;
import std::io;

alias Message = Vec{char};
alias Messages = Vec{Message};

fn int transport_port(int n) => 46000 + (int)(win::get_current_process_id() % 1000) * 8 + n;

// One end: what it has had.
struct End
{
	Transport net;
	Connection* other; // (CONNECT's)
	Messages got;
	int connects, disconnects;
}

fn End* new_end()
{
	End* e = mem::new(End);
	e.net = transport::new();
	return e;
}

fn void End.free(&self)
{
	foreach (&m : self.got) m.free();
	self.got.free();
	self.net.free();
	free(self);
}

fn void End.poll(&self)
{
	Event e;
	while (self.net.poll(&e))
	{
		switch (e.type)
		{
			case CONNECT:
				self.connects++;
				self.other = e.connection;
				e.data.free();
			case RECEIVE:
				self.got.push(e.data);
			case DISCONNECT:
				self.disconnects++;
				if (e.connection == self.other) self.other = null;
				e.data.free();
		}
	}
	self.net.flush();
}

alias DoneFn = fn bool(End*[] ends);

fn bool run_until(End*[] ends, DoneFn done, double seconds)
{
	double until = clock::now() + seconds;
	while (clock::now() < until)
	{
		foreach (e : ends) e.poll();
		if (done(ends)) return true;
		clock::sleep_ms(1);
	}
	return false;
}

fn char[4] number(uint n) => { (char)n, (char)(n >> 8), (char)(n >> 16), (char)(n >> 24) };

fn uint number_of(Vec{char}* b) => b.len() == 4 ? (uint)(b.items[0] | (uint)b.items[1] << 8 | (uint)b.items[2] << 16 | (uint)b.items[3] << 24) : 0xFFFFFFFF;

fn void connect(End* host, End* client, int port)
{
	assert(host.net.listen(port, 4) == "");
	assert(client.net.connect("127.0.0.1", port) == "");
	assert(run_until({ host, client }, fn bool(End*[] e) => e[0].other && e[1].other, 5));
}

fn void messages_whole_in_order_udp() @test
{
	End* host = new_end();
	End* client = new_end();
	defer
	{
		host.free();
		client.free();
	}
	connect(host, client, transport_port(0));
	// Reliable, both ways, in order; a big one whole.
	Vec{char} big;
	defer big.free();
	big.resize(5 << 20);
	for (sz i = 0; i < big.len(); i++) big[i] = (char)(i * 7 + i / 4096);
	for (uint i = 0; i < 300; i++)
	{
		client.net.send(client.other, &&number(i), true);
		if (i == 150) client.net.send(client.other, big.array_view(), true);
	}
	host.net.send(host.other, &&number(42), true);
	assert(run_until({ host, client }, fn bool(End*[] e) => e[0].got.len() == 301 && e[1].got.len() == 1, 10));
	uint i = 0;
	for (sz k = 0; k < 301; k++)
	{
		if (k == 151)
		{
			assert(host.got[k].array_view() == big.array_view());
			continue;
		}
		assert(number_of(host.got.get_ref(k)) == i++);
	}
	assert(number_of(client.got.get_ref(0)) == 42);
	// UDP: checked both ways, then used; never an older one after a newer one.
	assert(run_until({ host, client }, fn bool(End*[] e) => e[1].net.udp_ready(e[1].other) && e[0].net.udp_ready(e[0].other), 3));
	foreach (&m : host.got) m.free();
	host.got.clear();
	for (uint n = 1; n <= 100; n++)
	{
		client.net.send(client.other, &&number(n), false);
		client.poll();
	}
	run_until({ host, client }, fn bool(End*[] e) => !e[0].got.is_empty() && number_of(e[0].got.last()) == 100, 2);
	assert(!host.got.is_empty());
	uint last = 0;
	foreach (&m : host.got)
	{
		assert(number_of(m) > last);
		last = number_of(m);
	}
	// Too big for a datagram: over TCP instead, whole.
	char[4000] large;
	large[..] = 9;
	host.net.send(host.other, &large, false);
	assert(run_until({ host, client }, fn bool(End*[] e) => e[1].got.last().len() == 4000, 3));
}

fn void without_udp_over_tcp() @test
{
	End* host = new_end();
	End* client = new_end();
	defer
	{
		host.free();
		client.free();
	}
	client.net.set_udp_enabled(false);
	connect(host, client, transport_port(1));
	run_until({ host, client }, fn bool(End*[] e) => false, 0.5);
	assert(!client.net.udp_ready(client.other));
	assert(!host.net.udp_ready(host.other));
	for (uint i = 0; i < 50; i++)
	{
		client.net.send(client.other, &&number(i), false);
		host.net.send(host.other, &&number(i), false);
	}
	assert(run_until({ host, client }, fn bool(End*[] e) => e[0].got.len() == 50 && e[1].got.len() == 50, 3));
	assert(number_of(host.got.last()) == 49);
}

fn void closing_leaving_nobody_full() @test
{
	End* host = new_end();
	End* client = new_end();
	End* second = new_end();
	End* lonely = new_end();
	End* small = new_end();
	End* first = new_end();
	End* extra = new_end();
	Transport again = transport::new();
	defer
	{
		host.free();
		client.free();
		second.free();
		lonely.free();
		small.free();
		first.free();
		extra.free();
		again.free();
	}
	connect(host, client, transport_port(2));
	// The host sends one last message and closes: it arrives, then the end, on both sides.
	Connection* c = host.other;
	host.net.send(c, &&number(7), true);
	host.net.disconnect_later(c);
	host.net.send(c, &&number(8), true); // (after closing: not sent)
	assert(run_until({ host, client }, fn bool(End*[] e) => e[1].disconnects == 1 && e[0].disconnects == 1, 5));
	assert(client.got.len() == 1 && number_of(client.got.get_ref(0)) == 7);
	// A client that leaves is seen to go.
	assert(second.net.connect("127.0.0.1", transport_port(2)) == "");
	assert(run_until({ host, second }, fn bool(End*[] e) => e[1].other && e[0].connects == 2, 5));
	second.net.disconnect_now(second.other);
	assert(run_until({ host }, fn bool(End*[] e) => e[0].disconnects == 2, 5));
	assert(second.disconnects == 0); // (no event for our own disconnect_now)
	// Nobody on that port: the connection ends.
	assert(lonely.net.connect("127.0.0.1", transport_port(3)) == "");
	assert(run_until({ lonely }, fn bool(End*[] e) => e[0].disconnects == 1, 5));
	assert(lonely.connects == 0);
	// A full host turns the next one away.
	assert(small.net.listen(transport_port(4), 1) == "");
	assert(first.net.connect("127.0.0.1", transport_port(4)) == "");
	assert(run_until({ small, first }, fn bool(End*[] e) => e[1].connects == 1, 5));
	assert(extra.net.connect("127.0.0.1", transport_port(4)) == "");
	assert(run_until({ small, first, extra }, fn bool(End*[] e) => e[2].disconnects == 1, 5));
	assert(extra.connects == 0 && small.connects == 1);
	// A port in use can't be hosted on.
	@pool()
	{
		assert(again.listen(transport_port(4), 1).contains("in use"));
	};
	// Once the host stops, it can host there again at once.
	small.net.close();
	assert(again.listen(transport_port(4), 1) == "");
}
