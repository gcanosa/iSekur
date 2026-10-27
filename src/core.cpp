// Buffers, config loading and user commands.
#include <algorithm>
#include <cstdlib>
#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>
#include "core.h"

static const size_t MAX_LINES = 2000;

static std::string default_config_path() {
    if (const char* x = getenv("XDG_CONFIG_HOME")) return std::string(x) + "/isekur/isekur.conf";
    const char* h = getenv("HOME");
    return std::string(h ? h : ".") + "/.config/isekur/isekur.conf";
}

void Core::load_config() {
    if (cfg_path.empty()) cfg_path = default_config_path();
    bool found = ini_load(cfg_path, cfg);
    std::string err;
    if (!load_theme(ini_get(cfg, "general", "theme", "default"), theme, err)) {
        if (!load_theme("default", theme, err)) theme = Theme{};
    }
    Buffer* home = open(nullptr, BT::STATUS, "isekur");
    if (!found)
        print(home, LK::INFO, "No config at " + cfg_path +
                                  " - see iSekur.conf.example. Try /server irc.example.org 6697 -tls");
    print(home, LK::INFO, "Welcome to iSekur " ISEKUR_VERSION ". /help lists commands.");

    const char* login = getenv("USER");
    std::string gnick = ini_get(cfg, "general", "nick", login ? login : "isekur");
    for (auto& [sec, kv] : cfg) {
        if (sec.rfind("server.", 0) != 0) continue;
        ServerCfg sc;
        sc.name = sec.substr(7);
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
        sc.autojoin = split(ini_get(cfg, sec, "autojoin"), ',');
        Server* s = add_server(sc);
        if (sc.autoconnect) connect(s);
    }
}

Server* Core::add_server(const ServerCfg& sc) {
    auto s = std::make_unique<Server>();
    s->cfg = sc;
    s->nick = sc.nick;
    Server* sp = s.get();
    servers.push_back(std::move(s));
    sp->status = open(sp, BT::STATUS, sc.name);
    return sp;
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
            s->send_now("QUIT :" + quit_msg);
            if (s->conn.st == Conn::OPEN) s->conn.pump(0, s->inbuf);  // best-effort flush
            disconnect(s, "");
        }
        for (size_t i = bufs.size(); i-- > 0;)
            if (bufs[i]->srv == s && bufs[i].get() != b) close(bufs[i].get());
    } else if (b->type == BT::CHANNEL && b->joined && s) {
        s->send("PART " + b->name);
    }
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
    if (b != active) {
        int lvl = ln.hilight ? 3 : (ln.kind == LK::MSG || ln.kind == LK::ACTION) && !ln.self ? 2 : 1;
        if (lvl > b->activity) { b->activity = lvl; notify(Ev::BUFFER, b); }
    }
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

std::string Core::opt(const Server* s, const std::string& key, const std::string& def) const {
    std::string g = ini_get(cfg, "general", key, def);
    return s ? ini_get(cfg, "server." + s->cfg.name, key, g) : g;
}

bool Core::opt_on(const Server* s, const std::string& key, bool def) const {
    std::string v = opt(s, key, def ? "on" : "off");
    return v == "on" || v == "true" || v == "yes" || v == "1";
}

static void mkdir_p(const std::string& path) {
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
    if (!b->srv || b->type == BT::STATUS || !opt_on(b->srv, "log", false)) return;
    if (!b->log) {
        std::string path = log_path(b);
        mkdir_p(path.substr(0, path.rfind('/')));
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
    FILE* f = fopen(log_path(b).c_str(), "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f), from = std::max(0L, size - 512L * n);  // generous bytes/line guess
    fseek(f, from, SEEK_SET);
    std::string data((size_t)(size - from), '\0');
    data.resize(fread(&data[0], 1, data.size(), f));
    fclose(f);
    auto rows = split(data, '\n');
    if (from > 0 && !rows.empty()) rows.erase(rows.begin());  // partial first line
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
    if (!s || b->type == BT::STATUS) { print(b, LK::ERROR, "Not a channel or query window"); return; }
    if (s->state != SS::CONNECTED) { print(b, LK::ERROR, "Not connected"); return; }
    for (auto& part : chunk(text, 400)) {
        s->send("PRIVMSG " + b->name + " :" + (action ? "\x01" "ACTION " + part + "\x01" : part));
        if (!s->caps.count("echo-message"))
            add(b, Line{time(nullptr), action ? LK::ACTION : LK::MSG, s->nick, part, true});
    }
}

static const char* HELP =
    "Commands: /server host [port] [-tls] [-noverify] · /connect [name] · /disconnect [msg] · "
    "/quit [msg] · /join #chan [key] · /part [#chan] [msg] · /msg target text · /query nick [text] · "
    "/me action · /notice target text · /nick new · /topic [text] · /close · /win N · "
    "/raw line · /ctcp target VERSION|TIME|PING · /theme name · /set [section.]key [value] · /save · /ignore [mask] · /unignore mask · /clear. Unknown commands go to the server as-is (/whois, /mode, /away...).";

void Core::exec(Buffer* b, const std::string& input) {
    if (!b) return;
    if (input.find('\n') != std::string::npos) {
        for (auto& l : split(input, '\n')) exec(b, l);
        return;
    }
    std::string in = input;
    if (!in.empty() && in.back() == '\r') in.pop_back();
    if (in.empty()) return;
    if (in[0] != '/' || in.rfind("//", 0) == 0) {
        say(b, in[0] == '/' ? in.substr(1) : in);
        return;
    }
    size_t sp = in.find(' ');
    std::string cmd = in.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
    std::string args = sp == std::string::npos ? "" : in.substr(sp + 1);
    for (auto& ch : cmd) ch = (char)tolower((unsigned char)ch);
    auto argv = split(args, ' ');
    auto rest = [&](size_t n) {  // args after the first n words
        size_t pos = 0;
        for (size_t k = 0; k < n && pos != std::string::npos; k++) {
            pos = args.find_first_not_of(' ', pos);
            pos = pos == std::string::npos ? pos : args.find(' ', pos);
        }
        if (pos == std::string::npos) return std::string();
        pos = args.find_first_not_of(' ', pos);
        return pos == std::string::npos ? std::string() : args.substr(pos);
    };
    Server* s = b->srv;
    auto need = [&]() {
        if (s && s->state == SS::CONNECTED) return true;
        print(b, LK::ERROR, "Not connected to a server");
        return false;
    };

    if (cmd == "help") { print(b, LK::INFO, HELP); return; }
    if (cmd == "set") {  // /set [section.]key [value]; section defaults to general
        auto secret = [](const std::string& k) {
            return k.find("pass") != std::string::npos || k.find("hash") != std::string::npos;
        };
        if (argv.empty()) {
            for (auto& [sec, kv] : cfg)
                for (auto& [k, v] : kv)
                    print(b, LK::INFO, (sec == "general" ? "" : sec + ".") + k + " = " +
                                           (secret(k) ? "********" : v) +
                                           (unsaved.count(sec) && unsaved[sec].count(k) ? "  (unsaved)" : ""));
            return;
        }
        size_t dot = argv[0].rfind('.');
        std::string sec = dot == std::string::npos ? "general" : argv[0].substr(0, dot);
        std::string key = dot == std::string::npos ? argv[0] : argv[0].substr(dot + 1);
        std::string val = rest(1);
        if (key.empty() || sec.empty()) { print(b, LK::ERROR, "Usage: /set [section.]key value"); return; }
        if (argv.size() == 1) {
            print(b, LK::INFO, argv[0] + " = " + (secret(key) ? "********" : ini_get(cfg, sec, key, "(not set: default)")));
            return;
        }
        cfg[sec][key] = unsaved[sec][key] = val;
        std::string err;
        if (sec == "general" && key == "theme") {
            if (load_theme(val, theme, err)) notify(Ev::THEME, nullptr);
            else print(b, LK::ERROR, err);
        }
        dirty = true;
        print(b, LK::INFO, argv[0] + " = " + (secret(key) ? "********" : val) +
                               "  (/save writes it to the config; servers, web and keys apply on restart)");
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
    if (cmd == "save") {
        std::string err;
        if (unsaved.empty()) { print(b, LK::INFO, "Nothing to save"); return; }
        if (!ini_save(cfg_path, unsaved, err)) { print(b, LK::ERROR, err); return; }
        unsaved.clear();
        print(b, LK::INFO, "Saved " + cfg_path);
        return;
    }
    if (cmd == "quit") {
        quit_msg = args.empty() ? quit_msg : args;
        for (auto& sv : servers)
            if (sv->state != SS::DISCONNECTED) sv->user_quit = true, sv->send_now("QUIT :" + quit_msg);
        quit = true;
        return;
    }
    if (cmd == "server") {
        if (argv.empty()) { print(b, LK::ERROR, "Usage: /server host [port] [-tls] [-noverify]"); return; }
        for (auto& sv : servers)
            if (sv->cfg.name == argv[0]) { connect(sv.get()); select(sv->status); return; }
        ServerCfg sc;
        sc.name = sc.host = argv[0];
        const char* login = getenv("USER");
        sc.nick = ini_get(cfg, "general", "nick", login ? login : "isekur");
        sc.altnick = sc.nick + "_";
        sc.user = ini_get(cfg, "general", "user", sc.nick);
        sc.realname = ini_get(cfg, "general", "realname", "iSekur user");
        for (size_t i = 1; i < argv.size(); i++) {
            if (argv[i] == "-tls") sc.tls = true;
            else if (argv[i] == "-noverify") sc.tls_verify = false;
            else sc.port = atoi(argv[i].c_str());
        }
        if (sc.tls && sc.port == 6667) sc.port = 6697;
        // remembered like /set, so /save keeps it for the next start
        std::string sec = "server." + sc.name;
        for (auto& [k, v] : std::map<std::string, std::string>{
                 {"host", sc.host}, {"port", std::to_string(sc.port)},
                 {"tls", sc.tls ? "true" : "false"}, {"tls_verify", sc.tls_verify ? "true" : "false"}})
            cfg[sec][k] = unsaved[sec][k] = v;
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
        s->send_now("QUIT :" + (args.empty() ? quit_msg : args));
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
    if (cmd == "clear") { b->lines.clear(); b->scroll = 0; dirty = true; notify(Ev::BUFFER, b); return; }
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
    if (!need()) return;
    if (cmd == "join" || cmd == "j") {
        if (argv.empty()) return;
        std::string ch = s->is_chan(argv[0]) ? argv[0] : "#" + argv[0];
        s->send("JOIN " + ch + (argv.size() > 1 ? " " + argv[1] : ""));
        return;
    }
    if (cmd == "part") {
        std::string ch = !argv.empty() && s->is_chan(argv[0]) ? argv[0] : b->name;
        std::string msg = !argv.empty() && s->is_chan(argv[0]) ? rest(1) : args;
        if (!s->is_chan(ch)) { print(b, LK::ERROR, "Not a channel"); return; }
        s->send("PART " + ch + (msg.empty() ? "" : " :" + msg));
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
    // anything else goes to the server verbatim: /whois, /mode, /away, /kick...
    for (auto& ch : cmd) ch = (char)toupper((unsigned char)ch);
    s->send(cmd + (args.empty() ? "" : " " + args));
}
