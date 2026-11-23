// The network under multiplayer (net/session): messages between a host and
// its clients over TCP and UDP, through Winsock only. (src/net/transport.*:
// the same frames and datagrams.)
//
// TCP carries everything that must arrive, in order: each message a frame
// (u32 length, u8 kind, the bytes). It also carries the transport's own
// frames: the host's hello (a magic number, the transport version and a
// random token naming the connection), pings (every PING_EVERY seconds; a
// connection that hears nothing for TIMEOUT is dropped: TCP alone takes
// minutes to notice a peer that is gone) and the UDP handshake.
//
// UDP (the same port number as the host's TCP) carries what may be lost:
// positions and the like, where only the newest counts (older ones arriving
// late are dropped). Each datagram carries the connection's token and a
// sequence number. A client sends the host its token by UDP until the host
// answers by UDP; then it says so over TCP, and from then on both send
// unreliable messages by UDP: it's only used once it's known to work both
// ways. Until then (or if it never does: a firewall), and for a message too
// large for one datagram, unreliable messages go over TCP like the rest.
//
// IPv4, non-blocking; nothing here waits but resolving a host name.
module gocube::net::transport;
import gocube::net::sock, gocube::clock, gocube::util::vec, std::math;

const ushort VERSION = 1;
const sz MAX_MESSAGE = 32 << 20;     // bytes
const sz MAX_DATAGRAM = 1200;        // a UDP payload that crosses the internet whole
const sz MAX_PENDING = 96 << 20;     // queued for a connection that doesn't take it
const double PING_EVERY = 1.0;       // seconds
const double TIMEOUT = 10.0;         // nothing heard: gone
const double BIND_EVERY = 0.25;      // a client's UDP handshake, until answered ...
const double BIND_FOR = 5.0;         // ... or given up (TCP only)
const double LINGER = 3.0;           // disconnect_later: the most it waits for the other side

// TCP frames: u32 length (of kind + bytes), u8 kind, the bytes.
const char F_MESSAGE @private = 0; // the session's
const char F_HELLO @private = 1;   // host -> client: MAGIC u32, VERSION u16, token u64
const char F_PING @private = 2;    // (nothing: keeps the connection alive)
const char F_UDP_OK @private = 3;  // client -> host: your UDP answer came; UDP works both ways
// UDP datagrams: MAGIC u32, u8 kind, token u64, sequence u32, the bytes.
const char D_BIND @private = 1;    // client -> host: my token (and so my address)
const char D_ANSWER @private = 2;  // host -> client: yours came
const char D_MESSAGE @private = 3; // the session's
const uint MAGIC @private = 0x6E434F47; // "GOCn"
const sz FRAME_HEADER @private = 5;
const sz DATAGRAM_HEADER @private = 17;
const sz HELLO_SIZE @private = 14;

fn void put16(char* p, uint v) @private
{
	p[0] = (char)v;
	p[1] = (char)(v >> 8);
}

fn void put32(char* p, uint v) @private
{
	put16(p, v & 0xFFFF);
	put16(p + 2, v >> 16);
}

fn void put64(char* p, ulong v) @private
{
	put32(p, (uint)v);
	put32(p + 4, (uint)(v >> 32));
}

fn uint get16(char* p) @private => (uint)(p[0] | (uint)p[1] << 8);
fn uint get32(char* p) @private => get16(p) | get16(p + 2) << 16;
fn ulong get64(char* p) @private => (ulong)get32(p) | (ulong)get32(p + 4) << 32;

fn ulong random_token() @private
{
	ulong t = 0;
	while (t == 0)
	{
		if (sock::bcrypt_gen_random(null, (char*)&t, 8, sock::BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
		{
			t = (ulong)clock::now_us() * 0x9E3779B97F4A7C15;
		}
	}
	return t;
}

// A UDP socket, non-blocking, on `port` (0: any). INVALID_SOCKET on failure.
fn Socket udp_socket(ushort port) @private
{
	Socket s = sock::socket(sock::AF_INET, sock::SOCK_DGRAM, sock::IPPROTO_UDP);
	if (s == sock::INVALID_SOCKET) return s;
	SockaddrIn a = { .family = (short)sock::AF_INET, .port = sock::htons(port) };
	int off = 0;
	uint bytes = 0; // (else a datagram to a closed port makes the next read fail)
	sock::wsa_ioctl(s, sock::SIO_UDP_CONNRESET, &off, 4, null, 0, &bytes, null, null);
	if (sock::bind(s, &a, SockaddrIn::size) != 0 || !sock::set_nonblocking(s))
	{
		sock::closesocket(s);
		return sock::INVALID_SOCKET;
	}
	return s;
}

fn bool same_address(SockaddrIn* a, SockaddrIn* b) @private => a.addr == b.addr && a.port == b.port;

enum State : char
{
	CONNECTING,
	OPEN,
	CLOSING,
	DEAD,
}

struct Connection
{
	State state;
	Socket tcp;
	bool greeted; // a client: the host's hello came (CONNECT given)
	ulong token;
	Vec{char} out, in;
	sz out_at;
	bool shut;    // closing: our side of the TCP stream is ended
	// UDP: where its datagrams go; whether they go by UDP (checked both ways).
	SockaddrIn udp_to;
	bool udp_known, udp_ok;
	uint sent_seq, got_seq;
	bool got_any;
	double started, heard, said, bind_since, last_bind, closing_since;
}

fn void Connection.free(&self)
{
	self.out.free();
	self.in.free();
}

enum EventType : char
{
	CONNECT,
	RECEIVE,
	DISCONNECT,
}

struct Event
{
	EventType type;
	Connection* connection; // valid until the next poll() that returns no event
	Vec{char} data;         // RECEIVE: the message (the taker frees it)
}

struct Transport
{
	bool host;
	bool udp_disabled;
	Socket listener; // the host's TCP
	Socket udp;      // UDP: the host's, or a client's own
	int max_connections;
	Vec{Connection*} connections;
	Vec{Connection*} dead; // freed once their events are taken
	Vec{Event} events;
	sz events_head;
}

fn Transport new() => { .listener = sock::INVALID_SOCKET, .udp = sock::INVALID_SOCKET };

fn void Transport.free(&self)
{
	self.close();
	self.connections.free();
	self.dead.free();
	self.events.free();
}

fn bool Transport.active(&self) => self.listener != sock::INVALID_SOCKET || !self.connections.is_empty();

// Testing: no UDP (as behind a firewall that blocks it); unreliable messages go over TCP.
fn void Transport.set_udp_enabled(&self, bool on) => self.udp_disabled = !on;

// The host: takes up to `max_connections` clients on `port` (TCP and UDP).
// "" or what went wrong (temp).
fn String Transport.listen(&self, int port, int max_connections)
{
	self.close();
	if (!sock::winsock()) return "Couldn't start the network.";
	String in_use = string::tformat("Couldn't host on port %d (is it in use?).", port);
	Socket s = sock::socket(sock::AF_INET, sock::SOCK_STREAM, sock::IPPROTO_TCP);
	if (s == sock::INVALID_SOCKET) return "Couldn't start the network.";
	// (No SO_EXCLUSIVEADDRUSE: with it the port stays taken while the last
	// session's connections wait out TCP's TIME_WAIT, and hosting again at once fails.)
	SockaddrIn a = { .family = (short)sock::AF_INET, .port = sock::htons((ushort)port) };
	if (sock::bind(s, &a, SockaddrIn::size) != 0 || sock::listen(s, sock::SOMAXCONN) != 0 || !sock::set_nonblocking(s))
	{
		sock::closesocket(s);
		return in_use;
	}
	Socket u = udp_socket((ushort)port);
	if (u == sock::INVALID_SOCKET)
	{
		sock::closesocket(s);
		return in_use;
	}
	self.listener = s;
	self.udp = u;
	self.host = true;
	self.max_connections = max_connections;
	return "";
}

// A client: connects to the host (CONNECT once it answers; DISCONNECT if it
// can't). "" or what went wrong (temp).
fn String Transport.connect(&self, String address, int port)
{
	self.close();
	if (!sock::winsock()) return "Couldn't start the network.";
	AddrInfo hints = { .family = sock::AF_INET, .socktype = sock::SOCK_STREAM };
	AddrInfo* found;
	if (sock::getaddrinfo(address.zstr_tcopy(), null, &hints, &found) != 0 || !found) return string::tformat("Couldn't find %s.", address);
	SockaddrIn to = *found.addr;
	sock::freeaddrinfo(found);
	to.port = sock::htons((ushort)port);
	Socket s = sock::socket(sock::AF_INET, sock::SOCK_STREAM, sock::IPPROTO_TCP);
	if (s == sock::INVALID_SOCKET || !sock::set_nonblocking(s))
	{
		if (s != sock::INVALID_SOCKET) sock::closesocket(s);
		return "Couldn't start the network.";
	}
	sock::set_nodelay(s);
	if (sock::connect(s, &to, SockaddrIn::size) != 0 && !sock::would_block())
	{
		sock::closesocket(s);
		return string::tformat("Couldn't connect to %s.", address);
	}
	self.udp = udp_socket(0);
	Connection* c = mem::new(Connection);
	double t = clock::now();
	*c = { .tcp = s, .udp_to = to, .udp_known = true, .started = t, .heard = t, .said = t, .last_bind = -1e9 }; // (the host's UDP is on its TCP port)
	self.connections.push(c);
	self.host = false;
	return "";
}

// Whether unreliable messages to `c` go by UDP now.
fn bool Transport.udp_ready(&self, Connection* c) => c && !self.udp_disabled && self.udp != sock::INVALID_SOCKET && c.udp_ok && c.udp_known;

// --- Events --------------------------------------------------------------------

// The next event, false if there's none now (the taker frees its data).
// Reads, writes and keeps connections alive: call it often.
fn bool Transport.poll(&self, Event* event)
{
	if (self.events_head >= self.events.len())
	{
		self.events.clear();
		self.events_head = 0;
		self.pump();
	}
	if (self.events_head >= self.events.len()) return false;
	*event = self.events[self.events_head++];
	return true;
}

fn void Transport.push_event(&self, EventType type, Connection* c, char[] data = {})
{
	Event e = { .type = type, .connection = c };
	e.data.push_all(data);
	self.events.push(e);
}

fn void Transport.pump(&self)
{
	foreach (c : self.dead) // (their events have been taken)
	{
		c.free();
		free(c);
	}
	self.dead.clear();
	double t = clock::now();
	if (self.host && self.listener != sock::INVALID_SOCKET) self.accept_new();
	for (sz i = 0; i < self.connections.len(); i++) // (accepting adds to it)
	{
		Connection* c = self.connections[i];
		if (c.state == CONNECTING)
		{
			FdSet w = { 1, { [0] = c.tcp } };
			FdSet e = { 1, { [0] = c.tcp } };
			Timeval zero;
			if (sock::select(0, null, &w, &e, &zero) > 0)
			{
				if (sock::wsa_fd_is_set(c.tcp, &e))
				{
					self.drop(c, true); // refused, unreachable
					continue;
				}
				if (sock::wsa_fd_is_set(c.tcp, &w))
				{
					c.state = OPEN; // (CONNECT when the host says hello)
					c.heard = c.said = t;
				}
			}
		}
		if (c.state == OPEN || c.state == CLOSING) self.read_tcp(c);
		self.keep_alive(c, t);
		self.write(c);
	}
	self.read_udp();
	for (sz i = 0; i < self.connections.len();)
	{
		Connection* c = self.connections[i];
		if (c.state == DEAD)
		{
			self.dead.push(c);
			self.connections.remove_at(i);
		}
		else
		{
			i++;
		}
	}
}

fn void Transport.accept_new(&self)
{
	while (true)
	{
		SockaddrIn from;
		int len = SockaddrIn::size;
		Socket s = sock::accept(self.listener, &from, &len);
		if (s == sock::INVALID_SOCKET) return;
		int live = 0;
		foreach (c : self.connections) live += c.state != DEAD ? 1 : 0;
		if (live >= self.max_connections || !sock::set_nonblocking(s))
		{
			sock::closesocket(s); // full
			continue;
		}
		sock::set_nodelay(s);
		Connection* c = mem::new(Connection);
		double t = clock::now();
		*c = { .tcp = s, .state = OPEN, .greeted = true, .token = random_token(), .started = t, .heard = t, .said = t, .last_bind = -1e9 };
		char[HELLO_SIZE] hello;
		put32(&hello[0], MAGIC);
		put16(&hello[4], VERSION);
		put64(&hello[6], c.token);
		self.frame(c, F_HELLO, &hello);
		self.push_event(CONNECT, c);
		self.connections.push(c);
	}
}

fn void Transport.read_tcp(&self, Connection* c)
{
	bool ended = false;
	char[64 * 1024] buf;
	while (true)
	{
		int n = sock::recv(c.tcp, &buf, buf.len, 0);
		if (n > 0)
		{
			c.in.push_all(buf[:n]);
		}
		else if (n == 0)
		{
			ended = true; // the other side closed
			break;
		}
		else if (sock::would_block())
		{
			break;
		}
		else
		{
			ended = true; // reset
			break;
		}
	}
	sz at = 0;
	while (c.state != DEAD && c.in.len() - at >= 4)
	{
		uint len = get32(&c.in.items[at]);
		if (len < 1 || len > MAX_MESSAGE + 1)
		{
			self.drop(c, true); // not ours
			return;
		}
		if (c.in.len() - at - 4 < (sz)len) break; // (the rest is still coming)
		char kind = c.in[at + 4];
		self.on_frame(c, kind, c.in.items[at + FRAME_HEADER : (sz)len - 1]);
		at += 4 + (sz)len;
	}
	if (c.state == DEAD) return;
	c.in.remove_first(at);
	if (ended) self.drop(c, true);
}

fn void Transport.on_frame(&self, Connection* c, char kind, char[] data)
{
	c.heard = clock::now();
	switch (kind)
	{
		case F_MESSAGE:
			if (c.state == OPEN && c.greeted) self.push_event(RECEIVE, c, data);
		case F_HELLO:
			if (self.host || c.greeted) break;
			if (data.len != HELLO_SIZE || get32(data.ptr) != MAGIC || get16(data.ptr + 4) != VERSION)
			{
				self.drop(c, true); // not a GoCube host (or another network version)
				return;
			}
			c.token = get64(data.ptr + 6);
			c.greeted = true;
			c.bind_since = c.heard;
			self.push_event(CONNECT, c);
		case F_UDP_OK:
			if (self.host && c.udp_known) c.udp_ok = true;
		default: // F_PING, and kinds from a newer version
	}
}

fn Connection* Transport.by_token(&self, ulong token)
{
	foreach (c : self.connections)
	{
		if (c.token == token && c.state != DEAD) return c;
	}
	return null;
}

fn void Transport.read_udp(&self)
{
	if (self.udp == sock::INVALID_SOCKET) return;
	char[2048] buf;
	while (true)
	{
		SockaddrIn from;
		int len = SockaddrIn::size;
		int n = sock::recvfrom(self.udp, &buf, buf.len, 0, &from, &len);
		if (n < 0)
		{
			int err = sock::wsa_get_last_error();
			if (err == sock::WSAECONNRESET || err == sock::WSAEMSGSIZE) continue; // (one bad datagram)
			return;
		}
		if (self.udp_disabled || (sz)n < DATAGRAM_HEADER || get32(&buf) != MAGIC) continue;
		char kind = buf[4];
		ulong token = get64(&buf[5]);
		uint seq = get32(&buf[13]);
		Connection* c = null;
		if (self.host)
		{
			c = self.by_token(token);
		}
		else if (!self.connections.is_empty() && self.connections[0].greeted && self.connections[0].token == token &&
			same_address(&from, &self.connections[0].udp_to))
		{
			c = self.connections[0];
		}
		if (!c || c.state != OPEN) continue;
		if (self.host)
		{
			c.udp_to = from; // (where it is now, as its NAT says)
			c.udp_known = true;
		}
		if (kind == D_BIND && self.host)
		{
			self.datagram(c, D_ANSWER, {});
		}
		else if (kind == D_ANSWER && !self.host)
		{
			if (!c.udp_ok)
			{
				c.udp_ok = true;
				self.frame(c, F_UDP_OK, {});
			}
		}
		else if (kind == D_MESSAGE)
		{
			if (c.got_any && (int)(seq - c.got_seq) <= 0) continue; // older than one we had: newest wins
			c.got_any = true;
			c.got_seq = seq;
			self.push_event(RECEIVE, c, buf[DATAGRAM_HEADER .. n - 1]);
		}
	}
}

fn void Transport.keep_alive(&self, Connection* c, double t)
{
	switch (c.state)
	{
		case CONNECTING:
			if (t - c.started > TIMEOUT) self.drop(c, true);
		case OPEN:
			if (t - c.heard > TIMEOUT)
			{
				self.drop(c, true);
				break;
			}
			if (t - c.said >= PING_EVERY) self.frame(c, F_PING, {});
			if (!self.host && c.greeted && !c.udp_ok && !self.udp_disabled && self.udp != sock::INVALID_SOCKET &&
				t - c.bind_since < BIND_FOR && t - c.last_bind >= BIND_EVERY)
			{
				c.last_bind = t;
				self.datagram(c, D_BIND, {});
			}
		case CLOSING:
			if (!c.shut && c.out_at == c.out.len())
			{
				sock::shutdown(c.tcp, sock::SD_SEND); // all of it out: the other side sees the end, then closes
				c.shut = true;
			}
			if (t - c.closing_since > LINGER) self.drop(c, true);
		case DEAD:
	}
}

// --- Sending ---------------------------------------------------------------------

// Queues a message (written by poll() and flush()). Unreliable ones may be
// lost, and an older one is never delivered after a newer one.
fn void Transport.send(&self, Connection* to, char[] data, bool reliable)
{
	if (!to || to.state != OPEN || !to.greeted || data.len > MAX_MESSAGE) return;
	if (!reliable && self.udp_ready(to) && data.len + DATAGRAM_HEADER <= MAX_DATAGRAM)
	{
		self.datagram(to, D_MESSAGE, data);
	}
	else
	{
		self.frame(to, F_MESSAGE, data);
	}
}

fn void Transport.frame(&self, Connection* c, char kind, char[] data)
{
	char[FRAME_HEADER] header;
	put32(&header, (uint)data.len + 1);
	header[4] = kind;
	c.out.push_all(&header);
	c.out.push_all(data);
	c.said = clock::now();
	if (c.out.len() - c.out_at > MAX_PENDING) self.drop(c, true); // it doesn't take what we send
}

fn void Transport.datagram(&self, Connection* c, char kind, char[] data)
{
	if (self.udp == sock::INVALID_SOCKET || !c.udp_known) return;
	char[MAX_DATAGRAM] buf;
	put32(&buf, MAGIC);
	buf[4] = kind;
	put64(&buf[5], c.token);
	put32(&buf[13], kind == D_MESSAGE ? ++c.sent_seq : 0);
	if (data.len) buf[DATAGRAM_HEADER : data.len] = data[..];
	sock::sendto(self.udp, &buf, (int)(DATAGRAM_HEADER + data.len), 0, &c.udp_to, SockaddrIn::size); // (lost is fine)
}

fn void Transport.write(&self, Connection* c)
{
	if (c.state != OPEN && c.state != CLOSING) return;
	while (c.out_at < c.out.len())
	{
		sz chunk = math::min(c.out.len() - c.out_at, (sz)(1 << 20));
		int n = sock::send(c.tcp, &c.out.items[c.out_at], (int)chunk, 0);
		if (n > 0)
		{
			c.out_at += n;
		}
		else if (n < 0 && sock::would_block())
		{
			break;
		}
		else
		{
			self.drop(c, true);
			return;
		}
	}
	if (c.out_at == c.out.len())
	{
		c.out.clear();
		c.out_at = 0;
	}
	else if (c.out_at > (4 << 20))
	{
		c.out.remove_first(c.out_at);
		c.out_at = 0;
	}
}

// Writes what's queued, as far as the network takes it now.
fn void Transport.flush(&self)
{
	foreach (c : self.connections) self.write(c);
}

// --- Closing ---------------------------------------------------------------------

// Closes after what's queued is out (then DISCONNECT); what comes in meanwhile is dropped.
fn void Transport.disconnect_later(&self, Connection* c)
{
	if (c && c.state == OPEN)
	{
		c.state = CLOSING;
		c.closing_since = clock::now();
		self.write(c);
	}
}

// Closes at once, no event.
fn void Transport.disconnect_now(&self, Connection* c)
{
	if (c && c.state != DEAD)
	{
		self.write(c); // (what fits now still goes)
		sock::shutdown(c.tcp, sock::SD_SEND);
		self.drop(c, false);
	}
}

fn void Transport.drop(&self, Connection* c, bool event)
{
	if (c.state == DEAD) return;
	if (c.tcp != sock::INVALID_SOCKET)
	{
		sock::closesocket(c.tcp);
		c.tcp = sock::INVALID_SOCKET;
	}
	c.state = DEAD;
	if (event) self.push_event(DISCONNECT, c);
}

// Every connection, at once, no events.
fn void Transport.close(&self)
{
	foreach (c : self.connections) self.disconnect_now(c);
	foreach (c : self.connections)
	{
		c.free();
		free(c);
	}
	self.connections.clear();
	foreach (c : self.dead)
	{
		c.free();
		free(c);
	}
	self.dead.clear();
	for (sz i = self.events_head; i < self.events.len(); i++) self.events[i].data.free();
	self.events.clear();
	self.events_head = 0;
	if (self.listener != sock::INVALID_SOCKET)
	{
		sock::closesocket(self.listener);
		self.listener = sock::INVALID_SOCKET;
	}
	if (self.udp != sock::INVALID_SOCKET)
	{
		sock::closesocket(self.udp);
		self.udp = sock::INVALID_SOCKET;
	}
	self.host = false;
	self.max_connections = 0;
}
