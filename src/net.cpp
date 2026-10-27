#include "net.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
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
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    // ponytail: blocking DNS; freezes the UI for a resolver timeout at worst.
    int r = getaddrinfo(h.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (r) { err = gai_strerror(r); return false; }
    for (addrinfo* a = res; a; a = a->ai_next) {
        sockaddr_storage ss{};
        memcpy(&ss, a->ai_addr, a->ai_addrlen);
        addrs.push_back(ss);
    }
    freeaddrinfo(res);
    return next_addr();
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

bool Conn::start_tls() {
    if (!ctx) {
        ctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_default_verify_paths(ctx);
        SSL_CTX_set_verify(ctx, tls == 1 ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
        if (!cert.empty()) {
            if (SSL_CTX_use_certificate_chain_file(ctx, cert.c_str()) != 1 ||
                SSL_CTX_use_PrivateKey_file(ctx, (key.empty() ? cert : key).c_str(),
                                            SSL_FILETYPE_PEM) != 1) {
                err = "client cert: " + ssl_errstr();
                return false;
            }
        }
    }
    ssl = SSL_new(ctx);
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
    case CONNECTING: return POLLOUT;
    case HANDSHAKE: return want_write ? POLLOUT : POLLIN;
    case OPEN: return POLLIN | (out.empty() && !want_write ? 0 : POLLOUT);
    default: return 0;
    }
}

bool Conn::pump(short rev, std::string& in) {
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
    if (rev & (POLLIN | POLLHUP | POLLERR) || (ssl && SSL_pending(ssl))) {
        char buf[16384];
        for (;;) {
            ssize_t n;
            if (ssl) {
                n = SSL_read(ssl, buf, sizeof buf);
                if (n <= 0) {
                    int e = SSL_get_error(ssl, (int)n);
                    if (e == SSL_ERROR_WANT_READ) break;
                    if (e == SSL_ERROR_WANT_WRITE) { want_write = true; break; }
                    err = e == SSL_ERROR_ZERO_RETURN ? "connection closed" : ssl_errstr();
                    return false;
                }
            } else {
                n = read(fd, buf, sizeof buf);
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
                if (n <= 0) { err = n ? strerror(errno) : "connection closed"; return false; }
            }
            in.append(buf, (size_t)n);
            if (in.size() > (1 << 20)) { err = "input flood"; return false; }
        }
    }
    want_write = false;
    while (!out.empty()) {
        ssize_t n;
        if (ssl) {
            n = SSL_write(ssl, out.data(), (int)out.size());
            if (n <= 0) {
                int e = SSL_get_error(ssl, (int)n);
                if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) { want_write = true; break; }
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
    st = NONE;
    want_write = closing = false;
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
