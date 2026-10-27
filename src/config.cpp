// INI parsing, colors, themes, mIRC formatting, UTF-8 and small helpers.
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/time.h>
#include <openssl/evp.h>
#include "core.h"

static std::string trim(const std::string& s) {
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

bool ini_load(const std::string& path, Ini& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ini_parse(ss.str());
    return true;
}

std::string ini_get(const Ini& ini, const std::string& sec, const std::string& key,
                    const std::string& def) {
    auto s = ini.find(sec);
    if (s == ini.end()) return def;
    auto k = s->second.find(key);
    return k == s->second.end() ? def : k->second;
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

bool load_theme(const std::string& name, Theme& out, std::string& err) {
    std::vector<std::string> dirs = {"themes"};
    if (const char* x = getenv("XDG_CONFIG_HOME")) dirs.push_back(std::string(x) + "/isekur/themes");
    if (const char* h = getenv("HOME")) dirs.push_back(std::string(h) + "/.config/isekur/themes");
#ifdef ISEKUR_DATADIR
    dirs.push_back(ISEKUR_DATADIR "/themes");
#endif
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

// ---- mIRC formatting -------------------------------------------------------
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
            continue;
        }
        }
        uint32_t cp = utf8_next(s, i);
        if (cp < 0x20) continue;
        f(cp, st);
    }
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
