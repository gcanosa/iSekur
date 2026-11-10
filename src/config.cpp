// INI parsing, colors, themes, mIRC formatting, UTF-8 and small helpers.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iconv.h>
#include <sstream>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <climits>
#include <cstring>
#include <set>
#include <glob.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include "core.h"

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

Ini ini_parse(const std::string& text) {
    Ini ini;
    std::string sec, line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[' && line.back() == ']') {
            sec = trim(line.substr(1, line.size() - 2));
            ini[sec];
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string val = line.substr(eq + 1);
        size_t sc = val.find(" ;");  // inline comment; '#' can't be one: #chan, #rrggbb
        if (sc != std::string::npos) val.erase(sc);
        ini[sec][trim(line.substr(0, eq))] = trim(val);
    }
    return ini;
}

// Passwords iSekur has to send back (nickserv_pass, sasl_pass, password) are kept in the file
// as "enc:" + AES-256-GCM with a random key in secret.key next to it (0600); hashes stay as they are.
// ponytail: whoever can read both files can read the passwords: it keeps them out of a glance,
// a backup of the config alone, or a paste. A master password is the upgrade.
bool cfg_secret(const std::string& k) { return secret_key(k) && k.find("hash") == std::string::npos; }

static std::string cfg_key(const std::string& path, bool create) {
    std::string kp = path.substr(0, path.rfind('/') + 1) + "secret.key", k;
    std::ifstream f(kp, std::ios::binary);
    if (f) k.assign(std::istreambuf_iterator<char>(f), {});
    if (k.size() == 32 || !create) return k.size() == 32 ? k : "";
    unsigned char r[32];
    RAND_bytes(r, sizeof r);
    int fd = open(kp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, r, sizeof r) != (ssize_t)sizeof r) { if (fd >= 0) close(fd); return ""; }
    close(fd);
    return std::string((char*)r, sizeof r);
}

bool ini_load(const std::string& path, Ini& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ini_parse(ss.str());
    std::string key, err;
    Ini plain;  // typed into the file by hand: encrypted in place right away
    for (auto& [sec, kv] : out)
        for (auto& [k, v] : kv)
            if (!cfg_secret(k) || v.empty()) continue;
            else if (v.rfind("enc:", 0) != 0) plain[sec][k] = v;
            else {
                if (key.empty()) key = cfg_key(path, false);
                std::string p;
                v = e2e_open(key, v.substr(4), p) ? p : "";  // no secret.key: as if unset
            }
    if (!plain.empty()) ini_save(path, plain, err);
    return true;
}

// Rewrite `text` with the values in `changes`, keeping comments and order:
// existing keys are replaced in place, new ones go at the end of their section.
std::string ini_update(const std::string& text, const Ini& changes) {
    std::string out, sec, line;
    std::set<std::pair<std::string, std::string>> done;
    auto flush_sec = [&](const std::string& sname) {  // keys of sname not yet written
        auto it = changes.find(sname);
        if (it == changes.end()) return;
        // new keys go above the blank and comment lines that end a section: those comments are the next section's
        size_t cut = out.size();
        while (cut > 0) {
            size_t ls = cut < 2 ? 0 : out.rfind('\n', cut - 2);
            ls = ls == std::string::npos || cut < 2 ? 0 : ls + 1;
            std::string t = trim(out.substr(ls, cut - ls));
            if (!t.empty() && t[0] != '#' && t[0] != ';') break;  // a key, or the section's own header
            cut = ls;
        }
        std::string tail = out.substr(cut);
        out.resize(cut);
        for (auto& [k, v] : it->second)
            if (done.insert({sname, k}).second) out += k + " = " + v + "\n";
        out += tail;
    };
    std::set<std::string> seen;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (!t.empty() && t[0] == '[' && t.back() == ']') {
            flush_sec(sec);
            sec = trim(t.substr(1, t.size() - 2));
            seen.insert(sec);
        } else if (!t.empty() && t[0] != '#' && t[0] != ';' && t.find('=') != std::string::npos) {
            std::string k = trim(t.substr(0, t.find('=')));
            auto it = changes.find(sec);
            if (it != changes.end() && it->second.count(k)) {
                size_t sc = line.find(" ;", line.find('='));  // keep the line's comment, aligned as it was
                std::string kv = k + " = " + it->second.at(k), note = sc == std::string::npos ? "" : line.substr(sc + 1);
                if (!note.empty()) kv += std::string(sc + 1 > kv.size() ? sc + 1 - kv.size() : 1, ' ') + note;
                if (done.insert({sec, k}).second) out += kv + "\n";
                continue;  // duplicate keys collapse into the first one
            }
        }
        out += line + "\n";
    }
    flush_sec(sec);
    for (auto& [sname, kv] : changes)
        if (!seen.count(sname) && !sname.empty()) {
            out += "\n[" + sname + "]\n";
            flush_sec(sname);
        }
    return out;
}

// `text` without its [sec]: the header and every line up to the next header, but for the comments right above that
// header (after the section's last blank line): they describe the next section
std::string ini_drop(const std::string& text, const std::string& sec) {
    std::istringstream in(text);
    std::string out, line, held;
    bool drop = false;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (!t.empty() && t[0] == '[' && t.back() == ']') {
            if (drop) out += held;
            held.clear();
            if ((drop = trim(t.substr(1, t.size() - 2)) == sec)) continue;
        } else if (drop) {
            if (!t.empty() && (t[0] == '#' || t[0] == ';')) held += line + "\n";
            else held.clear();
            continue;
        }
        out += line + "\n";
    }
    return out;
}

static bool write_file(const std::string& path, const std::string& text, std::string& err);

// text without the lines setting keys (in any section), commented-out ones too ("; key = x"), and the
// comment lines right above a dropped one: what they described is gone
std::string ini_drop_keys(const std::string& text, const std::set<std::string>& keys) {
    std::istringstream in(text);
    std::string out, line, held;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        bool comment = !t.empty() && (t[0] == '#' || t[0] == ';');
        size_t from = comment ? t.find_first_not_of("#;") : 0;  // a line of only #s and ;s says nothing
        std::string body = !comment ? t : from == std::string::npos ? "" : trim(t.substr(from));
        size_t eq = body.find('=');
        std::string key = eq == std::string::npos ? "" : trim(body.substr(0, eq));
        bool is_key = !key.empty() && key.find(' ') == std::string::npos;
        if (is_key && keys.count(key)) { held.clear(); continue; }
        if (comment && !is_key) { held += line + "\n"; continue; }
        out += held + line + "\n", held.clear();
    }
    return out + held;
}

bool ini_drop_keys_file(const std::string& path, const std::set<std::string>& keys, std::string& err) {
    std::ifstream f(path);
    if (!f) return true;
    std::stringstream ss;
    ss << f.rdbuf();
    return write_file(path, ini_drop_keys(ss.str(), keys), err);
}

// text into path through a temporary file, so a crash never leaves half a config
static bool write_file(const std::string& link, const std::string& text, std::string& err) {
    char real[PATH_MAX];
    std::string path = realpath(link.c_str(), real) ? real : link;  // a config symlinked into a dotfiles repo stays a symlink
    std::string tmp = path + ".tmp";
    mode_t old = umask(077);  // the config holds passwords
    std::ofstream o(tmp, std::ios::trunc);
    umask(old);
    o << text;
    o.close();
    if (!o || rename(tmp.c_str(), path.c_str())) {
        err = std::string("can't write ") + path + ": " + strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

// text with [from]'s header renamed [to]: its keys and comments stay where they are
std::string ini_rename(const std::string& text, const std::string& from, const std::string& to) {
    std::istringstream in(text);
    std::string out, line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (!t.empty() && t[0] == '[' && t.back() == ']' && trim(t.substr(1, t.size() - 2)) == from) line = "[" + to + "]";
        out += line + "\n";
    }
    return out;
}

bool ini_rename_section(const std::string& path, const std::string& from, const std::string& to, std::string& err) {
    std::ifstream f(path);
    if (!f) return true;  // no file: nothing to rename
    std::stringstream ss;
    ss << f.rdbuf();
    return write_file(path, ini_rename(ss.str(), from, to), err);
}

bool ini_drop_section(const std::string& path, const std::string& sec, std::string& err) {
    std::ifstream f(path);
    if (!f) return true;  // no file: nothing to drop
    std::stringstream ss;
    ss << f.rdbuf();
    return write_file(path, ini_drop(ss.str(), sec), err);
}

bool ini_save(const std::string& path, const Ini& changes, std::string& err) {
    std::ifstream f(path);
    std::stringstream ss;
    if (f) ss << f.rdbuf();
    std::string dir = path.substr(0, path.rfind('/') + 1);
    if (!dir.empty()) mkdir(dir.c_str(), 0700);
    Ini sealed = changes;
    std::string key;
    for (auto& [sec, kv] : sealed)
        for (auto& [k, v] : kv)
            if (cfg_secret(k) && !v.empty() && v.rfind("enc:", 0) != 0) {
                if (key.empty() && (key = cfg_key(path, true)).empty()) { err = "can't write secret.key next to " + path; return false; }
                v = "enc:" + e2e_seal(key, v);
            }
    return write_file(path, ini_update(ss.str(), sealed), err);
}

std::string ini_get(const Ini& ini, const std::string& sec, const std::string& key,
                    const std::string& def) {
    auto s = ini.find(sec);
    if (s == ini.end()) return def;
    auto k = s->second.find(key);
    return k == s->second.end() || k->second.empty() ? def : k->second;  // "key =" is a placeholder
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == sep) { if (!cur.empty()) v.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

long long now_ms() {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

std::string fmt_time(time_t t, const std::string& fmt) {
    char buf[64];
    tm lt;
    localtime_r(&t, &lt);
    return strftime(buf, sizeof buf, fmt.c_str(), &lt) ? buf : "";
}

// ---- colors ----------------------------------------------------------------
RGB xterm256_rgb(int i) {
    static const RGB base[16] = {0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080,
                                 0x008080, 0xc0c0c0, 0x808080, 0xff0000, 0x00ff00, 0xffff00,
                                 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff};
    if (i < 16) return base[i];
    if (i < 232) {
        i -= 16;
        auto v = [](int x) { return x ? 55 + x * 40 : 0; };
        return v(i / 36) << 16 | v(i / 6 % 6) << 8 | v(i % 6);
    }
    int g = 8 + (i - 232) * 10;
    return g << 16 | g << 8 | g;
}

RGB parse_color(const std::string& s) {
    static const std::map<std::string, RGB> names = {
        {"black", 0x000000},   {"red", 0xcd3131},     {"green", 0x0dbc79},
        {"yellow", 0xe5e510},  {"blue", 0x2472c8},    {"magenta", 0xbc3fbc},
        {"cyan", 0x11a8cd},    {"white", 0xe5e5e5},   {"grey", 0x808080},
        {"gray", 0x808080},    {"orange", 0xff8700},  {"pink", 0xff5fd7},
        {"purple", 0x8700ff},  {"brightwhite", 0xffffff}};
    if (s.empty() || s == "default") return -1;
    if (s[0] == '#' && s.size() == 7) return (RGB)strtol(s.c_str() + 1, nullptr, 16);
    if (isdigit((unsigned char)s[0])) return xterm256_rgb(atoi(s.c_str()) & 255);
    auto it = names.find(s);
    return it == names.end() ? -1 : it->second;
}

std::string rgb_hex(RGB c) {
    if (c < 0) return "";
    char b[8];
    snprintf(b, sizeof b, "#%06x", (unsigned)c & 0xffffff);
    return b;
}

// ---- theme -----------------------------------------------------------------
// the INFO badge: the theme's note, else the hilight color as a block (its text color when its background is the page's,
// as in mirc) with black or white text, whichever reads better on it
static double rel_lum(RGB c) {
    auto ch = [](int v) { double x = v / 255.0; return x <= .03928 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4); };
    return .2126 * ch(c >> 16 & 255) + .7152 * ch(c >> 8 & 255) + .0722 * ch(c & 255);
}
Style note_style(const Theme& t) {
    if (t.el.count("note")) return t.get("note");
    Style h = t.get("hilight"), d = t.get("default");
    RGB bg = h.bg >= 0 && h.bg != d.bg ? h.bg : h.fg >= 0 ? h.fg : 0xc2185b;
    double l = rel_lum(bg);
    return {(1.05 / (l + .05) >= (l + .05) / .05) ? 0xffffff : 0x000000, bg, A_BOLD};
}

Style plugin_style(const Theme& t) {
    if (t.el.count("plugin")) return t.get("plugin");
    Style a = t.get("tab_active"), d = t.get("default");
    RGB bg = a.bg >= 0 && a.bg != d.bg ? a.bg : a.fg >= 0 ? a.fg : 0x5f87ff;
    double l = rel_lum(bg);
    return {(1.05 / (l + .05) >= (l + .05) / .05) ? 0xffffff : 0x000000, bg, A_BOLD};
}

Style Theme::get(const std::string& k) const {
    auto it = el.find(k);
    if (it != el.end()) return it->second;
    auto d = el.find("default");
    return d == el.end() ? Style{} : d->second;
}

std::string Theme::lay(const std::string& k, const std::string& def) const {
    auto it = layout.find(k);
    return it == layout.end() ? def : it->second;
}

// "fg bg attr..." e.g. "#ffffff #5f00af bold"
static Style parse_style(const std::string& v) {
    Style st;
    std::istringstream in(v);
    std::string tok;
    int n = 0;
    while (in >> tok) {
        if (tok == "bold") st.attr |= A_BOLD;
        else if (tok == "italic") st.attr |= A_ITALIC;
        else if (tok == "underline") st.attr |= A_UNDER;
        else if (tok == "reverse") st.attr |= A_REV;
        else if (tok == "dim") st.attr |= A_DIM;
        else if (n == 0) st.fg = parse_color(tok), n++;
        else if (n == 1) st.bg = parse_color(tok), n++;
    }
    return st;
}

std::string theme_dir = "themes";  // [general] theme_dir, set by Core

// where themes are looked for, first match wins: theme_dir, then the usual places (shipped ones stay found)
std::vector<std::string> theme_dirs() {
    std::string d = theme_dir;
    if (d.rfind("~/", 0) == 0)
        if (const char* h = getenv("HOME")) d = h + d.substr(1);
    std::vector<std::string> dirs;
    if (!d.empty() && d != "themes") dirs.push_back(d);
    dirs.push_back("themes");
    if (const char* x = getenv("XDG_CONFIG_HOME")) dirs.push_back(std::string(x) + "/isekur/themes");
    if (const char* h = getenv("HOME")) dirs.push_back(std::string(h) + "/.config/isekur/themes");
    // XDG data dir: where PREFIX=~/.local make install puts them
    if (const char* x = getenv("XDG_DATA_HOME")) dirs.push_back(std::string(x) + "/isekur/themes");
    if (const char* h = getenv("HOME")) dirs.push_back(std::string(h) + "/.local/share/isekur/themes");
#ifdef ISEKUR_DATADIR
    dirs.push_back(ISEKUR_DATADIR "/themes");
#endif
    return dirs;
}

// every installed theme by name, sorted (the settings windows cycle through them)
std::vector<std::string> theme_names() {
    std::set<std::string> names;
    for (auto& d : theme_dirs()) {
        glob_t g;
        if (glob((d + "/*.theme").c_str(), 0, nullptr, &g) == 0)
            for (size_t i = 0; i < g.gl_pathc; i++) {
                std::string p = g.gl_pathv[i];
                p = p.substr(p.rfind('/') + 1);
                names.insert(p.substr(0, p.size() - 6));
            }
        globfree(&g);
    }
    return {names.begin(), names.end()};
}

bool load_theme(const std::string& name, Theme& out, std::string& err) {
    auto dirs = theme_dirs();
    Ini ini;
    bool ok = false;
    for (auto& d : dirs)
        if ((ok = ini_load(d + "/" + name + ".theme", ini))) break;
    if (!ok) { err = "theme '" + name + "' not found"; return false; }
    Theme t;
    t.name = name;
    for (auto& [k, v] : ini["colors"]) t.el[k] = parse_style(v);
    std::istringstream pal(ini_get(ini, "nicks", "palette"));
    for (std::string tok; pal >> tok;) t.nicks.push_back(parse_color(tok));
    if (t.nicks.empty()) t.nicks = {0xff5f5f, 0x5fff5f, 0xffff5f, 0x5f87ff, 0xff5fff, 0x5fffff};
    t.layout = ini["layout"];
    mirc_colors = t.lay("mirc_colors", "on") != "off";
    out = t;
    return true;
}

// ---- UTF-8 -----------------------------------------------------------------
uint32_t utf8_next(const std::string& s, size_t& i) {
    unsigned char c = (unsigned char)s[i];
    int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    if (n > 0 && i + n < s.size()) {
        static const uint32_t minv[4] = {0, 0x80, 0x800, 0x10000};  // reject overlongs
        uint32_t cp = c & (0x3f >> n);
        int k = 1;
        for (; k <= n && ((unsigned char)s[i + k] & 0xc0) == 0x80; k++)
            cp = cp << 6 | ((unsigned char)s[i + k] & 0x3f);
        if (k > n && cp >= minv[n] && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff)) {
            i += n + 1;
            return cp;
        }
    }
    i++;
    return c;  // ASCII, or an invalid byte read as Latin-1
}

void utf8_put(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) o += (char)(0xc0 | cp >> 6), o += (char)(0x80 | (cp & 0x3f));
    else if (cp < 0x10000)
        o += (char)(0xe0 | cp >> 12), o += (char)(0x80 | (cp >> 6 & 0x3f)),
            o += (char)(0x80 | (cp & 0x3f));
    else
        o += (char)(0xf0 | cp >> 18), o += (char)(0x80 | (cp >> 12 & 0x3f)),
            o += (char)(0x80 | (cp >> 6 & 0x3f)), o += (char)(0x80 | (cp & 0x3f));
}

// ---- legacy charsets: [server.x] encoding = iso-8859-9 etc. Inside iSekur it is always UTF-8; only the wire changes
static iconv_t charset_cd(const char* from, const char* to, const char* tag = "") {
    static struct Cds : std::map<std::string, iconv_t> {  // ponytail: each pair opened once, closed at exit
        ~Cds() { for (auto& kv : *this) if (kv.second != (iconv_t)-1) iconv_close(kv.second); }
    } open;
    std::string k = std::string(from) + ">" + to + tag;
    auto it = open.find(k);
    if (it != open.end()) return it->second;
    return open[k] = iconv_open(to, from);
}

bool legacy_charset_ok(const std::string& name) {
    // it has to be ASCII-compatible: UTF-16 and friends would turn every IRC line into wide characters
    const std::string probe = "\r\n :!#Aa";
    iconv_t cd = charset_cd("UTF-8", name.c_str());
    return cd != (iconv_t)-1 && to_legacy(probe, name) == probe;
}

// the network's bytes, decoded to UTF-8: what doesn't decode becomes '?'
std::string from_legacy(const std::string& in, const std::string& cs) {
    iconv_t cd = charset_cd(cs.c_str(), "UTF-8");
    if (cd == (iconv_t)-1) return in;
    iconv(cd, nullptr, nullptr, nullptr, nullptr);  // a stateful charset (ISO-2022-JP): every line starts in its initial state
    std::string out, src = in;
    char* ip = &src[0];
    size_t il = src.size();
    while (il) {
        char buf[512];
        char* op = buf;
        size_t ol = sizeof buf;
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        out.append(buf, (size_t)(op - buf));
        if (r != (size_t)-1 || errno == E2BIG) continue;
        if (errno != EILSEQ && errno != EINVAL) break;  // not something a retry can fix
        out += '?', ip++, il--;
    }
    return out;
}

// whether cs can write ch (one UTF-8 character) as itself: converted alone from the initial state, it decodes back to ch.
// The return code can't say it: FreeBSD writes the euro sign as "EUR" in ISO-8859-9 and calls that a clean conversion
static bool writes_itself(const std::string& ch, const std::string& cs) {
    iconv_t cd = charset_cd("UTF-8", cs.c_str(), " probe");  // its own handle: the line's handle keeps its shift state
    if (cd == (iconv_t)-1) return false;
    iconv(cd, nullptr, nullptr, nullptr, nullptr);
    std::string c = ch;
    char buf[32];
    char* ip = &c[0];
    char* op = buf;
    size_t il = c.size(), ol = sizeof buf;
    if (iconv(cd, &ip, &il, &op, &ol) != 0 || iconv(cd, nullptr, nullptr, &op, &ol) == (size_t)-1) return false;
    return from_legacy(std::string(buf, (size_t)(op - buf)), cs) == ch;
}

// UTF-8 to the network's bytes, a character at a time: one it can't write becomes '?'
std::string to_legacy(const std::string& in, const std::string& cs) {
    iconv_t cd = charset_cd("UTF-8", cs.c_str());
    if (cd == (iconv_t)-1) return in;
    iconv(cd, nullptr, nullptr, nullptr, nullptr);
    std::string out;
    for (size_t i = 0; i < in.size();) {
        size_t j = i;
        utf8_next(in, j);
        std::string ch = in.substr(i, j - i);
        char buf[16];
        char* ip = &ch[0];
        char* op = buf;
        size_t il = ch.size(), ol = sizeof buf;
        bool ascii = ch.size() == 1 && (unsigned char)ch[0] < 0x80;  // legacy_charset_ok: the charset writes ASCII as is
        if (!ascii && !writes_itself(ch, cs)) out += '?';  // checked first: a converted and dropped character could move the shift state
        else if (iconv(cd, &ip, &il, &op, &ol) != 0) out += '?';
        else out.append(buf, (size_t)(op - buf));
        i = j;
    }
    char end[16];  // a stateful charset goes back to its initial state, or the next line's ASCII is read as Kanji
    char* op = end;
    size_t ol = sizeof end;
    if (iconv(cd, nullptr, nullptr, &op, &ol) != (size_t)-1) out.append(end, (size_t)(op - end));
    return out;
}

// ---- mIRC formatting -------------------------------------------------------
bool mirc_colors = true;
static const RGB mirc_pal[99] = {
    0xffffff, 0x000000, 0x00007f, 0x009300, 0xff0000, 0x7f0000, 0x9c009c, 0xfc7f00, 0xffff00,
    0x00fc00, 0x009393, 0x00ffff, 0x0000fc, 0xff00ff, 0x7f7f7f, 0xd2d2d2, 0x470000, 0x472100,
    0x474700, 0x324700, 0x004700, 0x00472c, 0x004747, 0x002747, 0x000047, 0x2e0047, 0x470047,
    0x47002a, 0x740000, 0x743a00, 0x747400, 0x517400, 0x007400, 0x007449, 0x007474, 0x004074,
    0x000074, 0x4b0074, 0x740074, 0x740045, 0xb50000, 0xb56300, 0xb5b500, 0x7db500, 0x00b500,
    0x00b571, 0x00b5b5, 0x0063b5, 0x0000b5, 0x7500b5, 0xb500b5, 0xb5006b, 0xff0000, 0xff8c00,
    0xffff00, 0xb2ff00, 0x00ff00, 0x00ffa0, 0x00ffff, 0x008cff, 0x0000ff, 0xa500ff, 0xff00ff,
    0xff0098, 0xff5959, 0xffb459, 0xffff71, 0xcfff60, 0x6fff6f, 0x65ffc9, 0x6dffff, 0x59b4ff,
    0x5959ff, 0xc459ff, 0xff66ff, 0xff59bc, 0xff9c9c, 0xffd39c, 0xffff9c, 0xe2ff9c, 0x9cff9c,
    0x9cffdb, 0x9cffff, 0x9cd3ff, 0x9c9cff, 0xdc9cff, 0xff9cff, 0xff94d3, 0x000000, 0x131313,
    0x282828, 0x363636, 0x4d4d4d, 0x656565, 0x818181, 0x9f9f9f, 0xbcbcbc, 0xe2e2e2, 0xffffff};

void mirc_walk(const std::string& s, Style base,
               const std::function<void(uint32_t, const Style&)>& f) {
    Style st = base;
    size_t i = 0;
    auto num = [&](int& out) {  // up to 2 digits
        if (i < s.size() && isdigit((unsigned char)s[i])) {
            out = s[i++] - '0';
            if (i < s.size() && isdigit((unsigned char)s[i])) out = out * 10 + s[i++] - '0';
            return true;
        }
        return false;
    };
    auto hex = [&](RGB& out) {
        if (i + 6 > s.size()) return false;
        for (int k = 0; k < 6; k++)
            if (!isxdigit((unsigned char)s[i + k])) return false;
        out = (RGB)strtol(s.substr(i, 6).c_str(), nullptr, 16);
        i += 6;
        return true;
    };
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case 0x02: st.attr ^= A_BOLD; i++; continue;
        case 0x1d: st.attr ^= A_ITALIC; i++; continue;
        case 0x1f: st.attr ^= A_UNDER; i++; continue;
        case 0x16: st.attr ^= A_REV; i++; continue;
        case 0x1e: case 0x11: i++; continue;  // strikethrough, monospace: ignored
        case 0x0f: st = base; i++; continue;
        case 0x03: {
            i++;
            int fg, bg;
            if (!num(fg)) { st.fg = base.fg, st.bg = base.bg; continue; }
            if (fg < 99) st.fg = mirc_pal[fg];
            else st.fg = base.fg;
            if (i + 1 < s.size() && s[i] == ',' && isdigit((unsigned char)s[i + 1])) {
                i++;
                num(bg);
                st.bg = bg < 99 ? mirc_pal[bg] : base.bg;
            }
            if (!mirc_colors) st.fg = base.fg, st.bg = base.bg;
            continue;
        }
        case 0x04: {
            i++;
            RGB fg, bg;
            if (!hex(fg)) { st.fg = base.fg, st.bg = base.bg; continue; }
            st.fg = fg;
            if (i < s.size() && s[i] == ',') {
                i++;
                if (hex(bg)) st.bg = bg;
            }
            if (!mirc_colors) st.fg = base.fg, st.bg = base.bg;
            continue;
        }
        }
        uint32_t cp = utf8_next(s, i);
        if (cp < 0x20) continue;
        f(cp, st);
    }
}

// s up to its n-th visible character, color and bold codes kept (/about's typing)
std::string mirc_prefix(const std::string& s, size_t n) {
    size_t i = 0, seen = 0;
    auto digits = [&] { size_t k = 0; while (k < 2 && i < s.size() && isdigit((unsigned char)s[i])) i++, k++; return k > 0; };
    auto hex6 = [&] {
        for (int k = 0; k < 6; k++)
            if (i + k >= s.size() || !isxdigit((unsigned char)s[i + k])) return false;
        return i += 6, true;
    };
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x03) {
            i++;
            if (digits() && i + 1 < s.size() && s[i] == ',' && isdigit((unsigned char)s[i + 1])) i++, digits();
            continue;
        }
        if (c == 0x04) {
            i++;
            if (hex6() && i < s.size() && s[i] == ',') i++, hex6();
            continue;
        }
        if (c < 0x20) { i++; continue; }
        if (seen == n) break;
        utf8_next(s, i), seen++;
    }
    return s.substr(0, i);
}

std::string mirc_strip(const std::string& s) {
    std::string out;
    mirc_walk(s, {}, [&](uint32_t cp, const Style&) { utf8_put(out, cp); });
    return out;
}

// ---- base64 ----------------------------------------------------------------
std::string b64(const std::string& s) {
    std::string o(4 * ((s.size() + 2) / 3) + 1, '\0');
    int n = EVP_EncodeBlock((unsigned char*)&o[0], (const unsigned char*)s.data(), (int)s.size());
    o.resize((size_t)n);
    return o;
}

std::string unb64(const std::string& s) {
    if (s.empty() || s.size() % 4) return "";
    std::string o(s.size() / 4 * 3, '\0');
    int n = EVP_DecodeBlock((unsigned char*)&o[0], (const unsigned char*)s.data(), (int)s.size());
    if (n < 0) return "";
    size_t pad = s.size() - s.find_last_not_of('=') - 1;
    o.resize((size_t)n - std::min(pad, (size_t)2));
    return o;
}
