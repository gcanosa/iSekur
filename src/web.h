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
    bool start(std::string& err);  // listen now (no-op if already running)
    void stop();
    static bool autostart(const Ini& cfg);
    std::string url;  // where it listens, "" when stopped
    void add_fds(std::vector<pollfd>& v);
    void process(const std::vector<pollfd>& v);  // after poll()
    void on_event(Ev e, Buffer* b, const Line* l);
    void flush();  // once per loop pass: send coalesced buffer updates
    bool running() const { return lfd >= 0; }
    bool attached() const;  // a logged-in browser is connected
    std::string stat_json();  // the status line: per network umodes, lag and away, status_info

private:
    struct Client {
        Conn conn;
        std::string in;
        bool ws = false;
        std::string frag;  // a fragmented message so far
        int frag_op = 0;
        time_t since = 0;    // accepted; a WebSocket: last heard from
        time_t pinged = 0;   // a WebSocket: our last ping
        std::string ip;      // a WebSocket: the browser's address (client_ip)
        FILE* file = nullptr;  // GET /files/name: streamed into conn.out as it drains
        unsigned long long left = 0;
        ~Client() { if (file) fclose(file); }
    };
    int lfd = -1;
    SSL_CTX* ctx = nullptr;
    bool secure = false;
    std::string user, pass_hash;
    std::vector<std::unique_ptr<Client>> clients;
    size_t fd_start = 0, fd_count = 0;
    std::set<int> pending, known;  // buffer ids: updates to send / already sent
    std::string last_order;
    std::map<std::string, time_t> sessions;                   // sha256(token) -> expiry
    std::string sessions_path;
    void load_sessions();
    void save_sessions();
    std::map<std::string, std::pair<int, time_t>> fails;      // ip -> (count, locked until)

    bool start_(std::string& err);
    void http(Client& cl);
    void ws_frames(Client& cl);
    void broadcast(const std::string& json);
    void snapshot(Client& cl);
    std::string buf_json(Buffer* b, bool full = false);  // full: with the nick list even if the browsers have it
    std::map<int, std::string> sent_nicks;  // buffer id -> the nick list JSON the browsers last got
    std::string order_json();
    std::string lines_json(Buffer* b);  // the last BACKLOG, for a new tab or browser
    std::string lines_json(Buffer* b, const char* t, size_t from, size_t to);
    void flush_buf(Buffer* b);
    std::string theme_json();
    std::string dcc_json(std::string* sig = nullptr);  // the transfers going on; sig: just which and their states
    std::string last_dcc, last_dcc_sig;
    time_t last_dcc_at = 0;
    std::string keys_json();  // [keys] for the webchat's own bindings
    std::string lock_json();
    // the screensaver: frames at the size the last page asked for (ssize; other pages stretch it), only the cells that changed
    int ss_w = 0, ss_h = 0;
    long long ss_at = 0;
    std::vector<Core::SCell> ss_last;
    void screen_poll();
    std::string update_json();   // /lock: on, msg (the box's error line)
    std::string split_json();  // the terminal's /split: buf (-1: none), first, mode, pm
    std::string last_keys, last_stat;
    time_t last_stat_at = 0;
    time_t ws_seen = 0;  // last time a browser was attached
    // the audit lines: ip -> (attached, last seen); gone once 30s pass without it (a reload or a blip isn't)
    std::map<std::string, std::pair<time_t, time_t>> ws_ips;
    void files(Client& cl, const std::string& path, const std::map<std::string, std::string>& h);  // dcc_web
    void feed(Client& cl);
};

// exposed for tests and --hash-password
std::string hash_password(const std::string& pw);
bool gen_selfsigned(const std::string& crt, const std::string& key, std::string& err);  // the webchat's and the BNC's own certificate
bool check_password(const std::string& pw, const std::string& stored);
std::string ws_frame(const std::string& payload, int opcode = 1);
// decodes one client frame from buf; returns bytes consumed, 0 = need more, -1 = bad.
// Without `fin`, a frame that isn't the last of its message is bad.
long ws_parse(const std::string& buf, int& opcode, std::string& payload, bool* fin = nullptr);
std::string json_str(const std::string& s);
// the browser's address: the proxy's X-Forwarded-For (its last hop) when the peer is this machine, else the peer
std::string client_ip(const std::string& peer, const std::string& xff);
std::string ua_short(const std::string& ua);  // "Firefox on macOS", "" when unknown
std::map<std::string, std::string> json_flat(const std::string& s);
