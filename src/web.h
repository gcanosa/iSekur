// Built-in webchat: HTTPS + WebSocket mirror of the live session.
#pragma once
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <poll.h>
#include "core.h"

struct Web {
    Core& c;
    explicit Web(Core& core) : c(core) {}
    ~Web();
    bool start(std::string& err);  // no-op when [web] enabled != true
    void add_fds(std::vector<pollfd>& v);
    void process(const std::vector<pollfd>& v);  // after poll()
    void on_event(Ev e, Buffer* b, const Line* l);
    void flush();  // once per loop pass: send coalesced buffer updates
    bool running() const { return lfd >= 0; }

private:
    struct Client {
        Conn conn;
        std::string in;
        bool ws = false;
        time_t since = 0;
    };
    int lfd = -1;
    SSL_CTX* ctx = nullptr;
    bool secure = false;
    std::string user, pass_hash;
    std::vector<std::unique_ptr<Client>> clients;
    size_t fd_start = 0, fd_count = 0;
    std::set<int> pending, known;  // buffer ids: updates to send / already sent
    std::string last_order;
    std::map<std::string, time_t> sessions;                   // token -> expiry
    std::map<std::string, std::pair<int, time_t>> fails;      // ip -> (count, locked until)

    void http(Client& cl);
    void ws_frames(Client& cl);
    void broadcast(const std::string& json);
    void snapshot(Client& cl);
    std::string buf_json(Buffer* b);
    std::string order_json();
    std::string lines_json(Buffer* b);
    void flush_buf(Buffer* b);
    std::string theme_json();
};

// exposed for tests and --hash-password
std::string hash_password(const std::string& pw);
bool check_password(const std::string& pw, const std::string& stored);
std::string ws_frame(const std::string& payload, int opcode = 1);
// decodes one client frame from buf; returns bytes consumed, 0 = need more, -1 = bad
long ws_parse(const std::string& buf, int& opcode, std::string& payload);
std::string json_str(const std::string& s);
std::map<std::string, std::string> json_flat(const std::string& s);
