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
    max_lines = (size_t)std::max(100, atoi(opt(nullptr, "scrollback", "2000").c_str()));
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
    print(home, LK::INFO, "\x03" "01,09" "\x02" " NEW HERE? " "\x0f" " Type " "\x03" "11" "\x02" "/guide" "\x0f"
                          " for a quick tour of the first steps and features");
    print(home, LK::INFO, "");  // room before what comes next (webchat, connecting...)
    if (!found || opt_on(nullptr, "show_guide", false)) guide(home, "");  // a new config has show_guide = on

    for (auto& [sec, kv] : cfg) {
        if (sec.rfind("server.", 0) != 0) continue;
        Server* s = add_server(server_cfg(sec.substr(7)));
        if (s->cfg.autoconnect) connect(s);
    }
}

// The startup banner: each letter in a color of the theme's nick palette, the
// shadow in its (dim) timestamp color; mono: the text color and gray.
// mIRC \x04 colors, so the webchat shows it too. shine: the column a diagonal
// band of light crosses at the top row (banner_anim), far off = no light.
// the letters' colors: the theme's nick colors, or for a theme with one (mirc: mIRC doesn't color nicks) mIRC's own,
// its dark ones on a light background and its bright ones on a dark one
static std::vector<RGB> banner_palette(const Theme& t, int light) {
    if (t.nicks.size() > 1) return t.nicks;
    if (light < 0) {
        RGB bg = t.get("default").bg;
        light = bg >= 0 && ((bg >> 16 & 255) * 299 + (bg >> 8 & 255) * 587 + (bg & 255) * 114) / 1000 > 140;
    }
    if (light) return {0xff0000, 0xfc7f00, 0x009300, 0x009393, 0x00007f, 0x9c009c};
    return {0xff5555, 0xfc7f00, 0x00fc00, 0x00fcfc, 0x5f87ff, 0xff55ff};
}

std::vector<std::string> banner_lines(const Theme& t, bool mono, double shine, int light) {
    static const char* art[] = {
        u8"██╗███████╗███████╗██╗  ██╗██╗   ██╗██████╗ ",
        u8"██║██╔════╝██╔════╝██║ ██╔╝██║   ██║██╔══██╗",
        u8"██║███████╗█████╗  █████╔╝ ██║   ██║██████╔╝",
        u8"██║╚════██║██╔══╝  ██╔═██╗ ██║   ██║██╔══██╗",
        u8"██║███████║███████╗██║  ██╗╚██████╔╝██║  ██║",
        u8"╚═╝╚══════╝╚══════╝╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝"};
    static const int ends[] = {3, 11, 19, 27, 36, 44};  // column after each letter: I S E K U R
    auto code = [](RGB c) { return c < 0 ? std::string("\x0f") : "\x04" + rgb_hex(c).substr(1); };
    RGB shade = mono || light == 1 ? 0x808080 : t.get("timestamp").fg;  // light: a gray dialog, whatever the theme's own shade
    auto pal = banner_palette(t, light);
    auto lit = [](RGB c, double k) {  // c moved toward white by k (0..1)
        if (c < 0) c = 0xc0c0c0;
        int r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
        auto up = [k](int v) { return v + (int)((255 - v) * k); };
        return up(r) << 16 | up(g) << 8 | up(b);
    };
    std::vector<std::string> out;
    int y = 0;
    for (const char* row : art) {
        std::string s = row, line, cur;
        size_t i = 0;
        for (int col = 0, letter = 0; i < s.size(); col++) {
            while (col >= ends[letter] && letter < 5) letter++;
            uint32_t cp = utf8_next(s, i);
            RGB c = cp != 0x2588 ? shade : mono ? t.get("default").fg : pal[(size_t)letter * pal.size() / 6];
            double k = 1 - std::abs(col + y - shine) / 4;  // the band leans: lower rows lag a column each
            if (k > 0 && cp != ' ') c = lit(c, k * (cp == 0x2588 ? 0.85 : 0.5));
            std::string want = cp == ' ' ? cur : code(c);
            if (want != cur) line += want, cur = want;
            utf8_put(line, cp);
        }
        out.push_back(line);
        y++;
    }
    return out;
}

// /about: the banner, then who and where. mIRC codes, so the webchat's dialog shows the same
std::vector<std::string> about_lines(const Theme& t, double shine, bool web, std::vector<int>* typed) {
    auto out = banner_lines(t, false, shine, web && t.lay("window", "") == "win95" ? 1 : -1);  // the webchat's win95 About: gray
    const char* k = "\x03" "14";  // labels gray, values in the text color
    for (std::string l : {std::string(""),
                          std::string("\x02iSekur " ISEKUR_VERSION "\x02  a colorful terminal IRC client with a built-in webchat"),
                          std::string(k) + "multi-server · IRCv3 · TLS + SASL · E2E channels and queries · /shell · detach and attach",
                          std::string(""),
                          std::string(k) + "Author     \x0f" "Gerardo Canosa",
                          std::string(k) + "Source     \x0f" "https://github.com/gcanosa/iSekur",
                          std::string(k) + "Download   \x0f" "https://github.com/gcanosa/iSekur/releases/latest",
                          std::string(k) + "License    \x0f" "GPL-3.0-or-later: use it, change it, share it",
                          std::string(""),
                          std::string(k) + "Sponsored  \x0f\x02" "irc.sekurnet.org\x02  the network iSekur calls home",
                          std::string(k) + "Support    \x0f\x02" "#iSekur\x02 there: questions, bugs, ideas",
                          std::string(k) + "           \x0f" "/server irc.sekurnet.org 6697 -tls -name sekurnet, then /join #iSekur",
                          std::string(""),
                          std::string(k) + (web ? "/help lists the commands · Esc closes this" : "/help lists the commands · Ctrl+W closes this tab")})
        out.push_back(l);
    if (typed) {
        typed->clear();
        for (size_t i = 0; i < out.size(); i++)
            if (out[i].find("a colorful terminal") != std::string::npos || out[i].find("#iSekur\x02 there") != std::string::npos) typed->push_back((int)i);
    }
    return out;
}

int Core::typing_quiet(bool on) {
    int n = 0;
    for (auto& t : bufs) {
        Buffer* x = t.get();
        if (x->type != BT::CHANNEL) continue;
        if (on && !x->quiet) x->quiet = x->quiet_typing = true, n++;
        else if (!on && x->quiet_typing) x->quiet = x->quiet_typing = false, n++;
        else continue;
        notify(Ev::BUFFER, x);
    }
    dirty = true;
    return n;
}

// Written to the file at once, like e2e_keys: a restart connects with the nick you last chose
// there. A server not saved yet (/server without /save) keeps it with its other unsaved keys.
void Core::remember_nick(Server* s, const std::string& nick) {
    std::string sec = "server." + s->cfg.name;
    s->last_nick = nick;
    if (ini_get(cfg, sec, "last_nick") == nick) return;
    cfg[sec]["last_nick"] = nick;
    bool saved = !cfg_path.empty() && !(unsaved.count(sec) && unsaved[sec].count("host"));
    std::string err;
    if (!saved || !ini_save(cfg_path, Ini{{sec, {{"last_nick", nick}}}}, err)) unsaved[sec]["last_nick"] = nick;
}

void Core::about(Buffer* from) {
    Buffer* b = open(nullptr, BT::QUERY, "*about");
    b->lines.clear(), b->scroll = b->unread = 0;
    notify(Ev::CLEAR, b);
    about_full = about_lines(theme, -99, false, &about_typed);
    for (size_t i = 0; i < about_full.size(); i++) {
        bool typing = tty && std::find(about_typed.begin(), about_typed.end(), (int)i) != about_typed.end();
        add(b, Line{time(nullptr), LK::INFO, "", typing ? "" : about_full[i], false, false, true});  // main.cpp types them out
    }
    if (!tty) about_typed.clear();
    about_t0 = now_ms();
    select(b);
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

bool secret_key(const std::string& k) {
    return k.find("pass") != std::string::npos || k.find("hash") != std::string::npos;
}

// where the password starts in text to NickServ or ChanServ, or npos; *end: where it ends (npos: the end of
// the text). Only the password: nicks, channels, emails and descriptions stay readable.
//   NickServ IDENTIFY|ID [account] <pw>      REGISTER <pw> <email>      GHOST|RECOVER|RELEASE|REGAIN <nick> <pw>
//            SET PASSWORD <pw>
//   ChanServ IDENTIFY <#chan> <pw>           REGISTER <#chan> <pw> ...  SET <#chan> PASSWORD <pw>  (Anope 1.x)
size_t services_secret(const std::string& tgt, const std::string& text, size_t* end) {
    const size_t npos = std::string::npos;
    if (end) *end = npos;
    std::string t = irc_lower(tgt);  // also NickServ@services.libera.chat
    bool ns = t.rfind("nickserv", 0) == 0, cs = t.rfind("chanserv", 0) == 0;
    if (!ns && !cs) return npos;
    std::vector<std::pair<size_t, size_t>> w;  // the words; a trailing space starts an empty one (the next, being typed)
    for (size_t i = 0; i < text.size();) {
        if (text[i] == ' ') { i++; continue; }
        size_t j = std::min(text.find(' ', i), text.size());
        w.push_back({i, j}), i = j;
    }
    if (w.empty()) return npos;
    if (text.back() == ' ') w.push_back({text.size(), text.size()});
    auto up = [&](size_t k) {
        std::string v = k < w.size() ? text.substr(w[k].first, w[k].second - w[k].first) : "";
        for (auto& ch : v) ch = (char)toupper((unsigned char)ch);
        return v;
    };
    auto word = [&](size_t k, bool one) {  // word k on (one: only it)
        if (k >= w.size()) return npos;
        if (one && end && k + 1 < w.size()) *end = w[k].second;
        return w[k].first;
    };
    std::string verb = up(0);
    if (ns) {
        if (verb == "IDENTIFY" || verb == "ID") return w.size() > 1 ? word(w.size() - 1, false) : npos;
        if (verb == "REGISTER") return word(1, true);
        if (verb == "GHOST" || verb == "RECOVER" || verb == "RELEASE" || verb == "REGAIN") return word(2, false);
        if (verb == "SET" && up(1) == "PASSWORD") return word(2, false);
        return npos;
    }
    if (verb == "IDENTIFY") return word(2, false);
    if (verb == "REGISTER") return word(2, true);
    if (verb == "SET" && up(2) == "PASSWORD") return word(3, false);
    return npos;
}

std::string mask_services(const std::string& tgt, const std::string& text) {
    size_t e, p = services_secret(tgt, text, &e);
    return p == std::string::npos || p >= text.size() || e == p ? text : text.substr(0, p) + "********" + (e == std::string::npos ? "" : text.substr(e));
}

// the input line as typed in the tab named buf: where a password starts, or npos
// ("/msg NickServ IDENTIFY x", "/query nickserv id x", or "identify x" in the NickServ tab)
size_t input_secret(const std::string& buf, const std::string& line, size_t* end) {
    if (line.empty() || line[0] != '/') return services_secret(buf, line, end);
    std::string l = irc_lower(line);
    size_t n = l.rfind("/msg ", 0) == 0 ? 5 : l.rfind("/query ", 0) == 0 ? 7 : 0;
    if (!n) return std::string::npos;
    size_t t = line.find_first_not_of(' ', n), e = t == std::string::npos ? t : line.find(' ', t);
    if (end) *end = std::string::npos;
    if (e == std::string::npos) return e;
    size_t p = services_secret(line.substr(t, e - t), line.substr(e + 1), end);
    if (end && *end != std::string::npos) *end += e + 1;
    return p == std::string::npos ? p : e + 1 + p;
}

std::vector<Core::Setting> Core::settings() const {
    Ini all = defaults;
    all["general"]["timestamp"] = theme.lay("timestamp", "%H:%M");  // empty: the theme's, so that's its default
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

// The settings window's groups, in order, each with its keys in order: [general] by topic (each its own
// category), a server's keys by what you fill in first. Keys in none of them: "Other", A-Z.
struct SetGroup { const char* name; std::vector<std::string> keys; };
static const std::vector<SetGroup> GENERAL_GROUPS = {
    {"Identity", {"nick", "altnick", "realname"}},
    {"Look", {"theme", "web_font", "web_font_size", "colors", "timestamp", "timestamp_align", "scrollback", "mouse", "status_info", "cpu_levels", "mem_levels", "typing_effect",
              "banner_mono", "banner_anim", "show_guide"}},
    {"Messages", {"hilight_window", "hilight_words", "bell_notifications", "ignore", "paste_confirm", "quit_message", "part_message",
                  "ctcp_version", "ctcp_time", "ctcp_ping"}},
    {"Channels", {"rejoin_on_kick", "history_fetch"}},
    {"Away", {"auto_away", "auto_away_idle", "away_message"}},
    {"Logs", {"log", "log_dir", "log_history", "log_rotate"}},
    {"Security", {"lock", "lock_idle", "lock_password_hash", "pam_service", "exec_timeout", "exec_web"}},
    {"Network", {"flood_burst", "flood_interval_ms"}},
    {"Files", {"dcc_dir", "dcc_ip", "dcc_ports", "dcc_web"}},
};
static const std::vector<SetGroup> SERVER_GROUPS = {
    {"Connection", {"host", "port", "tls", "tls_verify", "autoconnect"}},
    {"Identity", {"nick", "altnick", "user", "realname"}},
    {"Login", {"password", "auto_identify", "sasl_mech", "sasl_user", "sasl_pass", "nickserv_pass", "cert", "key"}},
    {"Channels", {"autojoin", "rejoin_on_kick", "e2e_keys"}},
};
struct SetPlace { int sec, key; std::string grp; };  // sort keys; the group's name
static SetPlace set_place(const std::string& sec, const std::string& key) {
    static std::map<std::string, int> secs, keys;  // other sections: "sec" / "sec\nkey" -> position in the template
    if (secs.empty()) {
        std::istringstream in(conf_template);
        std::string line, cur;
        int n = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line[0] == '[') { cur = line.substr(1, line.find(']') - 1); if (cur.rfind("server.", 0)) secs.emplace(cur, n++); continue; }
            size_t a = line.find_first_not_of("#; "), e = line.find('=');
            if (a == std::string::npos || e == std::string::npos || e < a) continue;
            std::string k = line.substr(a, e - a);
            while (!k.empty() && k.back() == ' ') k.pop_back();
            if (!k.empty() && k.find(' ') == std::string::npos) keys.emplace(cur + "\n" + k, n++);
        }
    }
    bool gen = sec == "general", srv = sec.rfind("server.", 0) == 0;
    if (gen || srv) {
        auto& gs = gen ? GENERAL_GROUPS : SERVER_GROUPS;
        for (size_t g = 0; g < gs.size(); g++)
            for (size_t k = 0; k < gs[g].keys.size(); k++)
                if (gs[g].keys[k] == key) return {gen ? 0 : 1, (int)(g * 1000 + k), gs[g].name};
        return {gen ? 0 : 1, 1 << 20, "Other"};
    }
    auto k = keys.find(sec + "\n" + key);
    return {2 + (secs.count(sec) ? secs[sec] : 1 << 20), k == keys.end() ? 1 << 20 : k->second, ""};
}

// a [section]'s unsaved keys, written to the config now: the whole section's, since a /server one isn't in the file yet
static bool save_section(Core& c, const std::string& sec, Buffer* b) {
    Ini part;
    part[sec] = c.unsaved[sec];
    c.unsaved.erase(sec);
    std::string err;
    if (ini_save(c.cfg_path, part, err)) return true;
    c.unsaved[sec].insert(part[sec].begin(), part[sec].end());
    c.print(b, LK::ERROR, err);
    return false;
}

std::vector<Core::SetRow> Core::setting_rows() const {
    Ini all = defaults;
    all["general"]["timestamp"] = theme.lay("timestamp", "%H:%M");  // empty: the theme's
    for (auto& [sec, kv] : cfg)
        for (auto& [k, v] : kv) all[sec][k];
    // every server shows the keys it can have, with what it uses when they aren't set
    for (auto& [sec, kv] : cfg) {
        if (sec.rfind("server.", 0) != 0) continue;
        ServerCfg d = server_cfg(sec.substr(7));
        const char* login = getenv("USER");
        std::string gnick = ini_get(cfg, "general", "nick", login ? login : "isekur");  // what nick falls back to
        for (auto& [k, v] : std::map<std::string, std::string>{
                 {"host", sec.substr(7)}, {"port", d.tls ? "6697" : "6667"}, {"tls", "false"}, {"tls_verify", "true"},
                 {"autoconnect", "true"}, {"auto_identify", "true"}, {"nick", gnick}, {"altnick", ini_get(cfg, "general", "altnick", d.nick + "_")},
                 {"user", ini_get(cfg, "general", "user", d.nick)}, {"realname", ini_get(cfg, "general", "realname", "iSekur user")},
                 {"password", ""}, {"sasl_user", ""}, {"sasl_pass", ""}, {"sasl_mech", ""}, {"nickserv_pass", ""}, {"autojoin", ""}})
            if (!all[sec].count(k) || all[sec][k].empty()) all[sec][k] = v;
    }
    std::vector<SetRow> out;
    auto themes = theme_names();
    for (auto& [sec, kv] : all)
        for (auto& [k, d] : kv) {
            SetRow r{sec, k, ini_get(cfg, sec, k), d, unsaved.count(sec) && unsaved.at(sec).count(k), secret_key(k), {}, {}, {}, {}};
            std::string v = r.val.empty() ? d : r.val;
            for (auto& ch : v) ch = (char)tolower((unsigned char)ch);
            if (sec == "general" && k == "theme") r.choices = themes;
            else if (sec == "general" && k == "timestamp_align") r.choices = {"left", "right"};
            else if (sec == "general" && k == "web_font") r.choices = {"theme", "mono", "sans", "serif"};  // theme: its font, else mono
            else if (sec == "general" && k == "web_font_size") r.choices = {"12", "13", "14", "15", "16", "18", "20", "22"};
            else if (v == "on" || v == "off") r.choices = {"on", "off"};
            else if (v == "true" || v == "false") r.choices = {"true", "false"};
            else if (v == "yes" || v == "no") r.choices = {"yes", "no"};
            out.push_back(r);
        }
    for (auto& r : out) {  // [general]: a category per group; a server, or [web]: one, with a header per group
        r.grp = set_place(r.sec, r.key).grp;
        bool srv = r.sec.rfind("server.", 0) == 0;
        r.cat = r.sec == "general" ? r.grp : r.sec;
        r.head = r.sec == "general" ? r.grp : srv ? "\u21E2 " + r.sec.substr(7) + " \u00B7 " + r.grp : "[" + r.sec + "]";
    }
    std::stable_sort(out.begin(), out.end(), [](const SetRow& a, const SetRow& b) {  // ties (unknown keys): A-Z, as the map had them
        auto pa = set_place(a.sec, a.key), pb = set_place(b.sec, b.key);
        return pa.sec != pb.sec ? pa.sec < pb.sec : a.sec != b.sec ? a.sec < b.sec : pa.key < pb.key;
    });
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
    sc.auto_identify = ini_get(cfg, sec, "auto_identify", "true") == "true";
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
    s->last_nick = ini_get(cfg, "server." + sc.name, "last_nick");  // the nick it had when iSekur stopped
    s->nick = s->last_nick.empty() ? sc.nick : s->last_nick;
    Server* sp = s.get();
    servers.push_back(std::move(s));
    sp->status = open(sp, BT::STATUS, sc.name);
    if (sp->twitch())  // a new tab, not each reconnect
        for (auto& l : twitch_banner()) add(sp->status, Line{time(nullptr), LK::INFO, "", l, false, false, true});
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
// modified builds); release tarballs without git just say "iSekur 0.5".
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
    if (t == BT::CHANNEL && opt_on(s, "typing_effect", false)) b->quiet = b->quiet_typing = true;  // joins and parts would be noise among typed lines
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

void Core::close(Buffer* b, const std::string& part_msg) {
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
        // a /server one not saved yet goes too, or a later /save would bring it back on the next start
        std::string sec = "server." + s->cfg.name;
        if (unsaved.count(sec) && unsaved[sec].count("host")) cfg.erase(sec), unsaved.erase(sec);
    } else if (b->type == BT::CHANNEL && b->joined && s) {  // /part, /close and Ctrl+W all end here
        std::string msg = part_msg.empty() ? bye(s, "part_message") : part_msg;
        s->send("PART " + b->name + (s->twitch() || msg.empty() ? "" : " :" + msg));  // Twitch has no part messages
        print(s->status, LK::INFO, "You left " + b->name);
    }
    if (pass_buf == b) pass_buf = nullptr, pass_done = nullptr;
    bool drop_server = s && b->type == BT::STATUS;
    int i = index_of(b);
    notify(Ev::CLOSE, b);
    if (active == b) active = bufs[(size_t)(i > 0 ? i - 1 : 1 % bufs.size())].get();
    bufs.erase(bufs.begin() + i);
    if (active == b) active = bufs.empty() ? nullptr : bufs[0].get();
    if (split_buf == b || split_buf == active) split_buf = nullptr;  // its pane closes with it
    if (drop_server)  // its transfers go with it
        dccs.erase(std::remove_if(dccs.begin(), dccs.end(), [&](auto& d) { return d->srv == s; }), dccs.end());
    if (drop_server)  // after the buffer: listeners may still read b->srv
        servers.erase(std::remove_if(servers.begin(), servers.end(),
                                     [&](auto& p) { return p.get() == s; }),
                      servers.end());
    dirty = true;
}

// /split [N|name|off]: a second pane with another tab, next to the current one
void Core::split_cmd(Buffer* b, const std::string& args) {
    auto label = [&](Buffer* t) { return std::to_string(index_of(t) + 1) + ":" + (t->srv && t->type == BT::STATUS ? t->srv->cfg.name : t->name); };
    // "side", "stack" or "auto" anywhere: the layout; what's left: which tab
    std::string a;
    int mode = -1;
    for (auto& w : split(args, ' ')) {
        std::string lw = irc_lower(w);
        if (lw == "side") mode = 1;
        else if (lw == "stack") mode = 2;
        else if (lw == "auto") mode = 0;
        else if (!w.empty()) a += (a.empty() ? "" : " ") + w;
    }
    static const char* how[] = {"", " side by side", " one over the other"};
    if (mode >= 0) split_mode = mode, dirty = true;
    if (mode >= 0 && a.empty() && split_buf) {  // just the layout
        print(b, LK::INFO, std::string("Split") + (mode ? how[mode] : " by the window's width") + " now");
        notify(Ev::SPLIT, nullptr);
        return;
    }
    if (a == "off") {
        print(b, LK::INFO, split_buf ? "Split closed" : "Not split");
        split_buf = nullptr, dirty = true;
        notify(Ev::SPLIT, nullptr);
        return;
    }
    Buffer* t = nullptr;
    if (a.empty()) {
        if (split_buf) { print(b, LK::INFO, "Split with " + label(split_buf) + ": F6 or Alt+O switches panes, /unsplit closes it"); return; }
        if (bufs.size() > 1) t = bufs[(size_t)(index_of(active) + 1) % bufs.size()].get();  // the next tab
    } else if (a.find_first_not_of("0123456789") == std::string::npos) {
        int n = atoi(a.c_str());
        if (n >= 1 && n <= (int)bufs.size()) t = bufs[(size_t)n - 1].get();
    } else {  // a name: this server's first, then any
        for (int pass = 0; pass < 2 && !t; pass++)
            for (auto& o : bufs)
                if ((pass || o->srv == b->srv) && irc_lower(o->name) == irc_lower(a)) { t = o.get(); break; }
    }
    if (!t) { print(b, LK::ERROR, a.empty() ? "Only one tab: nothing to split with" : "No tab " + a + " (/win shows numbers in the tab bar; join or open it first)"); return; }
    if (t == active) { print(b, LK::ERROR, "That tab is already the one on screen: pick another"); return; }
    split_buf = t, split_first = false, t->activity = 0, dirty = true;
    notify(Ev::BUFFER, t);
    notify(Ev::SPLIT, nullptr);
    print(b, LK::INFO, "Split" + std::string(how[split_mode]) + " with " + label(t) + ": F6 or Alt+O switches panes (or click one), /unsplit closes it");
}

void Core::select(Buffer* b) {
    if (!b) return;
    if (active && active != b && active->unread) active->unread = 0, notify(Ev::BUFFER, active);
    if (b == split_buf) split_buf = active, split_first = !split_first;  // the other pane's tab: focus moves there
    active = b;
    b->activity = 0;
    dirty = true;
    notify(Ev::BUFFER, b);
}

void Core::add(Buffer* b, Line l) {
    if (!b) return;
    // /typing: other people's messages type themselves out
    if (!l.self && (l.kind == LK::MSG || l.kind == LK::ACTION || l.kind == LK::NOTICE) && opt_on(b->srv, "typing_effect", false))
        l.typed_at = now_ms(), typing_until = std::max(typing_until, l.typed_at + 2100);
    b->lines.push_back(std::move(l));
    b->added++;
    while (b->lines.size() > max_lines) b->lines.pop_front();
    if (b->scroll) b->scroll++;  // keep the scrolled view still
    const Line& ln = b->lines.back();
    int lvl = ln.hilight ? 3 : (ln.kind == LK::MSG || ln.kind == LK::ACTION) && !ln.self ? 2 : b->quiet && noise(ln) ? 0 : 1;
    // the "new" marker goes above the first message you missed, then counts everything after it
    if (b->unread || (b != active && b != split_buf && lvl >= 2)) b->unread++;
    if (b != active && b != split_buf && lvl > b->activity) { b->activity = lvl; notify(Ev::BUFFER, b); }  // split: on screen too
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
bool Core::use_theme(const std::string& name, std::string& err) {
    if (!load_theme(name, theme, err)) return false;
    dirty = true;
    notify(Ev::THEME, nullptr);
    return true;
}

void Core::reload(Buffer* b) {
    Ini fresh;
    if (!ini_load(cfg_path, fresh)) { print(b, LK::ERROR, "Can't read " + cfg_path); return; }
    for (auto& [sec, kv] : unsaved)
        for (auto& [k, v] : kv) fresh[sec][k] = v;
    cfg = fresh;
    max_lines = (size_t)std::max(100, atoi(opt(nullptr, "scrollback", "2000").c_str()));
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
                           ". Server settings apply on the next /connect, [web] after /web restart");
}

void Core::ask_pass(Buffer* b, const std::string& label, std::function<void(const std::string&)> done, bool hidden,
                    const std::string& title, std::vector<std::string> info) {
    pass_buf = b, pass_label = label, pass_done = std::move(done), pass_hidden = hidden;
    pass_title = title, pass_info = std::move(info), pass_web = from_web;
    if (!tty || from_web) print(b, LK::INFO, "Type the " + label + (hidden ? " (hidden; an empty line cancels)" : " (an empty line cancels)"));  // the TUI asks in a dialog
    notify(Ev::BUFFER, b);  // the webchat's input line turns into a password field
}

void Core::ring(Server* s) {
    if (opt_on(s, "bell_notifications", true)) bell = dirty = true, notify(Ev::BELL, nullptr);
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
    b->added += (long long)hist.size() + 1;
}

void Core::print(Buffer* b, LK k, const std::string& text) {
    add(b ? b : bufs.front().get(), Line{time(nullptr), k, "", text});
}

void Core::notify(Ev e, Buffer* b, const Line* l) {
    if (listener) listener(e, b, l);
    if (e == Ev::THEME)  // the guide's colors are the theme's
        if (Buffer* g = find(nullptr, "*guide")) guide(g, "page");
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
    if (!s || b->type == BT::STATUS || b->name == "*channels" || b->name == "*snotices") {  // ours; *status (ZNC) is a real query
        print(b, LK::ERROR, "Not a channel or query window"); return; }
    if (s->state != SS::CONNECTED) { print(b, LK::ERROR, "Not connected"); return; }
    // /e2e: "+iSE " + b64(12 nonce + text + 16 tag) must fit the same 400 bytes
    // (a channel also has its name as AAD, so the 400 bytes count it: long names get less text)
    bool enc = e2e_on(b), chan = b->type == BT::CHANNEL;
    std::string key = !enc ? "" : chan ? s->chan_e2e[irc_lower(b->name)] : s->e2e[irc_lower(b->name)].key;
    for (auto& part : chunk(text, enc ? 250 : 400)) {
        std::string body = action ? "\x01" "ACTION " + part + "\x01" : part;
        s->send("PRIVMSG " + b->name + " :" + (enc ? "+iSE " + e2e_seal(key, body, chan ? irc_lower(b->name) : "") : body));
        if (!s->caps.count("echo-message"))
            add(b, Line{time(nullptr), action ? LK::ACTION : LK::MSG, s->nick, enc ? "\xf0\x9f\x94\x92 " + part : mask_services(b->name, part), true});
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
    if (b == pass_buf && from_web == pass_web) {  // a password prompt: this line is it, never sent or shown (only from the side that asked)
        auto done = std::move(pass_done);
        pass_buf = nullptr, pass_done = nullptr;
        notify(Ev::BUFFER, b);  // back to a normal input line (unless done asks again)
        if (in.empty()) print(b, LK::INFO, "Cancelled");
        else done(in);
        return;
    }
    if (in.empty()) return;
    last_input = time(nullptr);
    // a shell tab: everything goes to the shell (/bin/ls too) but iSekur's own commands
    if (b->type == BT::SHELL && (in[0] != '/' || !find_command(in.substr(1, in.find(' ') - 1)))) {
        shell_line(b, in);
        return;
    }
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
    if (s) s->typed_in = b->id, s->typed_at = time(nullptr);
    auto need = [&]() {
        if (s && s->state == SS::CONNECTED) return true;
        print(b, LK::ERROR, "Not connected to a server");
        return false;
    };

    if (cmd == "help") { help(b, args); return; }
    if (cmd == "about") { about(b); return; }
    if (cmd == "guide") { guide(b, args); return; }
    if (cmd == "typing") {  // /typing [on|off]: like /statusbar, kept until restart unless /save
        if (!args.empty() && args != "on" && args != "off") { print(b, LK::ERROR, "Usage: /typing [on|off]"); return; }
        bool on = args.empty() ? !opt_on(nullptr, "typing_effect", false) : args == "on";
        cfg["general"]["typing_effect"] = unsaved["general"]["typing_effect"] = on ? "on" : "off";
        int n = typing_quiet(on);
        print(b, LK::INFO, std::string("Typing effect ") + (on ? "on: messages type themselves out" : "off") + " (/save to keep it)");
        print(b, LK::INFO, on ? "Quiet mode is on as well" + std::string(n ? " in " + std::to_string(n) + " channel" + (n > 1 ? "s" : "") : "") +
                                    ": joins, parts, quits and nick changes are hidden while typing is on"
                              : n ? "Quiet mode is off again in " + std::to_string(n) + " channel" + (n > 1 ? "s" : "") +
                                        " (the ones you made quiet yourself stay quiet)"
                                  : "Quiet mode unchanged: the quiet channels were made quiet by you");
        return;
    }
    if (cmd == "e2e" || cmd == "sq") { e2e_cmd(b, args); return; }
    if (cmd == "exec" || cmd == "sh") { exec_cmd(b, args); return; }
    if (cmd == "dcc") { dcc_cmd(b, args); return; }
    if (cmd == "shell") { shell_cmd(b); return; }
    if (cmd == "attach") { attach_cmd(b, args); return; }
    if (cmd == "lock") { lock_cmd(b, args); return; }
    if (cmd == "servers") {  // the terminal: the settings window at the first server (the webchat opens its Servers window)
        if (tty && !from_web) { settings_win = settings_servers = dirty = true; return; }
        if (servers.empty()) { print(b, LK::INFO, "No servers: /server host [port] [-tls] adds one"); return; }
        for (auto& sv : servers)
            print(b, LK::INFO, sv->cfg.name + "  " + sv->cfg.host + ":" + std::to_string(sv->cfg.port) + (sv->cfg.tls ? " (TLS)" : "") +
                                   (sv->state == SS::CONNECTED ? "  connected" : sv->state == SS::DISCONNECTED ? "  not connected" : "  connecting") +
                                   (sv->cfg.autoconnect ? "" : "  (not at startup)"));
        return;
    }
    if (cmd == "set") {  // /set [section.]key [value]; section defaults to general
        auto secret = secret_key;
        if (argv.empty()) {
            if (tty && !from_web) { settings_win = dirty = true; return; }  // the terminal's settings window
            for (auto& st : settings()) print(b, LK::INFO, st.key + " = " + st.val);
            return;
        }
        size_t dot = argv[0].rfind('.');
        std::string sec = dot == std::string::npos ? "general" : argv[0].substr(0, dot);
        std::string key = dot == std::string::npos ? argv[0] : argv[0].substr(dot + 1);
        std::string val = rest(1);
        if (val == "\"\"") val.clear();  // /set key "": empty, i.e. the default again
        if (key.empty() || sec.empty()) { print(b, LK::ERROR, "Usage: /set [section.]key value"); return; }
        if (argv.size() == 1) {
            std::string def = ini_get(defaults, sec, key);
            print(b, LK::INFO, argv[0] + " = " + (secret(key) && !ini_get(cfg, sec, key).empty() ? "********" : ini_get(cfg, sec, key, def.empty() ? "(not set: default)" : def + "  (default)")));
            return;
        }
        if (!unsaved[sec].count(key)) set_was[sec][key] = cfg[sec][key];
        // back to what it was (the webchat's Close undoing a switch): nothing to save. Keys made unsaved by
        // /ignore, /hilight... have no set_was: they stay unsaved
        if (set_was.count(sec) && set_was[sec].count(key) && set_was[sec][key] == val) {
            unsaved[sec].erase(key), set_was[sec].erase(key);
            if (unsaved[sec].empty()) unsaved.erase(sec);
        } else unsaved[sec][key] = val;
        cfg[sec][key] = val;
        std::string err;
        bool saved_now = false;
        if (sec == "general" && key == "theme") {
            if (load_theme(val, theme, err)) {
                Ini part;
                part["general"]["theme"] = val;
                // a theme with its own font (mirc: Fixedsys) brings it; a /set web_font after this wins again
                if (!theme.lay("font", "").empty() && !ini_get(cfg, "general", "web_font").empty())
                    cfg["general"]["web_font"] = part["general"]["web_font"] = "";
                // a theme you keep sticks: written now, so the next start opens with it, no /save needed
                for (auto& [k, v] : part["general"]) unsaved["general"].erase(k), set_was["general"].erase(k);
                if (unsaved["general"].empty()) unsaved.erase("general");
                if (!cfg_path.empty()) {
                    if (ini_save(cfg_path, part, err)) saved_now = true;
                    else print(b, LK::ERROR, err), unsaved["general"].insert(part["general"].begin(), part["general"].end());
                }
                notify(Ev::THEME, nullptr);
            } else print(b, LK::ERROR, err);
        }
        // servers pick up host/port/sasl/... on their next connect; flood limits now
        for (auto& sv : servers) {
            sv->cfg = server_cfg(sv->cfg.name);
            if (key == "nick" && (sec == "general" || sec == "server." + sv->cfg.name) && !sv->last_nick.empty())
                remember_nick(sv.get(), "");  // /set nick wins over the last /nick
            load_flood(sv.get());
        }
        if (sec == "keys") cfg_changed = true;
        if (sec == "general" && key == "scrollback") max_lines = (size_t)std::max(100, atoi(opt(nullptr, "scrollback", "2000").c_str()));
        if (sec == "general" && (key.rfind("timestamp", 0) == 0 || key == "scrollback" || key.rfind("web_font", 0) == 0)) notify(Ev::THEME, nullptr);  // the webchat redraws its lines with them
        if (sec == "general" && key == "typing_effect") typing_quiet(opt_on(nullptr, "typing_effect", false));  // like /typing
        dirty = true;
        if (saved_now) { print(b, LK::INFO, argv[0] + " = " + val + "  (saved)"); return; }
        print(b, LK::INFO, argv[0] + " = " + (secret(key) ? "********" : val) +
                               (sec.rfind("server.", 0) == 0 ? "  (used on the next /connect)"
                                : sec == "web"              ? "  (applies after /web restart)"
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
        unsaved.clear(), set_was.clear();
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
    if (cmd == "server" && !argv.empty() && argv[0] == "del") {  // /server del name: out of iSekur and the config
        if (argv.size() < 2) { print(b, LK::ERROR, "Usage: /server del name"); return; }
        std::string name = argv[1], sec = "server." + name, err;
        Server* t = nullptr;
        for (auto& sv : servers)
            if (sv->cfg.name == name) t = sv.get();
        if (!t && !cfg.count(sec)) { print(b, LK::ERROR, "No server named " + name); return; }
        if (t) close(t->status);  // disconnects and closes its tabs; b may be one of them
        cfg.erase(sec), unsaved.erase(sec), set_was.erase(sec);
        Buffer* home = bufs.front().get();
        if (!cfg_path.empty() && !ini_drop_section(cfg_path, sec, err)) { print(home, LK::ERROR, err); return; }
        print(home, LK::INFO, "Deleted server " + name + " (and its [" + sec + "] in the config)");
        return;
    }
    if (cmd == "server") {
        if (argv.empty()) { print(b, LK::ERROR, "Usage: /server host [port] [-tls] [-noverify] [-name name] [-nick nick] | del name"); return; }
        std::string name = argv[0], nick, port;
        bool tls = false, verify = true;
        for (size_t i = 1; i < argv.size(); i++) {
            if (argv[i] == "-tls") tls = true;
            else if (argv[i] == "-noverify") verify = false;
            else if ((argv[i] == "-name" || argv[i] == "-nick") && i + 1 < argv.size()) (argv[i] == "-name" ? name : nick) = argv[i + 1], i++;
            else port = argv[i];
        }
        // the name is a config section: [server.<name>]
        if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos) {
            print(b, LK::ERROR, "A server name can have letters, digits, . _ and - only");
            return;
        }
        for (auto& sv : servers)
            if (sv->cfg.name == name) { connect(sv.get()); select(sv->status); return; }
        std::string sec = "server." + name;
        if (port.empty() || (tls && port == "6667")) port = tls ? "6697" : "6667";
        // remembered like /set, so /save keeps it for the next start
        for (auto& [k, v] : std::map<std::string, std::string>{
                 {"host", argv[0]}, {"port", port},
                 {"tls", tls ? "true" : "false"}, {"tls_verify", verify ? "true" : "false"}})
            cfg[sec][k] = unsaved[sec][k] = v;
        if (!nick.empty()) cfg[sec]["nick"] = unsaved[sec]["nick"] = nick;
        ServerCfg sc = server_cfg(name);
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
    if (cmd == "twitch") { twitch_cmd(b, args); return; }  // twitch.cpp
    if (cmd == "disconnect") {
        if (!s) return;
        s->user_quit = true;
        s->send_now("QUIT :" + (args.empty() ? bye(s, "quit_message") : args));
        if (s->conn.st == Conn::OPEN) s->conn.pump(0, s->inbuf);
        disconnect(s, "");
        return;
    }
    if (cmd == "reconnect") {  // a QUIT (message as given), then straight back in
        if (!s) { print(b, LK::ERROR, "Not a server's tab: /connect name"); return; }
        if (s->state != SS::DISCONNECTED) exec(b, "/disconnect " + args);
        connect(s);
        return;
    }
    if (cmd == "fold") {  // the server's channels out of the tab bar and back (right click a server tab)
        if (!s) { print(b, LK::ERROR, "Not a server's tab"); return; }
        fold(s->status);
        return;
    }
    if (cmd == "win" || cmd == "window") {
        int n = atoi(args.c_str());
        if (n >= 1 && n <= (int)bufs.size()) select(bufs[(size_t)n - 1].get());
        return;
    }
    if (cmd == "close" || cmd == "wc") { close(b); return; }
    if (cmd == "split") { split_cmd(b, args); return; }
    if (cmd == "unsplit" || cmd == "merge") { split_cmd(b, "off"); return; }
    if (cmd == "quiet") {  // per tab, until restart
        b->quiet = args == "on" || (args != "off" && !b->quiet);
        b->quiet_typing = false;  // your choice now: /typing off leaves it
        notify(Ev::BUFFER, b);
        print(b, LK::INFO, std::string("Quiet mode ") + (b->quiet ? "on: joins, parts, quits and nick changes are hidden here"
                                                                  : "off: showing joins, parts, quits and nick changes"));
        return;
    }
    if (cmd == "statusbar") {  // /statusbar [item [on|off]]: what status_info shows right of the status bar
        static const std::vector<std::string> items = {"up", "cpu", "mem", "load", "bat"};
        auto on = split(opt(nullptr, "status_info", "up"), ' ');
        if (argv.empty()) {
            std::string l;
            for (auto& it : items) l += " " + it + (std::find(on.begin(), on.end(), it) != on.end() ? ":on" : ":off");
            print(b, LK::INFO, "Status bar:" + l + "  (/statusbar item [on|off])");
            return;
        }
        if (std::find(items.begin(), items.end(), argv[0]) == items.end() || (argv.size() > 1 && argv[1] != "on" && argv[1] != "off")) {
            print(b, LK::ERROR, "Usage: /statusbar [up|cpu|mem|load|bat] [on|off]");
            return;
        }
        auto it = std::find(on.begin(), on.end(), argv[0]);
        bool want = argv.size() > 1 ? argv[1] == "on" : it == on.end();
        if (!want && it != on.end()) on.erase(it);
        if (want && it == on.end()) on.push_back(argv[0]);
        std::string joined;
        for (auto& x : items)  // kept in the order they're drawn
            if (std::find(on.begin(), on.end(), x) != on.end()) joined += (joined.empty() ? "" : " ") + x;
        cfg["general"]["status_info"] = unsaved["general"]["status_info"] = joined;
        dirty = true;
        print(b, LK::INFO, argv[0] + (want ? " on" : " off") + " in the status bar (/save to keep)");
        return;
    }
    if (cmd == "detach") {
        if (tty) detach_req = true;
        else print(b, LK::ERROR, "No terminal is attached; isekur --attach brings one");
        return;
    }
    if (cmd == "web") {  // /web [start|stop|restart|status]
        std::string a = argv.empty() ? "status" : argv[0];
        web_req_buf = b;
        if (a == "start") web_req = WebReq::START;
        else if ((a == "stop" || a == "restart") && daemon) print(b, LK::ERROR, "The webchat is the only way into this iSekur; not stopping it (restart iSekur instead)");
        else if (a == "stop") web_req = WebReq::STOP;
        else if (a == "restart") web_req = WebReq::RESTART;
        else if (a == "status") print(b, LK::INFO, web_url.empty() ? "Webchat is stopped (/web start)" : "Webchat on " + web_url);
        else print(b, LK::ERROR, "Usage: /web [start|stop|restart|status]");
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
    if (cmd == "clear") { b->lines.clear(); b->scroll = 0; dirty = true; notify(Ev::CLEAR, b); return; }
    if (cmd == "reload") { reload(b); return; }
    if (cmd == "theme") {
        std::string err;
        if (args.empty() && tty && !from_web) { theme_win = dirty = true; return; }  // the terminal's theme picker
        if (args.empty()) {
            std::string l;
            for (auto& n : theme_names()) l += (l.empty() ? "" : ", ") + n;
            print(b, LK::INFO, "Current theme: " + theme.name + "  (installed: " + l + ")");
        }
        else {  // like keeping it in a picker: /set theme, so it's written and the next start opens with it
            Theme t;
            if (load_theme(args, t, err)) exec(b, "/set theme " + args);
            else print(b, LK::ERROR, err);
        }
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
    if (cmd == "autoconnect" && s) {  // /autoconnect [on|off]: connect this server when iSekur starts; written now
        std::string sec = "server." + s->cfg.name;
        if (argv.empty()) { print(b, LK::INFO, s->cfg.name + (s->cfg.autoconnect ? " connects" : " doesn't connect") + " when iSekur starts (/autoconnect on|off)"); return; }
        if (argv[0] != "on" && argv[0] != "off") { print(b, LK::ERROR, "Usage: /autoconnect [on|off]"); return; }
        s->cfg.autoconnect = argv[0] == "on";
        cfg[sec]["autoconnect"] = unsaved[sec]["autoconnect"] = s->cfg.autoconnect ? "true" : "false";
        if (save_section(*this, sec, b)) print(b, LK::INFO, s->cfg.name + (s->cfg.autoconnect ? " connects" : " no longer connects") + " when iSekur starts (saved)");
        return;
    }
    if (!need()) return;
    if (cmd == "join" || cmd == "j") {
        if (argv.empty()) return;
        std::string ch = s->is_chan(argv[0]) ? argv[0] : "#" + argv[0];
        if (s->twitch()) ch = irc_lower(ch);  // Twitch ignores a JOIN #Name
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
        // written now, like snap: the tab menu's "Remove from autojoin" must hold at the next start without a /save
        if (save_section(*this, sec, b))
            print(b, LK::INFO, (argv[0] == "add" ? "Added " + ch + " to" : "Removed " + ch + " from") + " autojoin on " + s->cfg.name + " (saved)");
        return;
    }
    if (cmd == "part") {
        std::string ch = !argv.empty() && s->is_chan(argv[0]) ? argv[0] : b->name;
        std::string msg = !argv.empty() && s->is_chan(argv[0]) ? rest(1) : args;
        if (!s->is_chan(ch)) { print(b, LK::ERROR, "Not a channel"); return; }
        if (Buffer* cb = find(s, ch)) { close(cb, msg); return; }  // like /close and Ctrl+W: the tab goes too
        if (s->twitch()) s->send("PART " + irc_lower(ch));
        else s->send("PART " + ch + " :" + (msg.empty() ? bye(s, "part_message") : msg));
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
                                    s->nick, "-> " + argv[0] + ": " + mask_services(argv[0], part), true});
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
    // one-user mode shortcuts in a channel tab: /op bob alice, /ban spammer, /unban *!*@evil.host
    static const std::map<std::string, std::string> quick = {
        {"op", "+o"}, {"deop", "-o"}, {"voice", "+v"}, {"devoice", "-v"},
        {"halfop", "+h"}, {"dehalfop", "-h"}, {"ban", "+b"}, {"unban", "-b"}};
    auto qm = quick.find(cmd);
    if (qm != quick.end()) {
        if (b->type != BT::CHANNEL) { print(b, LK::ERROR, "/" + cmd + " works in a channel's tab"); return; }
        if (!need()) return;
        std::vector<std::string> who;
        for (auto& w : argv)
            if (!w.empty()) who.push_back(qm->second[1] == 'b' && w.find_first_of("!@*") == std::string::npos ? w + "!*@*" : w);  // a bare nick bans nick!*@*
        if (who.empty()) {
            if (cmd == "ban") s->send("MODE " + b->name + " +b");  // the ban list
            else print(b, LK::ERROR, "Usage: /" + cmd + (qm->second[1] == 'b' ? " nick|mask ..." : " nick ..."));
            return;
        }
        size_t per = (size_t)std::max(1, s->max_modes);
        for (size_t i = 0; i < who.size(); i += per) {
            std::string modes(1, qm->second[0]), list;
            for (size_t k = i; k < who.size() && k < i + per; k++) modes += qm->second[1], list += " " + who[k];
            s->send("MODE " + b->name + " " + modes + list);
        }
        return;
    }
    // in a channel's tab, /kick nick, /mode +m and /invite nick mean this channel
    if (s && b->type == BT::CHANNEL) {
        if (cmd == "kick" && !argv.empty() && !s->is_chan(argv[0])) args = b->name + " " + args, argv = split(args, ' ');
        else if (cmd == "mode" && (args.empty() || args[0] == '+' || args[0] == '-')) args = b->name + (args.empty() ? "" : " " + args), argv = split(args, ' ');
        else if (cmd == "invite" && argv.size() == 1) args += " " + b->name, argv = split(args, ' ');
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
    if (cmd == "list" && !argv.empty() && argv[0] == "sort") { list_sort(b, argv.size() > 1 ? argv[1] : ""); return; }
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
