// Non-blocking TCP + optional TLS connection, driven by poll().
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <sys/socket.h>

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

// Result of a background getaddrinfo(); `done` publishes the other fields.
struct Resolve {
    std::vector<sockaddr_storage> addrs, local;  // local: the vhost's addresses, port 0
    std::string err;
    std::atomic<bool> done{false};
};

// connect through: socks4(a), socks5 (the proxy resolves names), http CONNECT, or ssh -W on a jump host
struct Proxy {
    enum Kind { NONE, SOCKS4, SOCKS5, HTTP, SSH } kind = NONE;
    std::string host, user, pass;
    int port = 0;
};
// socks5://[user:pass@]host[:1080], socks4://, http://[user:pass@]host[:8080], ssh://[user@]host[:22]; false: why in err
bool parse_proxy(const std::string& url, Proxy& px, std::string& err);
std::string proxy_desc(const Proxy& px);  // "socks5 host:1080", for "Connecting to..."

struct Conn {
    enum St { NONE, RESOLVING, CONNECTING, PROXY, HANDSHAKE, OPEN } st = NONE;
    Proxy px;  // set before connect(); close() keeps it
    int fd = -1;
    SSL* ssl = nullptr;
    SSL_CTX* ctx = nullptr;  // owned for client conns
    bool server_side = false, want_write = false;
    // TLS can need the other direction: a read that must write first (key update), a write that
    // must read first. Polled for that instead of what we'd normally wait on, so neither stalls nor spins
    bool read_wants_write = false, write_wants_read = false;
    std::string out, err, host;
    std::vector<sockaddr_storage> addrs, laddrs;  // laddrs: the vhost's addresses; with a vhost, addrs[i] is tried from from[i]
    std::vector<sockaddr_storage> from;
    std::vector<std::string> vfail;  // why each vhost address couldn't be used, for the error when none could
    bool vbind_failed = false;
    int family = 0;  // set before connect() like px: AF_INET or AF_INET6 = only that IP version (/server -4, -6), 0 = either
    std::string vhost;  // set before connect() like px: the local name or address to connect from, "" = the OS picks
    std::shared_ptr<Resolve> res;  // while RESOLVING; fd is then the wakeup pipe
    size_t ai = 0;
    int tls = 0;  // client: 0 plain, 1 tls+verify, 2 tls no verify
    std::string cert, key;
    // set before connect() like px, close() keeps it: not empty = IRC over WebSocket (IRCv3 text/binary.ircv3.net) at this
    // path, once the TCP / TLS / proxy is up. `out` stays plain IRC lines, `in` gets them back: the framing is in pump()
    std::string ws;

    ~Conn() { close(); }
    // Start connecting; DNS runs on a thread so the UI never blocks.
    bool connect(const std::string& host, int port, int tls,
                 const std::string& cert, const std::string& key);
    bool accept_from(int fd, SSL_CTX* server_ctx);  // web server side
    short events() const;
    // Drive I/O; appends received bytes to `in`. false = closed (see err).
    bool pump(short revents, std::string& in);
    void close();
    bool closing = false;  // close once `out` is flushed (web)
    size_t in_cap = 0;     // > 0: stop reading at this many bytes, the rest waits for the next poll (/dcc, IRC servers)
    bool pending() const;  // TLS holds decrypted bytes poll() can't see: pump() even without POLLIN

private:
    std::string thost, pin;  // the target behind the proxy, the proxy's answer so far
    int tport = 0, pstep = 0;
    int ws_st = 0;  // 0 not started, 1 upgrade sent, 2 frames
    bool ws_bin = false;  // the server picked binary.ircv3.net: our messages go out as binary frames
    int wsfrag_op = 0;
    std::string ws_key, wsin, wsout, wsfrag;  // wsout: what goes on the wire (the request, then masked frames) in place of `out`
    bool ws_begin();
    bool ws_pull(std::string& in);
    void ws_wrap();
    int ssh_pid = -1, ssh_err = -1;  // ssh -W: its pid, its stderr
    std::string ssh_said();
    bool proxy_send();
    bool proxy_read(std::string& in);
    bool spawn_ssh();
    bool next_addr();
    void vhost_failed(const std::string& why);
    bool start_tls();
};

std::string ssl_errstr();
// WebSocket framing (RFC 6455). mask: a client's frames are masked (4 random bytes).
std::string ws_frame(const std::string& payload, int opcode = 1, bool mask = false);
// decodes one frame from buf; returns bytes consumed, 0 = need more, -1 = bad. from_client: the frame must be masked (we are the
// server), else it must not be (we are the client). Without `fin`, a frame that isn't the last of its message is bad.
long ws_parse(const std::string& buf, int& opcode, std::string& payload, bool* fin = nullptr, bool from_client = true);
// ws://host[:port]/path or wss://: bare host (no [] around an IPv6), port (80 / 443 when none), path ("/" when none), tls.
// false: not that, or a path with spaces or control characters
bool ws_url(const std::string& url, std::string& host, int& port, std::string& path, bool& tls);
int listen_tcp(const std::string& bind, int port, std::string& err);
// blocking, for a thread, both on fetch_once's verified TLS and 10s connect / 15s read timeouts; for iSekur's own fixed hosts, so
// without the private-address guard https_fetch has. The headers of GET https://host/path (a release's Location), "" on any failure
std::string https_head(const std::string& host, const std::string& path);
// GET https://host/path, at most cap bytes of body. true with a 200 and the body in out; false with why in out
bool https_get(const std::string& host, const std::string& path, size_t cap, std::string& out);
// blocking, for a thread: GET an https:// URL for a plugin (isekur.http). Redirects followed (https only), the body cut
// at cap, 10s connect and 15s read timeouts. Hosts that resolve to loopback, private or link-local addresses are
// refused: links people post mustn't reach your LAN. false with why in err; true with the status (any), the final url
std::string https_fetch_err(const std::string& url);  // "" when the URL itself is fine (https, a host)
// post not empty: a POST of it as text/plain (the redirects after it are GETs)
// headers: the plugin's (isekur.http's User-Agent and Accept), sent on every hop; one that is given replaces ours
bool https_fetch(std::string& url, size_t cap, int& status, std::string& ctype, std::string& body, std::string& err,
                 const std::string& post = "", const std::vector<std::pair<std::string, std::string>>* headers = nullptr);
// isekur.upload: a multipart/form-data POST of one file (u.field), after u.fields, with u.headers, to an https:// URL. Streamed from
// the disk, blocking, for a thread; the private-address guard of https_fetch, and https:// only, unless u.guard is false (the tests' own server); redirects not followed.
// true with the status (any) and up to 64 KB of the answer; false with why in err (no answer within u.wait seconds, the connection closed without one). A host that answers before it has the whole file (413, 412...) is heard: the rest isn't sent. u.sent counts the bytes written, for progress
struct Upload {
    std::string url, path, field = "file", filename, ctype = "application/octet-stream";
    std::vector<std::pair<std::string, std::string>> fields, headers;
    mutable std::atomic<unsigned long long> sent{0};
    bool guard = true;
    int wait = 120;  // seconds to wait for the host's answer once the file is sent
};
bool https_upload(Upload& u, int& status, std::string& body, std::string& err);
bool public_addr(const struct sockaddr* sa);
