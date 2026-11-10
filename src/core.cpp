// Buffers, config loading and user commands.
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <pwd.h>
#include <glob.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/utsname.h>
#include "core.h"
#include "net.h"
#include "session.h"
#include "web.h"  // check_password
#include <thread>
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
    defaults["general"]["nick"] = defaults["general"]["altnick"] = defaults["web"]["user"] = defaults["bnc"]["user"] = "";  // placeholders
    std::string err;
    theme_dir = ini_get(cfg, "general", "theme_dir", "themes");
    bool no_themes = false;
    if (!load_theme(ini_get(cfg, "general", "theme", "default"), theme, err)) {
        if (!load_theme("default", theme, err)) theme = Theme{}, no_themes = true;
    }
    Buffer* home = open(nullptr, BT::STATUS, "cmd");
    for (auto& l : banner_lines(theme, opt_on(nullptr, "banner_mono", false))) print(home, LK::INFO, l);
    if (created)
        print(home, LK::INFO, "Wrote " + cfg_path + " with every setting: edit it, or /set key value and /save. "
                              "Add a server with /server irc.sekurnet.org 6697 -tls -name sekurnet");
    else if (!found)
        print(home, LK::INFO, "No config at " + cfg_path +
                                  " - see iSekur.conf.example. Try /server irc.sekurnet.org 6697 -tls -name sekurnet");
    print(home, LK::INFO, "Welcome to " + version_string() + ". /help lists commands.");
    print(home, LK::INFO, "\x03" "01,09" "\x02" " NEW HERE? " "\x0f" " Type " "\x03" "11" "\x02" "/guide" "\x0f"
                          " for a quick tour of the first steps and features");
    if (no_themes) {  // else it just looks plain and nobody says why
        std::string ds;
        for (auto& d : theme_dirs()) ds += (ds.empty() ? "" : ", ") + d;
        print(home, LK::ERROR, "No themes found (running uncoloured). Looked in: " + ds +
                                   ". Put the .theme files in one of these, or set [general] theme_dir");
    }
    print(home, LK::INFO, "");  // room before what comes next (webchat, connecting...)
    if (!found || opt_on(nullptr, "show_guide", false)) guide(home, "");  // a new config has show_guide = on

    // [general] server_order (a server tab dragged, /move) first, then the rest A-Z
    std::vector<std::string> names, order = split(ini_get(cfg, "general", "server_order"), ',');
    for (auto& [sec, kv] : cfg)
        if (sec.rfind("server.", 0) == 0) names.push_back(sec.substr(7));
    auto rank = [&](const std::string& n) { return std::find(order.begin(), order.end(), n) - order.begin(); };
    std::stable_sort(names.begin(), names.end(), [&](auto& x, auto& y) { return rank(x) < rank(y); });
    sts_load();
    for (auto& n : names) add_server(server_cfg(n));
    plugins_sync();  // before connecting: a proxy plugin must see the first connection too
    for (auto& s : servers)
        if (s->cfg.autoconnect) connect(s.get());
    if (old_away_keys(*this) && access((plugin_dir() + "/autoaway.lua").c_str(), F_OK) != 0)
        info_note(home, "Auto-away is a plugin now, and your config still has its settings: /plugin install autoaway "
                              "moves them into it (until then nobody sets you away)");
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
std::string release_tag(const std::string& head) {
    std::string low = head;
    to_lower(low);
    size_t loc = low.find("\nlocation:");
    if (loc == std::string::npos) return "";
    size_t eol = low.find('\r', loc), tag = low.find("/tag/v", loc);
    if (tag == std::string::npos || tag > eol) return "";
    std::string v = head.substr(tag + 6, eol - tag - 6);
    return !v.empty() && v.find_first_not_of("0123456789.") == std::string::npos ? v : "";
}

bool version_newer(const std::string& a, const std::string& b) {
    auto pa = split(a, '.'), pb = split(b, '.');
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); i++) {
        int x = i < pa.size() ? atoi(pa[i].c_str()) : 0, y = i < pb.size() ? atoi(pb[i].c_str()) : 0;
        if (x != y) return x > y;
    }
    return false;
}

void Core::check_update(time_t now) {
    if (upd && upd->done.load(std::memory_order_acquire)) {
        std::string v = upd->ver;
        upd.reset();
        if (version_newer(v, ISEKUR_VERSION) && v != update_ver) {
            update_ver = v;
            info_note(nullptr, "\x02\u2191 A new release is available: iSekur " + v + "\x02 (you have " ISEKUR_VERSION ")");
            print(nullptr, LK::INFO, "  Update with \x02" "brew upgrade isekur\x02, or download it from https://github.com/gcanosa/iSekur/releases/latest · /about");
            if (Buffer* a = find(nullptr, "*about")) if (a == active) about(nullptr);
            notify(Ev::UPDATE, nullptr);
        }
    }
    if (upd || now < upd_at || !opt_on(nullptr, "check_updates", true)) return;
    upd_at = now + 86400;
    upd = std::make_shared<UpdateCheck>();
    std::thread([u = upd] {
        u->ver = release_tag(https_head("github.com", "/gcanosa/iSekur/releases/latest"));
        u->done.store(true, std::memory_order_release);
    }).detach();
}

std::vector<std::string> about_lines(const Theme& t, double shine, bool web, std::vector<int>* typed, const std::string& update) {
    auto out = banner_lines(t, false, shine, web && t.lay("window", "") == "win95" ? 1 : -1);  // the webchat's win95 About: gray
    const char* k = "\x03" "14";  // labels gray, values in the text color
    for (std::string l : {std::string(""),
                          std::string("\x02iSekur " ISEKUR_VERSION "\x02  a colorful terminal IRC client with a built-in webchat"),
                          std::string(k) + "multi-server · IRCv3 · TLS + SASL · E2E channels and queries · /shell · detach and attach",
                          std::string(""),
                          std::string(k) + "Author     \x0f" "Gerardo Canosa",
                          std::string(k) + "Website    \x0f" "https://gcanosa.github.io/isekur-site/",
                          std::string(k) + "Source     \x0f" "https://github.com/gcanosa/iSekur",
                          std::string(k) + "Download   \x0f" "https://github.com/gcanosa/iSekur/releases/latest",
                          update.empty() ? std::string("\x01") : std::string(k) + "Update     \x0f\x02iSekur " + update + " is out\x02  brew upgrade isekur, or the Download link",
                          std::string(k) + "License    \x0f" "GPL-3.0-or-later: use it, change it, share it",
                          std::string(""),
                          std::string(k) + "Sponsored  \x0f\x02" "irc.sekurnet.org\x02  the network iSekur calls home",
                          std::string(k) + "Support    \x0f\x02" "#iSekur\x02 there: questions, bugs, ideas",
                          std::string(k) + "           \x0f" "/server irc.sekurnet.org 6697 -tls -name sekurnet, then /join #iSekur",
                          std::string(""),
                          std::string(k) + (web ? "/help lists the commands · Esc closes this" : "/help lists the commands · Ctrl+W closes this tab")})
        if (l != "\x01") out.push_back(l);  // \x01: no update line
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
static void remember(Core& c, Server* s, const std::string& key, const std::string& v) {
    std::string sec = "server." + s->cfg.name;
    if (ini_get(c.cfg, sec, key) == v) return;
    c.cfg[sec][key] = v;
    bool saved = !c.cfg_path.empty() && !(c.unsaved.count(sec) && c.unsaved[sec].count("host"));
    std::string err;
    if (!saved || !ini_save(c.cfg_path, Ini{{sec, {{key, v}}}}, err)) c.unsaved[sec][key] = v;
}
void Core::remember_nick(Server* s, const std::string& nick) {
    s->last_nick = nick;
    remember(*this, s, "last_nick", nick);
}

void Core::about(Buffer* from) {
    Buffer* b = open(nullptr, BT::QUERY, "*about");
    b->lines.clear(), b->scroll = b->unread = 0;
    notify(Ev::CLEAR, b);
    about_full = about_lines(theme, -99, false, &about_typed, update_ver);
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
        else if ((sec == "general" && (t == "nick" || t == "altnick")) || ((sec == "web" || sec == "bnc") && t == "user"))
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
        to_upper(v);
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

std::string Core::complete_suffix() const {
    std::string v = ini_get(cfg, "general", "complete_suffix", ":");
    return v == "off" || v == "none" ? "" : v;
}

std::vector<Core::Setting> Core::settings() const {
    Ini all = defaults;
    all["general"]["timestamp"] = theme.lay("timestamp", "%H:%M");  // empty: the theme's, so that's its default
    all["general"]["nick_width"] = theme.lay("nick_width", "12");
    for (auto& [sec, kv] : cfg)
        for (auto& [k, v] : kv)
            if (sec.rfind("plugin.", 0) != 0 && (!v.empty() || !all[sec].count(k))) all[sec][k] = v;  // "key =" keeps the default; plugins: /plugin
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

// Every key iSekur reads from [general] and [server.<name>], in the settings window's order, with its topic
// and where it can be set: 'g' [general] only, 'n' a network only, 'b' [general] and a network can override
// it (opt() reads it that way). A key added here belongs in iSekur.conf.example too (the tests check both ways)
static const std::vector<KeyInfo> KEYS = {
    {"name", "Connection", 'n'}, {"host", "Connection", 'n'}, {"port", "Connection", 'n'}, {"tls", "Connection", 'n'},
    {"tls_verify", "Connection", 'n'}, {"autoconnect", "Connection", 'n'}, {"encoding", "Connection", 'n'},
    {"password", "Login", 'n'}, {"auto_identify", "Login", 'n'}, {"sasl_mech", "Login", 'n'}, {"sasl_user", "Login", 'n'},
    {"sasl_pass", "Login", 'n'}, {"nickserv_pass", "Login", 'n'}, {"cert", "Login", 'n'}, {"key", "Login", 'n'},
    {"nick", "Identity", 'b'}, {"altnick", "Identity", 'b'}, {"user", "Identity", 'b'}, {"realname", "Identity", 'b'},
    {"theme", "Look", 'g'}, {"theme_dir", "Look", 'g'}, {"web_font", "Look", 'g'}, {"web_font_size", "Look", 'g'}, {"web_tree_lines", "Look", 'g'}, {"web_tree_width", "Look", 'g'}, {"web_unread_badge", "Look", 'g'}, {"colors", "Look", 'g'},
    {"timestamp", "Look", 'g'}, {"timestamp_align", "Look", 'g'}, {"timestamp_hover", "Look", 'g'}, {"nick_width", "Look", 'g'}, {"scrollback", "Look", 'g'}, {"mouse", "Look", 'g'},
    {"status_info", "Look", 'g'}, {"cpu_levels", "Look", 'g'}, {"mem_levels", "Look", 'g'}, {"typing_effect", "Look", 'b'},
    {"banner_mono", "Look", 'g'}, {"banner_anim", "Look", 'g'}, {"show_guide", "Look", 'g'},
    {"hilight_words", "Messages", 'n'}, {"hilight_window", "Messages", 'b'}, {"bell_notifications", "Messages", 'b'},
    {"paste_confirm", "Messages", 'b'}, {"complete_suffix", "Messages", 'g'}, {"send_typing", "Messages", 'b'}, {"quit_message", "Messages", 'b'}, {"part_message", "Messages", 'b'},
    {"ignore", "Messages", 'n'}, {"mute", "Messages", 'n'}, {"notify", "Messages", 'n'},
    {"ctcp_version", "Messages", 'b'}, {"ctcp_time", "Messages", 'b'}, {"ctcp_ping", "Messages", 'b'},
    {"autojoin", "Channels", 'n'}, {"rejoin_on_kick", "Channels", 'b'}, {"history_fetch", "Channels", 'b'}, {"e2e_keys", "Channels", 'n'},
    {"log", "Logs", 'b'}, {"log_dir", "Logs", 'b'}, {"log_history", "Logs", 'b'}, {"log_rotate", "Logs", 'b'}, {"keep_history", "Logs", 'b'},
    {"flood_burst", "Network", 'b'}, {"flood_interval_ms", "Network", 'b'},
    {"dcc", "Files", 'g'}, {"dcc_dir", "Files", 'b'}, {"dcc_ip", "Files", 'b'}, {"dcc_ports", "Files", 'b'}, {"dcc_web", "Files", 'g'},
    {"lock", "Security", 'g'}, {"lock_idle", "Security", 'g'}, {"lock_password_hash", "Security", 'g'}, {"pam_service", "Security", 'g'},
    {"exec_timeout", "Security", 'g'}, {"exec_web", "Security", 'g'}, {"fifo", "Session", 'g'}, {"fifo_path", "Session", 'g'},
    {"check_updates", "Updates", 'g'},
};
const std::vector<KeyInfo>& setting_keys() { return KEYS; }
const KeyInfo* key_info(const std::string& key) {
    static const std::map<std::string, const KeyInfo*> by = [] {
        std::map<std::string, const KeyInfo*> m;
        for (auto& k : KEYS) m[k.key] = &k;
        return m;
    }();
    auto it = by.find(key);
    return it == by.end() ? nullptr : it->second;
}
// state iSekur keeps in the config itself: never listed
static bool state_key(const std::string& sec, const std::string& k) { return (sec == "general" && k == "server_order") || k == "last_nick" || k == "open_queries"; }

// the template's keys, commented or not ("; key =", "#key ="; "# prose = ..." isn't one), by section
std::vector<std::pair<std::string, std::string>> conf_keys() {
    static std::vector<std::pair<std::string, std::string>> out;
    if (!out.empty()) return out;
    std::istringstream in(conf_template);
    std::string line, cur;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == '[') { cur = line.substr(1, line.find(']') - 1); continue; }
        size_t a = line[0] == '#' ? 1 : line.find_first_not_of("; "), e = line.find('=');
        if (a == std::string::npos || e == std::string::npos || e < a || (line[0] == '#' && line.size() > 1 && line[1] == ' ')) continue;
        std::string k = line.substr(a, e - a);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        if (!k.empty() && k.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos) out.push_back({cur, k});
    }
    return out;
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
    std::vector<SetRow> out;
    auto themes = theme_names();
    auto add = [&](const std::string& sec, const std::string& k, const std::string& def, const std::string& grp) -> SetRow& {
        SetRow r{sec, k, ini_get(cfg, sec, k), def, unsaved.count(sec) && unsaved.at(sec).count(k), secret_key(k), {}, grp, {}, {}, false, {}};
        bool srv = sec.rfind("server.", 0) == 0;
        r.cat = sec == "general" ? "general/" + grp : srv ? sec + "/" + grp : sec;
        r.head = sec == "general" ? grp : srv ? "⇢ " + sec.substr(7) + " · " + grp : "[" + sec + "]";
        std::string v = r.val.empty() ? def : r.val;
        to_lower(v);
        if (sec == "general" && k == "theme") r.choices = themes;
        else if (sec == "general" && k == "timestamp_align") r.choices = {"left", "right"};
        else if (sec == "general" && k == "timestamp_hover") r.choices = {"off", "web", "term", "both"};
        else if (sec == "general" && k == "web_font") r.choices = {"theme", "mono", "sans", "serif"};  // theme: its font, else mono
        else if (sec == "general" && k == "web_font_size") r.choices = {"12", "13", "14", "15", "16", "18", "20", "22"};
        else if (v == "on" || v == "off") r.choices = {"on", "off"};
        else if (v == "true" || v == "false") r.choices = {"true", "false"};
        else if (v == "yes" || v == "no") r.choices = {"yes", "no"};
        out.push_back(r);
        return out.back();
    };
    // what [general] uses when a key isn't set: the template's value (the theme's timestamp and nick column)
    auto gdef = [&](const std::string& k) {
        return k == "timestamp" ? theme.lay("timestamp", "%H:%M") : k == "nick_width" ? theme.lay("nick_width", "12") : ini_get(defaults, "general", k);
    };
    std::vector<std::string> nets;
    for (auto& [sec, kv] : cfg)
        if (sec.rfind("server.", 0) == 0) nets.push_back(sec);
    // [general], by topic; a key a network overrides says where
    for (auto& k : KEYS) {
        if (k.scope == 'n') continue;
        SetRow& r = add("general", k.key, gdef(k.key), k.grp);
        if (k.scope == 'b')
            for (auto& sec : nets)
                if (std::string v = ini_get(cfg, sec, k.key); !v.empty())
                    r.over += (r.over.empty() ? "" : ", ") + sec.substr(7) + ": " + (secret_key(k.key) ? "********" : v);
    }
    auto unknown = [&](const std::string& sec) {  // keys in the config iSekur doesn't know (typos, old ones): Other
        if (cfg.count(sec))
            for (auto& [k, v] : cfg.at(sec))
                if (!key_info(k) && !state_key(sec, k)) add(sec, k, "", "Other");
    };
    unknown("general");
    // each network: its own keys, with what it uses when they aren't set (server_cfg's fallbacks), and
    // every [general] key it can override: not set here, it inherits [general]'s
    for (auto& sec : nets) {
        ServerCfg d = server_cfg(sec.substr(7));
        std::map<std::string, std::string> uses = {
            {"name", d.name}, {"host", d.host}, {"port", std::to_string(d.port)}, {"tls", d.tls ? "true" : "false"},
            {"tls_verify", d.tls_verify ? "true" : "false"}, {"autoconnect", d.autoconnect ? "true" : "false"},
            {"auto_identify", d.auto_identify ? "true" : "false"}, {"encoding", d.encoding.empty() ? "utf-8" : d.encoding}, {"nick", d.nick}, {"altnick", d.altnick}, {"user", d.user}, {"realname", d.realname}};
        for (auto& k : KEYS) {
            if (k.scope == 'g') continue;
            auto u = uses.find(k.key);
            SetRow& r = add(sec, k.key, u != uses.end() ? u->second : k.scope == 'b' ? ini_get(cfg, "general", k.key, gdef(k.key)) : "", k.grp);
            if (k.key == std::string("name")) r.val = d.name;  // not a key: the section's name, renamed by /server rename
            r.shared = k.scope == 'b';
        }
        unknown(sec);
    }
    // the other sections ([web], [keys]...): the template's keys in its order, then the config's
    std::vector<std::string> secs;
    for (auto& [sec, k] : conf_keys())
        if (sec != "general" && sec.rfind("server.", 0) != 0 && std::find(secs.begin(), secs.end(), sec) == secs.end()) secs.push_back(sec);
    for (auto& [sec, kv] : cfg)  // [plugin.*]: /plugin's
        if (sec != "general" && sec.rfind("server.", 0) != 0 && sec.rfind("plugin.", 0) != 0 && std::find(secs.begin(), secs.end(), sec) == secs.end()) secs.push_back(sec);
    for (auto& sec : secs) {
        std::vector<std::string> ks;
        for (auto& [s2, k] : conf_keys())
            if (s2 == sec && defaults.count(sec) && defaults.at(sec).count(k) && std::find(ks.begin(), ks.end(), k) == ks.end()) ks.push_back(k);
        if (cfg.count(sec))
            for (auto& [k, v] : cfg.at(sec))
                if (std::find(ks.begin(), ks.end(), k) == ks.end()) ks.push_back(k);
        for (auto& k : ks) add(sec, k, ini_get(defaults, sec, k), "");
    }
    return out;
}

// what a [server.<name>] can be called
bool server_name_ok(const std::string& n) {
    return !n.empty() && n.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == std::string::npos;
}

// a network's new name: its [server.x] (in the file at once, comments kept), unsaved changes, place in server_order,
// tab, logs folder (when the new one isn't there yet: logs are never merged) and its tabs' kept history.
// Only a local label: no reconnect
bool Core::rename_server(Buffer* b, const std::string& from, const std::string& to) {
    std::string os = "server." + from, ns = "server." + to, err;
    Server* t = nullptr;
    for (auto& sv : servers)
        if (sv->cfg.name == from) t = sv.get();
    if (!t && !cfg.count(os)) { print(b, LK::ERROR, "No server named " + from); return false; }
    if (from == to) return true;
    if (!server_name_ok(to)) { print(b, LK::ERROR, "A server name can have letters, digits, . _ and - only"); return false; }
    bool taken = cfg.count(ns);
    for (auto& sv : servers) taken = taken || sv->cfg.name == to;
    if (taken) { print(b, LK::ERROR, "There's a server named " + to + " already"); return false; }
    if (!cfg_path.empty() && !ini_rename_section(cfg_path, os, ns, err)) { print(b, LK::ERROR, err); return false; }
    std::string old_logs = t ? log_folder(t) : "";
    for (Ini* m : {&cfg, &unsaved, &set_was})
        if (m->count(os)) (*m)[ns] = std::move((*m)[os]), m->erase(os);
    auto ord = split(ini_get(cfg, "general", "server_order"), ',');
    if (std::find(ord.begin(), ord.end(), from) != ord.end()) {
        std::string v;
        for (auto& n : ord) v += (v.empty() ? "" : ",") + (n == from ? to : n);
        cfg["general"]["server_order"] = v;
        if (!ini_save(cfg_path, Ini{{"general", {{"server_order", v}}}}, err)) unsaved["general"]["server_order"] = v;
    }
    hist_read();
    for (auto it = hist_cache.begin(); it != hist_cache.end();)
        if (it->first.rfind(from + "\t", 0) == 0) hist_cache[to + it->first.substr(from.size())] = std::move(it->second), it = hist_cache.erase(it);
        else ++it;
    std::string note;
    if (t) {
        t->cfg.name = to;
        for (auto& p : bufs)
            if (p->srv == t) {
                if (p->log) fclose(p->log), p->log = nullptr;  // reopened in the new folder
                if (p->type == BT::STATUS) p->name = to;
                notify(Ev::BUFFER, p.get());
            }
        std::string new_logs = log_folder(t);
        struct stat st;
        if (old_logs != new_logs && stat(old_logs.c_str(), &st) == 0) {
            if (stat(new_logs.c_str(), &st) == 0) note = " (its logs stay in " + old_logs + ": " + new_logs + " exists)";
            else if (rename(old_logs.c_str(), new_logs.c_str()) != 0) note = " (its logs stay in " + old_logs + ": " + strerror(errno) + ")";
        }
    }
    save_history();
    dirty = true;
    print(b, LK::INFO, "Renamed " + from + " to " + to + note);
    return true;
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
    sc.port = atoi(ini_get(cfg, sec, "port").c_str());
    if (sc.port <= 0 || sc.port > 65535) sc.port = sc.tls ? 6697 : 6667;  // port 0 or junk: the usual one
    sc.tls_verify = ini_get(cfg, sec, "tls_verify", "true") == "true";
    sc.autoconnect = ini_get(cfg, sec, "autoconnect", "true") == "true";
    sc.auto_identify = ini_get(cfg, sec, "auto_identify", "true") == "true";
    sc.encoding = ini_get(cfg, sec, "encoding");
    std::string enc = sc.encoding;
    if (to_lower(enc) == "utf-8" || enc == "utf8") sc.encoding.clear();  // the default, spelled out
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
    for (auto& q : split(ini_get(cfg, "server." + sc.name, "open_queries"), ' '))  // open when iSekur stopped
        if (!q.empty()) open(sp, BT::QUERY, q);
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
// modified builds); release tarballs without git just say "iSekur 0.5.2".
std::string version_string() {
    std::string build = ISEKUR_BUILD, v = "iSekur " ISEKUR_VERSION;
    return build.empty() || build == "v" ISEKUR_VERSION ? v : v + " (" + build + ")";
}

// irssi/BitchX style: client, version, where it runs, a tagline and a link
std::string ctcp_version() {
    utsname u{};
    std::string os = uname(&u) == 0 ? std::string(u.sysname) + " " + u.release + "/" + u.machine : "unknown OS";
    return version_string() + " :: " + os +
           " :: modern, secure, lightweight & powerful IRCv3 client :: https://gcanosa.github.io/isekur-site/";
}

// ---- buffers ---------------------------------------------------------------
Buffer* Core::find(Server* s, const std::string& name) {
    std::string n = s ? s->lower(name) : irc_lower(name);
    for (auto& b : bufs)
        if (b->srv == s && b->type != BT::STATUS && (s ? s->lower(b->name) : irc_lower(b->name)) == n) return b.get();
    return nullptr;
}

int Core::index_of(Buffer* b) const {
    for (size_t i = 0; i < bufs.size(); i++)
        if (bufs[i].get() == b) return (int)i;
    return -1;
}

void Core::hist_read() {
    if (hist_loaded) return;
    hist_loaded = true;
    FILE* f = fopen(cfg_side(cfg_path, "history").c_str(), "r");
    if (!f) return;
    std::vector<Line>* cur = nullptr;
    char row[4096];
    while (fgets(row, sizeof row, f)) {
        std::string r = row;
        if (!r.empty() && r.back() == '\n') r.pop_back();
        Line l;
        if (r.rfind("=== ", 0) == 0) cur = &hist_cache[r.substr(4)];
        else if (cur && parse_log_line(r, l)) cur->push_back(l);
    }
    fclose(f);
}

// <config>.history: "=== server\tname" then that tab's rows in the log format. Private, written whole
// ponytail: tabs never reopened stay in it forever; prune by age if it ever grows
void Core::save_history() {
    if (cfg_path.empty()) return;
    hist_saved = now_ms();
    for (auto& sv : servers) {  // the open queries, reopened at the next start ([server.x] open_queries, state)
        std::string q;
        for (auto& b : bufs)
            if (b->srv == sv.get() && b->type == BT::QUERY && b->name[0] != '*') q += (q.empty() ? "" : " ") + b->name;
        remember(*this, sv.get(), "open_queries", q);
    }
    hist_read();  // first: the tabs not open now keep their lines
    auto rows = hist_cache;
    for (auto& b : bufs) {
        Server* s = b->srv;
        if (!s || (b->type != BT::CHANNEL && b->type != BT::QUERY) || b->name[0] == '*') continue;
        int n = atoi(opt(s, "log_history", "200").c_str());
        if (n <= 0 || opt_on(s, "log", false) || !opt_on(s, "keep_history", true)) continue;
        auto& v = rows[s->cfg.name + "\t" + irc_lower(b->name)];
        v.clear();
        for (auto& l : b->lines)
            if (!l.nolog) v.push_back(l);
        if (v.size() > (size_t)n) v.erase(v.begin(), v.end() - n);
    }
    std::string path = cfg_side(cfg_path, "history"), tmp = path + ".tmp";
    bool any = false;
    for (auto& kv : rows) any |= !kv.second.empty();
    if (!any) { unlink(path.c_str()); return; }  // nothing kept: no file, not an empty one
    mode_t old = umask(077);  // chat lines are private, like the logs
    FILE* f = fopen(tmp.c_str(), "w");
    umask(old);
    if (!f) return;
    for (auto& [key, v] : rows) {
        if (v.empty()) continue;
        fprintf(f, "=== %s\n", key.c_str());
        for (auto& l : v) fprintf(f, "%s\n", log_fmt(l).c_str());
    }
    if (fclose(f) == 0) rename(tmp.c_str(), path.c_str());
    else unlink(tmp.c_str());
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
        if (!s) { print(b, LK::ERROR, "Can't close the main window; use /exit"); return; }
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
    if (s && b->name == "*raw" && b->type == BT::QUERY) s->raw_hook = nullptr;  // /rawlog stops with its tab
    bool drop_server = s && b->type == BT::STATUS;
    crumb("close", b->type == BT::STATUS ? "status" : b->type == BT::CHANNEL ? "channel" : b->type == BT::QUERY ? "query" : "shell");
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

void Core::mark_all_read() {
    for (auto& b : bufs)
        if (b->activity || b->unread) b->activity = b->unread = 0, notify(Ev::BUFFER, b.get());
    dirty = true;
}

void Core::select(Buffer* b) {
    if (!b) return;
    if (active && active != b && active->unread) active->unread = 0, notify(Ev::BUFFER, active);
    if (b == split_buf) split_buf = active, split_first = !split_first;  // the other pane's tab: focus moves there
    active = b;
    b->activity = 0;
    dirty = true;
    notify(Ev::BUFFER, b);
    notify(Ev::FOCUS, b);  // the webchat follows the terminal (and a tab picked in the webchat moves the terminal)
}

void Core::add(Buffer* b, Line l) {
    if (!b) return;
    // /typing: other people's messages type themselves out
    if (!l.self && (l.kind == LK::MSG || l.kind == LK::ACTION || l.kind == LK::NOTICE) && opt_on(b->srv, "typing_effect", false))
        l.typed_at = now_ms(), typing_until = std::max(typing_until, l.typed_at + 2100);
    if (!plugin_depth) show(b, l);  // a plugin's string is what the screens show (lolcat); the log keeps what was said
    b->lines.push_back(std::move(l));
    b->added++;
    while (b->lines.size() > max_lines) b->lines.pop_front();
    if (b->scroll) b->scroll_new++;  // keep the scrolled view still: the tui knows how many rows it wraps to
    const Line& ln = b->lines.back();
    int lvl = ln.hilight ? 3 : (ln.kind == LK::MSG || ln.kind == LK::ACTION) && !ln.self ? 2 : b->quiet && noise(ln) ? 0 : 1;
    if (quiet_add || (lvl == 1 && time(nullptr) < b->sync_until)) lvl = 0;  // ponytail: sync_until is a fixed 5s window, end it at 376/422 if a slow MOTD outlasts it
    // the "new" marker goes above the first message you missed, then counts everything after it
    if (b->unread || (b != active && b != split_buf && lvl >= 2)) b->unread++;
    if (b != active && b != split_buf && lvl > b->activity) { b->activity = lvl; notify(Ev::BUFFER, b); }  // split: on screen too
    dirty = true;
    if (ln.said.empty()) log_line(b, ln);
    else { Line o = ln; o.text = ln.said; log_line(b, o); }
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

bool Core::show(Buffer* b, Line& l) {
    if (!b->srv || (l.kind != LK::MSG && l.kind != LK::ACTION && l.kind != LK::NOTICE)) return false;
    std::string orig = l.said.empty() ? l.text : l.said, t = orig;
    plugin_emit("show", b->srv, b->name, {{"nick", l.nick}, {"text", orig}, {"self", l.self},
                                          {"action", l.kind == LK::ACTION}, {"notice", l.kind == LK::NOTICE}}, &t);
    if (t == l.text) return false;
    l.said = t == orig ? "" : std::move(orig), l.text = std::move(t);
    return true;
}

// lolcat turned on paints the backlog too, turned off gives it back. ponytail: every tab at once (2000 lines each by
// default: a blink); the hidden tabs on their first look if a huge scrollback ever makes it slow
void Core::plugins_reshow() {
    if (plugin_depth) return;
    for (auto& bp : bufs) {
        bool changed = false;
        for (auto& l : bp->lines) changed |= show(bp.get(), l);
        if (changed) dirty = true, notify(Ev::CLEAR, bp.get());  // the webchat gets the tab's lines again
    }
}

void Core::fold(Buffer* st) {
    if (st->type != BT::STATUS || !st->srv) return;
    st->collapsed = !st->collapsed, dirty = true;
    notify(Ev::BUFFER, st);
}

// what you leave with: the config's quit_message / part_message, else what iSekur is
std::string Core::bye(const Server* s, const std::string& key) const {
    return opt(s, key, "iSekur IRC " ISEKUR_VERSION);
}

bool Core::muted(const Buffer* b) const {
    if (!b->srv || b->type == BT::STATUS) return false;
    for (auto& ch : split(ini_get(cfg, "server." + b->srv->cfg.name, "mute"), ' '))
        if (b->srv->lower(ch) == b->srv->lower(b->name)) return true;
    return false;
}

std::string Core::opt(const Server* s, const std::string& key, const std::string& def) const {
    std::string g = ini_get(cfg, "general", key, def);
    const KeyInfo* k = s ? key_info(key) : nullptr;
    return k && k->scope == 'g' ? g : s ? ini_get(cfg, "server." + s->cfg.name, key, g) : g;  // a [general]-only key: the network's is ignored
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
    std::vector<Server*> added;
    for (auto& [sec, kv] : cfg) {
        if (sec.rfind("server.", 0) != 0) continue;
        std::string name = sec.substr(7);
        bool known = false;
        for (auto& sv : servers) known = known || sv->cfg.name == name;
        if (known) continue;
        Server* s = add_server(server_cfg(name));
        print(b, LK::INFO, "New server " + name + (s->cfg.autoconnect ? ": connecting" : " (autoconnect off: /connect " + name + ")"));
        if (s->cfg.autoconnect) added.push_back(s);
    }
    cfg_changed = dirty = true;
    plugins_sync();  // enabled or disabled in the file; before the new servers connect (a proxy plugin)
    for (auto* s : added) connect(s);
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

// <log_dir>/<network>: a network's logs
std::string Core::log_folder(const Server* s) const {
    std::string dir = opt(s, "log_dir", cfg_path.substr(0, cfg_path.rfind('/') + 1) + "logs");
    if (dir.rfind("~/", 0) == 0)
        if (const char* h = getenv("HOME")) dir = h + dir.substr(1);
    return dir + "/" + safe_name(s->cfg.name);
}

std::string Core::log_path(Buffer* b) const { return log_folder(b->srv) + "/" + safe_name(b->name) + ".log"; }

// yesterday's lines go to <name>.YYYY-MM-DD.log; one already there (another iSekur, a clock set back) gets them added
// to its end, never overwritten by a rename
static void rotate_log(const std::string& from, const std::string& to) {
    if (access(to.c_str(), F_OK) != 0) { rename(from.c_str(), to.c_str()); return; }
    std::stringstream ss;
    { std::ifstream in(from, std::ios::binary); ss << in.rdbuf(); }
    mode_t old = umask(077);
    std::ofstream out(to, std::ios::binary | std::ios::app);
    umask(old);
    out << ss.str();
    out.close();
    if (out) unlink(from.c_str());
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
        rotate_log(path, path.substr(0, path.size() - 4) + "." + b->log_day + ".log");
    }
    if (!b->log) {
        std::string path = log_path(b);
        mkdir_p(path.substr(0, path.rfind('/')));
        struct stat st;
        if (rotate && stat(path.c_str(), &st) == 0) {  // left over from an earlier day
            std::string day = fmt_time(st.st_mtime, "%Y-%m-%d");
            if (day != today) rotate_log(path, path.substr(0, path.size() - 4) + "." + day + ".log");
        }
        b->log_day = today;
        mode_t old = umask(077);  // chat logs are private
        b->log = fopen(path.c_str(), "a");
        umask(old);
        if (!b->log) return;
        fprintf(b->log, "--- Log opened %s\n", fmt_time(time(nullptr), "%Y-%m-%d %H:%M:%S").c_str());
    }
    fprintf(b->log, "%s\n", log_fmt(l).c_str());
    fflush(b->log);
}

// A Line as one log row (what parse_log_line reads back): "2026-10-27 19:09:42 <nick> text"
std::string log_fmt(const Line& l) {
    std::string who;
    switch (l.kind) {
    case LK::MSG: who = "<" + l.nick + "> "; break;
    case LK::ACTION: who = "* " + l.nick + " "; break;
    case LK::NOTICE: who = "-" + l.nick + "- "; break;
    default: who = "-!- "; break;
    }
    return fmt_time(l.ts, "%Y-%m-%d %H:%M:%S") + " " + who + mirc_strip(l.text);
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
    if (n <= 0) return;
    if (!opt_on(b->srv, "log", false)) {  // no log: the last session's lines from <config>.history
        if (!opt_on(b->srv, "keep_history", true) || cfg_path.empty()) return;
        hist_read();
        auto it = hist_cache.find(b->srv->cfg.name + "\t" + irc_lower(b->name));
        if (it == hist_cache.end()) return;
        auto& h = it->second;
        for (size_t i = h.size() > (size_t)n ? h.size() - n : 0; i < h.size(); i++) {
            h[i].self = b->srv->lower(h[i].nick) == b->srv->lower(b->srv->nick);
            b->lines.push_back(h[i]);
        }
        hist_cache.erase(it);  // the tab has them now; save_history writes them back from it
        Line sep{time(nullptr), LK::INFO, "", "--- end of last session ---"};
        sep.nolog = true;  // not saved again: one separator, not one per restart
        b->lines.push_back(sep);
        return;
    }
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
        l.self = b->srv->lower(l.nick) == b->srv->lower(b->srv->nick);
        b->lines.push_back(l);  // not add(): history must not be logged again
    }
    b->lines.push_back(Line{time(nullptr), LK::INFO, "", "--- end of log history ---"});
    b->added += (long long)hist.size() + 1;
}

void Core::print(Buffer* b, LK k, const std::string& text) {
    add(b ? b : bufs.front().get(), Line{time(nullptr), k, "", text});
}

void Core::info_note(Buffer* b, const std::string& text) {
    Line l{time(nullptr), LK::INFO, "", text};
    l.badge = "INFO", l.badge_kind = 'i';
    add(b ? b : bufs.front().get(), l);
}

void Core::plug_print(Buffer* b, LK k, const std::string& text, const std::string& badge, char kind) {
    if (badge == "plugin" && kind == 'p') b = nullptr;  // /plugin's own messages (install, settings): the console tab, not the chat
    Line l{time(nullptr), k, "", text};
    l.badge = badge, l.badge_kind = kind;
    add(b ? b : bufs.front().get(), l);
}

// /rawlog: what a line shows with its secret blanked: PASS, OPER's password, SASL payloads,
// REGISTER, and what follows the first word to NickServ/ChanServ (IDENTIFY pw, also echoed back)
std::string raw_redact(const std::string& line) {
    size_t at = 0;
    auto word = [&]() {
        while (at < line.size() && line[at] == ' ') at++;
        size_t st = at;
        while (at < line.size() && line[at] != ' ') at++;
        return line.substr(st, at - st);
    };
    std::string w = word();
    if (!w.empty() && w[0] == '@') w = word();  // tags
    if (!w.empty() && w[0] == ':') w = word();  // prefix
    to_upper(w);
    size_t verb_end = at;
    std::string a = word();
    if (w == "PASS" || w == "REGISTER") return line.substr(0, verb_end) + " ***";
    if (w == "AUTHENTICATE") {
        static const std::set<std::string> plain{"PLAIN", "EXTERNAL", "SCRAM-SHA-256", "SCRAM-SHA-1", "+", "*"};
        return plain.count(a) ? line : line.substr(0, verb_end) + " ***";
    }
    if (w == "OPER" || w == "NS" || w == "CS" || w == "NICKSERV" || w == "CHANSERV")
        return a.empty() ? line : line.substr(0, at) + " ***";
    if (w == "PRIVMSG" || w == "NOTICE") {
        std::string t = irc_lower(a.substr(0, a.find('@')));
        if (t != "nickserv" && t != "chanserv") return line;
        size_t colon = line.find(" :", at);
        if (colon == std::string::npos) return line;
        size_t sp = line.find(' ', colon + 2);
        return sp == std::string::npos ? line : line.substr(0, sp) + " ***";
    }
    return line;
}

void Core::rawlog(Buffer* b, const std::string& arg) {
    Server* s = b->srv;
    if (!s) { print(b, LK::ERROR, "/rawlog is per server: use it in that server's tabs"); return; }
    Buffer* r = find(s, "*raw");
    if (arg == "off" || (arg != "on" && r)) { if (r) close(r); else print(b, LK::INFO, "Raw log is off"); return; }
    if (!r) {
        r = open(s, BT::QUERY, "*raw");
        s->raw_hook = [this, s](const char* dir, const std::string& l) {
            if (Buffer* rb = find(s, "*raw")) {
                Line ln{time(nullptr), LK::INFO, "", std::string(dir) + " " + raw_redact(l)};
                ln.nolog = true;  // never on disk: neither the log nor the history file
                add(rb, ln);
            }
        };
        print(r, LK::INFO, "Raw log of " + s->cfg.name + ": every line from the server (\xe2\x86\x90) and to it (\xe2\x86\x92), passwords blanked. "
                           "/rawlog off or closing this tab stops it");
    }
    select(r);
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
    if (!s || b->type == BT::STATUS || b->name == "*channels" || b->name == "*snotices" || b->name == "*raw") {  // ours; *status (ZNC) is a real query
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
    s->typing_sent[s->lower(b->name)] = {now_ms(), false};  // a message ends typing, no "done" needed
}

// draft/multiline: the lines go as one message in a BATCH; a line over 400 bytes goes on in
// draft/multiline-concat parts (cut on UTF-8 boundaries, nothing dropped)
void Core::say_multi(Buffer* b, const std::vector<std::string>& lines) {
    Server* s = b->srv;
    std::string ref = "m" + std::to_string(now_ms());
    s->send("BATCH +" + ref + " draft/multiline " + b->name);
    for (auto& l : lines) {
        for (size_t i = 0; i < l.size();) {
            size_t e = std::min(l.size(), i + 400);
            while (e < l.size() && e > i + 1 && ((unsigned char)l[e] & 0xc0) == 0x80) e--;
            s->send("@batch=" + ref + (i ? ";draft/multiline-concat" : "") + " PRIVMSG " + b->name + " :" + l.substr(i, e - i));
            i = e;
        }
        if (!s->caps.count("echo-message")) add(b, Line{time(nullptr), LK::MSG, s->nick, mask_services(b->name, l), true});
    }
    s->send("BATCH -" + ref);
    s->typing_sent[s->lower(b->name)] = {now_ms(), false};
}

// +typing: "active" at most every 3s while there's text to send, "done" when it's gone (sent or erased);
// best effort: never queued behind real lines, so it costs no flood budget
void Core::typed(Buffer* b, const std::string& input) {
    Server* s = b ? b->srv : nullptr;
    if (!s || s->state != SS::CONNECTED || (b->type != BT::CHANNEL && b->type != BT::QUERY) || b->name[0] == '*' ||
        b == pass_buf || !opt_on(s, "send_typing", true) || !s->tag_ok("typing"))
        return;
    bool text = !input.empty() && (input[0] != '/' || input.rfind("//", 0) == 0);
    auto& st = s->typing_sent[s->lower(b->name)];
    long long now = now_ms();
    if (text == st.second && (!text || now - st.first < 3000)) return;
    if (!s->sendq.empty()) return;
    st = {now, text};
    s->send_now(std::string("@+typing=") + (text ? "active" : "done") + " TAGMSG " + b->name);
}

// /notify: [server.X] notify = nicks, watched with MONITOR (or ISON once a minute where there's none)
void Core::notify_start(Server* s) {
    s->online.clear();
    std::vector<std::string> watch = split(ini_get(cfg, "server." + s->cfg.name, "notify"), ' ');
    if (watch.empty()) return;
    if (!s->isupport.count("MONITOR")) { s->ison_at = time(nullptr); return; }
    int lim = atoi(s->isupport["MONITOR"].c_str());
    if (lim > 0 && (int)watch.size() > lim) {
        print(s->status, LK::ERROR, "/notify: " + s->cfg.name + " watches " + std::to_string(lim) + " nicks at most, the rest are left out");
        watch.resize((size_t)lim);
    }
    std::string l;
    for (auto& n : watch) {
        if (!l.empty() && l.size() + n.size() > 400) s->send("MONITOR + " + l), l.clear();
        l += (l.empty() ? "" : ",") + n;
    }
    s->send("MONITOR + " + l);
}

void Core::notify_seen(Server* s, const std::string& nick, bool on, const std::string& host) {
    std::string k = s->lower(nick);
    auto it = s->online.find(k);
    bool known = it != s->online.end();
    if (known && it->second == on) return;
    s->online[k] = on;
    if (!known && !on) return;  // offline when we started watching: nothing happened
    std::string t = on ? "\x02" + nick + "\x02 is online" + (host.empty() ? "" : " (" + host + ")") : nick + " went offline";
    add(s->status, Line{time(nullptr), LK::INFO, "", t, false, on});  // online: a hilight, copied to window 1
    if (Buffer* q = find(s, nick); q && q->type == BT::QUERY) print(q, LK::INFO, t);
    if (on) ring(s);
}

void Core::notify_cmd(Buffer* b, const std::string& args) {
    Server* s = b->srv;
    if (!s) { print(b, LK::ERROR, "/notify works in a server's tabs"); return; }
    auto argv = split(args, ' ');
    std::string sec = "server." + s->cfg.name;
    std::vector<std::string> list = split(ini_get(cfg, sec, "notify"), ' ');
    if (argv.empty()) {
        if (list.empty()) { print(b, LK::INFO, "Nobody on /notify for " + s->cfg.name + ": /notify add nick"); return; }
        std::string on, off;
        for (auto& n : list) {
            auto it = s->online.find(s->lower(n));
            (it != s->online.end() && it->second ? on : off) += " " + n;
        }
        print(b, LK::INFO, "/notify on " + s->cfg.name + (s->state != SS::CONNECTED ? " (not connected):" + on + off
                                                           : ": online" + (on.empty() ? " none" : on) + " \xc2\xb7 offline" + (off.empty() ? " none" : off)));
        return;
    }
    if ((argv[0] != "add" && argv[0] != "del") || argv.size() < 2) { print(b, LK::ERROR, "Usage: /notify [add|del] nick [nick...]"); return; }
    bool add = argv[0] == "add";
    std::vector<std::string> changed;
    for (size_t i = 1; i < argv.size(); i++) {
        auto it = std::find_if(list.begin(), list.end(), [&](const std::string& n) { return s->lower(n) == s->lower(argv[i]); });
        if (add && it == list.end()) list.push_back(argv[i]), changed.push_back(argv[i]);
        if (!add && it != list.end()) list.erase(it), changed.push_back(argv[i]), s->online.erase(s->lower(argv[i]));
    }
    if (changed.empty()) { print(b, LK::INFO, add ? "Already on /notify" : "Not on /notify"); return; }
    std::string v, c;
    for (auto& n : list) v += (v.empty() ? "" : " ") + n;
    for (auto& n : changed) c += (c.empty() ? "" : ",") + n;
    cfg[sec]["notify"] = unsaved[sec]["notify"] = v;
    if (s->state == SS::CONNECTED && s->motd_done) {
        if (s->isupport.count("MONITOR")) s->send(std::string("MONITOR ") + (add ? "+ " : "- ") + c);
        else if (add) s->ison_at = time(nullptr);
    }
    if (save_section(*this, sec, b))
        print(b, LK::INFO, (add ? "Watching " : "No longer watching ") + c + " on " + s->cfg.name + " (saved)");
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

// What lets a grabbed session reach further than the chat is changed only with the lock password (when one is set): /exec from
// the webchat, the lock itself, the folder the webchat serves files from, the webchat's and the bouncer's logins and pages.
// ---- breadcrumbs for isekur.crash (core.h) ---------------------------------------------
static char g_crumbs[32][96], g_state[240];
static unsigned g_crumb_n;
static const char* volatile g_stage = "start";
void crumb(const char* what, const char* arg) {
    snprintf(g_crumbs[g_crumb_n++ % 32], sizeof g_crumbs[0], "%lld %.20s %.50s", (long long)time(nullptr), what, arg);
}
void crumb_stage(const char* literal) { g_stage = literal; }
void crumb_state(const char* s) { snprintf(g_state, sizeof g_state, "%s", s); }
void crumbs_write(int fd) {
    auto put = [&](const char* s) { (void)!write(fd, s, strlen(s)); };
    put("main loop stage: "), put(g_stage), put("\nlast frame: "), put(g_state), put("\nrecent, oldest first:\n");
    for (unsigned i = g_crumb_n > 32 ? g_crumb_n - 32 : 0; i < g_crumb_n; i++) put("  "), put(g_crumbs[i % 32]), put("\n");
}

// true: the password is being asked, and `line` runs again once it's right
static bool lock_ok = false;
static bool lock_gate(Core& c, Buffer* b, const std::string& line, const std::string& what) {
    if (!c.lock_on() || lock_ok) return false;
    c.ask_pass(b, "lock password", [&c, b, line](const std::string& p) {
        if (!check_password(p, c.opt(nullptr, "lock_password_hash", ""))) { c.print(b, LK::ERROR, "Wrong password: it stays as it was"); return; }
        lock_ok = true, c.exec(b, line), lock_ok = false;
    }, true, "Lock password", {"Changing " + what + ": the lock password first"});
    return true;
}
static bool lock_guarded(const std::string& sec, const std::string& key) {
    return (sec == "general" && (key == "exec_web" || key == "lock" || key == "lock_password_hash" || key == "dcc_web" || key == "fifo" || key == "fifo_path")) || key == "dcc_dir" ||
           sec == "web" || sec == "bnc";
}

void Core::exec(Buffer* b, const std::string& input) {
    if (!b) return;
    if (input.find('\n') != std::string::npos) {
        crumb("exec", "multi-line");
        std::vector<std::string> ls;
        bool chat = true;
        size_t bytes = 0;
        for (auto l : split(input, '\n')) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (l.empty()) continue;
            if (l[0] == '/' && l.rfind("//", 0) != 0) chat = false;
            ls.push_back(l[0] == '/' ? l.substr(1) : l), bytes += l.size() + 1;
        }
        Server* s = b->srv;
        // a paste of plain lines in a chat: a plugin's first (pasteguard: a pastebin link); true: it took it
        if (chat && ls.size() > 1 && s && b != pass_buf && (b->type == BT::CHANNEL || b->type == BT::QUERY) && !plugin_depth) {
            std::string all;
            for (auto& l : ls) all += (all.empty() ? "" : "\n") + l;
            if (plugin_emit("paste", s, b->name, {{"text", all}})) { last_input = time(nullptr); return; }
        }
        // a paste of plain lines: one draft/multiline message when the server takes it (and its limits allow)
        if (chat && ls.size() > 1 && s && s->state == SS::CONNECTED && s->caps.count("draft/multiline") && b != pass_buf &&
            (b->type == BT::CHANNEL || b->type == BT::QUERY) && b->name[0] != '*' && !e2e_on(b)) {
            size_t max_bytes = 0, max_lines = 0;
            for (auto& kv : split(s->cap_vals["draft/multiline"], ',')) {
                if (kv.rfind("max-bytes=", 0) == 0) max_bytes = (size_t)atol(kv.c_str() + 10);
                if (kv.rfind("max-lines=", 0) == 0) max_lines = (size_t)atol(kv.c_str() + 10);
            }
            if ((!max_bytes || bytes <= max_bytes) && (!max_lines || ls.size() <= max_lines)) {
                last_input = time(nullptr);
                s->label_buf = b->id;
                say_multi(b, ls);
                s->label_buf = 0;
                return;
            }
        }
        for (auto& l : split(input, '\n')) {
            if (index_of(b) < 0) break;  // a line (/close, /part, /disconnect) closed this tab: the rest has nowhere to go (b was freed)
            exec(b, l);
        }
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
        if (b->srv && b->name == "*channels") { list_filter(b, in); return; }  // typed in the /list table: it filters it
        say(b, in[0] == '/' ? in.substr(1) : in);
        return;
    }
    size_t sp = in.find(' ');
    std::string cmd = in.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
    std::string args = sp == std::string::npos ? "" : in.substr(sp + 1);
    to_lower(cmd);
    crumb("exec", ("/" + cmd).c_str());  // the name only, never what follows
    auto argv = split(args, ' ');
    auto rest = [&](size_t n) { return after_words(args, n); };
    Server* s = b->srv;
    if (s) s->typed_in = b->id, s->typed_at = time(nullptr);
    // labeled-response: every line this command sends is labeled with this tab
    struct LabelScope {  // /server del may have deleted s by the end
        Core& c; Server* s; int was;
        ~LabelScope() { for (auto& sv : c.servers) if (sv.get() == s) s->label_buf = was; }
    } label_scope{*this, s, s ? s->label_buf : 0};
    if (s) s->label_buf = b->id;
    auto need = [&]() {
        if (s && s->state == SS::CONNECTED) return true;
        print(b, LK::ERROR, "Not connected to a server");
        return false;
    };

    // what you type (not what plugins run): a handler returning true takes the command over
    if (!plugin_depth && plugin_emit("command", s, b->name, {{"name", cmd}, {"args", args}})) return;
    if (cmd == "plugin" || cmd == "plugins") { plugin_cmd(b, cmd == "plugins" ? "" : args); return; }
    if (!find_command(cmd) && plugin_run(b, cmd, args)) return;  // a plugin's command; iSekur's own always win
    if (cmd == "help") { help(b, args); return; }
    if (cmd == "about") { about(b); return; }
    if (cmd == "logs") {  // the log browser, at this tab's logs; text: searched there right away (the webchat opens its own)
        if (!tty || from_web) { print(b, LK::INFO, "The log browser is a window: /logs in the terminal or the webchat"); return; }
        logs_win = dirty = true, logs_q = args;
        logs_at = b->srv && b->type != BT::SHELL ? b->srv->cfg.name + (b->type == BT::STATUS ? "" : "\t" + safe_name(b->name)) : "";
        return;
    }
    if (cmd == "keys") {  // the terminal's window (the webchat opens its own dialog)
        if (tty && !from_web) keys_win = dirty = true;
        else print(b, LK::INFO, "The key list is a window: F1 or Alt+K in the terminal or the webchat");
        return;
    }
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
    if (plugin_depth && (cmd == "exec" || cmd == "sh" || cmd == "shell" || cmd == "attach")) { plug_print(b, LK::ERROR, "/" + cmd + " can't be run by a plugin"); return; }  // the sandbox: no programs
    if (cmd == "exec" || cmd == "sh") { exec_cmd(b, args); return; }
    if (cmd == "dcc") { dcc_cmd(b, args); return; }
    if (cmd == "shell") { shell_cmd(b); return; }
    if (cmd == "attach") { attach_cmd(b, args); return; }
    if (cmd == "lock") { lock_cmd(b, args); return; }
    if (cmd == "servers") {  // the terminal: the settings window at the first server, or at /servers name (the webchat opens its Servers window)
        if (!argv.empty() && !std::any_of(servers.begin(), servers.end(), [&](auto& sv) { return sv->cfg.name == argv[0]; })) {
            print(b, LK::ERROR, "No network " + argv[0]); return;
        }
        if (tty && !from_web) { settings_win = settings_servers = dirty = true; settings_srv = argv.empty() ? "" : argv[0]; return; }
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
        if (key == "name" && sec.rfind("server.", 0) == 0) {  // not a key: the settings window's name row
            if (argv.size() == 1) print(b, LK::INFO, argv[0] + " = " + sec.substr(7));
            else if (val.empty()) print(b, LK::ERROR, "A server needs a name: /server del " + sec.substr(7) + " removes it");
            else rename_server(b, sec.substr(7), val);
            return;
        }
        if (argv.size() == 1) {
            std::string def = ini_get(defaults, sec, key);
            if (sec == "general" && key == "timestamp") def = theme.lay("timestamp", "%H:%M") + ", the theme's";
            if (sec == "general" && key == "nick_width") def = theme.lay("nick_width", "12") + ", the theme's";
            print(b, LK::INFO, argv[0] + " = " + (secret(key) && !ini_get(cfg, sec, key).empty() ? "********" : ini_get(cfg, sec, key, def.empty() ? "(not set: default)" : def + "  (default)")));
            return;
        }
        // a grabbed session can't switch /exec on, or point the webchat at other folders or logins, without the lock password
        if (lock_guarded(sec, key) && val != ini_get(cfg, sec, key) && lock_gate(*this, b, in, key)) return;
        // a password typed into a *password_hash key is kept as its hash
        if (key.size() >= 13 && key.compare(key.size() - 13, 13, "password_hash") == 0 && !val.empty() && val.rfind("pbkdf2$", 0) != 0)
            val = hash_password(val);
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
            if (load_theme(val.empty() ? "default" : val, theme, err)) {  // "": the default theme, as at start
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
        if (sec == "general" && key == "theme_dir") {  // the current theme from there, if it has it
            theme_dir = val;
            Theme t;
            if (load_theme(ini_get(cfg, "general", "theme", "default"), t, err)) theme = t, notify(Ev::THEME, nullptr);
        }
        if (sec == "keys" || (sec == "general" && (key == "timestamp_hover" || key == "mouse"))) cfg_changed = true;  // the terminal applies them (tui apply_cfg)
        if (sec == "general" && key == "scrollback") max_lines = (size_t)std::max(100, atoi(opt(nullptr, "scrollback", "2000").c_str()));
        if (sec == "general" && (key.rfind("timestamp", 0) == 0 || key == "scrollback" || key == "nick_width" || key == "complete_suffix" || key.rfind("web_font", 0) == 0 || key.rfind("web_tree", 0) == 0 || key == "web_unread_badge")) notify(Ev::THEME, nullptr);  // the webchat redraws its lines with them
        if (key == "log") notify(Ev::THEME, nullptr);  // the webchat shows its Logs button only while logging
        if (sec == "general" && key == "typing_effect") typing_quiet(opt_on(nullptr, "typing_effect", false));  // like /typing
        if (sec == "general" && (key == "fifo" || key == "fifo_path")) fifo_open();  // the pipe opens, moves or closes at once
        // autostart on/off starts / stops the service now too (main loop answers); a daemon keeps its webchat, the only way back in
        bool svc_now = key == "autostart" && (sec == "bnc" || (sec == "web" && (Web::autostart(cfg) || !daemon)));
        if (svc_now) {
            std::string v = ini_get(cfg, sec, "autostart", "off");
            bool on = sec == "web" ? Web::autostart(cfg) : v == "on" || v == "true" || v == "yes" || v == "1";
            if (sec == "web") web_req_buf = b, web_req = on ? WebReq::START : WebReq::STOP;
            else bnc_req_buf = b, bnc_req = on ? BncReq::START : BncReq::STOP;
        }
        dirty = true;
        std::string shown = val.empty() ? "(default)" : secret(key) ? "********" : val;  // "": back to the default
        if (saved_now) { print(b, LK::INFO, argv[0] + " = " + shown + "  (saved)"); return; }
        print(b, LK::INFO, argv[0] + " = " + shown +
                               (sec.rfind("server.", 0) == 0 ? "  (used on the next /connect)"
                                : svc_now                   ? ""
                                : sec == "web"              ? "  (applies after /web restart)"
                                : sec == "bnc"              ? "  (applies after /bnc restart)"
                                                            : "") +
                               "  - /save writes it to the config");
        return;
    }
    if (cmd == "ignore" || cmd == "unignore") {  // masks: nick or nick!user@host globs, per network
        if (!s) { print(b, LK::ERROR, "Ignores are per network: use it in one of its tabs"); return; }
        std::string sec = "server." + s->cfg.name;
        auto masks = split(ini_get(cfg, sec, "ignore"), ' ');
        if (argv.empty()) {
            print(b, LK::INFO, masks.empty() ? "Ignore list on " + s->cfg.name + " is empty" : "Ignoring on " + s->cfg.name + ": " + ini_get(cfg, sec, "ignore"));
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
        cfg[sec]["ignore"] = unsaved[sec]["ignore"] = joined;
        print(b, LK::INFO, (cmd == "ignore" ? "Ignoring " : "No longer ignoring ") + mask + " on " + s->cfg.name + " (/save to keep)");
        return;
    }
    if (cmd == "hilight") {  // /hilight [add|del] word: words highlighted besides your nick, * matches anything, per network
        if (!s) { print(b, LK::ERROR, "Highlights are per network: use it in one of its tabs"); return; }
        std::string sec = "server." + s->cfg.name;
        auto words = split(ini_get(cfg, sec, "hilight_words"), ' ');
        if (argv.size() < 2 || (argv[0] != "add" && argv[0] != "del")) {
            if (!argv.empty()) { print(b, LK::ERROR, "Usage: /hilight [add|del] word"); return; }
            print(b, LK::INFO, words.empty() ? "Highlighting only your nick on " + s->cfg.name + " (/hilight add word)" : "Highlighting on " + s->cfg.name + " your nick and: " + ini_get(cfg, sec, "hilight_words"));
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
        cfg[sec]["hilight_words"] = unsaved[sec]["hilight_words"] = joined;
        print(b, LK::INFO, (argv[0] == "add" ? "Highlighting " : "No longer highlighting ") + w + " on " + s->cfg.name + " (/save to keep)");
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
    if (cmd == "quit") {  // this tab's server only (mIRC's way); /exit closes iSekur
        if (!s) { print(b, LK::ERROR, "Not a server's tab: /quit leaves the server of the tab you're on; /exit closes iSekur"); return; }
        if (s->state == SS::DISCONNECTED) { print(b, LK::ERROR, "Not connected to " + s->cfg.name); return; }
        exec(b, "/disconnect " + args);
        return;
    }
    if (cmd == "exit") {  // the frontends ask first while connected (/exit -y skips that); here it just goes
        // with a lock password, that password too: an unlocked terminal or a webchat left open can't close iSekur
        // (Alt+D still detaches freely: everything stays connected)
        static bool exit_ok = false;
        if (lock_on() && !exit_ok) {
            std::string line = in;
            ask_pass(b, "lock password", [this, b, line](const std::string& p) {
                if (!check_password(p, opt(nullptr, "lock_password_hash", ""))) { print(b, LK::ERROR, "Wrong password: iSekur keeps running"); return; }
                exit_ok = true, exec(b, line), exit_ok = false;
            }, true, "Exit", {"Locking is on: the lock password closes iSekur", "Alt+D detaches instead"});
            return;
        }
        if (args == "-y" || args.rfind("-y ", 0) == 0) args = args.size() > 3 ? args.substr(3) : "";
        quit_msg = args;
        for (auto& sv : servers)
            if (sv->state != SS::DISCONNECTED)
                sv->user_quit = true, sv->send_now("QUIT :" + (args.empty() ? bye(sv.get(), "quit_message") : args));
        quit = true;
        return;
    }
    if (cmd == "server" && !argv.empty() && argv[0] == "rename") {  // /server rename [old] [new]: asks for what's missing
        std::string from = argv.size() > 1 ? argv[1] : s ? s->cfg.name : "";
        if (from.empty()) { print(b, LK::ERROR, "Usage: /server rename old new"); return; }
        if (argv.size() < 3) {
            ask_pass(b, "new name", [this, from](const std::string& v) { if (!v.empty()) rename_server(active, from, v); }, false,
                     "Rename " + from, {"Its config section, tab and logs follow", "Letters, digits, . _ and -"});
            return;
        }
        rename_server(b, from, argv[2]);
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
        // an IPv6 address: bare (2001:db8::1) or in brackets with a port ([2001:db8::1]:6697); the lookup takes it bare
        std::string host = argv[0], nick, port;
        if (host[0] == '[') {
            size_t e = host.find(']');
            if (e != std::string::npos && e + 1 < host.size() && host[e + 1] == ':') port = host.substr(e + 2);
            host = host.substr(1, e == std::string::npos ? std::string::npos : e - 1);
        }
        // no -name: the host is the name, and what a [server.x] can't have (an address's :) turns to -
        std::string name = host;
        for (char& ch : name) if (!server_name_ok(std::string(1, ch))) ch = '-';
        bool tls = false, verify = true;
        for (size_t i = 1; i < argv.size(); i++) {
            if (argv[i] == "-tls") tls = true;
            else if (argv[i] == "-noverify") verify = false;
            else if ((argv[i] == "-name" || argv[i] == "-nick") && i + 1 < argv.size()) (argv[i] == "-name" ? name : nick) = argv[i + 1], i++;
            else port = argv[i];
        }
        // the name is a config section: [server.<name>]
        if (!server_name_ok(name)) {
            print(b, LK::ERROR, "A server name can have letters, digits, . _ and - only");
            return;
        }
        for (auto& sv : servers)
            if (sv->cfg.name == name) { connect(sv.get()); select(sv->status); return; }
        std::string sec = "server." + name;
        if (port.empty() || (tls && port == "6667")) port = tls ? "6697" : "6667";
        // remembered like /set, so /save keeps it for the next start
        for (auto& [k, v] : std::map<std::string, std::string>{
                 {"host", host}, {"port", port},
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
    if (cmd == "move") {  // /move N: this tab to place N among its server's tabs, a server tab (with its tabs) among the servers
        int n = atoi(args.c_str());
        Server* sv = b->srv;
        if (n < 1 || !sv) { print(b, LK::ERROR, "Usage: /move N, in a server, channel or query tab"); return; }
        std::string err;
        if (b->type == BT::STATUS) {  // before the n'th other server, or after the last; saved as [general] server_order
            std::vector<std::unique_ptr<Buffer>> mine, rest;
            for (auto& p : bufs) (p->srv == sv ? mine : rest).push_back(std::move(p));
            size_t pos = 0;
            int seen = 0;
            for (size_t i = 0; i < rest.size(); i++) {
                if (rest[i]->srv && rest[i]->type == BT::STATUS && ++seen == n) { pos = i; break; }
                if (rest[i]->srv || !pos) pos = i + 1;  // after the last server's tabs (the console stays first)
            }
            rest.insert(rest.begin() + (long)pos, std::make_move_iterator(mine.begin()), std::make_move_iterator(mine.end()));
            bufs = std::move(rest);
            std::string v;
            std::vector<Server*> ord;
            for (auto& p : bufs)
                if (p->srv && p->type == BT::STATUS) v += (v.empty() ? "" : ",") + p->srv->cfg.name, ord.push_back(p->srv);
            auto rank = [&](auto& x) { return std::find(ord.begin(), ord.end(), x.get()) - ord.begin(); };
            std::stable_sort(servers.begin(), servers.end(), [&](auto& x, auto& y) { return rank(x) < rank(y); });
            if (ini_get(cfg, "general", "server_order") != v) {  // like last_nick: state, written at once
                cfg["general"]["server_order"] = v;
                if (!ini_save(cfg_path, Ini{{"general", {{"server_order", v}}}}, err)) unsaved["general"]["server_order"] = v;
            }
        } else {
            auto it = std::find_if(bufs.begin(), bufs.end(), [&](auto& p) { return p.get() == b; });
            auto mine = std::move(*it);
            bufs.erase(it);
            size_t pos = 0;  // after the n-1'th of its server's other tabs, or its last
            int seen = 0;
            for (size_t i = 0; i < bufs.size(); i++)
                if (bufs[i]->srv == sv && (bufs[i]->type == BT::STATUS || seen++ < n - 1)) pos = i + 1;
            bufs.insert(bufs.begin() + (long)pos, std::move(mine));
            // the autojoin list follows the tabs: channels join (and their tabs open) in its order at the next start
            auto& aj = sv->cfg.autojoin;
            auto at = [&](const std::string& e) {
                std::string ch = sv->lower(split(e, ' ')[0]);
                for (size_t i = 0; i < bufs.size(); i++)
                    if (bufs[i]->srv == sv && bufs[i]->type == BT::CHANNEL && sv->lower(bufs[i]->name) == ch) return i;
                return bufs.size();
            };
            auto was = aj;
            std::stable_sort(aj.begin(), aj.end(), [&](auto& x, auto& y) { return at(x) < at(y); });
            if (aj != was) {
                std::string v, sec = "server." + sv->cfg.name;
                for (auto& e : aj) v += (v.empty() ? "" : ",") + e;
                cfg[sec]["autojoin"] = unsaved[sec]["autojoin"] = v;
                save_section(*this, sec, b);
            }
        }
        dirty = true;
        notify(Ev::BUFFER, b);
        return;
    }
    if (cmd == "close" || cmd == "wc") { close(b); return; }
    if (cmd == "split") { split_cmd(b, args); return; }
    if (cmd == "rawlog") { rawlog(b, args); return; }
    if (cmd == "unsplit" || cmd == "merge") { split_cmd(b, "off"); return; }
    if (cmd == "quiet") {  // per tab, until restart
        b->quiet = args == "on" || (args != "off" && !b->quiet);
        b->quiet_typing = false;  // your choice now: /typing off leaves it
        notify(Ev::BUFFER, b);
        print(b, LK::INFO, std::string("Quiet mode ") + (b->quiet ? "on: joins, parts, quits and nick changes are hidden here"
                                                                  : "off: showing joins, parts, quits and nick changes"));
        return;
    }
    if (cmd == "mute") {  // [server.X] mute: channels (or queries) that never hilight; saved now
        if (!s || b->type == BT::STATUS) { print(b, LK::ERROR, "Mute is for channels and queries: use it in that tab"); return; }
        bool on = args == "on" || (args != "off" && !muted(b));
        std::string sec = "server." + s->cfg.name, v;
        for (auto& ch : split(ini_get(cfg, sec, "mute"), ' '))
            if (!ch.empty() && s->lower(ch) != s->lower(b->name)) v += (v.empty() ? "" : " ") + ch;
        if (on) v += (v.empty() ? "" : " ") + b->name;
        cfg[sec]["mute"] = unsaved[sec]["mute"] = v;
        notify(Ev::BUFFER, b);
        // written now, like /autojoin: the tab menu's choice must hold at the next start without a /save
        if (save_section(*this, sec, b))
            print(b, LK::INFO, b->name + (on ? " muted: no hilights or alerts from it (saved)" : " unmuted: hilights and alerts are back (saved)"));
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
    if (cmd == "bnc") {  // /bnc [start|stop|restart|status|password]
        std::string a = argv.empty() ? "status" : argv[0];
        bnc_req_buf = b;
        if (a == "start") bnc_req = BncReq::START;
        else if (a == "stop") bnc_req = BncReq::STOP;
        else if (a == "restart") bnc_req = BncReq::RESTART;
        else if (a == "status") bnc_req = BncReq::STATUS;
        else if (a == "password") {
            if (lock_gate(*this, b, in, "the BNC password")) return;
            ask_pass(b, "BNC password", [this, b](const std::string& p1) {
                if (p1.empty()) { print(b, LK::ERROR, "No password: it stays as it was"); return; }
                ask_pass(b, "again", [this, b, p1](const std::string& p2) {
                    if (p1 != p2) { print(b, LK::ERROR, "The passwords don't match: it stays as it was, /bnc password to try again"); return; }
                    cfg["bnc"]["password_hash"] = unsaved["bnc"]["password_hash"] = hash_password(p1);  // not /set: it says "/save writes it", saved just below
                    if (save_section(*this, "bnc", b)) print(b, LK::INFO, "BNC password saved; /bnc restart uses it");
                }, true, "BNC password", {"Type it again"});
            }, true, "BNC password", {"The password your other IRC clients log in with"});
        } else print(b, LK::ERROR, "Usage: /bnc [start|stop|restart|status|password]");
        return;
    }
    if (cmd == "back") { exec(b, "/away"); return; }  // /away with no message
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
        while (!args.empty() && isspace((unsigned char)args.back())) args.pop_back();  // a picked suggestion leaves "ayu "
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
        if (argv.empty()) {
            if (s) print(b, LK::INFO, "Your nickname is " + (s->state != SS::DISCONNECTED ? s->nick : s->cfg.nick));
            else print(b, LK::ERROR, "Not a server's tab");
            return;
        }
        if (s && s->isupport.count("NICKLEN") && atoi(s->isupport["NICKLEN"].c_str()) > 0 &&
            argv[0].size() > (size_t)atoi(s->isupport["NICKLEN"].c_str())) {
            print(b, LK::ERROR, "Nicks on " + s->cfg.name + " have " + s->isupport["NICKLEN"] + " characters at most");
            return;
        }
        if (s && s->state != SS::DISCONNECTED) s->send("NICK " + argv[0]);
        else if (s) s->cfg.nick = argv[0], print(b, LK::INFO, "Not connected: you'll be " + argv[0] + " on the next connect");
        return;
    }
    if (cmd == "me") { if (!args.empty()) say(b, args, true); return; }
    if (cmd == "sv") { say(b, "is using " + ctcp_version(), true); return; }  // send version: the CTCP VERSION text, for all to see
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
                auto k = sv->chan_keys.find(sv->lower(bp->name));
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
    if (cmd == "notify") { notify_cmd(b, args); return; }
    if (!need()) return;
    // /reply nick text, /react emoji [nick]: their newest message here (msgid); /redact: your newest
    auto last_from = [&](const std::string& nick, bool mine) -> Line* {
        for (auto it = b->lines.rbegin(); it != b->lines.rend(); ++it)
            if ((it->kind == LK::MSG || it->kind == LK::ACTION || it->kind == LK::NOTICE) && it->self == mine &&
                (nick.empty() || s->lower(it->nick) == s->lower(nick)) && (mine || !nick.empty() || !it->msgid.empty()))
                return &*it;
        return nullptr;
    };
    bool chat = (b->type == BT::CHANNEL || b->type == BT::QUERY) && b->name[0] != '*';
    if (cmd == "reply") {
        if (argv.size() < 2 || !chat) { print(b, LK::ERROR, "Usage: /reply nick text (in a channel or query tab)"); return; }
        Line* t = last_from(argv[0], false);
        if (!t) { print(b, LK::ERROR, "No message from " + argv[0] + " in this tab"); return; }
        std::string text = rest(1);
        if (t->msgid.empty() || !s->tag_ok("reply") || e2e_on(b)) { say(b, t->nick + ": " + text); return; }  // no tags: the old way
        std::string q = "\x1d\x03" "14\xe2\x86\xaa " + t->nick + ": " + reply_snippet(t->text) + "\x0f ";
        auto parts = chunk(text, 400);
        s->send("@+draft/reply=" + escape_tag(t->msgid) + " PRIVMSG " + b->name + " :" + parts[0]);
        for (size_t i = 1; i < parts.size(); i++) s->send("PRIVMSG " + b->name + " :" + parts[i]);
        if (!s->caps.count("echo-message"))
            for (size_t i = 0; i < parts.size(); i++) add(b, Line{time(nullptr), LK::MSG, s->nick, (i ? "" : q) + parts[i], true});
        s->typing_sent[s->lower(b->name)] = {now_ms(), false};
        return;
    }
    if (cmd == "react") {
        if (argv.empty() || !chat) { print(b, LK::ERROR, "Usage: /react emoji [nick] (in a channel or query tab)"); return; }
        if (!s->tag_ok("react")) { print(b, LK::ERROR, s->cfg.name + " doesn't pass reactions (no message-tags, or CLIENTTAGDENY)"); return; }
        Line* t = last_from(argv.size() > 1 ? argv[1] : "", false);
        if (!t || t->msgid.empty()) { print(b, LK::ERROR, "No message to react to here" + std::string(argv.size() > 1 ? " from " + argv[1] : "")); return; }
        s->send("@+draft/react=" + escape_tag(argv[0]) + ";+draft/reply=" + escape_tag(t->msgid) + " TAGMSG " + b->name);
        if (!s->caps.count("echo-message"))
            add(b, Line{time(nullptr), LK::INFO, s->nick, "You reacted " + argv[0] + " to " + t->nick + "'s message " + reply_snippet(t->text), true});
        return;
    }
    if (cmd == "redact") {
        if (!chat) { print(b, LK::ERROR, "/redact works in a channel or query tab"); return; }
        if (!s->caps.count("draft/message-redaction")) { print(b, LK::ERROR, s->cfg.name + " can't delete messages (no draft/message-redaction)"); return; }
        Line* t = last_from("", true);
        while (t && t->msgid.empty()) t = nullptr;
        if (!t) { print(b, LK::ERROR, "No message of yours to delete here (it needs a msgid: echo-message)"); return; }
        s->send("REDACT " + b->name + " " + t->msgid + (args.empty() ? "" : " :" + args));
        return;
    }
    if (cmd == "setname") {
        if (args.empty()) { print(b, LK::ERROR, "Usage: /setname new real name"); return; }
        if (!s->caps.count("setname")) { print(b, LK::ERROR, s->cfg.name + " can't change your real name while connected (no setname): /set realname and reconnect"); return; }
        s->send("SETNAME :" + args);
        return;
    }
    if (cmd == "register" || cmd == "verify") {  // draft/account-registration
        if (!s->caps.count("draft/account-registration")) {
            print(b, LK::ERROR, s->cfg.name + " has no account registration command: try /msg NickServ REGISTER password email");
            return;
        }
        if (cmd == "verify") {
            if (argv.size() != 2) { print(b, LK::ERROR, "Usage: /verify account code"); return; }
            s->send("VERIFY " + argv[0] + " " + argv[1]);
            return;
        }
        bool need_mail = s->cap_vals["draft/account-registration"].find("email-required") != std::string::npos;
        std::string mail = argv.empty() ? "*" : argv[0];
        if (mail == "*" && need_mail) { print(b, LK::ERROR, s->cfg.name + " needs an email: /register you@example.org"); return; }
        int bid = b->id;
        ask_pass(b, "password for the account " + s->nick, [this, s, bid, mail](const std::string& p) {
            if (std::none_of(servers.begin(), servers.end(), [&](auto& sv) { return sv.get() == s; })) return;  // /server del meanwhile
            s->reg_pass = p;
            s->label_buf = bid;
            s->send("REGISTER * " + mail + " :" + p);
            s->label_buf = 0;
        }, true, "Register " + s->nick, {"An account for your nick on " + s->cfg.name});
        return;
    }
    if (cmd == "join" || cmd == "j") {
        if (argv.empty()) return;
        std::string ch = s->is_chan(argv[0]) ? argv[0] : "#" + argv[0];
        if (s->twitch()) ch = irc_lower(ch);  // Twitch ignores a JOIN #Name
        s->send("JOIN " + ch + (argv.size() > 1 ? " " + argv[1] : ""));
        for (auto& c1 : split(ch, ',')) s->join_focus.insert(s->lower(c1));
        if (argv.size() > 1) s->chan_keys[s->lower(ch)] = argv[1];
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
        auto it = std::find_if(aj.begin(), aj.end(), [&](const std::string& e) { return s->lower(split(e, ' ')[0]) == s->lower(ch); });
        if (argv[0] == "del") {
            if (it == aj.end()) { print(b, LK::ERROR, ch + " is not in autojoin"); return; }
            aj.erase(it);
        } else {
            std::string key = argv.size() > 2 ? argv[2] : s->chan_keys.count(s->lower(ch)) ? s->chan_keys[s->lower(ch)] : "";
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
        std::string chan = b->name;  // /topic #chan [text]: that channel, from any tab
        if (!argv.empty() && s->is_chan(argv[0])) chan = argv[0], args = args.size() > chan.size() ? args.substr(chan.size() + 1) : "";
        else if (b->type != BT::CHANNEL) { print(b, LK::ERROR, "Not a channel"); return; }
        if (args == "-delete") s->send("TOPIC " + chan + " :");  // like irssi and WeeChat: "TOPIC #c :" clears it
        else if (args.empty() && chan == b->name) print(b, LK::TOPIC, b->topic.empty() ? "No topic is set" : "Topic: " + b->topic);
        else if (args.empty()) s->send("TOPIC " + chan);
        else {
            int max = s->isupport.count("TOPICLEN") ? atoi(s->isupport["TOPICLEN"].c_str()) : 0;
            if (max > 0 && args.size() > (size_t)max) print(b, LK::ERROR, "That topic is " + std::to_string(args.size()) + " bytes: " + s->cfg.name + " keeps " + std::to_string(max));
            s->send("TOPIC " + chan + " :" + args);
        }
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
        to_upper(verb);
        std::string arg = rest(2);
        if (verb == "PING" && arg.empty()) arg = std::to_string(now_ms());
        s->send("PRIVMSG " + argv[0] + " :\x01" + verb + (arg.empty() ? "" : " " + arg) + "\x01");
        print(b, LK::INFO, "CTCP " + verb + " sent to " + argv[0]);
        return;
    }
    if (cmd == "list" && !argv.empty() && argv[0] == "sort") { list_sort(b, argv.size() > 1 ? argv[1] : ""); return; }
    if (cmd == "list" && !argv.empty() && argv[0] == "filter") { list_filter(b, rest(1)); return; }
    if (cmd == "oper" && argv.size() == 1) {  // no password typed: ask for it, hidden
        if (!need()) return;
        std::string user = argv[0];
        ask_pass(b, "oper password for " + user, [s, user](const std::string& p) { s->send("OPER " + user + " :" + p); });
        return;
    }
    // anything else goes to the server verbatim: /whois, /mode, /away, /kick...
    to_upper(cmd);
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
