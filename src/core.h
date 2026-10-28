// Core state shared by every frontend: servers, buffers, theme, commands.
#pragma once
#define ISEKUR_VERSION "0.2"
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
    bool nolog = false;  // /grep results: shown, never written to the log
};
// what /quiet hides: other people coming, going and renaming
inline bool noise(const Line& l) {
    return !l.self && (l.kind == LK::JOIN || l.kind == LK::PART || l.kind == LK::QUIT || l.kind == LK::NICK);
}

enum class BT { STATUS, CHANNEL, QUERY };
struct Server;
struct NickEntry { std::string nick, prefix; bool away = false; std::string host; };  // host: user@host, "" = unknown
struct Buffer {
    int id;
    BT type;
    std::string name;
    Server* srv;
    std::deque<Line> lines;
    std::string topic, modes;
    std::map<std::string, NickEntry> nicks;  // key: irc_lower(nick)
    int activity = 0;                        // 0 none, 1 data, 2 msg, 3 hilight
    int unread = 0;                          // lines below the "new" marker
    int scroll = 0;                          // rows scrolled back (tui)
    bool joined = false;
    bool quiet = false;                      // /quiet: hide joins, parts, quits, nick changes
    FILE* log = nullptr;                     // opened lazily when logging is on
    std::string log_day;                     // YYYY-MM-DD the open log belongs to
    // /topicrotate: topics set in turn every rot_min minutes
    std::vector<std::string> rot;
    int rot_min = 0;
    size_t rot_i = 0;
    time_t rot_at = 0;
    ~Buffer() { if (log) fclose(log); }
};

// ---- servers ---------------------------------------------------------------
struct ServerCfg {
    std::string name, host;
    int port = 6667;
    bool tls = false, tls_verify = true, autoconnect = true;
    std::string nick, altnick, user, realname, pass;
    std::string sasl_user, sasl_pass, sasl_mech, cert, key, nickserv_pass;
    std::vector<std::string> autojoin;  // "#chan" or "#chan key"
};
enum class SS { DISCONNECTED, CONNECTING, REGISTERING, CONNECTED };
// /kickban and /tempban; waits in kb_wait while USERHOST looks the host up
struct KickBan { std::string chan, nick, reason; int minutes = 0; };
struct TempBan { std::string chan, mask; time_t until; };

// /e2e session with one nick (e2e.cpp); keys live in memory only
struct E2E {
    std::string nick, salt, key;  // key = scrypt(password, salt), "" until the password is typed
    bool mine = false;            // we sent the invite (salt)
    bool ok = false;              // both sides proved the same key: messages are encrypted
};

struct Core;
struct Server {
    ServerCfg cfg;
    Conn conn;
    SS state = SS::DISCONNECTED;
    std::string nick, umodes, network;
    std::string prefix_modes = "ov", prefix_chars = "@+", chantypes = "#&";
    std::set<std::string> caps, caps_ls;  // enabled / advertised
    std::set<std::string> hist_batches;    // open chathistory BATCH ids
    std::string inbuf;
    std::string chanmodes = "beI,k,l,imnpst";
    int max_modes = 3;                      // 005 MODES: mode changes per MODE line
    std::deque<KickBan> kb_wait;            // one per USERHOST sent, answered in order
    std::vector<TempBan> tempbans;          // lifted by Core::chan_timers
    std::string whois;  // irc_lower(nick) between its 311 and 318: the lines get a frame
    std::string scram_bare, scram_sig;  // SCRAM client-first-bare, expected server sig
    int sasl_step = 0, nick_tries = 0, backoff = 0;
    bool cap_done = false, sasl_ok = false;
    std::map<std::string, E2E> e2e;  // irc_lower(nick) -> session
    std::map<std::string, std::string> chan_keys;  // irc_lower(chan) -> key from /join or autojoin, for rejoins
    time_t last_rx = 0, reconnect_at = 0, ping_at = 0, ctcp_at = 0;
    long long ping_sent_ms = 0;
    double lag = 0;
    bool user_quit = false;
    bool away = false, auto_away = false;  // 305/306 state; auto_away: we set it
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

// ---- command help (/help, Tab completion, webchat completion) ---------------
struct CmdInfo {
    const char *name, *group, *usage, *summary, *details;
    std::vector<const char*> examples;
    const char* aliases = "";  // space separated, e.g. "j"
};
const std::vector<CmdInfo>& commands();
const CmdInfo* find_command(const std::string& name);  // name or alias, no "/"

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
    bool quit = false, dirty = true, keys_changed = false;
    // auto-away: nobody attached (daemon, no web client) or no input for a while
    bool attached = true;
    time_t detached_at = 0, last_input = time(nullptr);
    const time_t started = time(nullptr);  // uptime in the nick list
    void set_attached(bool a);
    std::string quit_msg = "iSekur IRC";
    std::function<void(Ev, Buffer*, const Line*)> listener;  // web frontend
    // /web start|stop: done by main() after the event pass, since a stop asked
    // from the browser must not tear down the connection it is still reading
    enum class WebReq { NONE, START, STOP } web_req = WebReq::NONE;
    Buffer* web_req_buf = nullptr;  // where /web was typed: gets the answer
    std::string web_url;  // "" = webchat stopped
    bool daemon = false;      // the webchat is the only way in: /web stop is refused
    bool tty = false;         // a terminal is attached (the TUI is drawing)
    bool detach_req = false;  // /detach or Alt+D: main() lets go of the terminal
    bool bell = false;  // the TUI rings the terminal bell on its next draw
    void ring(Server* s);  // private message, mention or invite, if bell_notifications
    // /e2e, /oper: the next line typed in pass_buf is a password, handed to pass_done
    Buffer* pass_buf = nullptr;
    std::string pass_label;  // the TUI's prompt, e.g. "Password for bob"
    std::function<void(const std::string&)> pass_done;
    void ask_pass(Buffer* b, const std::string& label, std::function<void(const std::string&)> done);

    void load_config();
    // [server.<name>] key, falling back to [general], then `def`
    std::string opt(const Server* s, const std::string& key, const std::string& def) const;
    bool opt_on(const Server* s, const std::string& key, bool def) const;
    void log_line(Buffer* b, const Line& l);
    std::string log_path(Buffer* b) const;
    void load_history(Buffer* b);
    ServerCfg server_cfg(const std::string& name) const;
    Server* add_server(const ServerCfg& sc);
    void load_flood(Server* s);  // flood_burst / flood_interval_ms
    void connect(Server* s);
    void disconnect(Server* s, const std::string& why);
    void on_readable(Server* s, short revents);
    void tick(time_t now);  // pings, timeouts, reconnects, auto-away
    void auto_away(Server* s, time_t now);
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
    void help(Buffer* b, const std::string& topic);  // help.cpp
    bool chanop(Buffer* b, const std::string& cmd, const std::string& args);  // false: not one
    void kickban(Server* s, Buffer* b, const KickBan& kb, const std::string& userhost);
    void chan_timers(Server* s, time_t now);  // /tempban expiry, /topicrotate

    void handle(Server* s, const Msg& m);  // irc.cpp
    void e2e_cmd(Buffer* b, const std::string& args);  // e2e.cpp
    void e2e_pass(Buffer* q, const std::string& pass);
    void e2e_ctcp(Server* s, const std::string& nick, const std::string& verb, const std::string& arg);
    void e2e_note(Server* s, const std::string& nick, LK k, const std::string& text);
};

long long now_ms();
std::string b64(const std::string& s);
bool rank_ok(const Server* s, const std::string& pfx, char need);  // chanops.cpp
std::string unb64(const std::string& s);  // "" on bad input
std::string e2e_key(const std::string& pass, const std::string& salt);  // e2e.cpp
std::string e2e_seal(const std::string& key, const std::string& plain);  // b64(nonce|ct|tag)
bool e2e_open(const std::string& key, const std::string& sealed, std::string& plain);
std::string fmt_time(time_t t, const std::string& fmt);
std::string version_string();  // "iSekur 0.2 (v0.2-4-g686b410)"
std::string ctcp_version();    // the full CTCP VERSION reply, with the OS
std::vector<std::string> split(const std::string& s, char sep);
std::string after_words(const std::string& s, size_t n);  // the text after the first n words
std::string default_config_path();  // $XDG_CONFIG_HOME or ~/.config, /isekur/isekur.conf
void mkdir_p(const std::string& path);  // 0700
std::string safe_name(const std::string& n);  // network name -> safe log file name
bool parse_log_line(const std::string& s, Line& l);
