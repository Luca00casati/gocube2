// The Winsock calls the transport makes (IPv4 only), declared as Windows does.
module gocube::net::sock @link("ws2_32");

alias Socket = uptr;
const Socket INVALID_SOCKET = ~(uptr)0;

const int AF_INET = 2;
const int SOCK_STREAM = 1;
const int SOCK_DGRAM = 2;
const int IPPROTO_TCP = 6;
const int IPPROTO_UDP = 17;
const int SOMAXCONN = 0x7fffffff;
const uint FIONBIO = 0x8004667e;
const int TCP_NODELAY = 1;
const int SD_SEND = 1;
const int WSAEWOULDBLOCK = 10035;
const int WSAEMSGSIZE = 10040;
const int WSAECONNRESET = 10054;
const uint SIO_UDP_CONNRESET = 0x9800000C;

struct SockaddrIn
{
	short family;
	ushort port;   // network byte order
	uint addr;     // network byte order
	char[8] zero;
}

struct FdSet
{
	uint count;
	Socket[64] fds;
}

struct Timeval
{
	int sec, usec;
}

struct AddrInfo
{
	int flags, family, socktype, protocol;
	usz addrlen;
	char* canonname;
	SockaddrIn* addr;
	AddrInfo* next;
}

extern fn int wsa_startup(ushort version, void* data) @cname("WSAStartup");
extern fn int wsa_get_last_error() @cname("WSAGetLastError");
extern fn int wsa_ioctl(Socket s, uint code, void* in, uint in_size, void* out, uint out_size, uint* returned, void* overlapped, void* routine) @cname("WSAIoctl");
extern fn Socket socket(int af, int type, int protocol) @cname("socket");
extern fn int bind(Socket s, SockaddrIn* addr, int len) @cname("bind");
extern fn int listen(Socket s, int backlog) @cname("listen");
extern fn Socket accept(Socket s, SockaddrIn* addr, int* len) @cname("accept");
extern fn int connect(Socket s, SockaddrIn* addr, int len) @cname("connect");
extern fn int recv(Socket s, char* buf, int len, int flags) @cname("recv");
extern fn int send(Socket s, char* buf, int len, int flags) @cname("send");
extern fn int recvfrom(Socket s, char* buf, int len, int flags, SockaddrIn* from, int* from_len) @cname("recvfrom");
extern fn int sendto(Socket s, char* buf, int len, int flags, SockaddrIn* to, int to_len) @cname("sendto");
extern fn int closesocket(Socket s) @cname("closesocket");
extern fn int shutdown(Socket s, int how) @cname("shutdown");
extern fn int ioctlsocket(Socket s, uint cmd, uint* arg) @cname("ioctlsocket");
extern fn int setsockopt(Socket s, int level, int name, char* value, int len) @cname("setsockopt");
extern fn int select(int nfds, FdSet* read, FdSet* write, FdSet* except, Timeval* timeout) @cname("select");
extern fn int getaddrinfo(ZString node, ZString service, AddrInfo* hints, AddrInfo** result) @cname("getaddrinfo");
extern fn void freeaddrinfo(AddrInfo* info) @cname("freeaddrinfo");
extern fn int wsa_fd_is_set(Socket s, FdSet* set) @cname("__WSAFDIsSet");

fn ushort htons(ushort v) @inline => (ushort)(v << 8 | v >> 8);
fn uint htonl(uint v) @inline => (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);

fn bool would_block() => wsa_get_last_error() == WSAEWOULDBLOCK;

bool started @private;
bool start_ok @private;

// Winsock started (once).
fn bool winsock()
{
	if (!started)
	{
		started = true;
		char[512] data;
		start_ok = wsa_startup(0x0202, &data) == 0;
	}
	return start_ok;
}

fn bool set_nonblocking(Socket s)
{
	uint on = 1;
	return ioctlsocket(s, FIONBIO, &on) == 0;
}

fn void set_nodelay(Socket s)
{
	int on = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&on, 4);
}

module gocube::net::sock @link("bcrypt");

const uint BCRYPT_USE_SYSTEM_PREFERRED_RNG = 2;
extern fn int bcrypt_gen_random(void* alg, char* buffer, uint size, uint flags) @cname("BCryptGenRandom");
