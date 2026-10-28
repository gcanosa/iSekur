// Buffers, config loading and user commands.
#include <algorithm>
#include <cstdlib>
#include <unistd.h>
#include <pwd.h>
#include <glob.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/utsname.h>
#include "core.h"
#include "version.h"
#include "conf_template.h"

static const size_t MAX_LINES = 2000;

std::string default_config_path() {
    if (const char* x = getenv("XDG_CONFIG_HOME")) return std::string(x) + "/isekur/isekur.conf";
    const char* h = getenv("HOME");
    return std::string(h ? h : ".") + "/.config/isekur/isekur.conf";
}

void Core::load_config() {
    if (cfg_path.empty()) cfg_path = default_config_path();
    bool found = ini_load(cfg_path, cfg), created = false;
    if (!found) {  // first start: every setting in a file to edit or /set
        mkdir_p(cfg_path.substr(0, cfg_path.rfind('/')));
        mode_t old = umask(077);
        FILE* f = fopen(cfg_path.c_str(), "wx");
        umask(old);
        if (f) {
            std::string t = fresh_config();
            created = fwrite(t.data(), 1, t.size(), f) == t.size();
            created = (fclose(f) == 0) && created;
            found = created && ini_load(cfg_path, cfg);
        }
    }
    defaults = ini_parse(conf_template);
    for (auto it = defaults.begin(); it != defaults.end();)
        it = it->first.rfind("server.", 0) == 0 ? defaults.erase(it) : std::next(it);
    defaults["general"]["nick"] = defaults["general"]["altnick"] = defaults["web"]["user"] = "";  // placeholders
    std::string err;
    if (!load_theme(ini_get(cfg, "general", "theme", "default"), theme, err)) {
        if (!load_theme("default", theme, err)) theme = Theme{};
    }
    Buffer* home = open(nullptr, BT::STATUS, "cmd");
    for (auto& l : banner_lines(theme, opt_on(nullptr, "banner_mono", false))) print(home, LK::INFO, l);
    if (created)
        print(home, LK::INFO, "Wrote " + cfg_path + " with every setting: edit it, or /set key value and /save. "
                              "Add a server with /server irc.example.org 6697 -tls");
    else if (!found)
        print(home, LK::INFO, "No config at " + cfg_path +
                                  " - see iSekur.conf.example. Try /server irc.example.org 6697 -tls");
    print(home, LK::INFO, "Welcome to " + version_string() + ". /help lists commands.");

    for (auto& [sec, kv] : cfg) {
        if (sec.rfind("server.", 0) != 0) continue;
        Server* s = add_server(server_cfg(sec.substr(7)));
        if (s->cfg.autoconnect) connect(s);
    }
}

// The startup banner: each letter in a color of the theme's nick palette, the
// shadow in its (dim) timestamp color; mono: the text color and gray.
// mIRC \x04 colors, so the webchat shows it too.
std::vector<std::string> banner_lines(const Theme& t, bool mono) {
    static const char* art[] = {
        u8"██╗███████╗███████╗██╗  ██╗██╗   ██╗██████╗ ",
        u8"██║██╔════╝██╔════╝██║ ██╔╝██║   ██║██╔══██╗",
        u8"██║███████╗█████╗  █████╔╝ ██║   ██║██████╔╝",
        u8"██║╚════██║██╔══╝  ██╔═██╗ ██║   ██║██╔══██╗",
        u8"██║███████║███████╗██║  ██╗╚██████╔╝██║  ██║",
        u8"╚═╝╚══════╝╚══════╝╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝"};
    static const int ends[] = {3, 11, 19, 27, 36, 44};  // column after each letter: I S E K U R
    auto code = [](RGB c) { return c < 0 ? std::string("\x0f") : "\x04" + rgb_hex(c).substr(1); };
    RGB shade = mono ? 0x808080 : t.get("timestamp").fg;
    std::vector<std::string> out;
    for (const char* row : art) {
        std::string s = row, line, cur;
        size_t i = 0;
        for (int col = 0, letter = 0; i < s.size(); col++) {
            while (col >= ends[letter] && letter < 5) letter++;
            uint32_t cp = utf8_next(s, i);
            RGB c = cp != 0x2588 ? shade : mono || t.nicks.empty() ? t.get("default").fg : t.nicks[(size_t)letter * t.nicks.size() / 6];
            std::string want = cp == ' ' ? cur : code(c);
            if (want != cur) line += want, cur = want;
            utf8_put(line, cp);
        }
        out.push_back(line);
    }
    return out;
}

// the template as a first config: its example servers commented out, and the
// identity placeholders (mynick, me) empty, so they fall back to your login
std::string fresh_config() {
    std::istringstream in(conf_template);
    std::string out, line, sec;
    while (std::getline(in, line)) {
        std::string t = line.substr(0, line.find_first_of("=;#"));
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (!line.empty() && line[0] == '[') sec = line.substr(1, line.find(']') - 1);
        if (sec.rfind("server.", 0) == 0 && !line.empty() && line[0] != '#' && line[0] != ';') line = "; " + line;
        else if ((sec == "general" && (t == "nick" || t == "altnick")) || (sec == "web" && t == "user"))
            line = line.substr(0, line.find('=') + 1);
        out += line + "\n";
    }
    return out;
}

static bool secret_key(const std::string& k) {
    return k.find("pass") != std::string::npos || k.find("hash") != std::string::npos;
}

std::vector<Core::Setting> Core::settings() const {
    Ini all = defaults;
    for (auto& [sec, kv] : cfg)
        for (auto& [k, v] : kv)
            if (!v.empty() || !all[sec].count(k)) all[sec][k] = v;  // "key =" keeps the default
    std::vector<Setting> out;
    for (auto& [sec, kv] : all)
        for (auto& [k, v] : kv) {
            bool set = !ini_get(cfg, sec, k).empty();
            std::string val = !set && v.empty() ? "(not set)"
                              : secret_key(k) && !v.empty() ? "********"
                                                            : v;
            if (!set && !v.empty()) val += "  (default)";
            if (unsaved.count(sec) && unsaved.at(sec).count(k)) val += "  (unsaved)";
            out.push_back({(sec == "general" ? "" : sec + ".") + k, val});
        }
    return out;
}

// [server.<name>] with [general] fallbacks; also re-read by /set
ServerCfg Core::server_cfg(const std::string& name) const {
    const char* login = getenv("USER");
    std::string gnick = ini_get(cfg, "general", "nick", login ? login : "isekur");
    std::string sec = "server." + name;
    ServerCfg sc;
    sc.name = name;
    sc.host = ini_get(cfg, sec, "host", sc.name);
    sc.tls = ini_get(cfg, sec, "tls", "false") == "true";
    sc.port = atoi(ini_get(cfg, sec, "port", sc.tls ? "6697" : "6667").c_str());
    sc.tls_verify = ini_get(cfg, sec, "tls_verify", "true") == "true";
    sc.autoconnect = ini_get(cfg, sec, "autoconnect", "true") == "true";
    sc.nick = ini_get(cfg, sec, "nick", gnick);
    sc.altnick = ini_get(cfg, sec, "altnick", ini_get(cfg, "general", "altnick", sc.nick + "_"));
    sc.user = ini_get(cfg, sec, "user", ini_get(cfg, "general", "user", sc.nick));
    sc.realname = ini_get(cfg, sec, "realname", ini_get(cfg, "general", "realname", "iSekur user"));
    sc.pass = ini_get(cfg, sec, "password");
    sc.sasl_user = ini_get(cfg, sec, "sasl_user");
    sc.sasl_pass = ini_get(cfg, sec, "sasl_pass");
    sc.sasl_mech = ini_get(cfg, sec, "sasl_mech");
    sc.cert = ini_get(cfg, sec, "cert");
    sc.key = ini_get(cfg, sec, "key");
    sc.nickserv_pass = ini_get(cfg, sec, "nickserv_pass");
    for (auto& e : split(ini_get(cfg, sec, "autojoin"), ','))
        if (auto w = split(e, ' '); !w.empty()) sc.autojoin.push_back(w[0] + (w.size() > 1 ? " " + w[1] : ""));
    return sc;
}

Server* Core::add_server(const ServerCfg& sc) {
    auto s = std::make_unique<Server>();
    s->cfg = sc;
    s->nick = sc.nick;
    Server* sp = s.get();
    servers.push_back(std::move(s));
    sp->status = open(sp, BT::STATUS, sc.name);
    load_flood(sp);
    for (auto& e : split(ini_get(cfg, "server." + sc.name, "e2e_keys"), ','))  // "#chan:b64key", see e2e_keep
        if (size_t c = e.rfind(':'); c != std::string::npos && unb64(e.substr(c + 1)).size() == 32)
            sp->chan_e2e[irc_lower(e.substr(0, c))] = unb64(e.substr(c + 1));
    return sp;
}

void Core::load_flood(Server* s) {
    s->flood_burst = std::max(1, atoi(opt(s, "flood_burst", "5").c_str()));
    s->flood_ms = atoi(opt(s, "flood_interval_ms", "2000").c_str());
}

// The build id only shows when it says more than the version (untagged or
// modified builds); release tarballs without git just say "iSekur 0.3".
std::string version_string() {
    std::string build = ISEKUR_BUILD, v = "iSekur " ISEKUR_VERSION;
    return build.empty() || build == "v" ISEKUR_VERSION ? v : v + " (" + build + ")";
}

// irssi/BitchX style: client, version, where it runs, a tagline and a link
std::string ctcp_version() {
    utsname u{};
    std::string os = uname(&u) == 0 ? std::string(u.sysname) + " " + u.release + "/" + u.machine : "unknown OS";
    return version_string() + " :: " + os +
           " :: modern, secure, lightweight & powerful IRCv3 client :: https://github.com/gcanosa/iSekur";
}

// ---- buffers ---------------------------------------------------------------
Buffer* Core::find(Server* s, const std::string& name) {
    std::string n = irc_lower(name);
    for (auto& b : bufs)
        if (b->srv == s && b->type != BT::STATUS && irc_lower(b->name) == n) return b.get();
    return nullptr;
}

int Core::index_of(Buffer* b) const {
    for (size_t i = 0; i < bufs.size(); i++)
        if (bufs[i].get() == b) return (int)i;
    return -1;
}

Buffer* Core::open(Server* s, BT t, const std::string& name) {
    if (t != BT::STATUS)
        if (Buffer* b = find(s, name)) return b;
    auto b = std::make_unique<Buffer>();
    b->id = next_id++, b->type = t, b->name = name, b->srv = s;
    Buffer* bp = b.get();
    // keep each server's buffers together, status first
    size_t pos = bufs.size();
    for (size_t i = 0; i < bufs.size(); i++)
        if (bufs[i]->srv == s && s) pos = i + 1;
    bufs.insert(bufs.begin() + (long)pos, std::move(b));
    if (s && t != BT::STATUS) load_history(bp);
    if (!active) active = bp;
    dirty = true;
    notify(Ev::BUFFER, bp);
    return bp;
}

void Core::close(Buffer* b) {
    Server* s = b->srv;
    if (b->type == BT::STATUS) {
        if (!s) { print(b, LK::ERROR, "Can't close the main window; use /quit"); return; }
        if (s->state != SS::DISCONNECTED) {
            s->user_quit = true;
            s->send_now("QUIT :" + bye(s, "quit_message"));
            if (s->conn.st == Conn::OPEN) s->conn.pump(0, s->inbuf);  // best-effort flush
            disconnect(s, "");
        }
        for (size_t i = bufs.size(); i-- > 0;)
            if (bufs[i]->srv == s && bufs[i].get() != b) close(bufs[i].get());
    } else if (b->type == BT::CHANNEL && b->joined && s) {
        s->send("PART " + b->name + " :" + bye(s, "part_message"));
    }
    if (pass_buf == b) pass_buf = nullptr, pass_done = nullptr;
    bool drop_server = s && b->type == BT::STATUS;
    int i = index_of(b);
    notify(Ev::CLOSE, b);
    if (active == b) active = bufs[(size_t)(i > 0 ? i - 1 : 1 % bufs.size())].get();
    bufs.erase(bufs.begin() + i);
    if (active == b) active = bufs.empty() ? nullptr : bufs[0].get();
    if (drop_server)  // after the buffer: listeners may still read b->srv
        servers.erase(std::remove_if(servers.begin(), servers.end(),
                                     [&](auto& p) { return p.get() == s; }),
                      servers.end());
    dirty = true;
}

void Core::select(Buffer* b) {
    if (!b) return;
    if (active && active != b && active->unread) active->unread = 0, notify(Ev::BUFFER, active);
    active = b;
    b->activity = 0;
    dirty = true;
    notify(Ev::BUFFER, b);
}

void Core::add(Buffer* b, Line l) {
    if (!b) return;
    b->lines.push_back(std::move(l));
    if (b->lines.size() > MAX_LINES) b->lines.pop_front();
    if (b->scroll) b->scroll++;  // keep the scrolled view still
    const Line& ln = b->lines.back();
    int lvl = ln.hilight ? 3 : (ln.kind == LK::MSG || ln.kind == LK::ACTION) && !ln.self ? 2 : b->quiet && noise(ln) ? 0 : 1;
    // the "new" marker goes above the first message you missed, then counts everything after it
    if (b->unread || (b != active && lvl >= 2)) b->unread++;
    if (b != active && lvl > b->activity) { b->activity = lvl; notify(Ev::BUFFER, b); }
    dirty = true;
    log_line(b, ln);
    notify(Ev::LINE, b, &ln);
    // copy highlights and private messages to the first window, so whatever
    // happened while you were away is in one place
    Buffer* home = bufs.front().get();
    if (ln.hilight && b != home && b->srv && opt_on(b->srv, "hilight_window", true)) {
        Line copy = ln;
        copy.text = "\x02[" + b->srv->cfg.name + "/" + b->name + "]\x02 " + ln.text;
        add(home, copy);
    }
}

void Core::fold(Buffer* st) {
    if (st->type != BT::STATUS || !st->srv) return;
    st->collapsed = !st->collapsed, dirty = true;
    notify(Ev::BUFFER, st);
}

// what you leave with: the config's quit_message / part_message, else what iSekur is
std::string Core::bye(const Server* s, const std::string& key) const {
    return opt(s, key, "iSekur " ISEKUR_VERSION " :: colorful terminal IRC client :: https://github.com/gcanosa/iSekur");
}

std::string Core::opt(const Server* s, const std::string& key, const std::string& def) const {
    std::string g = ini_get(cfg, "general", key, def);
    return s ? ini_get(cfg, "server." + s->cfg.name, key, g) : g;
}

// /reload: the config file again, without restarting. Unsaved /set changes stay
// on top; servers pick up their settings like /set does (on the next connect),
// new [server.*] sections are added and connected when autoconnect is on.
void Core::reload(Buffer* b) {
    Ini fresh;
    if (!ini_load(cfg_path, fresh)) { print(b, LK::ERROR, "Can't read " + cfg_path); return; }
    for (auto& [sec, kv] : unsaved)
        for (auto& [k, v] : kv) fresh[sec][k] = v;
    cfg = fresh;
    std::string err;
    if (!load_theme(ini_get(cfg, "general", "theme", "default"), theme, err)) print(b, LK::ERROR, err);
    notify(Ev::THEME, nullptr);
    for (auto& sv : servers) sv->cfg = server_cfg(sv->cfg.name), load_flood(sv.get());
    for (auto& [sec, kv] : cfg) {
        if (sec.rfind("server.", 0) != 0) continue;
        std::string name = sec.substr(7);
        bool known = false;
        for (auto& sv : servers) known = known || sv->cfg.name == name;
        if (known) continue;
        Server* s = add_server(server_cfg(name));
        print(b, LK::INFO, "New server " + name + (s->cfg.autoconnect ? ": connecting" : " (autoconnect off: /connect " + name + ")"));
        if (s->cfg.autoconnect) connect(s);
    }
    cfg_changed = dirty = true;
    size_t n = 0;
    for (auto& [sec, kv] : unsaved) n += kv.size();
    print(b, LK::INFO, "Reloaded " + cfg_path + (n ? " (" + std::to_string(n) + " unsaved /set changes kept on top)" : "") +
                           ". Server settings apply on the next /connect, [web] after /web stop and /web start");
}

void Core::ask_pass(Buffer* b, const std::string& label, std::function<void(const std::string&)> done) {
    pass_buf = b, pass_label = label, pass_done = std::move(done);
    print(b, LK::INFO, "Type the " + label + " (hidden; an empty line cancels)");
}

void Core::ring(Server* s) {
    if (opt_on(s, "bell_notifications", true)) bell = dirty = true;
}

bool Core::opt_on(const Server* s, const std::string& key, bool def) const {
    std::string v = opt(s, key, def ? "on" : "off");
    return v == "on" || v == "true" || v == "yes" || v == "1";
}

void mkdir_p(const std::string& path) {
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0700);
}

// Names come from the network (query nicks): keep them inside the log dir.
std::string safe_name(const std::string& n) {
    std::string o = irc_lower(n);
    for (auto& ch : o)
        if (ch == '/' || ch == '\\' || (unsigned char)ch < 0x20) ch = '_';
    if (o.empty() || o[0] == '.') o.insert(0, "_");
    return o;
}

std::string Core::log_path(Buffer* b) const {
    std::string dir = opt(b->srv, "log_dir", cfg_path.substr(0, cfg_path.rfind('/') + 1) + "logs");
    if (dir.rfind("~/", 0) == 0)
        if (const char* h = getenv("HOME")) dir = h + dir.substr(1);
    return dir + "/" + safe_name(b->srv->cfg.name) + "/" + safe_name(b->name) + ".log";
}

// [general] log = on writes channels and queries to <log_dir>/<server>/<name>.log
void Core::log_line(Buffer* b, const Line& l) {
    if (l.nolog || !b->srv || b->type == BT::STATUS || !opt_on(b->srv, "log", false)) return;
    // log_rotate (default on): yesterday's lines move to <name>.YYYY-MM-DD.log
    bool rotate = opt_on(b->srv, "log_rotate", true);
    std::string today = fmt_time(time(nullptr), "%Y-%m-%d");
    if (b->log && rotate && b->log_day != today) {
        fclose(b->log), b->log = nullptr;
        std::string path = log_path(b);
        rename(path.c_str(), (path.substr(0, path.size() - 4) + "." + b->log_day + ".log").c_str());
    }
    if (!b->log) {
        std::string path = log_path(b);
        mkdir_p(path.substr(0, path.rfind('/')));
        struct stat st;
        if (rotate && stat(path.c_str(), &st) == 0) {  // left over from an earlier day
            std::string day = fmt_time(st.st_mtime, "%Y-%m-%d");
            if (day != today) rename(path.c_str(), (path.substr(0, path.size() - 4) + "." + day + ".log").c_str());
        }
        b->log_day = today;
        mode_t old = umask(077);  // chat logs are private
        b->log = fopen(path.c_str(), "a");
        umask(old);
        if (!b->log) return;
        fprintf(b->log, "--- Log opened %s\n", fmt_time(time(nullptr), "%Y-%m-%d %H:%M:%S").c_str());
    }
    std::string txt = mirc_strip(l.text), who;
    switch (l.kind) {
    case LK::MSG: who = "<" + l.nick + "> "; break;
    case LK::ACTION: who = "* " + l.nick + " "; break;
    case LK::NOTICE: who = "-" + l.nick + "- "; break;
    default: who = "-!- "; break;
    }
    fprintf(b->log, "%s %s%s\n", fmt_time(l.ts, "%Y-%m-%d %H:%M:%S").c_str(), who.c_str(), txt.c_str());
    fflush(b->log);
}

// One line written by log_line() back into a Line; false for headers/garbage.
bool parse_log_line(const std::string& s, Line& l) {
    tm t{};
    if (s.size() < 21 || !strptime(s.substr(0, 19).c_str(), "%Y-%m-%d %H:%M:%S", &t)) return false;
    t.tm_isdst = -1;
    l = Line{mktime(&t), LK::INFO, "", ""};
    std::string r = s.substr(20);
    size_t e;
    if (r.rfind("-!- ", 0) == 0) l.text = r.substr(4);
    else if (r[0] == '<' && (e = r.find("> ")) != std::string::npos)
        l.kind = LK::MSG, l.nick = r.substr(1, e - 1), l.text = r.substr(e + 2);
    else if (r.rfind("* ", 0) == 0 && (e = r.find(' ', 2)) != std::string::npos)
        l.kind = LK::ACTION, l.nick = r.substr(2, e - 2), l.text = r.substr(e + 1);
    else if (r[0] == '-' && (e = r.find("- ", 1)) != std::string::npos)
        l.kind = LK::NOTICE, l.nick = r.substr(1, e - 1), l.text = r.substr(e + 2);
    else return false;
    return true;
}

// Fill a newly opened tab with the tail of its log (log_history lines, default 200).
void Core::load_history(Buffer* b) {
    int n = atoi(opt(b->srv, "log_history", "200").c_str());
    if (n <= 0 || !opt_on(b->srv, "log", false)) return;
    // the current log, preceded by the newest rotated one when it is short
    std::string path = log_path(b);
    auto tail = [&](const std::string& p) {
        std::vector<std::string> rows;
        FILE* f = fopen(p.c_str(), "r");
        if (!f) return rows;
        fseek(f, 0, SEEK_END);
        long size = ftell(f), from = std::max(0L, size - 512L * n);  // generous bytes/line guess
        fseek(f, from, SEEK_SET);
        std::string data((size_t)(size - from), '\0');
        data.resize(fread(&data[0], 1, data.size(), f));
        fclose(f);
        rows = split(data, '\n');
        if (from > 0 && !rows.empty()) rows.erase(rows.begin());  // partial first line
        return rows;
    };
    auto rows = tail(path);
    glob_t g;
    if (rows.size() < (size_t)n &&
        glob((path.substr(0, path.size() - 4) + ".\?\?\?\?-\?\?-\?\?.log").c_str(), 0, nullptr, &g) == 0) {
        auto old = tail(g.gl_pathv[g.gl_pathc - 1]);  // glob sorts: newest date last
        rows.insert(rows.begin(), old.begin(), old.end());
        globfree(&g);
    }
    std::vector<Line> hist;
    for (auto& r : rows) {
        Line l;
        if (parse_log_line(r, l)) hist.push_back(l);
    }
    if (hist.size() > (size_t)n) hist.erase(hist.begin(), hist.end() - n);
    if (hist.empty()) return;
    for (auto& l : hist) {
        l.self = irc_lower(l.nick) == irc_lower(b->srv->nick);
        b->lines.push_back(l);  // not add(): history must not be logged again
    }
    b->lines.push_back(Line{time(nullptr), LK::INFO, "", "--- end of log history ---"});
}

void Core::print(Buffer* b, LK k, const std::string& text) {
    add(b ? b : bufs.front().get(), Line{time(nullptr), k, "", text});
}

void Core::notify(Ev e, Buffer* b, const Line* l) {
    if (listener) listener(e, b, l);
}

// ---- commands --------------------------------------------------------------
// Split text into <=max-byte chunks on UTF-8 boundaries, preferring spaces.
static std::vector<std::string> chunk(const std::string& t, size_t max) {
    std::vector<std::string> out;
    size_t i = 0;
    while (t.size() - i > max) {
        size_t end = i + max;
        while (end > i && ((unsigned char)t[end] & 0xc0) == 0x80) end--;
        size_t sp = t.rfind(' ', end);
        if (sp != std::string::npos && sp > i + max / 2) end = sp;
        out.push_back(t.substr(i, end - i));
        i = end + (t[end] == ' ');
    }
    out.push_back(t.substr(i));
    return out;
}

void Core::say(Buffer* b, const std::string& text, bool action) {
    Server* s = b->srv;
    if (!s || b->type == BT::STATUS || b->name[0] == '*') { print(b, LK::ERROR, "Not a channel or query window"); return; }
    if (s->state != SS::CONNECTED) { print(b, LK::ERROR, "Not connected"); return; }
    // /e2e: "+iSE " + b64(12 nonce + text + 16 tag) must fit the same 400 bytes
    // (a channel also has its name as AAD, so the 400 bytes count it: long names get less text)
    bool enc = e2e_on(b), chan = b->type == BT::CHANNEL;
    std::string key = !enc ? "" : chan ? s->chan_e2e[irc_lower(b->name)] : s->e2e[irc_lower(b->name)].key;
    for (auto& part : chunk(text, enc ? 250 : 400)) {
        std::string body = action ? "\x01" "ACTION " + part + "\x01" : part;
        s->send("PRIVMSG " + b->name + " :" + (enc ? "+iSE " + e2e_seal(key, body, chan ? irc_lower(b->name) : "") : body));
        if (!s->caps.count("echo-message"))
            add(b, Line{time(nullptr), action ? LK::ACTION : LK::MSG, s->nick, enc ? "\xf0\x9f\x94\x92 " + part : part, true});
    }
}

// the text after the first n words
std::string after_words(const std::string& args, size_t n) {
    size_t pos = 0;
    for (size_t k = 0; k < n && pos != std::string::npos; k++) {
        pos = args.find_first_not_of(' ', pos);
        pos = pos == std::string::npos ? pos : args.find(' ', pos);
    }
    if (pos == std::string::npos) return std::string();
    pos = args.find_first_not_of(' ', pos);
    return pos == std::string::npos ? std::string() : args.substr(pos);
}

void Core::exec(Buffer* b, const std::string& input) {
    if (!b) return;
    if (input.find('\n') != std::string::npos) {
        for (auto& l : split(input, '\n')) exec(b, l);
        return;
    }
    std::string in = input;
    if (!in.empty() && in.back() == '\r') in.pop_back();
    if (b == pass_buf) {  // a password prompt: this line is it, never sent or shown
        auto done = std::move(pass_done);
        pass_buf = nullptr, pass_done = nullptr;
        if (in.empty()) print(b, LK::INFO, "Cancelled");
        else done(in);
        return;
    }
    if (in.empty()) return;
    last_input = time(nullptr);
    if (in[0] != '/' || in.rfind("//", 0) == 0) {
        say(b, in[0] == '/' ? in.substr(1) : in);
        return;
    }
    size_t sp = in.find(' ');
    std::string cmd = in.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
    std::string args = sp == std::string::npos ? "" : in.substr(sp + 1);
    for (auto& ch : cmd) ch = (char)tolower((unsigned char)ch);
    auto argv = split(args, ' ');
    auto rest = [&](size_t n) { return after_words(args, n); };
    Server* s = b->srv;
    auto need = [&]() {
        if (s && s->state == SS::CONNECTED) return true;
        print(b, LK::ERROR, "Not connected to a server");
        return false;
    };

    if (cmd == "help") { help(b, args); return; }
    if (cmd == "e2e" || cmd == "sq") { e2e_cmd(b, args); return; }
    if (cmd == "exec" || cmd == "sh") { exec_cmd(b, args); return; }
    if (cmd == "set") {  // /set [section.]key [value]; section defaults to general
        auto secret = secret_key;
        if (argv.empty()) {
            for (auto& st : settings()) print(b, LK::INFO, st.key + " = " + st.val);
            return;
        }
        size_t dot = argv[0].rfind('.');
        std::string sec = dot == std::string::npos ? "general" : argv[0].substr(0, dot);
        std::string key = dot == std::string::npos ? argv[0] : argv[0].substr(dot + 1);
        std::string val = rest(1);
        if (key.empty() || sec.empty()) { print(b, LK::ERROR, "Usage: /set [section.]key value"); return; }
        if (argv.size() == 1) {
            std::string def = ini_get(defaults, sec, key);
            print(b, LK::INFO, argv[0] + " = " + (secret(key) && !ini_get(cfg, sec, key).empty() ? "********" : ini_get(cfg, sec, key, def.empty() ? "(not set: default)" : def + "  (default)")));
            return;
        }
        cfg[sec][key] = unsaved[sec][key] = val;
        std::string err;
        if (sec == "general" && key == "theme") {
            if (load_theme(val, theme, err)) notify(Ev::THEME, nullptr);
            else print(b, LK::ERROR, err);
        }
        // servers pick up host/port/sasl/... on their next connect; flood limits now
        for (auto& sv : servers) {
            sv->cfg = server_cfg(sv->cfg.name);
            if (key == "nick") sv->last_nick.clear();  // /set nick wins over the last /nick
            load_flood(sv.get());
        }
        if (sec == "keys") cfg_changed = true;
        dirty = true;
        print(b, LK::INFO, argv[0] + " = " + (secret(key) ? "********" : val) +
                               (sec.rfind("server.", 0) == 0 ? "  (used on the next /connect)"
                                : sec == "web"              ? "  (applies after /web stop and /web start)"
                                                            : "") +
                               "  - /save writes it to the config");
        return;
    }
    if (cmd == "ignore" || cmd == "unignore") {  // masks: nick or nick!user@host globs
        auto masks = split(ini_get(cfg, "general", "ignore"), ' ');
        if (argv.empty()) {
            print(b, LK::INFO, masks.empty() ? "Ignore list is empty" : "Ignoring: " + ini_get(cfg, "general", "ignore"));
            return;
        }
        std::string mask = argv[0];
        if (mask.find('!') == std::string::npos && mask.find('@') == std::string::npos) mask += "!*@*";
        auto it = std::find(masks.begin(), masks.end(), mask);
        if (cmd == "ignore" && it == masks.end()) masks.push_back(mask);
        if (cmd == "unignore") {
            if (it == masks.end()) { print(b, LK::ERROR, mask + " is not ignored"); return; }
            masks.erase(it);
        }
        std::string joined;
        for (auto& m : masks) joined += (joined.empty() ? "" : " ") + m;
        cfg["general"]["ignore"] = unsaved["general"]["ignore"] = joined;
        print(b, LK::INFO, (cmd == "ignore" ? "Ignoring " : "No longer ignoring ") + mask + " (/save to keep)");
        return;
    }
    if (cmd == "hilight") {  // /hilight [add|del] word: words highlighted besides your nick, * matches anything
        auto words = split(ini_get(cfg, "general", "hilight_words"), ' ');
        if (argv.size() < 2 || (argv[0] != "add" && argv[0] != "del")) {
            if (!argv.empty()) { print(b, LK::ERROR, "Usage: /hilight [add|del] word"); return; }
            print(b, LK::INFO, words.empty() ? "Highlighting only your nick (/hilight add word)" : "Highlighting your nick and: " + ini_get(cfg, "general", "hilight_words"));
            return;
        }
        std::string w = irc_lower(argv[1]);
        auto it = std::find(words.begin(), words.end(), w);
        if (argv[0] == "add" && it == words.end()) words.push_back(w);
        if (argv[0] == "del") {
            if (it == words.end()) { print(b, LK::ERROR, w + " is not highlighted"); return; }
            words.erase(it);
        }
        std::string joined;
        for (auto& x : words) joined += (joined.empty() ? "" : " ") + x;
        cfg["general"]["hilight_words"] = unsaved["general"]["hilight_words"] = joined;
        print(b, LK::INFO, (argv[0] == "add" ? "Highlighting " : "No longer highlighting ") + w + " (/save to keep)");
        return;
    }
    if (cmd == "save") {
        std::string err;
        if (unsaved.empty()) { print(b, LK::INFO, "Nothing to save"); return; }
        if (!ini_save(cfg_path, unsaved, err)) { print(b, LK::ERROR, err); return; }
        unsaved.clear();
        print(b, LK::INFO, "Saved " + cfg_path);
        return;
    }
    if (cmd == "quit") {
        quit_msg = args;
        for (auto& sv : servers)
            if (sv->state != SS::DISCONNECTED)
                sv->user_quit = true, sv->send_now("QUIT :" + (args.empty() ? bye(sv.get(), "quit_message") : args));
        quit = true;
        return;
    }
    if (cmd == "server") {
        if (argv.empty()) { print(b, LK::ERROR, "Usage: /server host [port] [-tls] [-noverify]"); return; }
        for (auto& sv : servers)
            if (sv->cfg.name == argv[0]) { connect(sv.get()); select(sv->status); return; }
        std::string sec = "server." + argv[0], port;
        bool tls = false, verify = true;
        for (size_t i = 1; i < argv.size(); i++) {
            if (argv[i] == "-tls") tls = true;
            else if (argv[i] == "-noverify") verify = false;
            else port = argv[i];
        }
        if (port.empty() || (tls && port == "6667")) port = tls ? "6697" : "6667";
        // remembered like /set, so /save keeps it for the next start
        for (auto& [k, v] : std::map<std::string, std::string>{
                 {"host", argv[0]}, {"port", port},
                 {"tls", tls ? "true" : "false"}, {"tls_verify", verify ? "true" : "false"}})
            cfg[sec][k] = unsaved[sec][k] = v;
        ServerCfg sc = server_cfg(argv[0]);
        Server* ns = add_server(sc);
        select(ns->status);
        connect(ns);
        return;
    }
    if (cmd == "connect") {
        Server* t = s;
        for (auto& sv : servers)
            if (!argv.empty() && sv->cfg.name == argv[0]) t = sv.get();
        if (!t) { print(b, LK::ERROR, "Usage: /connect name"); return; }
        connect(t);
        return;
    }
    if (cmd == "disconnect") {
        if (!s) return;
        s->user_quit = true;
        s->send_now("QUIT :" + (args.empty() ? bye(s, "quit_message") : args));
        if (s->conn.st == Conn::OPEN) s->conn.pump(0, s->inbuf);
        disconnect(s, "");
        return;
    }
    if (cmd == "win" || cmd == "window") {
        int n = atoi(args.c_str());
        if (n >= 1 && n <= (int)bufs.size()) select(bufs[(size_t)n - 1].get());
        return;
    }
    if (cmd == "close" || cmd == "wc") { close(b); return; }
    if (cmd == "quiet") {  // per tab, until restart
        b->quiet = args == "on" || (args != "off" && !b->quiet);
        print(b, LK::INFO, std::string("Quiet mode ") + (b->quiet ? "on: joins, parts, quits and nick changes are hidden here"
                                                                  : "off: showing joins, parts, quits and nick changes"));
        return;
    }
    if (cmd == "detach") {
        if (tty) detach_req = true;
        else print(b, LK::ERROR, "No terminal is attached; isekur --attach brings one");
        return;
    }
    if (cmd == "web") {  // /web [start|stop|status]
        std::string a = argv.empty() ? "status" : argv[0];
        web_req_buf = b;
        if (a == "start") web_req = WebReq::START;
        else if (a == "stop" && daemon) print(b, LK::ERROR, "The webchat is the only way into this iSekur; not stopping it");
        else if (a == "stop") web_req = WebReq::STOP;
        else if (a == "status") print(b, LK::INFO, web_url.empty() ? "Webchat is stopped (/web start)" : "Webchat on " + web_url);
        else print(b, LK::ERROR, "Usage: /web [start|stop|status]");
        return;
    }
    if (cmd == "away" && s) s->auto_away = false;  // yours now; falls through to the server
    if (cmd == "grep") {  // /grep [-all] text: matching lines from scrollback, newest last
        bool all = !argv.empty() && argv[0] == "-all";
        std::string q = irc_lower(all ? rest(1) : args);
        if (q.empty()) { print(b, LK::ERROR, "Usage: /grep [-all] text"); return; }
        std::vector<Line> hits;
        for (auto& bp : bufs) {
            if (!all && bp.get() != b) continue;
            for (auto& l : bp->lines) {
                if (l.nolog || irc_lower(l.nick + " " + mirc_strip(l.text)).find(q) == std::string::npos) continue;
                Line h{l.ts, LK::INFO, "", fmt_time(l.ts, "%Y-%m-%d %H:%M ") +
                                              (all ? "[" + bp->name + "] " : "") +
                                              (l.nick.empty() ? "" : "<" + l.nick + "> ") + mirc_strip(l.text)};
                h.nolog = true;
                hits.push_back(h);
            }
        }
        size_t n = hits.size(), from = n > 50 ? n - 50 : 0;  // ponytail: last 50, in-memory scrollback only
        Line head{time(nullptr), LK::INFO, "", "--- " + std::to_string(n) + " match" + (n == 1 ? "" : "es") +
                                                  " for \"" + q + "\"" + (from ? ", last 50 shown" : "") + " ---"};
        head.nolog = true;
        add(b, head);
        for (size_t i = from; i < n; i++) add(b, hits[i]);
        return;
    }
    if (cmd == "clear") { b->lines.clear(); b->scroll = 0; dirty = true; notify(Ev::BUFFER, b); return; }
    if (cmd == "reload") { reload(b); return; }
    if (cmd == "theme") {
        std::string err;
        if (args.empty()) print(b, LK::INFO, "Current theme: " + theme.name);
        else if (load_theme(args, theme, err)) { dirty = true; notify(Ev::THEME, nullptr); print(b, LK::INFO, "Theme: " + args); }
        else print(b, LK::ERROR, err);
        return;
    }
    if (cmd == "nick") {
        if (argv.empty()) return;
        if (s && s->state != SS::DISCONNECTED) s->send("NICK " + argv[0]);
        else if (s) s->cfg.nick = argv[0];
        return;
    }
    if (cmd == "me") { if (!args.empty()) say(b, args, true); return; }
    if (cmd == "autojoin" && !argv.empty() && argv[0] == "snap") {
        // autojoin = the open channel tabs (with their keys), every server or "here"; written now
        bool here = argv.size() > 1 && argv[1] == "here";
        if (here && !s) { print(b, LK::ERROR, "No server in this tab: /autojoin snap (all servers) or run it in a server's tab"); return; }
        Ini part;
        std::string done;
        for (auto& sv : servers) {
            if (here && sv.get() != s) continue;
            std::vector<std::string> aj;
            std::string v;
            for (auto& bp : bufs) {
                if (bp->srv != sv.get() || bp->type != BT::CHANNEL) continue;
                auto k = sv->chan_keys.find(irc_lower(bp->name));
                aj.push_back(bp->name + (k == sv->chan_keys.end() ? "" : " " + k->second));
                v += (v.empty() ? "" : ",") + aj.back();
            }
            std::string sec = "server." + sv->cfg.name;
            sv->cfg.autojoin = aj;
            cfg[sec]["autojoin"] = unsaved[sec]["autojoin"] = v;
            e2e_keep(sv.get(), false);
            part[sec] = unsaved[sec];  // the whole section: a /server one isn't in the file yet
            unsaved.erase(sec);
            done += (done.empty() ? "" : "; ") + sv->cfg.name + ": " + std::to_string(aj.size()) + " channel" + (aj.size() == 1 ? "" : "s");
        }
        if (part.empty()) { print(b, LK::INFO, "No servers"); return; }
        std::string err;
        if (!ini_save(cfg_path, part, err)) {
            for (auto& [sec, kv] : part) unsaved[sec].insert(kv.begin(), kv.end());
            print(b, LK::ERROR, err);
            return;
        }
        print(b, LK::INFO, "Autojoin saved (" + done + ")");
        return;
    }
    if (!need()) return;
    if (cmd == "join" || cmd == "j") {
        if (argv.empty()) return;
        std::string ch = s->is_chan(argv[0]) ? argv[0] : "#" + argv[0];
        s->send("JOIN " + ch + (argv.size() > 1 ? " " + argv[1] : ""));
        if (argv.size() > 1) s->chan_keys[irc_lower(ch)] = argv[1];
        return;
    }
    if (cmd == "autojoin") {  // /autojoin [add|del] [#chan [key]]: the server's autojoin list
        std::string sec = "server." + s->cfg.name;
        auto& aj = s->cfg.autojoin;
        if (argv.empty()) {
            std::string l;
            for (auto& e : aj) l += (l.empty() ? "" : ", ") + split(e, ' ')[0] + (e.find(' ') != std::string::npos ? " (key)" : "");
            print(b, LK::INFO, aj.empty() ? "No autojoin channels on " + s->cfg.name : "Autojoin on " + s->cfg.name + ": " + l);
            return;
        }
        if (argv[0] != "add" && argv[0] != "del") { print(b, LK::ERROR, "Usage: /autojoin [add|del] [#channel [key]] or /autojoin snap [here]"); return; }
        std::string ch = argv.size() > 1 ? (s->is_chan(argv[1]) ? argv[1] : "#" + argv[1]) : b->name;
        if (!s->is_chan(ch)) { print(b, LK::ERROR, "Not a channel: name one, or run it in the channel's tab"); return; }
        auto it = std::find_if(aj.begin(), aj.end(), [&](const std::string& e) { return irc_lower(split(e, ' ')[0]) == irc_lower(ch); });
        if (argv[0] == "del") {
            if (it == aj.end()) { print(b, LK::ERROR, ch + " is not in autojoin"); return; }
            aj.erase(it);
        } else {
            std::string key = argv.size() > 2 ? argv[2] : s->chan_keys.count(irc_lower(ch)) ? s->chan_keys[irc_lower(ch)] : "";
            std::string e = ch + (key.empty() ? "" : " " + key);
            if (it != aj.end()) *it = e;
            else aj.push_back(e);
        }
        std::string v;
        for (auto& e : aj) v += (v.empty() ? "" : ",") + e;
        cfg[sec]["autojoin"] = unsaved[sec]["autojoin"] = v;
        e2e_keep(s, false);
        print(b, LK::INFO, (argv[0] == "add" ? "Added " + ch + " to" : "Removed " + ch + " from") + " autojoin on " + s->cfg.name + " (/save to keep)");
        return;
    }
    if (cmd == "part") {
        std::string ch = !argv.empty() && s->is_chan(argv[0]) ? argv[0] : b->name;
        std::string msg = !argv.empty() && s->is_chan(argv[0]) ? rest(1) : args;
        if (!s->is_chan(ch)) { print(b, LK::ERROR, "Not a channel"); return; }
        s->send("PART " + ch + " :" + (msg.empty() ? bye(s, "part_message") : msg));
        return;
    }
    if (cmd == "msg" || cmd == "query" || cmd == "notice") {
        if (argv.empty()) return;
        std::string text = rest(1);
        if (cmd == "query") {
            Buffer* q = open(s, BT::QUERY, argv[0]);
            select(q);
            if (!text.empty()) say(q, text);
            return;
        }
        if (text.empty()) return;
        Buffer* t = find(s, argv[0]);
        if (cmd == "msg" && t) { say(t, text); return; }
        for (auto& part : chunk(text, 400)) {
            s->send((cmd == "msg" ? "PRIVMSG " : "NOTICE ") + argv[0] + " :" + part);
            if (!s->caps.count("echo-message"))
                add(t ? t : b, Line{time(nullptr), cmd == "msg" ? LK::MSG : LK::NOTICE,
                                    s->nick, "-> " + argv[0] + ": " + part, true});
        }
        return;
    }
    if (cmd == "topic") {
        if (b->type != BT::CHANNEL) { print(b, LK::ERROR, "Not a channel"); return; }
        if (args.empty()) print(b, LK::TOPIC, "Topic: " + b->topic);
        else s->send("TOPIC " + b->name + " :" + args);
        return;
    }
    if (chanop(b, cmd, args)) return;
    if (cmd == "raw" || cmd == "quote") { s->send(args); return; }
    if (cmd == "ctcp") {  // /ctcp nick VERSION|TIME|PING|...
        if (argv.size() < 2) { print(b, LK::ERROR, "Usage: /ctcp target VERSION|TIME|PING [args]"); return; }
        std::string verb = argv[1];
        for (auto& ch : verb) ch = (char)toupper((unsigned char)ch);
        std::string arg = rest(2);
        if (verb == "PING" && arg.empty()) arg = std::to_string(now_ms());
        s->send("PRIVMSG " + argv[0] + " :\x01" + verb + (arg.empty() ? "" : " " + arg) + "\x01");
        print(b, LK::INFO, "CTCP " + verb + " sent to " + argv[0]);
        return;
    }
    if (cmd == "oper" && argv.size() == 1) {  // no password typed: ask for it, hidden
        if (!need()) return;
        std::string user = argv[0];
        ask_pass(b, "oper password for " + user, [s, user](const std::string& p) { s->send("OPER " + user + " :" + p); });
        return;
    }
    // anything else goes to the server verbatim: /whois, /mode, /away, /kick...
    for (auto& ch : cmd) ch = (char)toupper((unsigned char)ch);
    // ...but the free text at the end needs a ':', or the server keeps only its first word
    static const std::map<std::string, int> trail = {
        {"AWAY", 0}, {"WALLOPS", 0}, {"GLOBOPS", 0}, {"LOCOPS", 0}, {"OPERWALL", 0},
        {"KILL", 1}, {"SQUIT", 1}, {"KICK", 2},
        {"KLINE", -1}, {"GLINE", -1}, {"ZLINE", -1}, {"DLINE", -1}, {"SHUN", -1}, {"ELINE", -1}};  // -1: mask [duration]
    auto t = trail.find(cmd);
    if (t != trail.end()) {
        auto dur = [](const std::string& w) {  // 0, 3600, 1d, 1h30m
            return !w.empty() && isdigit((unsigned char)w[0]) && w.find_first_not_of("0123456789smhdwy") == std::string::npos;
        };
        // a duration may come after the mask (Unreal, InspIRCd, sekurircd) or before it (charybdis)
        size_t n = t->second >= 0 ? (size_t)t->second : 1 + ((argv.size() > 2 && dur(argv[1])) || (argv.size() > 1 && dur(argv[0])));
        std::string tail = rest(n);
        if (!tail.empty() && tail[0] != ':')
            args = args.substr(0, args.size() - tail.size()) + ":" + tail;
    }
    s->send(cmd + (args.empty() ? "" : " " + args));
}
