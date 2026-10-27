// Core state shared by every frontend: servers, buffers, theme, commands.
#pragma once
#define ISEKUR_VERSION "0.1"
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "net.h"

// ---- INI (config + themes) -------------------------------------------------
using Ini = std::map<std::string, std::map<std::string, std::string>>;
Ini ini_parse(const std::string& text);
bool ini_load(const std::string& path, Ini& out);
std::string ini_update(const std::string& text, const Ini& changes);
bool ini_save(const std::string& path, const Ini& changes, std::string& err);
std::string ini_get(const Ini& ini, const std::string& sec, const std::string& key,
                    const std::string& def = "");

// ---- colors / theme --------------------------------------------------------
using RGB = int32_t;  // 0xRRGGBB, -1 = terminal default
enum { A_BOLD = 1, A_ITALIC = 2, A_UNDER = 4, A_REV = 8, A_DIM = 16 };
struct Style { RGB fg = -1, bg = -1; int attr = 0; };

RGB parse_color(const std::string& s);  // "#rrggbb", "0-255", name, "default"
RGB xterm256_rgb(int idx);
std::string rgb_hex(RGB c);             // "#rrggbb" or "" for default

struct Theme {
    std::string name;
    std::map<std::string, Style> el;
    std::vector<RGB> nicks;
    std::map<std::string, std::string> layout;
    Style get(const std::string& k) const;
    std::string lay(const std::string& k, const std::string& def) const;
};
bool load_theme(const std::string& name, Theme& out, std::string& err);

// mIRC formatting: walk text, emitting codepoints with their style.
void mirc_walk(const std::string& s, Style base,
               const std::function<void(uint32_t, const Style&)>& f);
std::string mirc_strip(const std::string& s);
uint32_t utf8_next(const std::string& s, size_t& i);  // invalid bytes -> Latin-1
void utf8_put(std::string& out, uint32_t cp);

// ---- IRC message -----------------------------------------------------------
struct Msg {
    std::map<std::string, std::string> tags;
    std::string prefix, nick, cmd;
    std::vector<std::string> params;
    const std::string& p(size_t i) const;  // "" if missing
};
bool irc_parse(const std::string& line, Msg& m);
std::string irc_lower(const std::string& s);  // rfc1459 casemapping
time_t parse_server_time(const std::string& iso);

// ---- buffers ---------------------------------------------------------------
enum class LK { MSG, ACTION, NOTICE, JOIN, PART, QUIT, NICK, MODE, TOPIC, INFO, ERROR };
struct Line {
    time_t ts;
    LK kind;
    std::string nick, text;
    bool self = false, hilight = false;
};

enum class BT { STATUS, CHANNEL, QUERY };
struct Server;
struct NickEntry { std::string nick, prefix; bool away = false; };
struct Buffer {
    int id;
    BT type;
    std::string name;
    Server* srv;
    std::deque<Line> lines;
    std::string topic, modes;
    std::map<std::string, NickEntry> nicks;  // key: irc_lower(nick)
    int activity = 0;                        // 0 none, 1 data, 2 msg, 3 hilight
    int scroll = 0;                          // rows scrolled back (tui)
    bool joined = false;
    FILE* log = nullptr;                     // opened lazily when logging is on
    ~Buffer() { if (log) fclose(log); }
};

// ---- servers ---------------------------------------------------------------
struct ServerCfg {
    std::string name, host;
    int port = 6667;
    bool tls = false, tls_verify = true, autoconnect = true;
    std::string nick, altnick, user, realname, pass;
    std::string sasl_user, sasl_pass, sasl_mech, cert, key;
    std::vector<std::string> autojoin;
};
enum class SS { DISCONNECTED, CONNECTING, REGISTERING, CONNECTED };

struct Core;
struct Server {
    ServerCfg cfg;
    Conn conn;
    SS state = SS::DISCONNECTED;
    std::string nick, umodes, network;
    std::string prefix_modes = "ov", prefix_chars = "@+", chantypes = "#&";
    std::set<std::string> caps, caps_ls;  // enabled / advertised
    std::string inbuf;
    std::string chanmodes = "beI,k,l,imnpst";
    std::string scram_bare, scram_sig;  // SCRAM client-first-bare, expected server sig
    int sasl_step = 0, nick_tries = 0, backoff = 0;
    bool cap_done = false;
    time_t last_rx = 0, reconnect_at = 0, ping_at = 0, ctcp_at = 0;
    long long ping_sent_ms = 0;
    double lag = 0;
    bool user_quit = false;
    Buffer* status = nullptr;

    // Flood control: once registered, lines go through a token bucket
    // (flood_burst lines at once, then one per flood_interval_ms).
    std::deque<std::string> sendq;
    double tokens = 0;
    long long tokens_at = 0;
    int flood_burst = 5, flood_ms = 2000;
    void send(const std::string& line);      // queued when registered
    void send_now(const std::string& line);  // PING/PONG/QUIT: never wait
    int drain(long long now);                // ms until the next line may go, -1 = idle
    bool is_chan(const std::string& t) const;
    int prefix_rank(const std::string& pfx) const;  // lower = higher rank
};

// ---- core ------------------------------------------------------------------
enum class Ev { LINE, BUFFER, CLOSE, THEME };

struct Core {
    Ini cfg, unsaved;  // unsaved: /set changes not yet written by /save
    std::string cfg_path;
    Theme theme;
    std::vector<std::unique_ptr<Server>> servers;
    std::vector<std::unique_ptr<Buffer>> bufs;  // tab order
    Buffer* active = nullptr;
    int next_id = 1;
    bool quit = false, dirty = true;
    std::string quit_msg = "iSekur IRC";
    std::function<void(Ev, Buffer*, const Line*)> listener;  // web frontend

    void load_config();
    // [server.<name>] key, falling back to [general], then `def`
    std::string opt(const Server* s, const std::string& key, const std::string& def) const;
    bool opt_on(const Server* s, const std::string& key, bool def) const;
    void log_line(Buffer* b, const Line& l);
    std::string log_path(Buffer* b) const;
    void load_history(Buffer* b);
    Server* add_server(const ServerCfg& sc);
    void connect(Server* s);
    void disconnect(Server* s, const std::string& why);
    void on_readable(Server* s, short revents);
    void tick(time_t now);  // pings, timeouts, reconnects
    int drain();            // flush send queues; ms until the next one is due
    bool ignored(const std::string& prefix) const;

    Buffer* find(Server* s, const std::string& name);
    Buffer* open(Server* s, BT t, const std::string& name);
    void close(Buffer* b);
    void select(Buffer* b);
    int index_of(Buffer* b) const;
    void add(Buffer* b, Line l);
    void print(Buffer* b, LK k, const std::string& text);
    void notify(Ev e, Buffer* b, const Line* l = nullptr);

    void exec(Buffer* b, const std::string& input);  // "/cmd" or text
    void say(Buffer* b, const std::string& text, bool action = false);

    void handle(Server* s, const Msg& m);  // irc.cpp
};

long long now_ms();
std::string b64(const std::string& s);
std::string unb64(const std::string& s);  // "" on bad input
std::string fmt_time(time_t t, const std::string& fmt);
std::vector<std::string> split(const std::string& s, char sep);
std::string safe_name(const std::string& n);  // network name -> safe log file name
bool parse_log_line(const std::string& s, Line& l);
