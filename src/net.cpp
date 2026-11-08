#include "net.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <strings.h>
#include <poll.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <csignal>
#include <sys/wait.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

std::string ssl_errstr() {
    unsigned long e = ERR_get_error();
    if (!e) return errno ? strerror(errno) : "connection closed";
    char buf[256];
    ERR_error_string_n(e, buf, sizeof buf);
    ERR_clear_error();
    return buf;
}

static void nonblock(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

bool Conn::connect(const std::string& h, int port, int t, const std::string& c,
                   const std::string& k) {
    close();
    host = h, tls = t, cert = c, key = k, err.clear(), out.clear();
    addrs.clear(), ai = 0;
    thost = h, tport = port, pin.clear(), pstep = 0;
    if (px.kind == Proxy::SSH) return spawn_ssh();
    if (px.kind) port = px.port;  // resolve and connect to the proxy, it reaches h for us
    const std::string& rh = px.kind ? px.host : h;
    int p[2];
    if (pipe(p)) { err = strerror(errno); return false; }
    nonblock(p[0]);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    auto r = std::make_shared<Resolve>();
    std::string port_s = std::to_string(port);
    // Detached: if we give up first (close()), the thread just finishes into
    // its own copy of `r` and a pipe nobody reads.
    std::thread([r, h = rh, port_s, wfd = p[1]] {
        addrinfo hints{}, *ai = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        int e = getaddrinfo(h.c_str(), port_s.c_str(), &hints, &ai);
        if (e) r->err = gai_strerror(e);
        for (addrinfo* a = ai; a; a = a->ai_next) {
            sockaddr_storage ss{};
            memcpy(&ss, a->ai_addr, a->ai_addrlen);
            r->addrs.push_back(ss);
        }
        if (ai) freeaddrinfo(ai);
        r->done.store(true, std::memory_order_release);
        char one = 1;
        if (write(wfd, &one, 1) < 0) {}  // reader may be gone; SIGPIPE is ignored
        ::close(wfd);
    }).detach();
    res = r;
    fd = p[0];
    st = RESOLVING;
    return true;
}

bool Conn::next_addr() {
    while (ai < addrs.size()) {
        sockaddr_storage& ss = addrs[ai++];
        socklen_t len = ss.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
        if (fd >= 0) ::close(fd);
        fd = socket(ss.ss_family, SOCK_STREAM, 0);
        if (fd < 0) { err = strerror(errno); continue; }
        nonblock(fd);
        if (::connect(fd, (sockaddr*)&ss, len) == 0 || errno == EINPROGRESS) {
            st = CONNECTING;
            return true;
        }
        err = strerror(errno);
    }
    if (fd >= 0) ::close(fd), fd = -1;
    st = NONE;
    return false;
}

static SSL_CTX* new_client_ctx() {
    SSL_CTX* c = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
    SSL_CTX_set_mode(c, SSL_MODE_RELEASE_BUFFERS);  // an idle connection gives back its ~34K of record buffers
    SSL_CTX_set_default_verify_paths(c);
#ifdef __APPLE__
    // release builds link OpenSSL statically: its own cert dir may not exist, macOS ships this bundle
    SSL_CTX_load_verify_locations(c, "/etc/ssl/cert.pem", nullptr);
#endif
    return c;
}

// not loopback, private, CGNAT, link-local, multicast or unspecified: somewhere on the internet
bool public_addr(const sockaddr* sa) {
    if (sa->sa_family == AF_INET) {
        uint32_t a = ntohl(((const sockaddr_in*)sa)->sin_addr.s_addr);
        return !((a >> 24) == 0 || (a >> 24) == 10 || (a >> 24) == 127 || (a >> 16) == 0xa9fe || (a >> 20) == 0xac1 ||
                 (a >> 16) == 0xc0a8 || (a >> 22) == (100u << 2 | 1) || a >= 0xe0000000u);
    }
    if (sa->sa_family != AF_INET6) return false;
    const unsigned char* b = ((const sockaddr_in6*)sa)->sin6_addr.s6_addr;
    static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (!memcmp(b, mapped, 12)) {  // ::ffff:a.b.c.d: the IPv4 one's rules
        sockaddr_in v4{};
        v4.sin_family = AF_INET, memcpy(&v4.sin_addr, b + 12, 4);
        return public_addr((sockaddr*)&v4);
    }
    bool zero = true;
    for (int i = 0; i < 15; i++) zero &= !b[i];
    return !(zero || (b[0] & 0xfe) == 0xfc || (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) || b[0] == 0xff);  // ::, ::1, fc00::/7, fe80::/10, ff00::/8
}

// https://host[:port]/path (or http://, tls false) -> host, port, path; false: not that
static bool split_https(const std::string& url, std::string& host, std::string& port, std::string& path, bool* tls = nullptr) {
    bool plain = url.size() > 8 && !strncasecmp(url.c_str(), "http://", 7);
    if (plain ? !tls : url.size() < 9 || strncasecmp(url.c_str(), "https://", 8)) return false;
    if (tls) *tls = !plain;
    size_t at = plain ? 7 : 8, e = url.find_first_of("/?#", at);
    std::string hp = url.substr(at, e == std::string::npos ? std::string::npos : e - at);
    path = e == std::string::npos ? "/" : url.substr(e);
    path = path.substr(0, path.find('#'));
    if (path.empty() || path[0] != '/') path = "/" + path;
    if (hp.find('@') != std::string::npos) return false;  // user:pass@: not for links people post
    port = plain ? "80" : "443";
    if (!hp.empty() && hp[0] == '[') {  // [::1]:443
        size_t c = hp.find(']');
        if (c == std::string::npos) return false;
        host = hp.substr(1, c - 1);
        if (c + 1 < hp.size()) { if (hp[c + 1] != ':') return false; port = hp.substr(c + 2); }
    } else {
        size_t c = hp.rfind(':');
        host = hp.substr(0, c);
        if (c != std::string::npos) port = hp.substr(c + 1);
    }
    if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos) return false;
    for (char ch : path) if ((unsigned char)ch <= ' ') return false;  // no request smuggling through the path
    return !host.empty() && host.find_first_of(" \r\n/\\") == std::string::npos;
}

std::string https_fetch_err(const std::string& url) {
    std::string h, p, path;
    bool tls;
    return split_https(url, h, p, path, &tls) ? "" : "not an http(s):// URL: " + url.substr(0, 200);
}

// one GET: false with err on a failure; true with the status line's code, headers and up to cap bytes of body
static bool fetch_once(const std::string& url, size_t cap, int& status, std::string& hdrs, std::string& body, std::string& err,
                       const std::string& post = "", bool guard = true) {
    static SSL_CTX* ctx = new_client_ctx();
    std::string host, port, path;
    bool tls = true;
    if (!split_https(url, host, port, path, &tls)) { err = https_fetch_err(url); return false; }
    addrinfo hints{}, *ai = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (int e = getaddrinfo(host.c_str(), port.c_str(), &hints, &ai)) { err = host + ": " + gai_strerror(e); return false; }
    int fd = -1;
    for (addrinfo* a = ai; a && guard; a = a->ai_next)  // every address must be public: a name that's half LAN is refused
        if (!public_addr(a->ai_addr)) { freeaddrinfo(ai); err = host + " is on a private network: not fetched"; return false; }
    for (addrinfo* a = ai; a && fd < 0; a = a->ai_next) {  // connect to the address checked, not a second lookup
        fd = socket(a->ai_family, SOCK_STREAM, 0);
        if (fd < 0) continue;
        nonblock(fd);
        pollfd pf{fd, POLLOUT, 0};
        int so = 0;
        socklen_t sl = sizeof so;
        if ((::connect(fd, a->ai_addr, a->ai_addrlen) == 0 || errno == EINPROGRESS) && poll(&pf, 1, 10000) == 1 &&
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) == 0 && so == 0) break;
        ::close(fd), fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0) { err = "can't connect to " + host; return false; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    timeval tv{15, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv), setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    SSL* ssl = tls ? SSL_new(ctx) : nullptr;  // plain http: the same request, read straight off the socket
    if (ssl) {
        SSL_set_fd(ssl, fd);
        in6_addr ip;
        bool literal = inet_pton(AF_INET, host.c_str(), &ip) == 1 || inet_pton(AF_INET6, host.c_str(), &ip) == 1;
        if (!literal) SSL_set_tlsext_host_name(ssl, host.c_str()), SSL_set1_host(ssl, host.c_str());
        else X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host.c_str());
        SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);
    }
    std::string got, req = (post.empty() ? "GET " : "POST ") + path + " HTTP/1.0\r\nHost: " + host + (port == (tls ? "443" : "80") ? "" : ":" + port) +
                             "\r\nUser-Agent: Mozilla/5.0 (compatible; iSekur link preview)\r\nAccept: text/html,*/*;q=0.8\r\n" +
                             (post.empty() ? "" : "Content-Type: text/plain; charset=utf-8\r\nContent-Length: " + std::to_string(post.size()) + "\r\n") +
                             "\r\n" + post;
    bool ok = ssl ? SSL_connect(ssl) == 1 && SSL_write(ssl, req.data(), (int)req.size()) == (int)req.size()
                  : ::send(fd, req.data(), req.size(), 0) == (ssize_t)req.size();
    if (!ok) err = ssl ? "TLS with " + host + ": " + ssl_errstr() : "can't send to " + host;
    char buf[16384];
    int n;
    size_t hdr = std::string::npos;
    while (ok && (n = ssl ? SSL_read(ssl, buf, sizeof buf) : (int)::recv(fd, buf, sizeof buf, 0)) > 0) {
        got.append(buf, (size_t)n);
        if (hdr == std::string::npos) hdr = got.find("\r\n\r\n");
        if (hdr == std::string::npos ? got.size() > 65536 : got.size() - hdr - 4 >= cap) break;  // enough: the rest isn't read
    }
    if (ssl) SSL_free(ssl);
    ::close(fd);
    if (!ok) return false;
    if (hdr == std::string::npos || got.compare(0, 5, "HTTP/")) { err = "no answer from " + host; return false; }
    status = atoi(got.c_str() + got.find(' ') + 1);
    hdrs = got.substr(0, hdr + 2);
    body = got.substr(hdr + 4, cap);
    return true;
}

static std::string header(const std::string& hdrs, const char* name) {  // name lowercase, with its ':'
    size_t at = 0, len = strlen(name);
    while ((at = hdrs.find("\r\n", at)) != std::string::npos) {
        at += 2;
        if (!strncasecmp(hdrs.c_str() + at, name, len)) {
            size_t e = hdrs.find("\r\n", at);
            std::string v = hdrs.substr(at + len, e - at - len);
            v.erase(0, v.find_first_not_of(" \t"));
            return v;
        }
    }
    return "";
}

bool https_fetch(std::string& url, size_t cap, int& status, std::string& ctype, std::string& body, std::string& err, const std::string& post) {
    for (int hop = 0; hop < 5; hop++) {
        std::string hdrs;
        if (!fetch_once(url, cap, status, hdrs, body, err, hop ? "" : post)) return false;  // a redirect after a POST: a GET (303)
        std::string loc = header(hdrs, "location:");
        if (status / 100 != 3 || loc.empty()) {
            ctype = header(hdrs, "content-type:");
            return true;
        }
        if (loc.rfind("//", 0) == 0) loc = url.substr(0, url.find(':') + 1) + loc;  // relative ones: from this URL's host or folder
        else if (loc.find("://") == std::string::npos) {
            size_t root = url.find('/', url.find("://") + 3);
            std::string base = root == std::string::npos ? url : url.substr(0, root);
            if (loc[0] == '/') loc = base + loc;
            else {
                std::string dir = root == std::string::npos ? "/" : url.substr(root, url.find_first_of("?#", root) - root);
                loc = base + dir.substr(0, dir.rfind('/') + 1) + loc;
            }
        }
        if (!strncasecmp(url.c_str(), "https://", 8) && !strncasecmp(loc.c_str(), "http://", 7)) {  // never down from TLS to plain text
            err = "redirected from https to plain http: not followed";
            return false;
        }
        url = loc;
    }
    err = "too many redirects";
    return false;
}

// iSekur's own fetches (the release check, the plugin catalog): fixed GitHub hosts, so no private-address guard (a split-horizon
// DNS may well answer a LAN address for them), but the same timeouts as any other fetch
std::string https_head(const std::string& host, const std::string& path) {
    int status = 0;
    std::string hdrs, body, err;
    return fetch_once("https://" + host + path, 0, status, hdrs, body, err, "", false) ? hdrs : "";  // cap 0: stops at the headers
}

bool https_get(const std::string& host, const std::string& path, size_t cap, std::string& out) {
    int status = 0;
    std::string hdrs, err;
    out.clear();
    if (!fetch_once("https://" + host + path, cap + 1, status, hdrs, out, err, "", false)) { out = err; return false; }
    if (status != 200) { out = host + ": HTTP " + std::to_string(status); return false; }
    if (out.size() > cap) { out = "too big (over " + std::to_string(cap / 1024) + " KB)"; return false; }
    return true;
}

bool Conn::start_tls() {
    // one context for every server without a client cert: the CA bundle (hundreds of KB) is
    // parsed once, not per server and again on every reconnect
    static SSL_CTX* shared = nullptr;
    SSL_CTX* use = ctx;
    if (!use && cert.empty()) use = shared ? shared : (shared = new_client_ctx());
    if (!use) {
        use = ctx = new_client_ctx();
        if (SSL_CTX_use_certificate_chain_file(ctx, cert.c_str()) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx, (key.empty() ? cert : key).c_str(), SSL_FILETYPE_PEM) != 1) {
            err = "client cert: " + ssl_errstr();
            return false;
        }
    }
    ssl = SSL_new(use);
    SSL_set_verify(ssl, tls == 1 ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
    SSL_set_fd(ssl, fd);
    SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_set_tlsext_host_name(ssl, host.c_str());
    if (tls == 1) SSL_set1_host(ssl, host.c_str());
    st = HANDSHAKE;
    return true;
}

bool Conn::accept_from(int lfd, SSL_CTX* sctx) {
    sockaddr_storage ss;
    socklen_t len = sizeof ss;
    fd = accept(lfd, (sockaddr*)&ss, &len);
    if (fd < 0) return false;
    nonblock(fd);
    char hbuf[NI_MAXHOST] = "";
    getnameinfo((sockaddr*)&ss, len, hbuf, sizeof hbuf, nullptr, 0, NI_NUMERICHOST);
    host = hbuf;
    server_side = true;
    if (sctx) {
        ssl = SSL_new(sctx);
        SSL_set_fd(ssl, fd);
        SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        st = HANDSHAKE;
    } else {
        st = OPEN;
    }
    return true;
}

short Conn::events() const {
    switch (st) {
    case RESOLVING: return POLLIN;
    case CONNECTING: return POLLOUT;
    case PROXY: return POLLIN;
    case HANDSHAKE: return want_write ? POLLOUT : POLLIN;
    case OPEN: return POLLIN | ((!out.empty() && !write_wants_read) || want_write || read_wants_write ? POLLOUT : 0);
    default: return 0;
    }
}

bool Conn::pump(short rev, std::string& in) {
    if (st == RESOLVING) {
        if (!res->done.load(std::memory_order_acquire)) return true;
        ::close(fd), fd = -1;
        addrs = res->addrs;
        err = res->err;
        res.reset();
        return next_addr();  // never fall through: the new socket isn't connected yet
    }
    if (st == CONNECTING) {
        int e = 0;
        socklen_t l = sizeof e;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l);
        if (e) {
            err = strerror(e);
            return next_addr();
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  // fails on ssh's socketpair, harmless
        if (px.kind && px.kind != Proxy::SSH) { st = PROXY; return proxy_send(); }
        if (tls) { if (!start_tls()) return false; }
        else st = OPEN;
    }
    if (st == PROXY) {
        if (!proxy_read(in)) return false;
        if (st == PROXY) return true;
    }
    if (st == HANDSHAKE) {
        int r = server_side ? SSL_accept(ssl) : SSL_connect(ssl);
        if (r != 1) {
            int e = SSL_get_error(ssl, r);
            want_write = e == SSL_ERROR_WANT_WRITE;
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return true;
            long v = server_side ? X509_V_OK : SSL_get_verify_result(ssl);
            err = v != X509_V_OK ? std::string("certificate: ") + X509_verify_cert_error_string(v)
                                 : ssh_pid > 0 ? ssh_said() : "TLS: " + ssl_errstr();
            return false;
        }
        want_write = false;
        st = OPEN;
    }
    if (st != OPEN) return true;
    if (rev & (POLLIN | POLLHUP | POLLERR) || (read_wants_write && (rev & POLLOUT)) || (ssl && SSL_pending(ssl))) {
        read_wants_write = false;
        char buf[16384];
        for (;;) {
            ssize_t n;
            if (ssl) {
                n = SSL_read(ssl, buf, sizeof buf);
                if (n <= 0) {
                    int e = SSL_get_error(ssl, (int)n);
                    if (e == SSL_ERROR_WANT_READ) break;
                    if (e == SSL_ERROR_WANT_WRITE) { read_wants_write = true; break; }
                    err = ssh_pid > 0 ? ssh_said() : e == SSL_ERROR_ZERO_RETURN ? "connection closed" : ssl_errstr();
                    return false;
                }
            } else {
                n = read(fd, buf, sizeof buf);
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
                if (n <= 0) { err = n ? strerror(errno) : ssh_pid > 0 ? ssh_said() : "connection closed"; return false; }
            }
            in.append(buf, (size_t)n);
            if (in_cap && in.size() >= in_cap) break;
            if (in.size() > (1 << 20)) { err = "input flood"; return false; }
        }
    }
    want_write = write_wants_read = false;
    while (!out.empty()) {
        ssize_t n;
        if (ssl) {
            n = SSL_write(ssl, out.data(), (int)out.size());
            if (n <= 0) {
                int e = SSL_get_error(ssl, (int)n);
                if (e == SSL_ERROR_WANT_WRITE) { want_write = true; break; }
                if (e == SSL_ERROR_WANT_READ) { write_wants_read = true; break; }  // POLLIN brings us back
                err = ssl_errstr();
                return false;
            }
        } else {
            n = write(fd, out.data(), out.size());
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
            if (n < 0) { err = strerror(errno); return false; }
        }
        out.erase(0, (size_t)n);
    }
    return !(closing && out.empty());
}

void Conn::close() {
    if (ssl) SSL_free(ssl), ssl = nullptr;
    if (ctx && !server_side) SSL_CTX_free(ctx), ctx = nullptr;
    if (fd >= 0) ::close(fd), fd = -1;
    if (ssh_err >= 0) ::close(ssh_err), ssh_err = -1;
    if (ssh_pid > 0) kill(ssh_pid, SIGKILL), waitpid(ssh_pid, nullptr, 0), ssh_pid = -1;  // ponytail: a tunnel has nothing to flush
    res.reset();
    st = NONE;
    want_write = closing = read_wants_write = write_wants_read = false;
}

int listen_tcp(const std::string& bind, int port, std::string& err) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int r = getaddrinfo(bind.empty() ? nullptr : bind.c_str(), std::to_string(port).c_str(),
                        &hints, &res);
    if (r) { err = gai_strerror(r); return -1; }
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) { err = strerror(errno); freeaddrinfo(res); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    // "::" takes IPv4 too (BSD/macOS default to v6 only, Linux doesn't)
    int zero = 0;
    if (res->ai_family == AF_INET6) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    if (::bind(fd, res->ai_addr, res->ai_addrlen) || listen(fd, 16)) {
        err = strerror(errno);
        ::close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    nonblock(fd);
    return fd;
}

// ---- proxies ------------------------------------------------------------------------------
static bool host_ok(const std::string& h) {  // a name or an address, never an option for ssh
    if (h.empty() || h[0] == '-' || h.size() > 255) return false;
    for (char ch : h)
        if (!isalnum((unsigned char)ch) && !strchr(".-_:", ch)) return false;
    return true;
}

bool parse_proxy(const std::string& url, Proxy& px, std::string& err) {
    px = {};
    size_t sep = url.find("://");
    std::string sch = sep == std::string::npos ? "" : url.substr(0, sep), rest = sep == std::string::npos ? "" : url.substr(sep + 3);
    for (auto& ch : sch) ch = (char)tolower((unsigned char)ch);
    if (sch == "socks5" || sch == "socks5h") px.kind = Proxy::SOCKS5, px.port = 1080;
    else if (sch == "socks4" || sch == "socks4a") px.kind = Proxy::SOCKS4, px.port = 1080;
    else if (sch == "http") px.kind = Proxy::HTTP, px.port = 8080;
    else if (sch == "ssh") px.kind = Proxy::SSH, px.port = 22;
    else { err = "a proxy is socks5://, socks4://, http:// or ssh://, then [user[:password]@]host[:port]"; return false; }
    while (!rest.empty() && rest.back() == '/') rest.pop_back();
    size_t at = rest.rfind('@');  // ponytail: no %-escapes, a password can't hold an '@'
    if (at != std::string::npos) {
        std::string up = rest.substr(0, at);
        rest.erase(0, at + 1);
        size_t c = up.find(':');
        px.user = up.substr(0, c);
        if (c != std::string::npos) px.pass = up.substr(c + 1);
        if (px.user.empty() || px.user.size() > 255 || px.pass.size() > 255) { err = "proxy: a user (and password) of 1-255 characters"; return false; }
    }
    std::string port;
    if (!rest.empty() && rest[0] == '[') {  // [::1]:1080
        size_t c = rest.find(']');
        px.host = rest.substr(1, c == std::string::npos ? std::string::npos : c - 1);
        if (c != std::string::npos && c + 1 < rest.size()) port = rest[c + 1] == ':' ? rest.substr(c + 2) : "x";
    } else {
        size_t c = rest.find(':');
        px.host = rest.substr(0, c);
        if (c != std::string::npos) port = rest.substr(c + 1);
    }
    if (!port.empty()) {
        px.port = port.size() <= 5 && port.find_first_not_of("0123456789") == std::string::npos ? atoi(port.c_str()) : 0;
        if (px.port < 1 || px.port > 65535) { err = "proxy: the port is 1-65535"; return false; }
    }
    if (!host_ok(px.host)) { err = "proxy: no host, or a host with odd characters"; return false; }
    if (px.kind == Proxy::SSH) {
        if (!px.pass.empty()) { err = "ssh:// logs in with keys or an agent, no password in the URL"; return false; }
        if (!px.user.empty() && (px.user[0] == '-' || px.user.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos)) {
            err = "ssh://: a user of letters, digits, . _ -";
            return false;
        }
    }
    return true;
}

std::string proxy_desc(const Proxy& px) {
    static const char* k[] = {"", "socks4", "socks5", "http proxy", "ssh"};
    std::string h = px.host.find(':') != std::string::npos ? "[" + px.host + "]" : px.host;
    return std::string(k[px.kind]) + " " + (px.user.empty() || px.kind != Proxy::SSH ? "" : px.user + "@") + h + ":" + std::to_string(px.port);
}

// the request for this step; ponytail: a few hundred bytes to a socket just connected, one write() takes it all
bool Conn::proxy_send() {
    std::string m, p{(char)(tport >> 8), (char)(tport & 255)};
    unsigned char a[16];
    bool v4 = inet_pton(AF_INET, thost.c_str(), a) == 1, v6 = !v4 && inet_pton(AF_INET6, thost.c_str(), a) == 1;
    if (px.kind == Proxy::SOCKS4) {
        if (v6) { err = "socks4 can't reach an IPv6 address"; return false; }
        // 4a: 0.0.0.1 and the name after the user, the proxy looks it up (no DNS leak here)
        m = "\x04\x01" + p + (v4 ? std::string((char*)a, 4) : std::string("\0\0\0\x01", 4)) + px.user + '\0' + (v4 ? "" : thost + '\0');
    } else if (px.kind == Proxy::HTTP) {
        std::string t = (v6 ? "[" + thost + "]" : thost) + ":" + std::to_string(tport);
        m = "CONNECT " + t + " HTTP/1.1\r\nHost: " + t + "\r\n";
        if (!px.user.empty()) {
            std::string up = px.user + ":" + px.pass, b(4 * ((up.size() + 2) / 3) + 1, '\0');
            b.resize((size_t)EVP_EncodeBlock((unsigned char*)&b[0], (const unsigned char*)up.data(), (int)up.size()));
            m += "Proxy-Authorization: Basic " + b + "\r\n";
        }
        m += "\r\n";
    } else if (pstep == 0) {  // socks5: the methods we have
        m = px.user.empty() ? std::string("\x05\x01\x00", 3) : std::string("\x05\x02\x00\x02", 4);
    } else if (pstep == 1) {  // user and password (RFC 1929)
        m = "\x01" + std::string(1, (char)px.user.size()) + px.user + std::string(1, (char)px.pass.size()) + px.pass;
    } else {  // connect: the name goes to the proxy, it resolves it
        if (thost.size() > 255) { err = "socks5: host name too long"; return false; }
        m = std::string("\x05\x01\x00", 3) + (v4 ? "\x01" + std::string((char*)a, 4) : v6 ? "\x04" + std::string((char*)a, 16)
                                               : "\x03" + std::string(1, (char)thost.size()) + thost) + p;
    }
    if (::write(fd, m.data(), m.size()) != (ssize_t)m.size()) { err = proxy_desc(px) + ": " + strerror(errno); return false; }
    return true;
}

bool Conn::proxy_read(std::string& in) {
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return true;
    std::string who = proxy_desc(px) + ": ";
    if (n <= 0) { err = who + (n ? strerror(errno) : "closed the connection"); return false; }
    pin.append(buf, (size_t)n);
    if (pin.size() > 16384) { err = who + "answer too long"; return false; }
    size_t used = 0;  // the bytes of a full answer
    const unsigned char* u = (const unsigned char*)pin.data();
    if (px.kind == Proxy::SOCKS4) {
        if (pin.size() < 8) return true;
        if (u[1] != 0x5a) { err = who + "refused (code " + std::to_string(u[1]) + ")"; return false; }
        used = 8;
    } else if (px.kind == Proxy::HTTP) {
        size_t e = pin.find("\r\n\r\n");
        if (e == std::string::npos) return true;
        std::string first = pin.substr(0, pin.find("\r\n"));
        int code = 0;
        if (sscanf(first.c_str(), "HTTP/%*s %d", &code) != 1 || code != 200) {
            err = who + first;
            return false;
        }
        used = e + 4;
    } else if (pstep < 2) {
        if (pin.size() < 2) return true;
        if (pstep == 0 && (u[0] != 5 || (u[1] != 0 && !(u[1] == 2 && !px.user.empty())))) {
            err = who + (u[0] != 5 ? "not a socks5 proxy" : px.user.empty() ? "wants a user and password" : "takes none of our logins");
            return false;
        }
        if (pstep == 1 && u[1] != 0) { err = who + "wrong user or password"; return false; }
        pstep = pstep == 0 && u[1] == 2 ? 1 : 2;
        pin.clear();
        return proxy_send();
    } else {
        if (pin.size() < 5) return true;
        static const char* why[] = {"", "general failure", "not allowed by its rules", "network unreachable", "host unreachable",
                                    "connection refused", "TTL expired", "command not supported", "address type not supported"};
        if (u[0] != 5 || u[1] != 0) { err = who + (u[1] < 9 ? why[u[1]] : "refused"); return false; }
        used = u[3] == 1 ? 10 : u[3] == 4 ? 22 : 7 + (size_t)u[4];
        if (pin.size() < used) return true;
    }
    in.append(pin, used, std::string::npos);  // a plain server can speak first: its bytes may have come along
    pin.clear();
    if (tls) return start_tls();
    st = OPEN;
    return true;
}

// ssh -W: ssh logs in on the jump host and carries the bytes to host:port over its stdin/stdout, our socketpair
bool Conn::spawn_ssh() {
    int sv[2], ep[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { err = strerror(errno); return false; }
    if (pipe(ep)) { err = strerror(errno); ::close(sv[0]), ::close(sv[1]); return false; }
    nonblock(sv[0]), nonblock(ep[0]);
    std::string w = (thost.find(':') != std::string::npos ? "[" + thost + "]" : thost) + ":" + std::to_string(tport),
                dest = (px.user.empty() ? "" : px.user + "@") + px.host, port = std::to_string(px.port);
    const char* argv[] = {"ssh", "-W", w.c_str(), "-p", port.c_str(), "-o", "BatchMode=yes", "-o", "ExitOnForwardFailure=yes",
                          "-o", "LogLevel=ERROR", "-o", "ServerAliveInterval=60", "--", dest.c_str(), nullptr};
    pid_t pid = fork();
    if (pid < 0) { err = std::string("fork: ") + strerror(errno); ::close(sv[0]), ::close(sv[1]), ::close(ep[0]), ::close(ep[1]); return false; }
    if (pid == 0) {
        setsid();  // no terminal: ssh never asks anything over the TUI (BatchMode: keys or an agent)
        for (int sig : {SIGPIPE, SIGHUP, SIGINT, SIGTERM, SIGQUIT, SIGCHLD}) signal(sig, SIG_DFL);
        dup2(sv[1], 0), dup2(sv[1], 1), dup2(ep[1], 2);
        if (sv[1] > 2) ::close(sv[1]);
        if (ep[1] > 2) ::close(ep[1]);
        execvp("ssh", (char* const*)argv);
        dprintf(2, "can't run ssh: %s\n", strerror(errno));
        _exit(127);
    }
    ::close(sv[1]), ::close(ep[1]);
    fd = sv[0], ssh_err = ep[0], ssh_pid = pid;
    st = CONNECTING;  // a socketpair is connected already: the first pump goes on to TLS or OPEN
    return true;
}

std::string Conn::ssh_said() {  // why ssh ended: its first line on stderr, it has exited by now
    char buf[512];
    for (int i = 0; i < 5; i++) {  // ponytail: up to 50ms for the exiting ssh to say it
        ssize_t n = read(ssh_err, buf, sizeof buf - 1);
        if (n > 0) {
            std::string l(buf, (size_t)n);
            l = l.substr(0, l.find_first_of("\r\n"));
            return proxy_desc(px) + ": " + l;
        }
        if (n == 0) break;
        usleep(10000);
    }
    return proxy_desc(px) + ": closed (it logs in with keys or an agent only; try ssh " + px.host + " by hand)";
}
