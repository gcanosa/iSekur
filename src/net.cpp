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
#include <openssl/err.h>
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
    int p[2];
    if (pipe(p)) { err = strerror(errno); return false; }
    nonblock(p[0]);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    auto r = std::make_shared<Resolve>();
    std::string port_s = std::to_string(port);
    // Detached: if we give up first (close()), the thread just finishes into
    // its own copy of `r` and a pipe nobody reads.
    std::thread([r, h, port_s, wfd = p[1]] {
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

// Blocking: for a background thread. One HEAD over verified TLS; the reply's headers, or "".
// ponytail: no timeout, a black-holed network parks the (detached) thread until the OS gives up
std::string https_head(const std::string& host, const std::string& path) {
    static SSL_CTX* ctx = new_client_ctx();
    BIO* b = BIO_new_ssl_connect(ctx);
    SSL* ssl = nullptr;
    BIO_get_ssl(b, &ssl);
    SSL_set_tlsext_host_name(ssl, host.c_str());
    SSL_set1_host(ssl, host.c_str());
    SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);
    BIO_set_conn_hostname(b, (host + ":443").c_str());
    std::string req = "HEAD " + path + " HTTP/1.1\r\nHost: " + host + "\r\nUser-Agent: iSekur\r\nConnection: close\r\n\r\n", got;
    char buf[4096];
    int n;
    if (BIO_do_connect(b) == 1 && BIO_write(b, req.data(), (int)req.size()) == (int)req.size())
        while (got.find("\r\n\r\n") == std::string::npos && got.size() < 65536 && (n = BIO_read(b, buf, sizeof buf)) > 0) got.append(buf, (size_t)n);
    BIO_free_all(b);
    return got;
}

bool https_get(const std::string& host, const std::string& path, size_t cap, std::string& out) {
    static SSL_CTX* ctx = new_client_ctx();
    BIO* b = BIO_new_ssl_connect(ctx);
    SSL* ssl = nullptr;
    BIO_get_ssl(b, &ssl);
    SSL_set_tlsext_host_name(ssl, host.c_str());
    SSL_set1_host(ssl, host.c_str());
    SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);
    BIO_set_conn_hostname(b, (host + ":443").c_str());
    // HTTP/1.0: no chunked bodies, the server closes at the end
    std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nUser-Agent: iSekur\r\n\r\n", got;
    out.clear();
    if (BIO_do_connect(b) != 1) { out = "can't connect to " + host + ": " + ssl_errstr(); BIO_free_all(b); return false; }
    timeval tv{20, 0};  // a stalled server doesn't park the thread for good
    int fd = (int)BIO_get_fd(b, nullptr);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv), setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char buf[16384];
    int n;
    if (BIO_write(b, req.data(), (int)req.size()) == (int)req.size())
        while (got.size() <= cap + 65536 && (n = BIO_read(b, buf, sizeof buf)) > 0) got.append(buf, (size_t)n);
    BIO_free_all(b);
    size_t hdr = got.find("\r\n\r\n");
    if (hdr == std::string::npos) { out = "no answer from " + host; return false; }
    if (got.compare(0, 5, "HTTP/") != 0 || got.find(" 200 ") > got.find("\r\n")) {
        out = host + ": " + got.substr(0, got.find("\r\n"));
        return false;
    }
    if (got.size() - hdr - 4 > cap) { out = "too big (over " + std::to_string(cap / 1024) + " KB)"; return false; }
    out = got.substr(hdr + 4);
    return true;
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
static bool fetch_once(const std::string& url, size_t cap, int& status, std::string& hdrs, std::string& body, std::string& err) {
    static SSL_CTX* ctx = new_client_ctx();
    std::string host, port, path;
    bool tls = true;
    if (!split_https(url, host, port, path, &tls)) { err = https_fetch_err(url); return false; }
    addrinfo hints{}, *ai = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (int e = getaddrinfo(host.c_str(), port.c_str(), &hints, &ai)) { err = host + ": " + gai_strerror(e); return false; }
    int fd = -1;
    for (addrinfo* a = ai; a; a = a->ai_next)  // every address must be public: a name that's half LAN is refused
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
    std::string got, req = "GET " + path + " HTTP/1.0\r\nHost: " + host + (port == (tls ? "443" : "80") ? "" : ":" + port) +
                             "\r\nUser-Agent: Mozilla/5.0 (compatible; iSekur link preview)\r\nAccept: text/html,*/*;q=0.8\r\n\r\n";
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

bool https_fetch(std::string& url, size_t cap, int& status, std::string& ctype, std::string& body, std::string& err) {
    for (int hop = 0; hop < 5; hop++) {
        std::string hdrs;
        if (!fetch_once(url, cap, status, hdrs, body, err)) return false;
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
        url = loc;
    }
    err = "too many redirects";
    return false;
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
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (tls) { if (!start_tls()) return false; }
        else st = OPEN;
    }
    if (st == HANDSHAKE) {
        int r = server_side ? SSL_accept(ssl) : SSL_connect(ssl);
        if (r != 1) {
            int e = SSL_get_error(ssl, r);
            want_write = e == SSL_ERROR_WANT_WRITE;
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return true;
            long v = server_side ? X509_V_OK : SSL_get_verify_result(ssl);
            err = v != X509_V_OK ? std::string("certificate: ") + X509_verify_cert_error_string(v)
                                 : "TLS: " + ssl_errstr();
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
                    err = e == SSL_ERROR_ZERO_RETURN ? "connection closed" : ssl_errstr();
                    return false;
                }
            } else {
                n = read(fd, buf, sizeof buf);
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
                if (n <= 0) { err = n ? strerror(errno) : "connection closed"; return false; }
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
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    // "::" takes IPv4 too (BSD/macOS default to v6 only, Linux doesn't)
    int zero = 0;
    if (res->ai_family == AF_INET6) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    if (fd < 0 || ::bind(fd, res->ai_addr, res->ai_addrlen) || listen(fd, 16)) {
        err = strerror(errno);
        if (fd >= 0) ::close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    nonblock(fd);
    return fd;
}
