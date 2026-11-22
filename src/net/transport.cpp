#include "net/transport.h"

#include "platform/clock.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace gc {

namespace {

// TCP frames: u32 length (of kind + bytes), u8 kind, the bytes.
enum Frame : uint8_t {
	F_MESSAGE = 0, // the session's
	F_HELLO = 1,   // host -> client: MAGIC u32, VERSION u16, token u64
	F_PING = 2,    // (nothing: keeps the connection alive)
	F_UDP_OK = 3,  // client -> host: your UDP answer came; UDP works both ways
};
// UDP datagrams: MAGIC u32, u8 kind, token u64, sequence u32, the bytes.
enum Datagram : uint8_t {
	D_BIND = 1,    // client -> host: my token (and so my address)
	D_ANSWER = 2,  // host -> client: yours came
	D_MESSAGE = 3, // the session's
};
constexpr uint32_t MAGIC = 0x6E434F47; // "GOCn"
constexpr size_t FRAME_HEADER = 5, DATAGRAM_HEADER = 17, HELLO_SIZE = 14;

void put16(uint8_t *p, uint16_t v) {
	p[0] = uint8_t(v);
	p[1] = uint8_t(v >> 8);
}
void put32(uint8_t *p, uint32_t v) {
	put16(p, uint16_t(v));
	put16(p + 2, uint16_t(v >> 16));
}
void put64(uint8_t *p, uint64_t v) {
	put32(p, uint32_t(v));
	put32(p + 4, uint32_t(v >> 32));
}
uint16_t get16(const uint8_t *p) {
	return uint16_t(p[0] | p[1] << 8);
}
uint32_t get32(const uint8_t *p) {
	return uint32_t(get16(p)) | uint32_t(get16(p + 2)) << 16;
}
uint64_t get64(const uint8_t *p) {
	return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32;
}

bool would_block() {
	return WSAGetLastError() == WSAEWOULDBLOCK;
}

bool winsock() {
	static const bool ok = [] {
		WSADATA data;
		return WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}();
	return ok;
}

uint64_t random_token() {
	uint64_t t = 0;
	while (t == 0) {
		if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&t), sizeof(t), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
			t = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) * 0x9E3779B97F4A7C15ull;
		}
	}
	return t;
}

bool set_nonblocking(SOCKET s) {
	u_long on = 1;
	return ioctlsocket(s, FIONBIO, &on) == 0;
}

void set_nodelay(SOCKET s) {
	BOOL on = TRUE;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof(on));
}

// A UDP socket, non-blocking, on `port` (0: any). INVALID_SOCKET on failure.
SOCKET udp_socket(uint16_t port) {
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) {
		return s;
	}
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons(port);
	BOOL off = FALSE;
	DWORD bytes = 0; // (else a datagram to a closed port makes the next read fail)
	WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
	if (bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || !set_nonblocking(s)) {
		closesocket(s);
		return INVALID_SOCKET;
	}
	return s;
}

bool same_address(const sockaddr_in &a, const sockaddr_in &b) {
	return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

} // namespace

struct Transport::Connection {
	enum State { CONNECTING, OPEN, CLOSING, DEAD };
	State state = CONNECTING;
	SOCKET tcp = INVALID_SOCKET;
	bool greeted = false; // a client: the host's hello came (CONNECT given)
	uint64_t token = 0;
	std::vector<uint8_t> out, in;
	size_t out_at = 0;
	bool shut = false; // closing: our side of the TCP stream is ended
	// UDP: where its datagrams go; whether they go by UDP (checked both ways).
	sockaddr_in udp_to{};
	bool udp_known = false, udp_ok = false;
	uint32_t sent_seq = 0, got_seq = 0;
	bool got_any = false;
	double started = 0, heard = 0, said = 0, bind_since = 0, last_bind = -1e9, closing_since = 0;
};

Transport::Transport() : listener_(INVALID_SOCKET), udp_(INVALID_SOCKET) {}

Transport::~Transport() {
	close();
}

bool Transport::active() const {
	return listener_ != INVALID_SOCKET || !connections_.empty();
}

std::string Transport::listen(int port, int max_connections) {
	close();
	if (!winsock()) {
		return "Couldn't start the network.";
	}
	const std::string in_use = "Couldn't host on port " + std::to_string(port) + " (is it in use?).";
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) {
		return "Couldn't start the network.";
	}
	// (No SO_EXCLUSIVEADDRUSE: with it the port stays taken while the last
	// session's connections wait out TCP's TIME_WAIT, and hosting again at once fails.)
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons(uint16_t(port));
	if (bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || ::listen(s, SOMAXCONN) != 0 ||
			!set_nonblocking(s)) {
		closesocket(s);
		return in_use;
	}
	const SOCKET u = udp_socket(uint16_t(port));
	if (u == INVALID_SOCKET) {
		closesocket(s);
		return in_use;
	}
	listener_ = s;
	udp_ = u;
	host_ = true;
	max_connections_ = max_connections;
	return "";
}

std::string Transport::connect(const std::string &address, int port) {
	close();
	if (!winsock()) {
		return "Couldn't start the network.";
	}
	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo *found = nullptr;
	if (getaddrinfo(address.c_str(), nullptr, &hints, &found) != 0 || !found) {
		return "Couldn't find " + address + ".";
	}
	sockaddr_in to = *reinterpret_cast<const sockaddr_in *>(found->ai_addr);
	freeaddrinfo(found);
	to.sin_port = htons(uint16_t(port));

	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET || !set_nonblocking(s)) {
		if (s != INVALID_SOCKET) {
			closesocket(s);
		}
		return "Couldn't start the network.";
	}
	set_nodelay(s);
	if (::connect(s, reinterpret_cast<sockaddr *>(&to), sizeof(to)) != 0 && !would_block()) {
		closesocket(s);
		return "Couldn't connect to " + address + ".";
	}
	udp_ = udp_socket(0);
	auto c = std::make_unique<Connection>();
	c->tcp = s;
	c->udp_to = to; // (the host's UDP is on its TCP port)
	c->udp_known = true;
	c->started = c->heard = c->said = now();
	connections_.push_back(std::move(c));
	host_ = false;
	return "";
}

bool Transport::udp_ready(const Connection *c) const {
	return c && udp_enabled_ && udp_ != INVALID_SOCKET && c->udp_ok && c->udp_known;
}

// --- Events ----------------------------------------------------------------------

bool Transport::poll(Event &event) {
	if (events_.empty()) {
		pump();
	}
	if (events_.empty()) {
		return false;
	}
	event = std::move(events_.front());
	events_.pop_front();
	return true;
}

void Transport::pump() {
	dead_.clear(); // (their events have been taken)
	const double t = now();
	if (host_ && listener_ != INVALID_SOCKET) {
		accept_new();
	}
	for (size_t i = 0; i < connections_.size(); i++) { // (accepting adds to it)
		Connection &c = *connections_[i];
		if (c.state == Connection::CONNECTING) {
			fd_set w, e;
			FD_ZERO(&w);
			FD_ZERO(&e);
			FD_SET(c.tcp, &w);
			FD_SET(c.tcp, &e);
			timeval zero{0, 0};
			if (select(0, nullptr, &w, &e, &zero) > 0) {
				if (FD_ISSET(c.tcp, &e)) {
					drop(c, true); // refused, unreachable
					continue;
				}
				if (FD_ISSET(c.tcp, &w)) {
					c.state = Connection::OPEN; // (CONNECT when the host says hello)
					c.heard = c.said = t;
				}
			}
		}
		if (c.state == Connection::OPEN || c.state == Connection::CLOSING) {
			read_tcp(c);
		}
		keep_alive(c, t);
		write(c);
	}
	read_udp();
	for (auto it = connections_.begin(); it != connections_.end();) {
		if ((*it)->state == Connection::DEAD) {
			dead_.push_back(std::move(*it));
			it = connections_.erase(it);
		} else {
			++it;
		}
	}
}

void Transport::accept_new() {
	for (;;) {
		sockaddr_in from{};
		int len = sizeof(from);
		const SOCKET s = accept(listener_, reinterpret_cast<sockaddr *>(&from), &len);
		if (s == INVALID_SOCKET) {
			return;
		}
		const auto live = std::count_if(connections_.begin(), connections_.end(),
				[](const auto &c) { return c->state != Connection::DEAD; });
		if (live >= max_connections_ || !set_nonblocking(s)) {
			closesocket(s); // full
			continue;
		}
		set_nodelay(s);
		auto c = std::make_unique<Connection>();
		c->tcp = s;
		c->state = Connection::OPEN;
		c->greeted = true;
		c->token = random_token();
		c->started = c->heard = c->said = now();
		uint8_t hello[HELLO_SIZE];
		put32(hello, MAGIC);
		put16(hello + 4, VERSION);
		put64(hello + 6, c->token);
		frame(*c, F_HELLO, hello, sizeof(hello));
		Event e;
		e.type = Event::CONNECT;
		e.connection = c.get();
		events_.push_back(std::move(e));
		connections_.push_back(std::move(c));
	}
}

void Transport::read_tcp(Connection &c) {
	bool ended = false;
	uint8_t buf[64 * 1024];
	for (;;) {
		const int n = recv(c.tcp, reinterpret_cast<char *>(buf), int(sizeof(buf)), 0);
		if (n > 0) {
			c.in.insert(c.in.end(), buf, buf + n);
		} else if (n == 0) {
			ended = true; // the other side closed
			break;
		} else if (would_block()) {
			break;
		} else {
			ended = true; // reset
			break;
		}
	}
	size_t at = 0;
	while (c.state != Connection::DEAD && c.in.size() - at >= 4) {
		const uint32_t len = get32(c.in.data() + at);
		if (len < 1 || len > MAX_MESSAGE + 1) {
			drop(c, true); // not ours
			return;
		}
		if (c.in.size() - at - 4 < len) {
			break; // (the rest is still coming)
		}
		const uint8_t kind = c.in[at + 4];
		on_frame(c, kind, c.in.data() + at + FRAME_HEADER, len - 1);
		at += 4 + size_t(len);
	}
	if (c.state == Connection::DEAD) {
		return;
	}
	c.in.erase(c.in.begin(), c.in.begin() + std::ptrdiff_t(at));
	if (ended) {
		drop(c, true);
	}
}

void Transport::on_frame(Connection &c, uint8_t kind, const uint8_t *data, size_t size) {
	c.heard = now();
	switch (kind) {
		case F_MESSAGE:
			if (c.state == Connection::OPEN && c.greeted) {
				Event e;
				e.type = Event::RECEIVE;
				e.connection = &c;
				e.data.assign(data, data + size);
				events_.push_back(std::move(e));
			}
			break;
		case F_HELLO:
			if (host_ || c.greeted) {
				break;
			}
			if (size != HELLO_SIZE || get32(data) != MAGIC || get16(data + 4) != VERSION) {
				drop(c, true); // not a GoCube host (or another network version)
				return;
			}
			c.token = get64(data + 6);
			c.greeted = true;
			c.bind_since = c.heard;
			{
				Event e;
				e.type = Event::CONNECT;
				e.connection = &c;
				events_.push_back(std::move(e));
			}
			break;
		case F_UDP_OK:
			if (host_ && c.udp_known) {
				c.udp_ok = true;
			}
			break;
		default: // F_PING, and kinds from a newer version
			break;
	}
}

void Transport::read_udp() {
	if (udp_ == INVALID_SOCKET) {
		return;
	}
	uint8_t buf[2048];
	for (;;) {
		sockaddr_in from{};
		int len = sizeof(from);
		const int n = recvfrom(udp_, reinterpret_cast<char *>(buf), int(sizeof(buf)), 0,
				reinterpret_cast<sockaddr *>(&from), &len);
		if (n < 0) {
			const int err = WSAGetLastError();
			if (err == WSAECONNRESET || err == WSAEMSGSIZE) {
				continue; // (one bad datagram)
			}
			return;
		}
		if (!udp_enabled_ || size_t(n) < DATAGRAM_HEADER || get32(buf) != MAGIC) {
			continue;
		}
		const uint8_t kind = buf[4];
		const uint64_t token = get64(buf + 5);
		const uint32_t seq = get32(buf + 13);
		Connection *c = nullptr;
		if (host_) {
			c = by_token(token);
		} else if (!connections_.empty() && connections_[0]->greeted && connections_[0]->token == token &&
				same_address(from, connections_[0]->udp_to)) {
			c = connections_[0].get();
		}
		if (!c || c->state != Connection::OPEN) {
			continue;
		}
		if (host_) {
			c->udp_to = from; // (where it is now, as its NAT says)
			c->udp_known = true;
		}
		if (kind == D_BIND && host_) {
			datagram(*c, D_ANSWER, nullptr, 0);
		} else if (kind == D_ANSWER && !host_) {
			if (!c->udp_ok) {
				c->udp_ok = true;
				frame(*c, F_UDP_OK, nullptr, 0);
			}
		} else if (kind == D_MESSAGE) {
			if (c->got_any && int32_t(seq - c->got_seq) <= 0) {
				continue; // older than one we had: newest wins
			}
			c->got_any = true;
			c->got_seq = seq;
			Event e;
			e.type = Event::RECEIVE;
			e.connection = c;
			e.data.assign(buf + DATAGRAM_HEADER, buf + n);
			events_.push_back(std::move(e));
		}
	}
}

void Transport::keep_alive(Connection &c, double t) {
	switch (c.state) {
		case Connection::CONNECTING:
			if (t - c.started > TIMEOUT) {
				drop(c, true);
			}
			break;
		case Connection::OPEN:
			if (t - c.heard > TIMEOUT) {
				drop(c, true);
				break;
			}
			if (t - c.said >= PING_EVERY) {
				frame(c, F_PING, nullptr, 0);
			}
			if (!host_ && c.greeted && !c.udp_ok && udp_enabled_ && udp_ != INVALID_SOCKET &&
					t - c.bind_since < BIND_FOR && t - c.last_bind >= BIND_EVERY) {
				c.last_bind = t;
				datagram(c, D_BIND, nullptr, 0);
			}
			break;
		case Connection::CLOSING:
			if (!c.shut && c.out_at == c.out.size()) {
				shutdown(c.tcp, SD_SEND); // all of it out: the other side sees the end, then closes
				c.shut = true;
			}
			if (t - c.closing_since > LINGER) {
				drop(c, true);
			}
			break;
		case Connection::DEAD:
			break;
	}
}

// --- Sending ---------------------------------------------------------------------

void Transport::send(Connection *to, const uint8_t *data, size_t size, bool reliable) {
	if (!to || to->state != Connection::OPEN || !to->greeted || size > MAX_MESSAGE) {
		return;
	}
	if (!reliable && udp_ready(to) && size + DATAGRAM_HEADER <= MAX_DATAGRAM) {
		datagram(*to, D_MESSAGE, data, size);
	} else {
		frame(*to, F_MESSAGE, data, size);
	}
}

void Transport::frame(Connection &c, uint8_t kind, const uint8_t *data, size_t size) {
	uint8_t header[FRAME_HEADER];
	put32(header, uint32_t(size + 1));
	header[4] = kind;
	c.out.insert(c.out.end(), header, header + FRAME_HEADER);
	if (size) {
		c.out.insert(c.out.end(), data, data + size);
	}
	c.said = now();
	if (c.out.size() - c.out_at > MAX_PENDING) {
		drop(c, true); // it doesn't take what we send
	}
}

void Transport::datagram(Connection &c, uint8_t kind, const uint8_t *data, size_t size) {
	if (udp_ == INVALID_SOCKET || !c.udp_known) {
		return;
	}
	uint8_t buf[MAX_DATAGRAM];
	put32(buf, MAGIC);
	buf[4] = kind;
	put64(buf + 5, c.token);
	put32(buf + 13, kind == D_MESSAGE ? ++c.sent_seq : 0);
	if (size) {
		std::memcpy(buf + DATAGRAM_HEADER, data, size);
	}
	sendto(udp_, reinterpret_cast<const char *>(buf), int(DATAGRAM_HEADER + size), 0,
			reinterpret_cast<const sockaddr *>(&c.udp_to), sizeof(c.udp_to)); // (lost is fine)
}

void Transport::write(Connection &c) {
	if (c.state != Connection::OPEN && c.state != Connection::CLOSING) {
		return;
	}
	while (c.out_at < c.out.size()) {
		const size_t chunk = std::min<size_t>(c.out.size() - c.out_at, 1u << 20);
		const int n = ::send(c.tcp, reinterpret_cast<const char *>(c.out.data() + c.out_at), int(chunk), 0);
		if (n > 0) {
			c.out_at += size_t(n);
		} else if (n < 0 && would_block()) {
			break;
		} else {
			drop(c, true);
			return;
		}
	}
	if (c.out_at == c.out.size()) {
		c.out.clear();
		c.out_at = 0;
	} else if (c.out_at > (4u << 20)) {
		c.out.erase(c.out.begin(), c.out.begin() + std::ptrdiff_t(c.out_at));
		c.out_at = 0;
	}
}

void Transport::flush() {
	for (auto &c : connections_) {
		write(*c);
	}
}

// --- Closing ---------------------------------------------------------------------

void Transport::disconnect_later(Connection *c) {
	if (c && c->state == Connection::OPEN) {
		c->state = Connection::CLOSING;
		c->closing_since = now();
		write(*c);
	}
}

void Transport::disconnect_now(Connection *c) {
	if (c && c->state != Connection::DEAD) {
		write(*c); // (what fits now still goes)
		shutdown(c->tcp, SD_SEND);
		drop(*c, false);
	}
}

void Transport::drop(Connection &c, bool event) {
	if (c.state == Connection::DEAD) {
		return;
	}
	if (c.tcp != INVALID_SOCKET) {
		closesocket(c.tcp);
		c.tcp = INVALID_SOCKET;
	}
	c.state = Connection::DEAD;
	if (event) {
		Event e;
		e.type = Event::DISCONNECT;
		e.connection = &c;
		events_.push_back(std::move(e));
	}
}

Transport::Connection *Transport::by_token(uint64_t token) {
	for (auto &c : connections_) {
		if (c->token == token && c->state != Connection::DEAD) {
			return c.get();
		}
	}
	return nullptr;
}

void Transport::close() {
	for (auto &c : connections_) {
		disconnect_now(c.get());
	}
	connections_.clear();
	dead_.clear();
	events_.clear();
	if (listener_ != INVALID_SOCKET) {
		closesocket(listener_);
		listener_ = INVALID_SOCKET;
	}
	if (udp_ != INVALID_SOCKET) {
		closesocket(udp_);
		udp_ = INVALID_SOCKET;
	}
	host_ = false;
	max_connections_ = 0;
}

} // namespace gc
