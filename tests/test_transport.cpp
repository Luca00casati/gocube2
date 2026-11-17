// The transport under multiplayer (net/transport.h), on localhost: TCP for
// what must arrive, UDP for what may be lost once it's known to work, TCP for
// that too when it doesn't; closing after the last message; refusals.

#include "net/transport.h"

#include <doctest/doctest.h>

#include <process.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

using gc::Transport;

int test_port(int n) {
	return 36000 + (_getpid() % 1000) * 8 + n;
}

// One end: what it has had.
struct End {
	Transport net;
	Transport::Connection *other = nullptr; // (CONNECT's)
	std::vector<std::vector<uint8_t>> got;
	int connects = 0, disconnects = 0;

	void poll() {
		Transport::Event e;
		while (net.poll(e)) {
			if (e.type == Transport::Event::CONNECT) {
				connects++;
				other = e.connection;
			} else if (e.type == Transport::Event::RECEIVE) {
				got.push_back(std::move(e.data));
			} else {
				disconnects++;
				if (e.connection == other) {
					other = nullptr;
				}
			}
		}
		net.flush();
	}
};

bool run_until(std::vector<End *> ends, const std::function<bool()> &done, double seconds) {
	const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
	while (std::chrono::steady_clock::now() < until) {
		for (End *e : ends) {
			e->poll();
		}
		if (done()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

std::vector<uint8_t> number(uint32_t n) {
	return {uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24)};
}

uint32_t number_of(const std::vector<uint8_t> &b) {
	return b.size() == 4 ? uint32_t(b[0] | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24) : 0xFFFFFFFF;
}

void connect(End &host, End &client, int port) {
	REQUIRE(host.net.listen(port, 4) == "");
	REQUIRE(client.net.connect("127.0.0.1", port) == "");
	REQUIRE(run_until({&host, &client}, [&] { return host.other && client.other; }, 5));
}

} // namespace

TEST_CASE("transport: messages arrive whole and in order; unreliable ones by UDP once it works") {
	End host, client;
	connect(host, client, test_port(0));

	// Reliable, both ways, in order; a big one whole.
	std::vector<uint8_t> big(5u << 20);
	for (size_t i = 0; i < big.size(); i++) {
		big[i] = uint8_t(i * 7 + i / 4096);
	}
	for (uint32_t i = 0; i < 300; i++) {
		client.net.send(client.other, number(i), true);
		if (i == 150) {
			client.net.send(client.other, big, true);
		}
	}
	host.net.send(host.other, number(42), true);
	REQUIRE(run_until({&host, &client}, [&] { return host.got.size() == 301 && client.got.size() == 1; }, 10));
	bool in_order = true;
	for (uint32_t i = 0, k = 0; k < 301; k++) {
		if (k == 151) {
			CHECK(host.got[k] == big);
			continue;
		}
		in_order = in_order && number_of(host.got[k]) == i++;
	}
	CHECK(in_order);
	CHECK(number_of(client.got[0]) == 42);

	// UDP: checked both ways, then used; never an older one after a newer one.
	REQUIRE(run_until({&host, &client},
			[&] { return client.net.udp_ready(client.other) && host.net.udp_ready(host.other); }, 3));
	host.got.clear();
	for (uint32_t i = 1; i <= 100; i++) {
		client.net.send(client.other, number(i), false);
		client.poll();
	}
	run_until({&host, &client}, [&] { return !host.got.empty() && number_of(host.got.back()) == 100; }, 2);
	REQUIRE_FALSE(host.got.empty());
	uint32_t last = 0;
	bool newer = true;
	for (const auto &m : host.got) {
		newer = newer && number_of(m) > last;
		last = number_of(m);
	}
	CHECK(newer);
	// Too big for a datagram: over TCP instead, whole.
	const std::vector<uint8_t> large(4000, 9);
	host.net.send(host.other, large, false);
	CHECK(run_until({&host, &client}, [&] { return client.got.back() == large; }, 3));
}

TEST_CASE("transport: without UDP, unreliable messages go over TCP") {
	End host, client;
	client.net.set_udp_enabled(false);
	connect(host, client, test_port(1));
	run_until({&host, &client}, [] { return false; }, 0.5);
	CHECK_FALSE(client.net.udp_ready(client.other));
	CHECK_FALSE(host.net.udp_ready(host.other));
	for (uint32_t i = 0; i < 50; i++) {
		client.net.send(client.other, number(i), false);
		host.net.send(host.other, number(i), false);
	}
	CHECK(run_until({&host, &client}, [&] { return host.got.size() == 50 && client.got.size() == 50; }, 3));
	CHECK(number_of(host.got.back()) == 49);
}

TEST_CASE("transport: closing after the last message; leaving; nobody there; full") {
	End host, client;
	connect(host, client, test_port(2));
	// The host sends one last message and closes: it arrives, then the end, on both sides.
	Transport::Connection *c = host.other;
	host.net.send(c, number(7), true);
	host.net.disconnect_later(c);
	host.net.send(c, number(8), true); // (after closing: not sent)
	REQUIRE(run_until({&host, &client}, [&] { return client.disconnects == 1 && host.disconnects == 1; }, 5));
	REQUIRE(client.got.size() == 1);
	CHECK(number_of(client.got[0]) == 7);

	// A client that leaves is seen to go.
	End second;
	REQUIRE(second.net.connect("127.0.0.1", test_port(2)) == "");
	REQUIRE(run_until({&host, &second}, [&] { return second.other && host.connects == 2; }, 5));
	second.net.disconnect_now(second.other);
	CHECK(run_until({&host}, [&] { return host.disconnects == 2; }, 5));
	CHECK(second.disconnects == 0); // (no event for our own disconnect_now)

	// Nobody on that port: the connection ends.
	End lonely;
	REQUIRE(lonely.net.connect("127.0.0.1", test_port(3)) == "");
	CHECK(run_until({&lonely}, [&] { return lonely.disconnects == 1; }, 5));
	CHECK(lonely.connects == 0);

	// A full host turns the next one away.
	End small, first, extra;
	REQUIRE(small.net.listen(test_port(4), 1) == "");
	REQUIRE(first.net.connect("127.0.0.1", test_port(4)) == "");
	REQUIRE(run_until({&small, &first}, [&] { return first.connects == 1; }, 5));
	REQUIRE(extra.net.connect("127.0.0.1", test_port(4)) == "");
	CHECK(run_until({&small, &first, &extra}, [&] { return extra.disconnects == 1; }, 5));
	CHECK(extra.connects == 0);
	CHECK(small.connects == 1);

	// A port in use can't be hosted on.
	Transport again;
	CHECK(again.listen(test_port(4), 1).find("in use") != std::string::npos);
	// Once the host stops (closing its players' connections first), it can host there again at once.
	small.net.close();
	CHECK(again.listen(test_port(4), 1) == "");
}
