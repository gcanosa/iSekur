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
    std::vector<sockaddr_storage> addrs;
    std::string err;
    std::atomic<bool> done{false};
};

struct Conn {
    enum St { NONE, RESOLVING, CONNECTING, HANDSHAKE, OPEN } st = NONE;
    int fd = -1;
    SSL* ssl = nullptr;
    SSL_CTX* ctx = nullptr;  // owned for client conns
    bool server_side = false, want_write = false;
    std::string out, err, host;
    std::vector<sockaddr_storage> addrs;
    std::shared_ptr<Resolve> res;  // while RESOLVING; fd is then the wakeup pipe
    size_t ai = 0;
    int tls = 0;  // client: 0 plain, 1 tls+verify, 2 tls no verify
    std::string cert, key;

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

private:
    bool next_addr();
    bool start_tls();
};

std::string ssl_errstr();
int listen_tcp(const std::string& bind, int port, std::string& err);
