// Core state shared by every frontend: servers, buffers, theme, commands.
#pragma once
#define ISEKUR_VERSION "0.5.1"
#include <atomic>
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
#include <poll.h>
#include <sys/types.h>
#include "net.h"

// ---- INI (config + themes) -------------------------------------------------
using Ini = std::map<std::string, std::map<std::string, std::string>>;
bool secret_key(const std::string& k);  // passwords and hashes: never shown
bool cfg_secret(const std::string& k);  // passwords sent back as typed: "enc:" in the file (config.cpp)
// "IDENTIFY pass" (and REGISTER, GHOST...) to NickServ/ChanServ as it is shown and logged: "IDENTIFY ********"
std::string mask_services(const std::string& tgt, const std::string& text);
// where that password starts, or npos; *end: where it ends (npos: the end of the text)
size_t services_secret(const std::string& tgt, const std::string& text, size_t* end = nullptr);
size_t input_secret(const std::string& buf, const std::string& line, size_t* end = nullptr);  // the same in a typed line ("/msg NickServ ...")
Ini ini_parse(const std::string& text);
bool ini_load(const std::string& path, Ini& out);
std::string ini_update(const std::string& text, const Ini& changes);
bool ini_save(const std::string& path, const Ini& changes, std::string& err);
std::string ini_drop(const std::string& text, const std::string& sec);  // text without [sec]
bool ini_drop_section(const std::string& path, const std::string& sec, std::string& err);
std::string ini_rename(const std::string& text, const std::string& from, const std::string& to);  // [from] is [to] now
bool ini_rename_section(const std::string& path, const std::string& from, const std::string& to, std::string& err);
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
extern std::string theme_dir;  // [general] theme_dir: searched first for <name>.theme
bool load_theme(const std::string& name, Theme& out, std::string& err);
std::vector<std::string> theme_names();  // installed themes, sorted
extern bool mirc_colors;  // the theme's [layout] mirc_colors (off: mono, colour codes in text show as plain text)

// mIRC formatting: walk text, emitting codepoints with their style.
void mirc_walk(const std::string& s, Style base,
               const std::function<void(uint32_t, const Style&)>& f);
std::string mirc_strip(const std::string& s);
std::string mirc_prefix(const std::string& s, size_t n);  // up to the n-th visible character, codes kept
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
std::string irc_lower(const std::string& s, int casemap = 0);  // 0 rfc1459, 1 ascii, 2 strict-rfc1459 (005 CASEMAPPING)
std::string escape_tag(const std::string& v);  // an IRCv3 tag value as it goes on the wire
std::string reply_snippet(const std::string& text);  // “the first 40 characters…” of a line, for replies and reactions
time_t parse_server_time(const std::string& iso);

// ---- buffers ---------------------------------------------------------------
enum class LK { MSG, ACTION, NOTICE, JOIN, PART, QUIT, NICK, MODE, TOPIC, INFO, ERROR };
struct Line {
    time_t ts;
    LK kind;
    std::string nick, text;
    bool self = false, hilight = false;
    bool nolog = false;  // /grep results: shown, never written to the log
    long long typed_at = 0;  // /typing: when it started typing itself out (ms), 0 = shown at once
    RGB color = -1;          // the nick's own color (Twitch "color" tag), -1 = theme's
    std::string msgid = {};  // IRCv3 msgid tag: what /reply, /react and REDACT point at
};
// /typing: how many of a line's len visible characters show at now; ~30 a second, never over 2s a line
inline size_t typed_chars(const Line& l, size_t len, long long now) {
    long long per = len ? std::min<long long>(33, 2000 / (long long)len) : 33;
    return now <= l.typed_at ? 0 : (size_t)((now - l.typed_at) / std::max<long long>(per, 1));
}
// what /quiet hides: other people coming, going and renaming
inline bool noise(const Line& l) {
    return !l.self && (l.kind == LK::JOIN || l.kind == LK::PART || l.kind == LK::QUIT || l.kind == LK::NICK);
}

enum class BT { STATUS, CHANNEL, QUERY, SHELL };  // SHELL: /shell, no server
struct Server;
struct Core;
struct VTerm;
// /shell: a shell on a pty and its screen (exec.cpp); the TUI draws vt
struct Term {
    pid_t pid = -1;
    int fd = -1;  // the pty master
    VTerm* vt = nullptr;
    int rows = 24, cols = 80;
    bool cursor = true;
    std::string host, rcfg;  // /attach host [config]: ssh to another iSekur, "" = /shell
    Core* c = nullptr;
    struct Buffer* b = nullptr;
    ~Term();  // hangs up the shell
};
// host: user@host, "" = unknown; account: logged in as (account-notify, account-tag, WHOX), "" = no or unknown;
// realname: WHOX, SETNAME; bot: the 005 BOT mode (WHOX flags, the bot tag)
struct NickEntry { std::string nick, prefix; bool away = false; std::string host, account, realname; bool bot = false; };
struct Buffer {
    int id;
    BT type;
    std::string name;
    Server* srv;
    std::deque<Line> lines;
    long long added = 0;                     // lines ever put in: lines[i] is line number added - lines.size() + i (the webchat asks for older ones by it)
    std::string topic, modes;
    std::map<std::string, NickEntry> nicks;  // key: irc_lower(nick)
    int activity = 0;                        // 0 none, 1 data, 2 msg, 3 hilight
    int unread = 0;                          // lines below the "new" marker
    int scroll = 0;                          // rows scrolled back (tui)
    bool joined = false;
    bool quiet = false;                      // /quiet: hide joins, parts, quits, nick changes
    bool quiet_typing = false;               // quiet only because /typing turned it on: /typing off undoes it
    bool collapsed = false;                  // a server tab hiding its channels in the tab bar (right click)
    std::unique_ptr<Term> term;              // a shell tab's running shell
    FILE* log = nullptr;                     // opened lazily when logging is on
    std::string log_day;                     // YYYY-MM-DD the open log belongs to
    // /topicrotate: topics set in turn every rot_min minutes
    std::vector<std::string> rot;
    int rot_min = 0;
    size_t rot_i = 0;
    time_t rot_at = 0;
    std::map<std::string, long long> typing;  // +typing: irc_lower(nick) -> until when (ms) they're typing
    time_t read_ts = 0;                       // draft/read-marker: read up to here (MARKREAD), on any client
    time_t read_sent = 0;                     // the newest MARKREAD we sent
    ~Buffer() { if (log) fclose(log); }
};

// ---- servers ---------------------------------------------------------------
struct ServerCfg {
    std::string name, host;
    int port = 6667;
    bool tls = false, tls_verify = true, autoconnect = true, auto_identify = true;  // auto_identify: SASL / NickServ on connect
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

// /exec: a running shell command (exec.cpp)
struct Job {
    pid_t pid = -1;
    int fd = -1;                   // its stdout+stderr, -1 once at EOF
    std::string cmd, partial, note;  // partial: an unfinished line; note: why it was stopped
    bool say = false, yes = false;   // -o: say the output in `to`; -y: even if it's long
    Buffer* to = nullptr;
    std::vector<std::string> out;    // -o: the lines, said when it ends
    size_t nlines = 0;
    time_t started = 0;
};

// /dcc: a file offered to us (DCC SEND) or by us (/dcc send), and its transfer (dcc.cpp)
struct Dcc {
    int id = 0;
    Server* srv = nullptr;
    std::string nick, file, host, path, err;  // file: the name offered (cleaned when it came to us); path: on this machine
    int port = 0;
    bool send = false;                        // we offer it
    // got: bytes from the start of the file (a resume starts at pos); queued: read from the file so far (send)
    unsigned long long size = 0, got = 0, pos = 0, queued = 0;  // size 0 = not said
    // RESUME: we asked to resume, waiting for their ACCEPT; WAIT: our offer, listening on lfd
    enum St { OFFER, RESUME, WAIT, RUN, DONE, FAILED } st = OFFER;
    Conn conn;
    int lfd = -1;
    FILE* f = nullptr;
    std::string acks;                         // send: a half-read ack
    time_t at = 0, last_rx = 0, ended = 0;    // offered, then started; the last progress; done or failed (the status bar shows it a while)
    unsigned long long rate = 0, rate_got = 0;  // bytes/s over the last second
    ~Dcc();
};

struct Core;
// twitch.cpp; with make notwitch twitch_host() is always false, so none of it runs
bool twitch_host(const std::string& host);  // irc.chat.twitch.tv and the like
struct Server {
    ServerCfg cfg;
    Conn conn;
    SS state = SS::DISCONNECTED;
    std::string nick, umodes, network;
    std::string last_nick;  // set by a /nick of ours, used on reconnect and restart ([server.x] last_nick) over cfg.nick
    std::string prefix_modes = "ov", prefix_chars = "@+", chantypes = "#&";
    std::set<std::string> caps, caps_ls;  // enabled / advertised
    std::map<std::string, std::string> cap_vals;  // CAP LS values: "sts" -> "port=6697,duration=..."
    std::map<std::string, std::string> isupport;  // 005 tokens: "MONITOR" -> "100", "WHOX" -> ""
    int casemap = 0;                       // 005 CASEMAPPING: irc_lower's mode
    std::string lower(const std::string& x) const { return irc_lower(x, casemap); }
    int sts_port = 0;                      // STS: TLS on this port, whatever cfg says (a policy or an upgrade)
    std::set<std::string> hist_batches;    // open chathistory BATCH ids
    // labeled-response: label -> the tab the command was typed in; batch -> the label's tab
    std::map<std::string, int> labels, batch_labels;
    int label_buf = 0, label_next = 1;     // label_buf: Core::exec is running for that tab: its lines get labels
    struct Multi { Msg open; std::string cmd; std::vector<std::string> lines; };
    std::map<std::string, Multi> multi;    // draft/multiline batches being received
    std::function<void(const char*, const std::string&)> raw_hook;  // /rawlog: every line in or out, set while its *raw tab is open
    std::set<std::string> who_auto;        // channels whose WHO we asked for on join: the replies aren't shown
    std::map<std::string, bool> online;    // /notify (MONITOR or ISON): irc_lower(nick) -> online
    time_t ison_at = 0;
    bool motd_done = false;                // the first 376/422 of this connection: /notify starts
    int ison_wait = 0;                     // our ISONs not yet answered: their 303s aren't shown
    std::map<std::string, std::pair<long long, bool>> typing_sent;  // our +typing per target: when, active
    std::string reg_pass;                  // /register: the password, for sasl_pass once it worked
    std::string inbuf;
    std::string chanmodes = "beI,k,l,imnpst";
    int max_modes = 3;                      // 005 MODES: mode changes per MODE line
    std::deque<KickBan> kb_wait;            // one per USERHOST sent, answered in order
    std::vector<TempBan> tempbans;          // lifted by Core::chan_timers
    std::string whois;  // irc_lower(nick) between its 311 and 318: the lines get a frame
    struct ListEntry { std::string chan; int users; std::string topic; };
    std::vector<ListEntry> list;  // /list: the 322s, kept after the 323 so /list sort redraws them
    bool listing = false;
    char list_sort = 'u';        // 'u' users, 'n' name
    bool list_rev = false;       // users ascending / names Z-A
    std::string scram_bare, scram_sig;  // SCRAM client-first-bare, expected server sig
    int sasl_step = 0, nick_tries = 0, backoff = 0;
    bool cap_done = false, sasl_ok = false;
    std::map<std::string, E2E> e2e;  // irc_lower(nick) -> session
    std::map<std::string, std::string> chan_e2e;  // irc_lower(chan) -> key from /e2e #chan, memory only
    std::map<std::string, std::string> chan_keys;  // irc_lower(chan) -> key from /join or autojoin, for rejoins
    time_t last_rx = 0, reconnect_at = 0, ping_at = 0, ctcp_at = 0;
    std::string to_self;  // echo-message: a message to our own nick comes twice (echo + delivery); the first one's msgid or text
    time_t to_self_at = 0;
    time_t ident_at = 0;   // our automatic NickServ IDENTIFY: its echo and NickServ's answers go to the server tab for a while
    bool tag_ok(const std::string& tag) const;  // a client tag (no +) isn't in 005 CLIENTTAGDENY, and message-tags is on
    int typed_in = 0;      // the buffer id a /command for this server was last typed in: its replies go there
    time_t typed_at = 0;
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
    // Twitch chat is IRC with its own CAPs, tags and commands: irc.chat.twitch.tv
    bool twitch() const { return twitch_host(cfg.host); }
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
// check_updates: the latest release's version, read on a thread; `done` publishes `ver`
struct UpdateCheck {
    std::string ver;
    std::atomic<bool> done{false};
};
enum class Ev { LINE, BUFFER, CLOSE, THEME, CLEAR, FOCUS, NOTE, BELL, SPLIT, LOCK, UPDATE };  // CLEAR: b->lines was emptied (the webchat reloads them); FOCUS: the webchat shows b too; NOTE: a popup in the webchat (l: nick = title, text); BELL: ring() (b is null); SPLIT: the terminal's /split changed (b is null); LOCK: locked or lock_msg changed (b is null)

struct Core {
    Ini cfg, unsaved;  // unsaved: /set changes not yet written by /save
    Ini set_was;       // /set: a key's value before its first unsaved change; set back to it, it's no longer unsaved
    Ini defaults;      // iSekur.conf.example (compiled in) minus its example servers
    // what /set lists: every key in the config or the template, the config's value winning
    struct Setting { std::string key, val; };  // key: "nick", "web.port", "server.x.host"
    std::vector<Setting> settings() const;
    // every key, for the settings windows (terminal and webchat): val is "" when not set
    // choices: what Space / the webchat's switch or list steps through: on/off (true/false, yes/no), the themes
    // cat: the settings window's category: "general/Look", "server.libera/Login", "web";
    // head: the header over its rows ("Look", "⇢ libera · Login", "[web]")
    // shared: a [general] key under a network: not set there (val ""), def is [general]'s; over: under [general],
    // the networks that override it ("libera: off")
    struct SetRow { std::string sec, key, val, def; bool unsaved, secret; std::vector<std::string> choices; std::string grp, cat, head; bool shared; std::string over; };
    std::vector<SetRow> setting_rows() const;
    bool settings_win = false;  // /set alone in the terminal: the TUI opens its settings window
    bool settings_servers = false;  // /servers: that window, at the first server
    bool keys_win = false;      // /keys in the terminal: the TUI opens its key list
    bool theme_win = false;     // /theme alone in the terminal: the TUI opens its theme picker
    bool use_theme(const std::string& name, std::string& err);  // switch the theme, terminal and webchat, silently
    std::string cfg_path;
    Theme theme;
    std::vector<std::unique_ptr<Server>> servers;
    std::vector<std::unique_ptr<Buffer>> bufs;  // tab order
    Buffer* active = nullptr;
    // /split: the terminal's second pane shows this tab; focus (F6, Alt+O, a click) swaps it with active,
    // so input, the nicklist and the topic always follow the focused pane. split_first: it's the left/top one
    Buffer* split_buf = nullptr;
    bool split_first = false;
    int split_mode = 0;  // 0 by the window's width, 1 side by side, 2 one over the other
    int split_pm = 500;  // the first pane's share, per mille (the divider drag); a browser that connects starts like the terminal
    void split_cmd(Buffer* b, const std::string& args);
    int next_id = 1;
    size_t max_lines = 2000;  // [general] scrollback: lines kept per tab
    bool quit = false, dirty = true, cfg_changed = false;  // cfg_changed: the TUI re-reads keys, mouse, colors
    // auto-away: nobody attached (daemon, no web client) or no input for a while
    bool attached = true;
    time_t detached_at = 0, last_input = time(nullptr);
    const time_t started = time(nullptr);  // uptime in the nick list
    void set_attached(bool a);
    std::string quit_msg;  // from /quit text; else quit_message
    std::string bye(const Server* s, const std::string& key) const;  // quit_message / part_message
    std::function<void(Ev, Buffer*, const Line*)> listener;  // web frontend
    // /web start|stop|restart: done by main() after the event pass, since a stop asked
    // from the browser must not tear down the connection it is still reading
    enum class WebReq { NONE, START, STOP, RESTART } web_req = WebReq::NONE;
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
    bool pass_hidden = true;  // false: a login name, shown as typed
    bool pass_web = false;    // asked by the webchat: its answer comes from there, the TUI shows no dialog
    // the TUI asks in a dialog box: pass_title on its border, pass_info above the field ("!..." = an error)
    std::string pass_title;
    std::vector<std::string> pass_info;
    // lock = on with lock_password_hash: the terminal and the webchat show only a password box at
    // startup, on every attach, after /lock and after lock_idle minutes without input (exec.cpp)
    bool locked = false;
    void set_lock(bool on);
    // the lock screen's animation, in the terminal (the webchat plays its own off the same events):
    // 'l' it locks, 'f' a wrong password, 'u' it unlocks; 0 when none is running
    char lock_anim = 0;
    long long lock_anim_at = 0;  // both sides at once: the webchat gets nothing of the session while locked
    std::string lock_msg;  // the box's error line ("!...")
    bool lock_on() const;
    void lock_cmd(Buffer* b, const std::string& args);
    void unlock(const std::string& pass);
    bool asking(const Buffer* b) const { return b && pass_buf == b && !pass_web; }  // the TUI's dialog is up on b
    bool twitch_handle(Server* s, const Msg& m, time_t ts);  // Twitch-only commands; true: handled
    void twitch_cmd(Buffer* b, const std::string& arg);  // /twitch [token]
    void ask_pass(Buffer* b, const std::string& label, std::function<void(const std::string&)> done, bool hidden = true,
                  const std::string& title = "", std::vector<std::string> info = {});

    void load_config();
    void reload(Buffer* b);  // /reload
    // [server.<name>] key, falling back to [general], then `def`
    std::string opt(const Server* s, const std::string& key, const std::string& def) const;
    bool muted(const Buffer* b) const;  // in [server.X] mute: no hilights, alerts or copies to window 1 from it
    bool opt_on(const Server* s, const std::string& key, bool def) const;
    void log_line(Buffer* b, const Line& l);
    std::string log_path(Buffer* b) const;
    std::string log_folder(const Server* s) const;  // <log_dir>/<network>
    bool rename_server(Buffer* b, const std::string& from, const std::string& to);  // /server rename
    void load_history(Buffer* b);
    // keep_history (log off): each tab's last log_history lines in <config>.history, saved on quit and
    // every 5 minutes, put back when the tab opens again. Keyed "server\tname"; tabs not reopened stay
    std::map<std::string, std::vector<Line>> hist_cache;
    bool hist_loaded = false;
    long long hist_saved = 0;
    void save_history();
    void hist_read();  // <config>.history into hist_cache, once
    ServerCfg server_cfg(const std::string& name) const;
    Server* add_server(const ServerCfg& sc);
    void load_flood(Server* s);  // flood_burst / flood_interval_ms
    void connect(Server* s);
    void disconnect(Server* s, const std::string& why);
    void on_readable(Server* s, short revents);
    void tick(time_t now);  // pings, timeouts, reconnects, auto-away
    void auto_away(Server* s, time_t now);
    bool away_wanted(Server* s, time_t now) const;
    void sts_load();                    // the STS policies file next to the config
    std::map<std::string, std::pair<int, time_t>> sts;  // host -> TLS port, until
    void sts_save();
    void notify_cmd(Buffer* b, const std::string& args);  // /notify [add|del] nick: MONITOR (or ISON) watch list
    void notify_start(Server* s);       // after 001: MONITOR + the list
    void notify_seen(Server* s, const std::string& nick, bool on, const std::string& host = "");
    void typed(Buffer* b, const std::string& input);  // the input line changed: +typing=active / done (send_typing)
    void mark_read(Server* s, time_t now);  // draft/read-marker: MARKREAD for the tabs on screen
    Line* by_msgid(Buffer* b, const std::string& id);
    void say_multi(Buffer* b, const std::vector<std::string>& lines);  // draft/multiline: a paste as one message
    int drain();            // flush send queues; ms until the next one is due
    bool ignored(const Server* s, const std::string& prefix) const;  // per network: [server.X] ignore

    Buffer* find(Server* s, const std::string& name);
    Buffer* open(Server* s, BT t, const std::string& name);
    void close(Buffer* b, const std::string& part_msg = "");  // a channel: PART (part_msg, else part_message) and the tab goes
    void select(Buffer* b);
    int index_of(Buffer* b) const;
    void add(Buffer* b, Line l);
    void print(Buffer* b, LK k, const std::string& text);
    void rawlog(Buffer* b, const std::string& arg);  // /rawlog [on|off]: the server's *raw tab
    void notify(Ev e, Buffer* b, const Line* l = nullptr);

    void exec(Buffer* b, const std::string& input);  // "/cmd" or text
    void say(Buffer* b, const std::string& text, bool action = false);
    void help(Buffer* b, const std::string& topic);  // help.cpp
    void guide(Buffer* from, const std::string& arg);  // help.cpp: the *guide tab; "", N, next, prev, on, off, close
    int guide_page = 0;
    bool chanop(Buffer* b, const std::string& cmd, const std::string& args);  // false: not one
    void kickban(Server* s, Buffer* b, const KickBan& kb, const std::string& userhost);
    void chan_timers(Server* s, time_t now);  // /tempban expiry, /topicrotate
    std::vector<std::pair<std::string, std::string>> nick_menu(Buffer* b, const std::string& nick) const;  // label, command
    std::vector<std::pair<std::string, std::string>> tab_menu(Buffer* b) const;  // a channel tab's
    void about(Buffer* from);
    std::string update_ver;  // a newer release than this one, once check_updates found it (UPDATE: it changed)
    std::shared_ptr<UpdateCheck> upd;  // while a check runs
    time_t upd_at = 0;  // the next check
    void check_update(time_t now);  // tick(): starts one a day, takes in a finished one
    void remember_nick(Server* s, const std::string& nick);  // last_nick, kept in the config for the next start
    int typing_quiet(bool on);  // /typing on|off: quiet mode with it in the channel tabs; returns how many changed  // /about: the *about tab; main.cpp sweeps its banner while it's shown
    long long about_t0 = 0;
    long long typing_until = 0;  // /typing: the last line still typing ends then (main.cpp redraws until)
    std::vector<std::string> about_full;  // its lines in full; about_typed get typed out in turn
    std::vector<int> about_typed;
    void list_table(Server* s);  // chanops.cpp: the collected /list as a table in *channels
    void list_sort(Buffer* b, const std::string& by);  // "name", "users" or "" (flip the current one)
    void fold(Buffer* status);  // a server tab: hide or show its channels in the tab bar and the webchat list

    void handle(Server* s, const Msg& m);  // irc.cpp
    std::vector<std::unique_ptr<Dcc>> dccs;  // dcc.cpp
    int dcc_next = 1;
    void dcc_ctcp(Server* s, const std::string& nick, const std::string& arg);  // CTCP DCC SEND/RESUME/ACCEPT
    std::string dcc_dir(const Server* s = nullptr) const;  // dcc_dir, ~ expanded
    void dcc_cmd(Buffer* b, const std::string& args);
    void dccs_poll(const std::vector<pollfd>& fds, size_t from);  // their sockets from fds[from] on
    void dcc_tick(time_t now);  // rates, stalled downloads
    std::vector<Job> jobs;  // /exec, exec.cpp
    bool from_web = false;  // exec() called for the webchat: /exec needs exec_web
    void exec_cmd(Buffer* b, const std::string& args);
    void shell_cmd(Buffer* b);                       // /shell: asks the system login first
    void shell_login(Buffer* sb, const std::string& err);  // the login dialog, again after a wrong one
    void shell_start(Buffer* sb, const std::string& host = "", const std::string& rcfg = "");  // host: ssh -t host isekur [-c rcfg] --attach
    void attach_cmd(Buffer* b, const std::string& args);  // /attach host [config]: a remote iSekur in a tab
    bool os_login(const std::string& login, const std::string& pass, std::string& why);
    void shell_line(Buffer* b, const std::string& l);  // a line typed in a shell tab with no shell
    void term_resize(Term* t, int rows, int cols);
    void terms_poll(const std::vector<pollfd>& fds, size_t from);  // their ptys from fds[from] on
    void jobs_poll(const std::vector<pollfd>& fds, size_t from);  // their pipes from fds[from] on
    void job_read(Job& j);
    void job_line(Job& j, const std::string& raw);
    void e2e_cmd(Buffer* b, const std::string& args);  // e2e.cpp
    void e2e_pass(Buffer* q, const std::string& pass);
    void e2e_ctcp(Server* s, const std::string& nick, const std::string& verb, const std::string& arg);
    void e2e_note(Server* s, const std::string& nick, LK k, const std::string& text);
    void e2e_chan(Buffer* b, const std::string& pass);
    void e2e_keep(Server* s, bool now);
};

long long now_ms();
std::string b64(const std::string& s);
bool rank_ok(const Server* s, const std::string& pfx, char need);  // chanops.cpp
std::string unb64(const std::string& s);  // "" on bad input
std::string e2e_key(const std::string& pass, const std::string& salt);  // e2e.cpp
std::string e2e_seal(const std::string& key, const std::string& plain, const std::string& aad = "");  // b64(nonce|ct|tag)
bool e2e_open(const std::string& key, const std::string& sealed, std::string& plain, const std::string& aad = "");
bool e2e_on(const Buffer* b);  // a 🔒 tab: an encrypted query, or a channel with a key
std::string fmt_time(time_t t, const std::string& fmt);
std::string human_bytes(unsigned long long n);  // 512M, 3.2G
std::string status_info_json(const Core& c);  // tui.cpp: the status_info items for the webchat, [[[text, fg, bg], ...], ...]
std::string version_string();  // "iSekur 0.2 (v0.2-4-g686b410)"
std::string ctcp_version();    // the full CTCP VERSION reply, with the OS
std::vector<std::string> split(const std::string& s, char sep);
std::string after_words(const std::string& s, size_t n);  // the text after the first n words
std::vector<std::string> banner_lines(const Theme& t, bool mono = false, double shine = -99, int light = -1);  // the startup banner, theme colors or gray; light: 1 on a light background whatever the theme says
// typed: the lines /about types out (the tagline, the support channel)
std::vector<std::string> twitch_banner();  // twitch.cpp: a Twitch server tab's banner
void twitch_login(Server* s);  // PASS + Twitch CAPs on connect
bool twitch_user(Buffer* b, const Msg& m, Line& l);  // a message's Twitch tags; true: nick list changed
std::string release_tag(const std::string& head);  // "0.6" from a HEAD reply's Location: …/releases/tag/v0.6
bool version_newer(const std::string& a, const std::string& b);  // a > b, dotted numbers
std::vector<std::string> about_lines(const Theme& t, double shine = -99, bool web = false, std::vector<int>* typed = nullptr, const std::string& update = "");  // /about: the banner + version, author, links
std::string fresh_config();  // the compiled-in template as a first config file
std::string default_config_path();  // $XDG_CONFIG_HOME or ~/.config, /isekur/isekur.conf
void mkdir_p(const std::string& path);  // 0700
// the settings' keys (core.cpp): scope 'g' [general] only, 'n' a network only, 'b' both (a network overrides [general])
struct KeyInfo { const char* key; const char* grp; char scope; };
const std::vector<KeyInfo>& setting_keys();
const KeyInfo* key_info(const std::string& key);                 // nullptr: not one iSekur knows
std::vector<std::pair<std::string, std::string>> conf_keys();   // iSekur.conf.example's [section] keys, commented or not
bool server_name_ok(const std::string& n);  // letters, digits, . _ -
std::string safe_name(const std::string& n);  // network name -> safe log file name
bool parse_log_line(const std::string& s, Line& l);
std::string log_fmt(const Line& l);  // a Line as one log row
std::string raw_redact(const std::string& line);  // /rawlog: a line with its password or SASL payload blanked
