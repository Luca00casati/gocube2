#pragma once
// The network under multiplayer (net/session.h): messages between a host and
// its clients over TCP and UDP, through Winsock only.
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

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace gc {

class Transport {
public:
	static constexpr uint16_t VERSION = 1;
	static constexpr size_t MAX_MESSAGE = 32u << 20;     // bytes (ENet's default largest packet)
	static constexpr size_t MAX_DATAGRAM = 1200;         // a UDP payload that crosses the internet whole
	static constexpr size_t MAX_PENDING = 96u << 20;     // queued for a connection that doesn't take it
	static constexpr double PING_EVERY = 1.0;            // seconds
	static constexpr double TIMEOUT = 10.0;              // nothing heard: gone
	static constexpr double BIND_EVERY = 0.25;           // a client's UDP handshake, until answered ...
	static constexpr double BIND_FOR = 5.0;              // ... or given up (TCP only)
	static constexpr double LINGER = 3.0;                // disconnect_later: the most it waits for the other side

	struct Connection; // (transport.cpp)

	struct Event {
		enum Type { CONNECT, RECEIVE, DISCONNECT };
		Type type = RECEIVE;
		Connection *connection = nullptr; // valid until the next poll() that returns no event
		std::vector<uint8_t> data;        // RECEIVE: the message
	};

	Transport();
	~Transport();
	Transport(const Transport &) = delete;
	Transport &operator=(const Transport &) = delete;

	// The host: takes up to `max_connections` clients on `port` (TCP and UDP). "" or what went wrong.
	std::string listen(int port, int max_connections);
	// A client: connects to the host (CONNECT once it answers; DISCONNECT if it can't).
	std::string connect(const std::string &address, int port);
	bool active() const;

	// The next event, false if there's none now. Reads, writes and keeps
	// connections alive: call it often.
	bool poll(Event &event);
	// Queues a message (written by poll() and flush()). Unreliable ones may be
	// lost, and an older one is never delivered after a newer one.
	void send(Connection *to, const uint8_t *data, size_t size, bool reliable);
	void send(Connection *to, const std::vector<uint8_t> &bytes, bool reliable) {
		send(to, bytes.data(), bytes.size(), reliable);
	}
	void flush(); // writes what's queued, as far as the network takes it now
	// Closes after what's queued is out (then DISCONNECT); what comes in meanwhile is dropped.
	void disconnect_later(Connection *c);
	// Closes at once, no event.
	void disconnect_now(Connection *c);
	void close(); // every connection, at once, no events

	// Testing: no UDP (as behind a firewall that blocks it); unreliable messages go over TCP.
	void set_udp_enabled(bool on) { udp_enabled_ = on; }
	// Whether unreliable messages to `c` go by UDP now.
	bool udp_ready(const Connection *c) const;

private:
	void pump();
	void accept_new();
	void read_tcp(Connection &c);
	void read_udp();
	void on_frame(Connection &c, uint8_t kind, const uint8_t *data, size_t size);
	void keep_alive(Connection &c, double now);
	void write(Connection &c);
	void frame(Connection &c, uint8_t kind, const uint8_t *data, size_t size);
	void datagram(Connection &c, uint8_t kind, const uint8_t *data, size_t size);
	void drop(Connection &c, bool event); // closes it; DISCONNECT if `event`
	Connection *by_token(uint64_t token);

	bool host_ = false;
	bool udp_enabled_ = true;
	uintptr_t listener_;  // SOCKET (the host's TCP)
	uintptr_t udp_;       // SOCKET (UDP: the host's, or a client's own)
	int max_connections_ = 0;
	std::vector<std::unique_ptr<Connection>> connections_;
	std::vector<std::unique_ptr<Connection>> dead_; // freed once their events are taken
	std::deque<Event> events_;
};

} // namespace gc
