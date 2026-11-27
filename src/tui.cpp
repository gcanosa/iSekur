// Terminal frontend: layout, rendering, keyboard.
#include "tui.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fcntl.h>
#include <random>
#include <sys/wait.h>
#include <unordered_map>
#include "core.h"
#include "termbox2.h"
#include <unistd.h>

static const int TB_MOD_SUPER = 16;  // vendor/termbox2.c: CSI u / modifyOtherKeys, Cmd
static bool g_true = true;  // truecolor output, else nearest xterm-256

static int nearest256(RGB c) {
    int r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
    auto q = [](int v) { return v < 48 ? 0 : v < 115 ? 1 : (v - 35) / 40; };
    auto lv = [](int x) { return x ? 55 + x * 40 : 0; };
    int qr = q(r), qg = q(g), qb = q(b);
    int dr = lv(qr) - r, dg = lv(qg) - g, db = lv(qb) - b;
    int avg = (r + g + b) / 3, gi = avg > 238 ? 23 : avg < 8 ? 0 : (avg - 8) / 10, gv = 8 + gi * 10;
    int dgray = (gv - r) * (gv - r) + (gv - g) * (gv - g) + (gv - b) * (gv - b);
    return dgray < dr * dr + dg * dg + db * db ? 232 + gi : 16 + 36 * qr + 6 * qg + qb;
}

static uintattr_t tbc(RGB c) {
    if (c < 0) return TB_DEFAULT;
    if (g_true) return c == 0 ? TB_HI_BLACK : (uintattr_t)c;
    int i = nearest256(c);
    return i == 0 ? TB_HI_BLACK : (uintattr_t)i;
}

// isekur.tint (API 15): the tint being drawn with (null: none). Each (fg, bg) pair is worked out once per change of the tint
static const Tint* g_tint = nullptr;
static unsigned g_tint_ver = 0;
static std::unordered_map<uint64_t, std::pair<RGB, RGB>> g_tint_memo;
static std::pair<RGB, RGB> tinted(RGB fg, RGB bg) {
    if (!g_tint) return {fg, bg};
    uint64_t k = (uint64_t)(uint32_t)(fg + 1) << 32 | (uint32_t)(bg + 1);
    if (auto it = g_tint_memo.find(k); it != g_tint_memo.end()) return it->second;
    if (g_tint_memo.size() > 8192) g_tint_memo.clear();
    return g_tint_memo[k] = {tint_fg(fg, bg, *g_tint), tint_color(bg, *g_tint)};
}

extern "C" void isekur_link(int x, int y, int id);
static void cell(int x, int y, uint32_t cp, const Style& s) {
    isekur_link(x, y, 0);  // whatever is drawn here is not the link that was (a window over the chat); draw_row sets its own after
    auto [sf, sb] = tinted(s.fg, s.bg);
    uintattr_t fg = tbc(sf), bg = tbc(sb);
    if (s.attr & A_BOLD) fg |= TB_BOLD;
    if (s.attr & A_ITALIC) fg |= TB_ITALIC;
    if (s.attr & A_UNDER) fg |= TB_UNDERLINE;
    if (s.attr & A_REV) fg |= TB_REVERSE;
    if (s.attr & A_DIM) fg |= TB_DIM;
    tb_set_cell(x, y, cp, fg, bg);
}

static int cwidth(uint32_t cp) {
    int w = wcwidth((wchar_t)cp);
    return w < 0 ? 1 : w;
}

struct Cell { uint32_t cp; Style st; int w; int link = 0; };  // link: frame_urls index + 1
using Row = std::vector<Cell>;

// clickable URLs (OSC 8, vendor/termbox2.c): the ones drawn this frame
extern "C" void isekur_links_begin(void);
extern "C" void isekur_links_urls(const char* const* urls, int n);
extern "C" void isekur_link(int x, int y, int id);
extern "C" int isekur_link_at(int x, int y);
extern "C" void isekur_link_underline(int id);
static int g_hover_x = -1, g_hover_y = -1, g_hover_id = 0;  // the pointer (mode 1003) and the link under it, underlined when the frame is shown
extern "C" int isekur_present(void);
extern "C" void isekur_cell_reverse(int x, int y);
static std::vector<std::string> frame_urls;

// unlocking: the session opens from the middle out. g_reveal 0..1, or -1: none
static double g_reveal = -1;
static Style g_cover, g_edge;

// The text cursor, drawn by us: a reverse-video cell in every terminal and theme (the terminal's own
// can be invisible, over SSH or on the input line's color). /set cursor: 0 off, 1 block, 2 blink.
// put_cursor says where, present() draws it last, over whatever the frame put there.
static int g_cur_style = 2, g_cur_x = -1, g_cur_y = -1, g_shown_x = -1, g_shown_y = -1;
static long long g_cur_moved = 0;  // when it last moved: a blinking cursor starts "on"
static bool g_cur_on = true;
static constexpr int BLINK_MS = 530;
static bool cur_phase() { return g_cur_style != 2 || (now_ms() - g_cur_moved) / BLINK_MS % 2 == 0; }
static void put_cursor(int x, int y) {
    g_cur_x = x, g_cur_y = y;
    tb_hide_cursor();
}

static void present() {
    if (g_cur_style && g_cur_x >= 0) {
        if (g_cur_x != g_shown_x || g_cur_y != g_shown_y) g_cur_moved = now_ms();
        g_shown_x = g_cur_x, g_shown_y = g_cur_y;
        if ((g_cur_on = cur_phase())) isekur_cell_reverse(g_cur_x, g_cur_y);
    } else g_shown_x = -1;
    if (g_reveal >= 0) {
        int W = tb_width(), H = tb_height();
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                double nx = (x - W / 2.0) / (W / 2.0), ny = (y - H / 2.0) / (H / 2.0);
                double d = std::sqrt(nx * nx + ny * ny) / 1.4143 + (unsigned)((x * 73856093) ^ (y * 19349663)) % 100 / 800.0;
                double r = g_reveal * 1.2;  // a ragged ring: the noise keeps it from looking drawn with a compass
                if (d > r + 0.05) cell(x, y, ' ', g_cover);
                else if (d > r) cell(x, y, U'░', g_edge);
            }
    }
    std::vector<const char*> u;
    for (auto& s : frame_urls) u.push_back(s.c_str());
    isekur_links_urls(u.data(), (int)u.size());
    g_hover_id = g_hover_x >= 0 ? isekur_link_at(g_hover_x, g_hover_y) : 0;
    isekur_link_underline(g_hover_id);
    isekur_present();
}

// http(s)://... in a row of cells, like the webchat: up to a space or < > " ', without the
// trailing punctuation of the sentence. Printable ASCII only, since it goes into an escape
// sequence (anything else ends the link).
static RGB readable(RGB c, RGB bg);
static void linkify(Row& r, const Theme& T) {
    bool styled = false;
    Style ls;  // the theme's link look (link_style), worked out when the row has a link
    auto at = [&](size_t i, const char* s) {
        for (size_t k = 0; s[k]; k++)
            if (i + k >= r.size() || r[i + k].cp != (uint32_t)s[k]) return false;
        return true;
    };
    for (size_t i = 0; i < r.size(); i++) {
        if (!at(i, "http://") && !at(i, "https://")) continue;
        if (i && r[i - 1].cp < 128 && isalnum((int)r[i - 1].cp)) continue;
        size_t j = i;
        while (j < r.size() && r[j].cp > 0x20 && r[j].cp < 0x7f && !strchr("<>\"'", (int)r[j].cp)) j++;
        while (j > i && strchr(".,;:!?)]", (int)r[j - 1].cp)) j--;
        std::string url;
        for (size_t k = i; k < j; k++) url += (char)r[k].cp;
        if (url.size() <= 8 || url.size() > 2000 || frame_urls.size() >= 65535) { i = j; continue; }
        frame_urls.push_back(url);
        if (!styled) ls = link_style(T), styled = true;
        for (size_t k = i; k < j; k++) {
            r[k].link = (int)frame_urls.size();
            if (ls.fg >= 0) r[k].st.fg = readable(ls.fg, r[k].st.bg);  // on the line's own background (the topic bar's is another)
            r[k].st.attr |= ls.attr;
        }
        i = j;
    }
}

// the chat's theme while its lines are built (wrap): a nick color of it, or yours, is shown as the theme made it, in the
// nick column and in a text a plugin painted (nickcolors) alike
static const Theme* tint_theme = nullptr;
static RGB tint_self = -1;
static bool theme_nick(RGB c) { return tint_theme && (c == tint_self || std::find(tint_theme->nicks.begin(), tint_theme->nicks.end(), c) != tint_theme->nicks.end()); }
static void cells(Row& out, const std::string& s, Style st, bool mirc = true) {
    auto put = [&](uint32_t cp, const Style& cs) {
        int w = cwidth(cp);
        if (!w) return;
        out.push_back({cp, cs, w});
        // a mIRC color on the theme's background (yellow on rose-pine-dawn): toward black or white until it reads
        if (mirc && cs.fg != st.fg && cs.bg == st.bg && !theme_nick(cs.fg)) out.back().st.fg = readable(cs.fg, st.bg);
    };
    if (mirc) mirc_walk(s, st, put);
    else
        for (size_t i = 0; i < s.size();) put(utf8_next(s, i), st);
}

// draw cells from x, clipped at xmax; returns the next x
static int draw_row(int x, int y, int xmax, const Row& r) {
    for (auto& c : r) {
        if (x + c.w > xmax) break;
        cell(x, y, c.cp, c.st);
        if (c.link) isekur_link(x, y, c.link);
        x += c.w;
    }
    return x;
}

static int text(int x, int y, int xmax, const std::string& s, Style st, bool mirc = true) {
    Row r;
    cells(r, s, st, mirc);
    return draw_row(x, y, xmax, r);
}

static int width(const Row& r) {
    int w = 0;
    for (auto& c : r) w += c.w;
    return w;
}

static void fill(int y, int x0, int x1, Style st) {
    for (int x = x0; x < x1; x++) cell(x, y, ' ', st);
}

// a window nearly as wide as the screen (x0..x1, x1 past its edge): blank the 1-2 columns beside it, rows y0..y1-1,
// or the chat under it peeks out there as stray letters
static void gutters(int W, int x0, int x1, int y0, int y1, Style st) {
    if (x0 > 2 && W - x1 > 2) return;
    for (int y = y0; y < y1; y++) fill(y, 0, x0, st), fill(y, x1, W, st);
}

// A box's top row (x0, x1: its corners): the title, and the theme's window buttons like the webchat's dialogs.
// window = mac: red, yellow and green dots before the title; win95 (mirc): a navy title bar, white bold title,
// gray _ □ × at the right. Other themes: the title on the border
// returns the close button's columns ({-1, -1}: none)
static std::pair<int, int> box_title(const Theme& T, int x0, int x1, int y, const std::string& title, Style hs) {
    std::string ws = T.lay("window", "");
    if (ws == "win95") {
        Style bar{0xffffff, 0x000080, A_BOLD}, btn{0x000000, 0xc0c0c0, A_BOLD};
        for (int x = x0 + 1; x < x1; x++) cell(x, y, U' ', bar);
        // Minimize and close only (maximize did nothing): two gray buttons, three cells each with the glyph in the
        // middle, a navy cell between them like Windows' gap before close. Solid faces: edge lines inside a cell read
        // as dividers, half-blocks made notches, and a maximize □ is two cells wide in fonts like Iosevka
        text(x0 + 2, y, x1 - 9, title, bar, false);
        int bx[] = {x1 - 8, x1 - 4};
        char32_t g[] = {U'_', U'\u00D7'};
        for (int i = 0; i < 2; i++) cell(bx[i], y, U' ', btn), cell(bx[i] + 1, y, g[i], btn), cell(bx[i] + 2, y, U' ', btn);
        return {bx[1], bx[1] + 2};
    }
    int tx = x0 + 2;
    std::pair<int, int> close{-1, -1};
    if (ws == "mac") {
        close = {tx, tx};  // the red one
        Style dot = hs;
        dot.attr = 0;
        for (RGB col : {0xff5f57, 0xfebc2e, 0x28c840}) dot.fg = col, cell(tx, y, U'\u25CF', dot), tx += 2;
    }
    text(tx, y, x1 - 1, " " + title + " ", hs, false);
    return close;
}

// twilight's sky, three rows of ASCII (one cell wide in every font) from x, y: the sun by day, the sun on the horizon at dusk, the moon at night
static void draw_sky(int x, int y, double sky, const Style& main, const Style& sub) {
    static const char* const art[3][3] = {{" \\  |  / ", " --(O)-- ", " /  |  \\ "}, {"  \\ | /  ", "  .-'-.  ", "~~~~~~~~~"}, {"  .--.  *", " (  (     ", "  `--'  ."}};
    const char* const* a = art[sky < 0.25 ? 0 : sky < 0.75 ? 1 : 2];
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 9; k++)
            if (a[r][k] != ' ') cell(x + k, y + r, (char32_t)a[r][k], strchr("*.~", a[r][k]) ? sub : main);
}

// WCAG contrast of two colors (1..21), like the webchat's: picks colors that read whatever the theme
static double contrast(RGB a, RGB b) {
    auto lum = [](RGB c) {
        auto ch = [&](int sh) { double v = (c >> sh & 255) / 255.0; return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        return 0.2126 * ch(16) + 0.7152 * ch(8) + 0.0722 * ch(0);
    };
    double x = lum(a), y = lum(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

// c, or if it doesn't read on bg (4.5:1), c toward black or white until it does (the webchat's readOn)
static RGB readable(RGB c, RGB bg) {
    if (c < 0 || bg < 0 || contrast(c, bg) >= 4.5) return c;
    bool dark = contrast(0x000000, bg) > contrast(0xffffff, bg);
    RGB to = dark ? 0x000000 : 0xffffff, out = c;
    for (double k = 0.05; contrast(out, bg) < 4.5 && k <= 1.0; k += 0.05) {
        auto mix = [&](int sh) { int a = c >> sh & 255, b = to >> sh & 255; return (int)std::lround(a + (b - a) * k) << sh; };
        out = mix(16) | mix(8) | mix(0);
    }
    return out;
}

// a word-wrapped question for a yes/no box
static std::vector<std::string> wrap_words(const std::string& s, int w) {
    std::vector<std::string> out;
    std::string line;
    for (auto& word : split(s, ' ')) {
        if (!line.empty() && (int)(line.size() + 1 + word.size()) > w) out.push_back(line), line.clear();
        line += (line.empty() ? "" : " ") + word;
    }
    if (!line.empty()) out.push_back(line);
    return out;
}


static std::string to_utf8(const std::u32string& s) {
    std::string o;
    for (char32_t c : s) utf8_put(o, c);
    return o;
}

static std::u32string from_utf8(const std::string& s) {
    std::u32string o;
    for (size_t i = 0; i < s.size();) o += (char32_t)utf8_next(s, i);
    return o;
}

// s drawn on a row styled `row`: its background, and the reverse that marks a selection or a bar in a theme with no colors
static void on_row(Style& s, const Style& row) { s.bg = row.bg, s.attr |= row.attr & A_REV; }

// theme element with a fallback element instead of "default"
static Style sty(const Theme& t, const std::string& k, const std::string& fb) {
    return t.el.count(k) ? t.get(k) : t.get(fb);
}

static Style nick_style(const Theme& t, const std::string& nick) {
    Style s = t.get("default");
    if (!t.nicks.empty()) s.fg = nick_rgb(t, nick);  // no theme file found: no nick colors
    return s;
}

// a status tab shows the network (NETWORK= from 005, e.g. OFTC), the config name until then
static std::string buf_label(const Buffer* b) {
    if (b->type == BT::STATUS && !b->srv) return "CMD";  // the home tab: capitals, unlike any channel
    return b->type == BT::STATUS && b->srv && !b->srv->network.empty() ? b->srv->network : b->name;
}

// ---- status_info: this machine's CPU, memory, load and battery -------------
// src/sysinfo.cpp, apart since macOS headers have their own Style
int cpu_pct();
bool mem_free(unsigned long long& avail, unsigned long long& total);
int battery_pct();

// "a is typing…", "a and b are…", "a, b and c are…", "a, b and 4 others are…": a busy channel can't fill the bar
std::string typing_text(const std::vector<std::string>& n) {
    if (n.empty()) return "";
    std::string s = n[0];
    size_t shown = n.size() > 3 ? 2 : n.size();
    for (size_t i = 1; i < shown; i++) s += (i + 1 == shown && n.size() <= 3 ? " and " : ", ") + n[i];
    if (n.size() > 3) s += " and " + std::to_string(n.size() - 2) + " others";
    return s + (n.size() == 1 ? " is typing\u2026" : " are typing\u2026");
}

// uptime as 3d4h22m, seconds below a day
std::string fmt_dur(long up) {
    std::string u;
    const std::pair<long, const char*> units[] = {{2592000, "mo"}, {86400, "d"}, {3600, "h"}, {60, "m"}, {1, "s"}};
    bool days = up >= 86400;
    for (auto& [n, name] : units)
        if ((!u.empty() || up >= n || n == 1) && !(n == 1 && days)) u += std::to_string(up / n) + name, up %= n;
    return u;
}

// cpu_levels / mem_levels: "min avg max" percents; at min a value turns status_warn, at avg
// status_high, at max status_crit (0 = none of them). Anything but three rising numbers
// 0-100 means the defaults.
static int level(const Core& c, const std::string& key, const char* def, int pct) {
    int l[3];
    auto parse = [&](const std::string& v) {
        return sscanf(v.c_str(), "%d %d %d", &l[0], &l[1], &l[2]) == 3 && 0 <= l[0] && l[0] <= l[1] && l[1] <= l[2] && l[2] <= 100;
    };
    if (!parse(c.opt(nullptr, key, def))) parse(def);
    return pct >= l[2] ? 3 : pct >= l[1] ? 2 : pct >= l[0] ? 1 : 0;
}

// a status_info color from the theme, on base; a theme without it gets fg (and bg)
static Style info_style(const Theme& T, const std::string& k, Style base, RGB fg, RGB bg = -1) {
    Style t = T.el.count(k) ? T.get(k) : Style{fg, bg, 0};
    if (t.fg >= 0) base.fg = t.fg;
    if (t.bg >= 0) base.bg = t.bg;
    base.attr |= t.attr;
    return base;
}
// status_ok, status_warn, status_high, status_crit: green, yellow, orange, red by default
static Style level_style(const Theme& T, int lv, Style base) {
    static const char* keys[] = {"status_ok", "status_warn", "status_high", "status_crit"};
    static const RGB fgs[] = {0x5fd75f, 0xffd75f, 0xffaf00, 0xff5f5f};
    return info_style(T, keys[lv], base, fgs[lv]);
}

// the status_info items that are on, in a fixed order, drawn in st
static std::vector<Row> status_info(const Core& c, Style st, bool dcc = true) {
    const Theme& T = c.look(false);
    auto on = split(c.opt(nullptr, "status_info", "up"), ' ');
    auto has = [&](const char* k) { return std::find(on.begin(), on.end(), k) != on.end(); };
    std::vector<Row> out;
    auto add = [&](const std::string& s) { out.emplace_back(), cells(out.back(), s, st, false); };
    // /dcc transfers, always and first (the last items drop): ⇣name ▮▮▮▯▯▯▯▯ 42% 1.2M/s +1 (⇡ sending)
    std::vector<const Dcc*> rx;
    for (auto& d : c.dccs)
        if (d->st == Dcc::RUN) rx.push_back(d.get());
    if (!dcc) {  // the webchat draws its own
    } else if (!rx.empty()) {
        const Dcc* d = rx[0];
        Style es = info_style(T, "status_bat", st, 0x6c6c6c), bs = level_style(T, 0, st);
        es.bg = bs.bg = st.bg;
        Row r;
        cells(r, (d->send ? "\u21E1" : "\u21E3") + (mirc_strip(d->file).size() > 16 ? mirc_prefix(d->file, 15) + "\u2026" : d->file) + " ", st, false);
        if (d->size) {
            int pct = (int)(d->got * 100 / d->size), full = pct * 8 / 100;
            for (int i = 0; i < 8; i++) cells(r, i < full ? "\u25AE" : "\u25AF", i < full ? bs : es, false);
            cells(r, " " + std::to_string(pct) + "%", st, false);
        } else {
            cells(r, human_bytes(d->got), st, false);
        }
        cells(r, " " + human_bytes(d->rate) + "/s" + (rx.size() > 1 ? " +" + std::to_string(rx.size() - 1) : ""), st, false);
        out.push_back(r);
    } else {  // nothing moving: the last one that ended, for 10s (✓ green, ✗ red), else the offers waiting
        const Dcc* last = nullptr;
        size_t offers = 0;
        for (auto& d : c.dccs) {
            if (d->ended && time(nullptr) - d->ended < 10 && (!last || d->ended >= last->ended)) last = d.get();
            offers += d->st == Dcc::OFFER;
        }
        auto nm = [](const std::string& f) { return mirc_strip(f).size() > 16 ? mirc_prefix(f, 15) + "\u2026" : f; };
        if (last) {
            bool ok = last->st == Dcc::DONE;
            Row r;
            cells(r, std::string(ok ? "\u2713" : "\u2717") + nm(last->file), level_style(T, ok ? 0 : 3, st), false);
            cells(r, ok ? " done" : last->err == "cancelled" ? " stopped" : " failed", st, false);
            out.push_back(r);
        } else if (offers) {
            out.emplace_back();
            cells(out.back(), "\u21E3" + std::to_string(offers) + " offered (/dcc)", sty(T, "hilight", "status_hi"), false);
        }
    }
    if (c.radio.st != Core::Radio::OFF) {  // a plugin's radio: R: ON (MUTE when muted, … while it tunes), always, next to the items you chose
        bool play = c.radio.st == Core::Radio::PLAYING;
        Row r;
        cells(r, "R: ", st, false);
        cells(r, c.radio.muted ? "MUTE" : play ? "ON" : "\u2026", level_style(T, play && !c.radio.muted ? 0 : 1, st), false);
        out.push_back(r);
    }
    if (has("up")) add("u:" + fmt_dur((long)(time(nullptr) - c.started)));
    // label in st, the value in its level's color
    auto leveled = [&](const std::string& label, const std::string& val, const std::string& key, const char* def, int pct) {
        int lv = pct >= 0 ? level(c, key, def, pct) : 0;
        Style vs = lv ? level_style(T, lv, st) : st;
        out.emplace_back();
        cells(out.back(), label, st, false);
        cells(out.back(), val, vs, false);
    };
    if (has("cpu")) {
        int p = cpu_pct();
        leveled("cpu:", (p < 0 ? std::string("--") : std::to_string(p)) + "%", "cpu_levels", "50 75 90", p);
    }
    unsigned long long a, t;
    if (has("mem") && mem_free(a, t))  // colored by how much is used
        leveled("free:", human_bytes(a) + "/" + human_bytes(t), "mem_levels", "70 85 95", t ? (int)((t - a) * 100 / t) : -1);
    double ld[3];
    if (has("load") && getloadavg(ld, 3) == 3) {
        char buf[48];
        snprintf(buf, sizeof buf, "load:%.2f %.2f %.2f", ld[0], ld[1], ld[2]);
        add(buf);
    }
    int bp = has("bat") ? battery_pct() : -1;
    if (bp >= 0) {  // ▮▮▯╸64%: the bars on a status_bat body, 3 status_ok, 2 warn, 1 high, 1 crit below 15%
        int bars = bp > 66 ? 3 : bp > 33 ? 2 : 1;
        Style es = info_style(T, "status_bat", st, 0x6c6c6c, 0x3a3a3a);  // fg: empty bars and the tip, bg: the body
        Style bs = level_style(T, bp > 66 ? 0 : bp > 33 ? 1 : bp >= 15 ? 2 : 3, st), tip = es;
        bs.bg = es.bg, tip.bg = st.bg;  // no edge lines: fonts put ▕ ▏ in different places
        Row r;
        for (int i = 0; i < 3; i++) cells(r, i < bars ? "\u25AE" : "\u25AF", i < bars ? bs : es, false);
        cells(r, "\u2578", tip, false);
        cells(r, std::to_string(bp) + "%", bp < 15 ? Style{bs.fg, st.bg, st.attr} : st, false);
        out.push_back(r);
    }
    return out;
}

std::string json_str(const std::string& s);  // web.cpp
std::string status_info_json(const Core& c) {
    // isekur.tint (twilight): the page paints these with the colors given here (not the theme's), so they are tinted here, like the theme's
    const Theme& T = c.look(true);
    const Tint& tn = c.tint;
    bool tw = tn.web && tn.on();
    RGB ref = T.get("status").bg >= 0 ? T.get("status").bg : T.get("default").bg;  // what an item without a background of its own sits on
    auto shade = [&](Style s) { if (tw) s.fg = tint_fg(s.fg, s.bg >= 0 ? s.bg : ref, tn), s.bg = tint_color(s.bg, tn); return s; };
    std::string o = "[";
    for (auto& r : status_info(c, sty(T, "status_info", "status_hi"), false)) {
        o += o.size() > 1 ? ",[" : "[";
        std::string run;
        Style cur;
        const Style* at = nullptr;  // runs of one color: [text, fg, bg]
        auto put = [&] { if (at) o += std::string(o.back() == '[' ? "" : ",") + "[" + json_str(run) + "," + json_str(rgb_hex(at->fg)) + "," + json_str(rgb_hex(at->bg)) + "]"; };
        for (auto& cl : r) {
            Style st = shade(cl.st);
            if (!at || st.fg != at->fg || st.bg != at->bg) put(), run.clear(), cur = st, at = &cur;
            run += to_utf8(std::u32string(1, (char32_t)cl.cp));
        }
        put();
        o += "]";
    }
    return o + "]";
}

// ---- line layout -----------------------------------------------------------
// full: a /typing line at its whole length, so its height holds still (scrolled back)
std::map<std::string, std::string> json_flat(const std::string& s);  // web.cpp

// Line::embeds ("{...},{...}") as one map per object: commas inside strings don't split
static std::vector<std::map<std::string, std::string>> embed_list(const std::string& s) {
    std::vector<std::map<std::string, std::string>> out;
    size_t start = 0;
    bool str = false, esc = false;
    for (size_t i = 0; i < s.size(); i++) {
        char ch = s[i];
        if (str) { if (esc) esc = false; else if (ch == '\\') esc = true; else if (ch == '"') str = false; }
        else if (ch == '"') str = true;
        else if (ch == '}') out.push_back(json_flat(s.substr(start, i + 1 - start))), start = i + 2;
    }
    return out;
}
// r cut to w columns, "…" at the end when it was longer
static void clip(Row& r, int w, Style st) {
    if (width(r) <= w) return;
    while (!r.empty() && width(r) > w - 1) r.pop_back();
    r.push_back({U'…', st, 1});
}

// what a copy needs of a line's rows: its time, its nick column, where its text starts, each text row's width
struct WrapInfo { std::string ts, marker; bool ts_right = false; int indent = 0, tsx = -1, tsw = 0; std::vector<int> bw; };

// tsf: a time stamp format of its own (the log browser's has the seconds)
static std::vector<Row> wrap(const Core& c, const Line& l, const Buffer* b, int width_, bool full = false, const char* tsf = nullptr,
                             WrapInfo* info = nullptr) {
    const Theme& T = c.look(false);
    tint_theme = &T, tint_self = c.self_nick(T);
    int nw = atoi(ini_get(c.cfg, "general", "nick_width", T.lay("nick_width", "12")).c_str());  // /set nick_width beats the theme's
    std::string tsfmt = tsf ? tsf : ini_get(c.cfg, "general", "timestamp", T.lay("timestamp", "%H:%M"));
    // timestamp_align = right: the time at the right edge of the first row, the text wraps short of it
    bool ts_right = ini_get(c.cfg, "general", "timestamp_align") == "right";
    Row pre, tsr;
    bool table = b->name == "*channels";  // the /list table: no time, no "--" column, the whole width for its rows
    if (!table)
        cells(ts_right ? tsr : pre, fmt_time(l.ts, tsfmt) + (ts_right ? "" : " "), T.get("timestamp"), false);

    std::string marker;
    Style ms, body = T.get("default");
    std::string txt = l.text;
    switch (l.kind) {
    case LK::MSG: {
        std::string pfx;
        if (b->srv) {  // the cmd tab has no server (and no nicks), its lines still can be msgs
            auto it = b->nicks.find(b->srv->lower(l.nick));
            if (it != b->nicks.end() && !it->second.prefix.empty()) pfx = it->second.prefix.substr(0, 1);
        }
        marker = pfx + l.nick;
        if (auto br = T.lay("nick_brackets", ""); br.size() == 2) marker = br[0] + marker + br[1];  // mirc: <nick>
        ms = l.self ? T.get("msg_self") : nick_style(T, l.nick);
        if (l.self) ms.fg = c.self_nick(T);  // own_nick_color: a color none of the others has
        if (!l.self && l.color >= 0) ms.fg = l.color;  // Twitch: the color they picked
        if (l.hilight) ms = T.get("hilight"), body = sty(T, "hilight_text", "default");
        break;
    }
    case LK::ACTION:
        marker = "*", ms = body = T.get("action");
        txt = l.nick + " " + l.text;
        if (l.hilight) ms = T.get("hilight");
        break;
    case LK::NOTICE: marker = "-" + l.nick + "-", ms = T.get("notice"); break;
    case LK::JOIN: marker = "-->", ms = body = T.get("join"); break;
    case LK::PART: marker = "<--", ms = body = T.get("part"); break;
    case LK::QUIT: marker = "<--", ms = body = sty(T, "quit", "part"); break;
    case LK::ERROR: marker = "!!", ms = body = T.get("error"); break;
    case LK::NICK: case LK::MODE: case LK::TOPIC: case LK::HOST: marker = "--", ms = body = sty(T, "event", "info"); break;  // mirc: green, like joins
    default: marker = "--", ms = body = T.get("info"); break;
    }
    if (l.badge_kind == 'f') body = fold_style(T);  // eventshidemgr's row: the theme's accent, apart from the chat
    if (auto em = T.lay("event_marker", ""); !em.empty() && l.kind != LK::MSG && l.kind != LK::NOTICE)  // mirc: "*"
        marker = l.kind == LK::INFO && !b->srv ? "" : em;  // home tab's banner and welcome: bare
    // the 🔒 (drawn, not in the text: a typed one can't pass for it) in a column of its own, opposite the time: at the
    // right edge, or with timestamp_align = right at the left, in front of the nick column (a long nick gives it 2 cells)
    Row lock;
    if (l.e2e) cells(lock, "\xf0\x9f\x94\x92", body, false);
    bool lock_left = l.e2e && ts_right && !table && nw > 2, lock_right = l.e2e && !ts_right;
    bool lock_text = l.e2e && !lock_left && !lock_right;  // nick_width 0: no column to line up with
    Row nick;
    if (!table) {
    cells(nick, marker, ms, false);
    int room = nw - (lock_left ? 2 : 0);
    if (nw > 0 && width(nick) > room) {  // nick_width 0: no column, the nick as long as it is
        while (!nick.empty() && width(nick) > room - 1) nick.pop_back();
        nick.push_back({'+', T.get("separator"), 1});
    }
    if (lock_left) pre.insert(pre.end(), lock.begin(), lock.end());
    for (int pad = room - width(nick); pad > 0; pad--) pre.push_back({' ', T.get("default"), 1});
    pre.insert(pre.end(), nick.begin(), nick.end());
    std::string sep = T.lay("separator", "│");
    if (!marker.empty() || !sep.empty()) cells(pre, sep.empty() ? " " : " " + sep + " ", T.get("separator"), false);
    }

    int tsw = tsr.empty() ? 0 : width(tsr) + 1, indent = width(pre), avail = std::max(10, width_ - indent - tsw - (lock_right ? 3 : 0));
    if (l.typed_at && !full) {  // /typing: the part typed so far, and the cursor
        size_t len = 0, n;
        for (unsigned char ch : mirc_strip(txt)) len += (ch & 0xc0) != 0x80;  // characters, not bytes
        n = typed_chars(l, len, now_ms());
        if (n < len) txt = mirc_prefix(txt, n) + "\x0f\u258C";
    }
    Row body_cells;
    if (!l.badge.empty())  // iSekur's notices (INFO), a plugin's lines (its name)
        cells(body_cells, " " + l.badge + " ", l.badge_kind == 'i' ? note_style(T) : plugin_style(T), false), cells(body_cells, " ", body, false);
    if (lock_text) cells(body_cells, "\xf0\x9f\x94\x92 ", body, false);
    size_t fx0 = body_cells.size();
    cells(body_cells, txt, body);
    linkify(body_cells, T);
    if (l.fx_at && !full) {  // e2e_effect: noise over the text's one-column glyphs; spaces and widths stay, so the wrap holds still
        long long now = now_ms();
        size_t n = body_cells.size() - fx0;
        Style ns = sty(T, "e2e", "timestamp");
        for (size_t k = 0; k < n; k++) {
            Cell& ce = body_cells[fx0 + k];
            if (ce.w != 1 || ce.cp == ' ') continue;
            if (uint32_t g = e2e_glyph(l, k, n, now)) ce.cp = g, ce.st = ns;
        }
    }

    std::vector<Row> rows;
    Row cur = pre;
    size_t i = 0;
    while (i < body_cells.size()) {
        // take as many cells as fit, break at the last space if there is one
        int w = 0;
        size_t j = i, last_space = std::string::npos;
        while (j < body_cells.size() && w + body_cells[j].w <= avail) {
            if (body_cells[j].cp == ' ') last_space = j;
            w += body_cells[j++].w;
        }
        if (j < body_cells.size() && last_space != std::string::npos && last_space > i) j = last_space + 1;
        cur.insert(cur.end(), body_cells.begin() + (long)i, body_cells.begin() + (long)j);
        if (info) info->bw.push_back(width(cur) - indent);
        rows.push_back(cur);
        cur.assign((size_t)indent, Cell{' ', T.get("default"), 1});
        i = j;
    }
    if (rows.empty()) rows.push_back(cur);
    if (info) {
        if (info->bw.empty()) info->bw.push_back(0);
        info->indent = indent, info->marker = table ? "" : marker;
        if (!table) {
            info->ts = fmt_time(l.ts, tsfmt), info->ts_right = ts_right, info->tsx = ts_right ? width_ - width(tsr) : 0;
            Row t;
            cells(t, info->ts, T.get("timestamp"), false), info->tsw = width(t);
        }
    }
    if (!tsr.empty()) {
        Row& r0 = rows[0];
        for (int w = width(r0); w < width_ - (tsw - 1); w++) r0.push_back({' ', T.get("default"), 1});
        r0.insert(r0.end(), tsr.begin(), tsr.end());
    }
    if (lock_right) {
        Row& r0 = rows[0];
        for (int w = width(r0); w < width_ - 2; w++) r0.push_back({' ', T.get("default"), 1});
        r0.insert(r0.end(), lock.begin(), lock.end());
    }
    // a plugin's link previews (isekur.embed), as the webchat's cards but text only: "▎ site · title", "▎ description",
    // a row each. Pictures and players are the webchat's alone. Untrusted text: no mIRC codes read in it
    if (!l.embeds.empty())
        for (auto& e : embed_list(l.embeds)) {
            if (e["type"] != "link" || e["title"].empty() || e["term"] == "false") continue;
            Style bar = sty(T, "embed_bar", "separator"), dim = T.get("timestamp"), tit = T.get("default");
            tit.attr |= A_BOLD;
            int room = std::max(4, avail - 2);
            auto row = [&](Row part) {
                Row r((size_t)indent, Cell{' ', T.get("default"), 1});
                cells(r, "\u258e ", bar, false);
                r.insert(r.end(), part.begin(), part.end());
                rows.push_back(r);
            };
            Row t;
            if (!e["site"].empty()) cells(t, e["site"] + " \u00b7 ", dim, false);
            cells(t, e["title"], tit, false);
            clip(t, room, tit), row(t);
            if (e["desc"].empty()) continue;
            Row d;
            cells(d, e["desc"], dim, false);
            clip(d, room, dim), row(d);
        }
    return rows;
}

// ---- drawing ---------------------------------------------------------------
// a channel's nicklist: the separator at column sx, the list right of it up to xend. Only the
// focused pane's scrolls (nl_scroll); a double click on a nick opens a query
void Tui::draw_nicks(Buffer* b, int sx, int x0, int xend, int top, int ch, bool focus) {
    const Theme& T = c.look(false);
    Style bs = sty(T, "nicklist_border", "separator");
    for (int y = top; y < top + ch; y++) text(sx, y, sx + 1, T.lay("separator", "│"), bs, false);
    std::vector<const NickEntry*> v;
    for (auto& kv : b->nicks) v.push_back(&kv.second);
    Server* s = b->srv;
    bool paint = c.nicklist_colored(b);  // a plugin (nickcolors) wants each nick in its chat color
    // the map is keyed by irc_lower(nick), so it's already in name order: only rank moves them
    std::stable_sort(v.begin(), v.end(), [&](const NickEntry* x, const NickEntry* y) {
        return s->prefix_rank(x->prefix) < s->prefix_rank(y->prefix);
    });
    int sc = 0;
    if (focus) {
        if (nl_buf != b) nl_buf = b, nl_scroll = 0, nick_sel.clear();  // the selection is this list's
        sc = nl_scroll = std::max(0, std::min(nl_scroll, (int)v.size() - (ch - 1)));
    }
    v.erase(v.begin(), v.begin() + sc);
    Style hs = sty(T, "nicklist_title", "nicklist");
    fill(top, x0, xend, hs);
    text(x0 + 1, top, xend, std::to_string(v.size() + (size_t)sc) + (v.size() + (size_t)sc == 1 ? " user" : " users") + (sc ? " ↑" : ""), hs, false);
    for (size_t i = 0; i < v.size() && (int)i < ch - 1; i++) {
        const NickEntry* e = v[i];
        if (focus) hits.push_back({top + 1 + (int)i, x0, xend, -1, e->nick});
        Style ns = e->prefix.empty() ? T.get("nicklist")
                 : e->prefix[0] == '+' ? sty(T, "nicklist_voice", "nicklist")
                                       : sty(T, "nicklist_op", "nicklist");
        if (e->away) ns.attr |= A_DIM;
        if (e->bot) ns.attr |= A_ITALIC;  // the 005 BOT mode
        bool sel = focus && std::find(nick_sel.begin(), nick_sel.end(), e->nick) != nick_sel.end();
        if (sel) ns = sty(T, "menu_sel", "tab_active");
        fill(top + 1 + (int)i, x0, xend, sel ? ns : T.get("nicklist"));
        std::string pf = e->prefix.empty() ? " " : e->prefix.substr(0, 1);
        if (paint && !sel) {  // the prefix keeps its rank color, the nick takes its own
            int nx = text(x0 + 1, top + 1 + (int)i, xend, pf, ns, false);
            if (RGB nc = c.nick_color(s, e->nick, T); nc >= 0) ns.fg = nc;  // exactly the nick column's
            text(nx, top + 1 + (int)i, xend, e->nick, ns, false);
        } else text(x0 + 1, top + 1 + (int)i, xend, pf + e->nick, ns, false);
    }
    for (int y = top + 1 + (int)v.size(); y < top + ch; y++) fill(y, x0, xend, T.get("nicklist"));
}

// eventshidemgr: the row for a run of join, part, quit and nick lines (newest first): "› 3 joined (a, b, +1), 2 left" ("v" when open),
// drawn in the theme's accent (badge_kind 'f', see fold_style) so it stands out from the chat on every theme. The marks are the two
// that are one cell wide in every monospace font (Iosevka draws ▸ ▾ ⇄ and all other arrows two cells wide, over what follows)
static Line fold_line(const std::vector<const Line*>& run, bool open, bool names) {
    int n[7] = {};
    std::vector<std::string> who[7];
    for (auto it = run.rbegin(); it != run.rend(); ++it) {
        const Line& l = **it;
        int k = fold_kind(l);  // joined, left (parts and quits), renamed, host changed, mode set, kicked, topic changed
        if (k < 0) continue;
        n[k]++;
        if (std::find(who[k].begin(), who[k].end(), l.nick) == who[k].end()) who[k].push_back(l.nick);
    }
    static const char* W[7][2] = {{"joined", "joined"}, {"left", "left"}, {"renamed", "renamed"}, {"host changed", "hosts changed"},
                                  {"mode set", "modes set"}, {"kicked", "kicked"}, {"topic changed", "topics changed"}};
    std::string t = open ? "v " : "\u203A ";
    bool first = true;
    for (int k = 0; k < 7; k++) {
        if (!n[k]) continue;
        t += (first ? "" : ", ") + std::to_string(n[k]) + " " + W[k][n[k] == 1 ? 0 : 1], first = false;
        if (!names) continue;
        t += " (";
        for (size_t i = 0; i < who[k].size() && i < 3; i++) t += (i ? ", " : "") + who[k][i];
        t += (who[k].size() > 3 ? ", +" + std::to_string(who[k].size() - 3) : "") + ")";
    }
    Line row{(open ? run.back() : run.front())->ts, LK::NICK, "", t};
    row.badge_kind = 'f';
    return row;
}

void Tui::draw_chat(Buffer* b, int x, int y, int w, int h) {
    const Theme& T = c.look(false);
    auto fc = c.fold_cfg(b);  // eventshidemgr: runs of join/part/quit lines as one row, a click opens them
    bool zhide = zen && !c.view.events;  // the focus view hides them unless it shows them
    bool hide = zhide || (b->quiet && !(fc.on && fc.quiet));  // join/part/quit lines: a quiet tab's (its summary rows stay when the plugin keeps them) and the focus view's
    bool fold = fc.on && !zhide;
    auto foldable = [&](const Line& l) { return fc.has(fold_kind(l)); };  // the kinds the plugin was asked to fold
    auto hidden = [&](const Line& l) { return noise(l) && (hide || (b->smart && l.idle) || (fold && b->quiet && !foldable(l))); };  // smart: only those of people who haven't spoken lately
    if (b->scroll)  // scrolled back: the lines that came in since push the view up by all their rows, not one
        for (auto it = b->lines.rbegin(); it != b->lines.rend() && b->scroll_new > 0; ++it, b->scroll_new--)
            if (!hidden(*it) && !(fold && foldable(*it))) b->scroll += (int)wrap(c, *it, b, w, true).size();  // ponytail: a folded line counts no rows, so a new join may shift the view by one
    b->scroll_new = 0;
    std::vector<Row> rows;  // bottom-up
    std::vector<CopyRow> meta;  // rows' copy info, y filled in when drawn
    std::vector<const Line*> lof;  // each row's line (timestamp_hover)
    std::vector<char> dim;  // the focus view: rows of the lines before the last c.view.lines (none while scrolled back)
    std::vector<long long> fk;  // a folded run's row: the run's key (its first line's number), else -1
    std::deque<Line> synth;  // the summary rows' lines, alive while this frame is drawn
    int need = h + b->scroll, nth = 0;
    long long nl = (long long)b->lines.size();
    auto put = [&](const Line& ln, int id, long long key, const Line* hov) {
        WrapInfo wi;
        auto r = wrap(c, ln, b, w, b->scroll > 0, nullptr, &wi);
        for (size_t k = r.size(); k-- > 0;) {
            CopyRow cr{0, x, x + wi.indent, 0, id, k == 0, wi.ts, wi.marker, -1, -1};
            if (k < wi.bw.size()) cr.bend = cr.bx + wi.bw[k];
            else cr.bend = x + width(r[k]), cr.id = -1;  // a link preview's row: a line of its own
            if (k == 0 && wi.tsx >= 0) cr.tsx0 = x + wi.tsx, cr.tsx1 = cr.tsx0 + wi.tsw;
            meta.push_back(cr), lof.push_back(hov ? hov : &ln), fk.push_back(key);
        }
        rows.insert(rows.end(), r.rbegin(), r.rend());
        dim.insert(dim.end(), r.size(), zen && !b->scroll && nth++ >= c.view.lines);
    };
    for (long long n = 0; n < nl && (int)rows.size() < need; n++) {
        if (b->unread && n == b->unread) {  // read marker above the new lines
            Row mk;
            std::string bar = "── new ";
            for (int k = 7; k < w; k++) bar += "─";
            Style mst = sty(T, "marker", "separator");
            if (mst.bg < 0) mst.bg = T.get("default").bg;  // the chat's own background, not the terminal's (mono-light's black marker was black on a dark terminal)
            mst.fg = readable(mst.fg, mst.bg);              // "new" is text: it reads on it
            cells(mk, bar, mst, false);
            rows.push_back(mk), meta.push_back({0, x, x, x, -2, false, "", "", -1, -1}), lof.push_back(nullptr), fk.push_back(-1), dim.push_back(zen && !b->scroll && nth >= c.view.lines);  // with the older lines it marks
        }
        const Line& ln = b->lines[(size_t)(nl - 1 - n)];
        if (hidden(ln)) continue;
        int base = 1000000 * (x + 1);  // one id per line, per pane
        if (!fold || !foldable(ln)) { put(ln, (int)n + base, -1, nullptr); continue; }
        std::vector<const Line*> run;  // this run of join/part/quit lines, newest first (ri: how far from the end each is)
        std::vector<long long> ri;
        long long last = n, n2 = n;
        for (; n2 < nl && !(b->unread && n2 == b->unread && n2 > n); n2++) {  // the read marker ends a run
            const Line& l2 = b->lines[(size_t)(nl - 1 - n2)];
            if (hidden(l2)) continue;
            if (!foldable(l2)) break;
            run.push_back(&l2), ri.push_back(n2), last = n2;
        }
        if (run.size() < (b->quiet && run.size() == 1 && noise(*run[0]) ? 1u : 2u)) {  // a lone join stays a line (in a quiet tab a hidden kind has no other way to show)
            for (size_t i = 0; i < run.size(); i++) put(*run[i], (int)ri[i] + base, -1, nullptr);
            n = n2 - 1;
            continue;
        }
        long long key = b->added - 1 - last;
        bool open = fold_open.count({b->id, key}) > 0;
        if (open)
            for (size_t i = 0; i < run.size(); i++) put(*run[i], (int)ri[i] + base, -1, nullptr);
        synth.push_back(fold_line(run, open, fc.names));
        put(synth.back(), -2, key, open ? run.back() : run.front());
        n = n2 - 1;
    }
    if ((int)rows.size() < need) b->scroll = std::max(0, (int)rows.size() - h);
    if (ts_hover && mouse_on) {  // the times show only on the line under the mouse, fading in and out
        if (hover_x >= x && hover_x < x + w && hover_y >= y && hover_y < y + h) {
            size_t idx = (size_t)(b->scroll + y + h - 1 - hover_y);
            hover_in = true, hover_set(idx < lof.size() ? lof[idx] : nullptr);
        }
        RGB dbg = T.get("default").bg;
        for (int k = 0; k < h; k++) {
            size_t idx = (size_t)(b->scroll + k);
            if (idx >= rows.size()) break;
            if (!meta[idx].first || !lof[idx] || meta[idx].tsx0 < 0) continue;
            double a = hover_alpha(lof[idx]);
            int cx = x;
            for (auto& cl : rows[idx]) {
                if (cx >= meta[idx].tsx0 && cx < meta[idx].tsx1) {
                    RGB bg = cl.st.bg >= 0 ? cl.st.bg : dbg, fg = cl.st.fg;
                    if (a < 0.05 || ((bg < 0 || fg < 0) && a < 0.3)) cl.cp = ' ';
                    else if (a < 1 && (bg < 0 || fg < 0)) { if (a < 0.7) cl.st.attr |= A_DIM; }  // the terminal's own colors: faint between
                    else if (a < 1) {
                        auto mix = [&](int sh) { return (int)(((bg >> sh) & 255) * (1 - a) + ((fg >> sh) & 255) * a) << sh; };
                        cl.st.fg = mix(16) | mix(8) | mix(0);
                    }
                }
                cx += cl.w;
            }
        }
    }
    RGB dbg = T.get("default").bg;
    for (int k = 0; k < h; k++) {
        size_t idx = (size_t)(b->scroll + k);
        if (idx >= rows.size()) break;
        if (dim[idx])  // older than the last lines: their colors 45% of the way from the background, not bold
            for (auto& cl : rows[idx]) {
                RGB bg = cl.st.bg >= 0 ? cl.st.bg : dbg, fg = cl.st.fg;
                cl.st.attr &= ~A_BOLD;
                if (bg < 0 || fg < 0) { cl.st.attr |= A_DIM; continue; }  // the terminal's own colors: all it can do
                auto mix = [&](int sh) { return (int)(((bg >> sh) & 255) * 0.55 + ((fg >> sh) & 255) * 0.45) << sh; };
                cl.st.fg = mix(16) | mix(8) | mix(0);
            }
        if (fk[idx] >= 0 && mouse_on && g_hover_y == y + h - 1 - k && g_hover_x >= x && g_hover_x < x + w) {  // the pointer is on an eventshidemgr row: its text in reverse, a pressable look (an underline is a link's)
            int cx = x;
            for (auto& cl : rows[idx]) {
                if (cx >= meta[idx].bx && cx < meta[idx].bend) cl.st.attr |= A_REV;
                cx += cl.w;
            }
        }
        draw_row(x, y + h - 1 - k, x + w, rows[idx]);
        meta[idx].y = y + h - 1 - k, crows.push_back(meta[idx]);
        if (fk[idx] >= 0) fhits.push_back({y + h - 1 - k, x, x + w, b->id, fk[idx]});
    }
}

int Tui::chat_height() const { return std::max(1, tb_height() - 2 - top_row()); }

int Tui::blink_wait() const { return g_cur_style == 2 && g_shown_x >= 0 ? BLINK_MS - (int)((now_ms() - g_cur_moved) % BLINK_MS) + 1 : -1; }
bool Tui::blink_flipped() const { return g_cur_style == 2 && g_shown_x >= 0 && cur_phase() != g_cur_on; }

void Tui::draw() {
    g_cur_x = -1;  // none wanted until a field says so
    xbtn = XBtn{};
    if (c.cfg_changed) c.cfg_changed = false, apply_cfg();
    if (c.settings_win) c.settings_win = false, sw = SetWin{}, sw.open = true, sw.to_servers = c.settings_servers, c.settings_servers = false, c.set_denied.clear();
    if (c.settings_srv.size()) sw.cat = "server." + c.settings_srv, c.settings_srv.clear();  // /servers name: that network's category
    if (c.theme_win) {
        c.theme_win = false, tw = ThemeWin{}, tw.open = true, tw.orig = c.theme.name;
        auto l = theme_list();
        tw.sel = std::max(0, (int)(std::find(l.begin(), l.end(), tw.orig) - l.begin()) % std::max(1, (int)l.size()));
    }
    if (c.keys_win) c.keys_win = false, kw = KeysWin{}, kw.open = true;
    if (c.plugin_win) c.plugin_win = false, pw = PlugWin{}, pw.open = true;
    if (c.logs_win) c.logs_win = false, logs_init();
    if (c.bans_win) c.bans_win = false, bw = BanWin{}, bw.open = true, bw.buf = c.bans_buf;
    if (c.bell) c.bell = false, tb_send("\a", 1);
    if (!c.copy_out.empty()) copy_text(c.copy_out), c.copy_out.clear();  // isekur.copy
    const Theme& T = c.look(false);
    mirc_colors = T.lay("mirc_colors", "on") != "off";  // the drawn theme's, whichever was loaded last
    Style def = T.get("default");
    g_tint = c.tint.term && c.tint.on() ? &c.tint : nullptr;
    if (c.tint.ver != g_tint_ver) g_tint_memo.clear(), g_tint_ver = c.tint.ver;
    auto [dfg, dbg] = tinted(def.fg, def.bg);
    tb_set_clear_attrs(tbc(dfg), tbc(dbg));
    tb_clear();
    isekur_links_begin(), frame_urls.clear();
    int W = tb_width(), H = tb_height();
    sync_input();  // a tab switch from outside a key (a command, the webchat, a closed tab)
    Buffer* b = c.active;
    c.dirty = false;
    {  // for isekur.crash: what this frame is about to draw
        char st[240];
        snprintf(st, sizeof st, "%dx%d theme=%s tab=%s lines=%zu scroll=%d unread=%d quiet=%d split=%d locked=%d saver=%s", W, H, T.name.c_str(),
                 !b ? "none" : b->type == BT::STATUS ? (b->srv ? "status" : "cmd") : b->type == BT::CHANNEL ? "channel" : "query",
                 b ? b->lines.size() : 0, b ? b->scroll : 0, b ? b->unread : 0, b && b->quiet, c.split_buf != nullptr, c.locked,
                 c.screen_by.empty() ? "-" : c.screen_by.c_str());
        crumb_state(st);
    }
    hits.clear(), crows.clear(), fhits.clear(), nl_x = -1;
    if (b && b->id != fold_tab) fold_tab = b->id, fold_open.clear();  // another tab: every opened run folds up again
    zen = c.view_on(false);
    if (!c.locked || c.screen_by.empty()) peek_at = 0;
    long long la = c.lock_anim ? now_ms() - c.lock_anim_at : 0;
    g_reveal = c.lock_anim == 'u' && la >= 350 ? std::min(1.0, (la - 350) / 550.0) : -1;
    g_cover = def, g_edge = sty(T, "menu_border", "separator"), g_edge.bg = def.bg;
    if (!c.screen_by.empty()) {  // a plugin's screensaver: its frame on black, over everything (the lock screen too)
        std::vector<Core::SCell> g;
        tb_set_clear_attrs(TB_DEFAULT, tbc(0)), tb_clear();
        if (c.screen_frame(W, H, "term", g))
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                    if (auto& sc = g[(size_t)y * W + x]; sc.ch) tb_set_cell(x, y, sc.ch, tbc(g_tint ? tint_color(sc.fg ? sc.fg : 0x010101, *g_tint) : sc.fg ? sc.fg : 0x010101), tbc(0));
        if (long long to = atoi(c.opt(nullptr, "lock_prompt_timeout", "30").c_str()) * 1000LL; peek_at && to > 0 && now_ms() - peek_at > to) peek_at = 0, lock_in.clear();
        if (c.locked && peek_at) draw_lock(W, H, la);  // the password box over it: Esc or its X and the screensaver goes on
        else tb_hide_cursor();
        present();
        return;
    }
    if (c.locked || (c.lock_anim == 'u' && la < 350)) {  // the lock screen: nothing of the session shows until the password
        draw_lock(W, H, la);
        present();
        return;
    }
    if (H < 6 || W < 30 || !b) {
        text(0, 0, W, "terminal too small", def, false);
        present();
        return;
    }

    // topic bar
    if (topicbar && !zen) {
        Style ts = T.get("topic");
        fill(0, 0, W, ts);
        std::string topic;
        if (b->type == BT::CHANNEL) topic = b->topic;
        else if (b->name == "*channels") topic = "Channel list: type a word to filter by name or topic, Esc clears · /list again refreshes it · /join #channel";
        else if (b->name == "*raw" && b->srv) topic = "Raw log: \xe2\x86\x90 from the server, \xe2\x86\x92 to it · /rawlog off or Ctrl+W stops it";
        else if (b->name == "*about" && !b->srv) topic = "About iSekur";
        else if (b->name == "*guide" && !b->srv) topic = "First steps guide: ← → turn the pages · /guide N jumps · q, Esc or Ctrl+W closes";
        else if (b->type == BT::QUERY) topic = "Query with " + b->name;
        else if (b->srv) {
            static const char* st[] = {"disconnected", "connecting", "registering", "connected"};
            topic = b->srv->cfg.host + ":" + std::to_string(b->srv->cfg.port) +
                    (b->srv->cfg.tls ? " (TLS)" : "") + " - " + st[(int)b->srv->state];
        } else topic = "iSekur - multi-server IRC with a built-in webchat";
        if (topic_buf != b) topic_buf = b, topic_off = 0, topic_open = false;
        Row r;
        cells(r, topic, ts);
        linkify(r, T);
        int tw = W - 2, maxh = std::max(1, (H - 4) / 2);
        if (topic_open && width(r) > tw) {  // unfolded: wrapped over as many rows as it needs
            std::vector<Row> rows(1);
            for (auto& cl : r) {
                if (width(rows.back()) + cl.w > tw) {
                    if ((int)rows.size() == maxh) break;
                    rows.emplace_back();
                }
                rows.back().push_back(cl);
            }
            topic_h = (int)rows.size();
            for (int i = 0; i < topic_h; i++) fill(i, 0, W, ts), draw_row(1, i, W - 1, rows[(size_t)i]);
        } else {
            topic_h = 1, topic_open = false;
            topic_off = std::max(0, std::min(topic_off, width(r) - tw));
            int skip = topic_off;
            size_t i = 0;
            while (i < r.size() && skip > 0) skip -= r[i++].w;
            r.erase(r.begin(), r.begin() + (long)i);
            fill(0, 0, W, ts);
            draw_row(1, 0, W - 1, r);
            if (topic_off) cell(0, 0, U'‹', ts);
            if (width(r) > tw) cell(W - 1, 0, U'›', ts);
        }
    } else topic_h = 1;

    // tab bar
    Style tbs = T.get("tabbar");
    // a collapsed server tab hides its channels and queries (the active one stays), shows "+N"
    // and takes their activity color; Ctrl+N/P, Ctrl+A and /win still go to them
    std::vector<Row> labels;
    std::vector<int> vis;  // c.bufs index of each label
    std::vector<Style> lsty;  // ...and its style
    std::vector<int> lhid;    // ...and how many tabs its collapse hides
    int ai = c.index_of(b), av = 0;  // av: the active tab among the labels
    for (size_t i = 0; i < c.bufs.size(); i++) {
        Buffer* t = c.bufs[i].get();
        if (t != b && t->type != BT::STATUS && t->srv && t->srv->status->collapsed) continue;
        int act = t->activity, hid = 0;
        if (t->type == BT::STATUS && t->collapsed)
            for (auto& o : c.bufs)
                if (o->srv == t->srv && o.get() != t && o.get() != b) hid++, act = std::max(act, o->activity);
        Style s = t->id == tab_drop ? T.get("tab_active")  // where a dragged tab would land: reversed below
                : t == b ? (t->quiet ? sty(T, "tab_quiet_act", "tab_active") : T.get("tab_active"))
                : act == 3 ? T.get("tab_hilight")
                : act == 2 ? T.get("tab_msg")
                : act == 1 ? T.get("tab_data")
                : t->quiet ? sty(T, "tab_quiet", "tabbar")
                : !t->srv ? sty(T, "tab_home", "tab_server")
                : t->type == BT::STATUS ? sty(T, "tab_server", "tabbar") : tbs;
        if (t->id == tab_drop) s.attr ^= A_REV;
        Row r;
        cells(r, " " + std::to_string(i + 1) + ":" + buf_label(t) + (c.muted(t) ? " \xf0\x9f\x94\x87" : "") + (e2e_on(t) ? " \xf0\x9f\x94\x92" : "") + (hid ? " +" + std::to_string(hid) : "") + " ", s, false);
        if (t == b) av = (int)labels.size();
        labels.push_back(r);
        vis.push_back((int)i), lsty.push_back(s), lhid.push_back(hid);
    }
    // tab_bar_position = left | right: when the chat keeps 40 columns beside it
    side_w = 0;
    std::string tbar = zen ? "" : ini_get(c.cfg, "general", "tab_bar_position");
    if (tbar == "left" || tbar == "right") {
        int sw0 = std::clamp(atoi(ini_get(c.cfg, "general", "tab_bar_width", T.lay("tab_bar_width", "20")).c_str()), 10, 60);  // /set beats the theme's
        if (W - sw0 - 1 >= 40) side_w = sw0;
    }
    bool right = side_w && tbar == "right";
    side_x = right ? W - side_w : 0, sep_x = !side_w ? -1 : right ? side_x - 1 : side_w;
    chat_x = side_w && !right ? side_w + 1 : 0, chat_end = right ? sep_x : W;
    if (zen) {  // the focus view: the chat alone, in a column in the middle
        int zw = std::min(W - 2, c.view.width);
        chat_x = (W - zw) / 2, chat_end = chat_x + zw;
    }
    // tabs wrap onto more rows, whole labels only, filled left to right. Past TAB_ROWS the last row
    // scrolls: the active tab stays in view, "‹N" / "N›" count the hidden ones on each side (colored
    // like the most urgent of them) and a click on it brings the next one in
    int nt = (int)labels.size();
    if (side_w) {  // the tree: a network's channels and queries under it, ├ └ joining them, the active one in view
        tab_h = 0;
        int sx1 = side_x + side_w, y0 = top_row(), rows = std::max(1, H - 2 - y0), nw = (int)std::to_string(c.bufs.size()).size();
        if (b->id != side_act) {
            side_act = b->id;
            if (av < side_top) side_top = av;
            if (av >= side_top + rows) side_top = av - rows + 1;
        }
        side_top = std::clamp(side_top, 0, std::max(0, nt - rows));
        Style ls = T.get("separator"), lines = sty(T, "tree_lines", "separator");
        for (int y2 = y0; y2 < y0 + rows; y2++) fill(y2, side_x, sx1, tbs), text(sep_x, y2, sep_x + 1, T.lay("separator", "│"), ls, false);
        for (int i = side_top; i < nt && i - side_top < rows; i++) {
            Buffer* t = c.bufs[(size_t)vis[(size_t)i]].get();
            int y2 = y0 + i - side_top;
            bool kid = t->srv && t->type != BT::STATUS;
            bool last = kid && (i + 1 == nt || c.bufs[(size_t)vis[(size_t)i + 1]]->srv != t->srv);
            Style s = lsty[(size_t)i];
            fill(y2, side_x, sx1, s);
            std::string num = std::to_string(vis[(size_t)i] + 1);
            int x2 = text(side_x + nw - (int)num.size() + 1, y2, sx1, num + " ", s, false);
            if (kid) {
                Style l2 = lines;
                on_row(l2, s);
                x2 = text(x2, y2, sx1, last ? "└ " : "├ ", l2, false);
            }
            Style ns = s;
            if (t->type == BT::STATUS && t->srv) ns.attr |= A_BOLD;
            int hid = lhid[(size_t)i];
            std::string m = hid ? "+" + std::to_string(hid) : "";  // collapsed: +N at the right edge
            Row nr;
            cells(nr, buf_label(t) + (c.muted(t) ? " \xf0\x9f\x94\x87" : "") + (e2e_on(t) ? " \xf0\x9f\x94\x92" : ""), ns, false);
            clip(nr, sx1 - x2 - (hid ? (int)m.size() + 1 : 0), ns);
            x2 = draw_row(x2, y2, sx1, nr);
            if (hid) text(sx1 - (int)m.size(), y2, sx1, m, s, false);
            hits.push_back({y2, side_x, sx1, vis[(size_t)i], ""});
        }
        // more above / below: the wheel scrolls; in that row's own colors, not a hole in the active tab's
        auto rsty = [&](int r) { int i = side_top + r; return i < nt ? lsty[(size_t)i] : tbs; };
        if (side_top > 0) text(sx1 - 2, y0, sx1, "↑", rsty(0), false);
        if (side_top + rows < nt) text(sx1 - 2, y0 + rows - 1, sx1, "↓", rsty(rows - 1), false);
    } else if (zen) tab_h = 0;
    else {
    const int TAB_ROWS = 3;  // ponytail: fixed cap, a setting if anyone wants more
    std::vector<int> starts{0};  // the first label of each row
    for (int i = 0, x = 0; i < nt; i++) {
        int w = width(labels[(size_t)i]);
        if (x && x + w > W) starts.push_back(i), x = 0;
        x += w;
    }
    if ((int)starts.size() > TAB_ROWS) starts.resize(TAB_ROWS);
    tab_h = (int)starts.size();
    int ty0 = top_row() - tab_h;
    auto more = [&](int ty, int from, int to, bool left, int lm) {
        int act = 0;
        for (int i = from; i < to; i++) act = std::max(act, c.bufs[(size_t)vis[(size_t)i]]->activity);
        Style ms = act == 3 ? T.get("tab_hilight") : act == 2 ? T.get("tab_msg") : act == 1 ? T.get("tab_data") : tbs;
        std::string num = std::to_string(to - from), m = left ? "‹" + num : num + "›";
        int x0 = left ? 1 : W - (int)num.size() - 2;
        text(x0, ty, W, m, ms, false);
        hits.push_back({ty, left ? 0 : x0, left ? lm : W, vis[(size_t)(left ? to - 1 : from)], ""});
    };
    for (int r = 0; r < tab_h; r++) {
        int ty = ty0 + r, from = starts[(size_t)r], to = r + 1 < tab_h ? starts[(size_t)r + 1] : nt;
        fill(ty, 0, W, tbs);
        if (from >= to) continue;
        int a = std::clamp(av, from, to - 1), total = 0, first = from, lm = 0, rm = 0;
        for (int i = from; i < to; i++) total += width(labels[(size_t)i]);
        auto pick = [&] {
            int span = 0;
            first = from;
            for (int i = from; i <= a; i++) span += width(labels[(size_t)i]);
            while (span > W - lm - rm && first < a) span -= width(labels[(size_t)first++]);
        };
        if (total > W) rm = 4, pick();
        if (first > from) lm = 4, pick();
        int last = first, tx = lm;
        for (int i = first; i < to && (i == first || tx + width(labels[(size_t)i]) <= W - rm); i++) {
            int x0 = tx;
            tx = draw_row(tx, ty, W - rm, labels[(size_t)i]);
            hits.push_back({ty, x0, tx, vis[(size_t)i], ""});
            last = i;
        }
        if (first > from) more(ty, from, first, true, lm);
        if (last < to - 1) more(ty, last + 1, to, false, lm);
    }
    }

    // the chat and its nicklist, or /split: two panes, each with its own title row (the focused one in
    // tab_active) and, for a channel with room, its own nicklist. Side by side when the window is wide
    int top = top_row(), ch = chat_height();
    int nlw0 = std::clamp(atoi(ini_get(c.cfg, "general", "nicklist_width", T.lay("nicklist_width", "18")).c_str()), 8, 40);  // /set beats the theme's
    Buffer* sb = c.split_buf && c.split_buf != b ? c.split_buf : nullptr;
    bool two = sb && ch >= 7 && !zen;
    nl_x = -1, other = Box{}, div_x = div_y = -1;
    nl_left = ini_get(c.cfg, "general", "nicklist_side") == "left";
    sel_lo = chat_x, sel_hi = chat_end;  // the chat's columns, less a lone pane's nicklist (set by draw_pane)
    int AW = chat_end - chat_x;  // the chat's width: beside the tab list, if it's at a side
    auto draw_pane = [&](Buffer* pb, Box bx, bool focus) {
        if (two) {
            Style hs = focus ? T.get("tab_active") : T.get("tabbar");
            fill(bx.y, bx.x, bx.x + bx.w, hs);
            text(bx.x + 1, bx.y, bx.x + bx.w, std::to_string(c.index_of(pb) + 1) + ":" + buf_label(pb) + (focus ? "" : "  (F6 / Alt+O / click: focus)"), hs, false);
            bx.y++, bx.h--;
        }
        int nw = nicklist && !zen && pb->type == BT::CHANNEL && bx.w >= 60 ? nlw0 : 0, cwp = bx.w - (nw ? nw + 1 : 0);
        int cx = nw && nl_left ? bx.x + nw + 1 : bx.x;  // nicklist_side = left: the list, its │, then the chat
        if (nw) {
            int sx = nl_left ? bx.x + nw : bx.x + cwp, l0 = nl_left ? bx.x : sx + 1;
            draw_nicks(pb, sx, l0, l0 + nw, bx.y, bx.h, focus);
            if (focus) nl_x = sx, nl_x0 = l0, nl_x1 = l0 + nw, (nl_left ? sel_lo : sel_hi) = nl_left ? sx + 1 : sx;
        }
        draw_chat(pb, cx, bx.y, cwp, bx.h);
    };
    hover_in = false;
    if (!two) draw_pane(b, Box{chat_x, top, AW, ch}, true);
    else {
        Box p1{chat_x, top, AW, ch}, p2 = p1;
        if (c.split_mode == 1 || (c.split_mode == 0 && AW >= 140)) {  // side by side, a separator column between
            p1.w = std::max(10, std::min(AW - 11, (AW - 1) * c.split_pm / 1000)), p2.x = chat_x + p1.w + 1, p2.w = chat_end - p2.x;
            for (int y = top; y < top + ch; y++) text(p2.x - 1, y, p2.x, T.lay("separator", "│"), T.get("separator"), false);
            div_x = p2.x - 1;
        } else p1.h = std::max(3, std::min(ch - 3, ch * c.split_pm / 1000)), p2.y = top + p1.h, p2.h = ch - p1.h, div_y = p2.y;
        other = c.split_first ? p1 : p2;
        draw_pane(b, c.split_first ? p2 : p1, true);
        draw_pane(sb, other, false);
    }
    if (!hover_in) hover_set(nullptr);  // the mouse over no chat: the last line's time fades out
    int cw = !two && nl_x >= 0 && !nl_left ? nl_x : chat_end, nlw = !two && nl_x >= 0 && !nl_left;  // the status line lines up with a lone nicklist
    if (sel_x0 >= 0) {  // the selection, reversed in place
        tb_cell* cb = tb_cell_buffer();
        for (int yy = top; yy < top + ch; yy++)
            for (int xx = sel_lo; xx < sel_hi; xx++) {
                tb_cell& cl = cb[yy * W + xx];
                if (cl.ch && in_sel(xx, yy)) tb_set_cell(xx, yy, cl.ch, cl.fg | TB_REVERSE, cl.bg);
            }
    }

    // live suggestions ("/set key", "/jo", "/help k"), Tab completes
    std::vector<Sugg> sk;
    if (suggest(sk) < 0 || sugg_sel >= (int)sk.size()) sugg_sel = -1;
    if (!sk.empty()) {
        int sz = (int)sk.size(), n = std::min(sz, std::max(1, ch / 2));
        // scrolled so the highlight stays above the "... more" row
        int off = sugg_sel >= n - 1 ? sugg_sel - n + 2 : 0;
        if (off + n >= sz) off = sz - n;
        // /set rows: key = value (the value in set_value); command rows: the name, its usage in the input's own color
        // (the separator's is for lines, it can't be read), its summary in info's
        Style ks = T.get("input"), kk = sty(T, "set_key", "info"), kv = sty(T, "set_value", "input"), ke = T.get("separator"),
              ku = sty(T, "sugg_usage", "input"), kd = sty(T, "sugg_desc", "info");
        ku.attr &= ~A_BOLD, kd.attr &= ~A_BOLD;
        for (Style* st : {&kk, &kv, &ke, &ku, &kd})
            if (st->bg < 0) st->bg = ks.bg;
        for (int i = 0; i < n; i++) {
            int yy = top + ch - n + i, idx = off + i;
            fill(yy, chat_x, chat_end, ks);
            if (i == n - 1 && idx < sz - 1) {
                text(chat_x + 1, yy, chat_end, "... " + std::to_string(sz - idx) + " more", ks, false);
                break;
            }
            const Sugg& g = sk[(size_t)idx];
            bool cmd = g.sep != " = ";
            Style rk = kk, re = cmd ? ku : ke, rv = cmd ? kd : kv;
            if (idx == sugg_sel) {
                for (Style* st : {&rk, &re, &rv}) st->attr |= A_REV;
                Style rs = ks;
                rs.attr |= A_REV;
                fill(yy, chat_x, chat_end, rs);
            }
            int x2 = text(chat_x + 1, yy, chat_end, g.word, rk, false);
            x2 = text(x2, yy, chat_end, g.sep, re, false);
            text(x2, yy, chat_end, g.val, rv, false);
        }
    }

    // status bar (none in the focus view: a blank row over the input)
    int y = H - 2, x = 0;
    if (!zen) {
        Style ss = T.get("status"), sh = sty(T, "status_hi", "status");
        sh.fg = readable(sh.fg, sh.bg >= 0 ? sh.bg : ss.bg);  // the bar's bright text reads whatever the theme (tokyonight-day: 4.0:1)
        fill(y, 0, W, ss);
        auto seg = [&](const std::string& inner, Style is) {
            is.attr |= ss.attr & A_REV;
            x = text(x, y, W, "[", ss, false);
            x = text(x, y, W, inner, is, false);
            x = text(x, y, W, "] ", ss, false);
        };
        x = text(x, y, W, " ", ss, false);
        seg(fmt_time(time(nullptr), "%H:%M"), sh);
        Server* s = b->srv;
        if (s) seg(s->nick + (s->umodes.empty() ? "" : "(+" + s->umodes + ")"), sh);
        seg(std::to_string(ai + 1) + ":" + (s && b->type != BT::STATUS ? (s->network.empty() ? s->cfg.name : s->network) + "/" : "") +
                buf_label(b) + (b->modes.empty() ? "" : "(+" + b->modes + ")"),
            sh);
        if (s && s->state == SS::CONNECTED && s->lag >= 1) {
            char lag[32];
            snprintf(lag, sizeof lag, "Lag %.1fs", s->lag);
            seg(lag, sh);
        } else if (s && s->state != SS::CONNECTED) {
            seg(s->state == SS::DISCONNECTED ? "disconnected" : "connecting", T.get("error"));
        }
        if (!b->typing.empty()) {  // +typing
            std::vector<std::string> who;
            for (auto& kv : b->typing) {
                auto it = b->nicks.find(kv.first);
                who.push_back(it != b->nicks.end() ? it->second.nick : b->type == BT::QUERY ? b->name : kv.first);
            }
            seg(typing_text(who), ss);
        }
        std::string away;  // [away: net, net]: every network you're away on (the server's 306), whichever tab this is
        for (auto& sv : c.servers)
            if (sv->away && sv->state == SS::CONNECTED) away += (away.empty() ? "" : ", ") + (sv->network.empty() ? sv->cfg.name : sv->network);
        if (!away.empty()) {
            x = text(x, y, W, "[away: ", ss, false);
            x = text(x, y, W, away, sh, false);
            x = text(x, y, W, "] ", ss, false);
        }
        if (!c.update_ver.empty()) {  // check_updates found a newer release; a click opens /about
            Style us = sh;
            us.attr |= A_BOLD;
            int x0 = x;
            seg("\u2191v" + c.update_ver, us);
            hits.push_back({y, x0, x - 1, -2, ""});
        }
        std::string acts;
        std::vector<std::pair<int, int>> act;
        for (size_t i = 0; i < c.bufs.size(); i++)
            if (c.bufs[i]->activity) act.push_back({(int)i + 1, c.bufs[i]->activity});
        if (!act.empty()) {
            x = text(x, y, W, "[Act: ", ss, false);
            for (size_t i = 0; i < act.size(); i++) {
                Style as = act[i].second == 3 ? T.get("tab_hilight") : act[i].second == 2 ? T.get("tab_msg") : T.get("tab_data");
                on_row(as, ss);
                if (i) x = text(x, y, W, ",", as, false);
                int x0 = x;
                x = text(x, y, W, std::to_string(act[i].first), as, false);
                hits.push_back({y, x0, x, act[i].first - 1, ""});
            }
            x = text(x, y, W, "] ", ss, false);
        }
        if (b->scroll) {  // -- MORE --: the hilight's colors where they read on the status bar (github-dark's hilight text
                          // is near-black, made for its pink background), else the bar's bright text; a default bar: hilight
            Style hl = T.get("hilight"), ms = sh;
            ms.bg = ss.bg, ms.attr |= A_BOLD;
            if (ss.bg < 0) ms = hl;
            else {
                ms.fg = -1;
                for (RGB cand : {hl.bg, hl.fg, sh.fg})
                    if (cand >= 0 && contrast(cand, ss.bg) >= 4.5) { ms.fg = cand; break; }
                if (ms.fg < 0) ms.fg = readable(sh.fg >= 0 ? sh.fg : hl.fg, ss.bg);
            }
            seg("-- MORE --", ms);
        }
        if (!c.flash.empty()) seg(c.flash, sh);
        {  // status_info (uptime, cpu, ...), under the nick list (right edge without one); the last ones drop when it won't fit
            Style is = sty(T, "status_info", "status_hi");
            auto parts = status_info(c, is);
            for (; !parts.empty(); parts.pop_back()) {
                Row u;
                for (auto& p2 : parts) {
                    if (!u.empty()) cells(u, " ", is, false);
                    u.insert(u.end(), p2.begin(), p2.end());
                }
                int uw = width(u), ux = nlw && cw + 2 + uw <= W ? cw + 2 : W - uw - 1;
                if (ux > x) { draw_row(ux, y, W, u); break; }
            }
        }

    }
    // nick menu, over everything but the input line
    if (!menu.items.empty()) {
        Style ms = sty(T, "menu", "nicklist"), ss2 = sty(T, "menu_sel", "tab_active"), sep = sty(T, "menu_border", "separator");
        if (sep.bg < 0) sep.bg = ms.bg;
        int mw = 0;
        for (auto& it : menu.items) mw = std::max(mw, (int)it.first.size() + 2);
        mw = std::max(mw, (int)menu.nick.size() + 2);
        int mh = (int)menu.items.size() + 1;
        menu.x = std::max(0, std::min(menu.x, W - mw)), menu.y = std::max(0, std::min(menu.y, H - 2 - mh));
        Style hs = ms;
        hs.attr |= A_BOLD;
        fill(menu.y, menu.x, menu.x + mw, hs);
        text(menu.x + 1, menu.y, menu.x + mw, menu.nick, hs, false);
        for (int i = 0; i < (int)menu.items.size(); i++) {
            int yy = menu.y + 1 + i;
            auto& it = menu.items[(size_t)i];
            Style st = i == menu.sel ? ss2 : ms;
            fill(yy, menu.x, menu.x + mw, st);
            if (it.first.empty()) for (int xx = menu.x; xx < menu.x + mw; xx++) cell(xx, yy, U'─', sep);
            else text(menu.x + 1, yy, menu.x + mw, it.first, st, false);
        }
    }

    // input line
    y = H - 1;
    Style is = T.get("input");
    fill(y, 0, W, is);
    in_px = -1;  // no input line to drag over until it's drawn below (a box, a question: none)
    if (!c.choices.empty()) {  // a plugin's question
        draw_choice(W, H);
        present();
        return;
    }
    if (tw.open) {  // /theme: the picker has the keys
        draw_themes(W, H);
        present();
        return;
    }
    if (fw.open) {  // Ctrl+G: the tab finder has the keys
        draw_find(W, H);
        present();
        return;
    }
    if (kw.open) {  // /keys, F1: the key list
        draw_keys(W, H);
        present();
        return;
    }
    if (lw.open) {  // /logs: the log browser
        draw_logs(W, H);
        present();
        return;
    }
    if (bw.open) {  // /ban, /bans: the list box
        draw_bans(W, H);
        present();
        return;
    }
    if (pw.open) {  // /plugin: the plugin window
        draw_plugins(W, H);
        present();
        return;
    }
    if (c.pwin.open) {  // a plugin's window (the radio's)
        draw_pwin(W, H);
        present();
        return;
    }
    if (pu.live) pu = PWinUi{};
    if (sw.open) {  // /set: the settings window has the keys (a question over it: perform's delete, or the lock password a guarded key asks for)
        draw_settings(W, H);
        if (c.asking(c.active))
            draw_dialog(W, H, c.pass_title.empty() ? "Password" : c.pass_title, c.pass_info, c.pass_label, c.pass_hidden, input, cur, "Enter: OK · Esc: cancel", 0, 0, 'a');
        else if (!confirm.empty())
            draw_dialog(W, H, confirm_title.empty() ? "iSekur" : confirm_title, wrap_words(confirm.substr(0, confirm.find('\n')), 44), "",
                        false, U"", 0, "y: yes · n or Esc: no", 0, 0, 'c');
        present();
        return;
    }
    if (!confirm.empty()) {  // a question, in the centered box like every prompt (the webchat's is the same)
        draw_dialog(W, H, confirm_title.empty() ? "iSekur" : confirm_title, wrap_words(confirm.substr(0, confirm.find('\n')), 44), "",
                    false, U"", 0, "y: yes · n or Esc: no", 0, 0, 'c');
        present();
        return;
    }
    if (!paste.empty()) {
        size_t n = 0;
        for (auto& l : split(paste, '\n')) n += !l.empty();
        draw_dialog(W, H, "Paste", wrap_words("Send " + std::to_string(n) + " lines to " + buf_label(b) + "?", 44), "", false, U"", 0,
                    "y: send · n or Esc: back to edit it", 0, 0, 'p');
        present();
        return;
    }
    if (c.asking(b)) {  // /e2e and /oper passwords: a dialog box
        draw_dialog(W, H, c.pass_title.empty() ? "Password" : c.pass_title, c.pass_info, c.pass_label, c.pass_hidden, input, cur, "Enter: OK · Esc: cancel", 0, 0, 'a');
        present();
        return;
    }
    int px = zen ? text(chat_x, y, W, buf_label(b) + " \u203a ", sty(T, "input_prompt", "input"), false)  // the focus view: "#chan › " under the column
                 : text(0, y, W, "[" + buf_label(b) + "] ", sty(T, "input_prompt", "input"), false);
    int avail = std::max(1, (zen ? chat_end : W) - px - 1);
    std::u32string in = shown_input();
    int curw = 0, totw = 0;
    for (size_t i = 0; i < in.size(); i++) (i < cur ? curw : totw) += cwidth(in[i]);
    totw += curw;
    // the view moves only to keep the cursor on it (a drag over the line doesn't slide it under the mouse)
    if (curw < in_off) in_off = curw;
    if (curw > in_off + avail) in_off = curw - avail;
    in_off = std::min(in_off, std::max(0, totw - avail));
    int off = in_off;
    in_px = px, in_w = avail;
    size_t s0 = std::min(isel_a, isel_b), s1 = std::max(isel_a, isel_b);
    int col = 0;
    for (size_t i = 0; i < in.size(); i++) {
        char32_t ch32 = in[i];
        int w = cwidth(ch32);
        if (col >= off && col - off + w <= avail) {
            Style is0 = is;
            if (in_selecting && i >= s0 && i < s1) is.attr |= A_REV;  // being dragged over
            if (ch32 == 3) {  // a color code: ◆ in the color its digits pick (dim until there are some)
                std::string code = "\x03";
                for (size_t j = i + 1; j < in.size() && j < i + 3 && in[j] >= U'0' && in[j] <= U'9'; j++) code += (char)in[j];
                Style d = sty(T, "timestamp", "input");
                if (code.size() > 1) mirc_walk(code + "x", is, [&](uint32_t, const Style& st) { d = is, d.fg = st.fg; });
                cell(px + col - off, y, U'◆', d);
            } else if (ch32 < 0x20) {  // other formatting codes show as reverse letters, like irssi
                Style r = is;
                r.attr |= A_REV;
                cell(px + col - off, y, ch32 == U'\n' ? U'↵' : '@' + ch32, r);
            } else cell(px + col - off, y, ch32, is);
            is = is0;
        }
        col += w;
    }
    put_cursor(px + curw - off, y);
    present();
}

// the password prompt over the middle of the screen:
//   ┌─ Lock password ────────────────┐
//   │  Type it again                 │
//   │  again: ********               │
//   │  Enter: OK · Esc: cancel       │
//   └────────────────────────────────┘
// label "": a yes/no box, the info lines and the hint, no field
void Tui::draw_dialog(int W, int H, const std::string& title, const std::vector<std::string>& info, const std::string& label,
                      bool hidden, const std::u32string& in, size_t at, const std::string& hint, int dx, int dy, char kind) {
    const Theme& T = c.look(false);
    Style bs = sty(T, "menu", "nicklist"), fs = T.get("input"), es = T.get("error"), hs = bs, ns = sty(T, "timestamp", "menu"), ls = sty(T, "menu_border", "separator");
    for (Style* st : {&es, &ns, &ls}) st->bg = bs.bg;
    hs.attr |= A_BOLD;
    int bw = std::min(W - 2, 48), x0 = (W - bw) / 2, inner = bw - 4;
    bool field = !label.empty();
    int bh = (field ? 5 : 4) + (int)info.size(), y0 = std::max(0, (H - bh) / 2 - 1);
    x0 += dx, y0 += dy;  // the lock screen's animations move it
    for (int yy = y0; yy < y0 + bh; yy++) fill(yy, x0, x0 + bw, bs);
    for (int xx = x0; xx < x0 + bw; xx++) cell(xx, y0, U'─', ls), cell(xx, y0 + bh - 1, U'─', ls);
    for (int yy = y0; yy < y0 + bh; yy++) cell(x0, yy, U'│', ls), cell(x0 + bw - 1, yy, U'│', ls);
    cell(x0, y0, U'┌', ls), cell(x0 + bw - 1, y0, U'┐', ls), cell(x0, y0 + bh - 1, U'└', ls), cell(x0 + bw - 1, y0 + bh - 1, U'┘', ls);
    if (auto cb = box_title(T, x0, x0 + bw - 1, y0, title, hs); cb.first >= 0 && kind) xbtn = {y0, cb.first, cb.second, kind};
    int yy = y0 + 1;
    for (auto& l : info)
        text(x0 + 2, yy++, x0 + 2 + inner, !l.empty() && l[0] == '!' ? l.substr(1) : l, !l.empty() && l[0] == '!' ? es : bs, false);
    if (!field) {  // a yes/no: the hint under a blank line
        text(x0 + 2, yy + 1, x0 + 2 + inner, hint, ns, false);
        tb_hide_cursor();
        return;
    }
    int fy = yy, fx = text(x0 + 2, fy, x0 + 2 + inner, label + ": ", hs, false);
    int avail = std::max(1, x0 + 2 + inner - fx);
    fill(fy, fx, fx + avail, fs);
    // a password shows 1-3 dots per character, picked as it's typed: you see each key
    // land, someone watching can't count the length
    static std::minstd_rand rng{std::random_device{}()};
    while (pass_dots.size() < in.size()) pass_dots.push_back((uint8_t)(1 + rng() % 3));
    pass_dots.resize(in.size());
    std::u32string shown;
    if (!hidden) shown = in;
    else for (uint8_t d : pass_dots) shown.append(d, U'•');
    int curw = 0;
    if (hidden) curw = (int)shown.size();  // the cursor stays at the end
    else for (size_t i = 0; i < at; i++) curw += cwidth(in[i]);
    int off = std::max(0, curw - avail + 1), col = 0;
    for (char32_t ch32 : shown) {
        int w = cwidth(ch32);
        if (col >= off && col - off + w <= avail) cell(fx + col - off, fy, ch32 < 0x20 ? U' ' : ch32, fs);
        col += w;
    }
    text(x0 + 2, fy + 2, x0 + 2 + inner, hint, ns, false);
    if (fy >= 0) put_cursor(fx + curw - off, fy);
    else tb_hide_cursor();
}

// isekur.choose: the title, the plugin's lines and its items numbered, the one under the cursor on the selection color
//   ┌─ Upload ────────────────────────┐
//   │  photo.jpg · 2.4 MB · image     │
//   │                                 │
//   │  ▶ 1  Send the link to #linux   │
//   │    2  Copy the link only        │
//   │    3  Cancel                    │
//   └─────────────────────────────────┘
void Tui::draw_choice(int W, int H) {
    const Core::Choice& q = c.choices.front();
    std::string key = q.by + "\n" + q.id + "\n" + q.title + "\n" + std::to_string(q.items.size());
    if (key != cu.key) cu.key = key, cu.sel = q.sel, cu.at = now_ms(), cu.text = from_utf8(q.value), cu.cur = cu.text.size();  // a new question: the cursor starts where the plugin says
    int n = (int)q.items.size();
    bool field = !q.input.empty();  // with a text field the digits are text: the rows are not numbered
    std::vector<std::string> info;
    for (auto& l : q.text)
        for (auto& w : wrap_words(l, 44)) info.push_back(w);
    if (!info.empty()) info.push_back("");
    size_t first = info.size();
    for (int i = 0; i < n; i++) info.push_back(std::string(i + 1 == cu.sel ? "\u25b6 " : "  ") + (field ? "" : std::to_string(i + 1) + "  ") + q.items[(size_t)i]);
    if (field) info.push_back("");  // the rows, a gap, the field
    draw_dialog(W, H, q.title, info, q.input, false, cu.text, cu.cur,
                field ? "\u2191\u2193: choose \u00b7 Enter: OK \u00b7 Esc: cancel" : "\u2191\u2193 or 1-" + std::to_string(n) + " \u00b7 Enter: OK \u00b7 Esc: cancel", 0, 0, 'q');
    const Theme& T = c.look(false);
    int bw = std::min(W - 2, 48), bh = (field ? 5 : 4) + (int)info.size();
    cu.x0 = (W - bw) / 2, cu.w = bw, cu.y0 = std::max(0, (H - bh) / 2 - 1) + 1 + (int)first;
    Style ss = sty(T, "menu_sel", "tab_active");
    int y = cu.y0 + cu.sel - 1;
    fill(y, cu.x0 + 1, cu.x0 + bw - 1, ss);
    text(cu.x0 + 2, y, cu.x0 + bw - 2, info[first + (size_t)cu.sel - 1], ss, false);
}

void Tui::choice_key(int mod, int k, uint32_t ch) {
    const Core::Choice& q = c.choices.front();
    int n = (int)q.items.size();
    bool field = !q.input.empty(), early = now_ms() - cu.at < 400;  // early: typed ahead of the box (a message being typed, a paste): not an answer
    if (mod & (TB_MOD_ALT | TB_MOD_SUPER)) return;
    if ((k == TB_KEY_ESC && !ch) || k == TB_KEY_CTRL_C) { c.choice_act(0); return; }
    c.dirty = true;
    if (early && (k == TB_KEY_ENTER || ch || k == TB_KEY_SPACE)) return;
    if (k == TB_KEY_ARROW_UP) cu.sel = (cu.sel - 2 + n) % n + 1;
    else if (k == TB_KEY_ARROW_DOWN || k == TB_KEY_TAB) cu.sel = cu.sel % n + 1;
    else if (k == TB_KEY_ENTER) c.choice_act(cu.sel, to_utf8(cu.text));
    else if (!field) {
        if (ch >= '1' && ch <= '9' && (int)(ch - '1') < n) c.choice_act((int)(ch - '0'));
    } else if (k == TB_KEY_ARROW_LEFT) cu.cur -= cu.cur > 0;
    else if (k == TB_KEY_ARROW_RIGHT) cu.cur += cu.cur < cu.text.size();
    else if (k == TB_KEY_HOME || k == TB_KEY_CTRL_A) cu.cur = 0;
    else if (k == TB_KEY_END || k == TB_KEY_CTRL_E) cu.cur = cu.text.size();
    else if ((k == TB_KEY_BACKSPACE || k == TB_KEY_BACKSPACE2) && cu.cur > 0) cu.text.erase(--cu.cur, 1);
    else if (k == TB_KEY_DELETE && cu.cur < cu.text.size()) cu.text.erase(cu.cur, 1);
    else if (k == TB_KEY_CTRL_U) cu.text.clear(), cu.cur = 0;
    else if ((ch >= 32 || k == TB_KEY_SPACE) && cu.text.size() < 300) cu.text.insert(cu.cur++, 1, ch ? (char32_t)ch : U' ');
}

void Tui::choice_mouse(int mod, int k, int x, int y) {
    const Core::Choice& q = c.choices.front();
    int n = (int)q.items.size();
    if (mod & TB_MOD_MOTION) return;
    if (k == TB_KEY_MOUSE_WHEEL_UP) cu.sel = (cu.sel - 2 + n) % n + 1;
    else if (k == TB_KEY_MOUSE_WHEEL_DOWN) cu.sel = cu.sel % n + 1;
    else if (k == TB_KEY_MOUSE_LEFT && x > cu.x0 && x < cu.x0 + cu.w - 1 && y >= cu.y0 && y < cu.y0 + n) c.choice_act(y - cu.y0 + 1, to_utf8(cu.text));
    c.dirty = true;
}

// the lock screen: a padlock over the password box. Core::lock_anim: 'l' both drop in from the top
// and the shackle snaps shut, 'f' (a wrong password) they shake and flash red, 'u' the shackle
// springs open (then present() opens the session from the middle out)
void Tui::draw_lock(int W, int H, long long ms) {
    const Theme& T = c.look(false);
    std::vector<std::string> info;
    if (!c.lock_msg.empty()) info.push_back(c.lock_msg);
    int bh = 5 + (int)info.size(), y0 = std::max(0, (H - bh) / 2 - 1);
    int dx = 0, dy = 0, lift = 0;
    bool open = c.lock_anim == 'u', red = false;
    if (c.lock_anim == 'l' && ms < 650) {
        double t = ms / 650.0, u = t - 1;
        double e = 1 + 2.70158 * u * u * u + 1.70158 * u * u;  // ease out back: past the spot a little, then back
        dy = (int)std::lround((e - 1) * (y0 + bh + 6));
        lift = t < 0.75 ? 2 : t < 0.9 ? 1 : 0;  // the shackle closes as it lands
    } else if (c.lock_anim == 'f' && ms < 450) {
        double t = ms / 450.0;
        dx = (int)std::lround(3 * std::sin(t * 6 * M_PI) * (1 - t)), red = true;
    } else if (open) lift = ms < 120 ? 1 : 2;
    draw_dialog(W, H, "iSekur is locked", info, "password", true, lock_in, lock_in.size(), peek_at ? "Enter: unlock · Esc: back to the screensaver" : c.plugin_hooks("lock", nullptr, "") ? "Enter: unlock · Esc: screensaver · Alt+D: detach" : "Enter: unlock · Alt+D: detach", dx, dy, peek_at ? 'L' : 0);
    if (y0 < 7) return;  // no room for the padlock
    Style def = T.get("default"), ps = red ? T.get("error") : open ? T.get("join") : sty(T, "menu_border", "separator");
    ps.bg = def.bg, ps.attr |= A_BOLD;
    int px = (W - 9) / 2 + dx, py = y0 + dy - 6;
    text(px + 2, py - lift, px + 7, "╭───╮", ps, false);  // the shackle, its legs down to the body
    for (int y = py - lift + 1; y <= py + 1; y++) text(px + 2, y, px + 7, open && y == py + 1 ? "│    " : "│   │", ps, false);
    text(px, py + 2, px + 9, open ? "╭─┴─────╮" : "╭─┴───┴─╮", ps, false);
    text(px, py + 3, px + 9, red ? "│   ✕   │" : open ? "│   ○   │" : "│   ●   │", ps, false);
    text(px, py + 4, px + 9, "╰───────╯", ps, false);
}

// ---- the settings window (/set alone) ----------------------------------------
//   ┌─ Settings ────────────────────────────────────────────────┐
//   │ / search: log                                             │
//   │ All          12 │ ⇢ libera · Logs                         │
//   │ Global        5 │  log          ● on  ← overrides Global  │
//   │   Logs        5 │  log_history  inherits: 200             │
//   │ ⇢ libera      5 │  log_rotate   ● on  (Global)            │
//   │     Logs      5 │                                         │
//   │ ↑↓ move · e edit · Space on/off · d default · s save · q  │
//   └───────────────────────────────────────────────────────────┘
static std::string lower(std::string s) { to_lower(s); return s; }
static bool truthy_str(const std::string& v) { return v == "on" || v == "true" || v == "yes" || v == "1"; }
static std::string full_key(const Core::SetRow& r) { return (r.sec == "general" ? "" : r.sec + ".") + r.key; }

// the categories with how many keys match the search, and the rows of the chosen one. Two levels:
// "general" (Global) over "general/Look"..., "server.x" over "server.x/Login"...; a network's topics show
// while it (or one of them) is chosen. cat: the chosen one's name ("" All), kept when the list folds
static std::string cat_parent(const std::string& c) { size_t sl = c.find('/'); return sl == std::string::npos ? "" : c.substr(0, sl); }
static int set_view(const std::vector<Core::SetRow>& all, const std::string& q, std::string& cat,
                    std::vector<std::string>& cats, std::vector<int>& counts, std::vector<const Core::SetRow*>& rows) {
    auto match = [&](const Core::SetRow& r) { return q.empty() || lower(full_key(r)).find(q) != std::string::npos || (!r.secret && lower(r.val).find(q) != std::string::npos); };
    auto in = [](const Core::SetRow& r, const std::string& c) { return c.empty() || r.cat == c || r.cat.rfind(c + "/", 0) == 0; };
    std::vector<std::string> full = {""};
    for (auto& r : all) {
        std::string p = cat_parent(r.cat);
        if (!p.empty() && std::find(full.begin(), full.end(), p) == full.end()) full.push_back(p);
        if (std::find(full.begin(), full.end(), r.cat) == full.end()) full.push_back(r.cat);
    }
    std::string open = cat_parent(cat).empty() ? cat : cat_parent(cat);  // the network whose topics show
    cats.clear(), counts.clear();
    for (auto& c : full) {
        std::string p = cat_parent(c);
        if (p.rfind("server.", 0) == 0 && p != open) continue;
        int n = 0;
        for (auto& r : all) n += match(r) && in(r, c);
        cats.push_back(c), counts.push_back(n);
    }
    int at = (int)(std::find(cats.begin(), cats.end(), cat) - cats.begin());
    if (at >= (int)cats.size() || !counts[(size_t)at]) cat.clear(), at = 0;  // gone, or a search with no hits here: All
    rows.clear();
    for (auto& r : all)
        if (match(r) && in(r, cat)) rows.push_back(&r);
    return at;
}
static std::string cat_label(const std::string& c) {
    if (c.empty()) return "All";
    if (c == "general") return "Global";
    if (c.rfind("server.", 0) == 0 && cat_parent(c).empty()) return "⇢ " + c.substr(7);
    if (!cat_parent(c).empty()) return (cat_parent(c) == "general" ? "  " : "    ") + c.substr(c.find('/') + 1);
    return c == "web" ? "Web" : c == "bnc" ? "BNC" : c == "keys" ? "Keys" : c == "aliases" ? "Aliases" : c;
}

// the server the selected key or category is about ("" for none): its keys, or a "server.x" category
static std::string set_server(const std::string& cat, const Core::SetRow* r) {
    if (r && r->sec.rfind("server.", 0) == 0) return r->sec.substr(7);
    std::string p = cat_parent(cat).empty() ? cat : cat_parent(cat);
    return p.rfind("server.", 0) == 0 ? p.substr(7) : "";
}

void Tui::draw_settings(int W, int H) {
    const Theme& T = c.look(false);
    Style bs = sty(T, "menu", "nicklist"), ls = sty(T, "menu_border", "separator"), hs = bs, dim = sty(T, "timestamp", "menu"),
          sel = sty(T, "menu_sel", "tab_active"), fs = T.get("input"), keyst = sty(T, "set_key", "info"), val = sty(T, "set_value", "input"),
          secst = sty(T, "info", "menu"), mark = sty(T, "hilight", "menu");
    for (Style* st : {&ls, &dim, &keyst, &val, &secst}) st->bg = bs.bg;
    mark.fg = sty(T, "input_prompt", "hilight").fg, mark.bg = bs.bg;
    hs.attr |= A_BOLD;
    int bw = std::min(W - 2, 110), bh = std::min(H - 2, 34);
    if (bw < 40 || bh < 10) { text(0, H - 1, W, "The terminal is too small for the settings window (Esc closes it)", T.get("error"), false); return; }
    int x0 = (W - bw) / 2, y0 = (H - bh) / 2, x1 = x0 + bw - 1, y1 = y0 + bh - 1;
    gutters(W, x0, x1 + 1, y0, y1 + 1, T.get("default"));
    for (int yy = y0; yy <= y1; yy++) fill(yy, x0, x1 + 1, bs);
    for (int xx = x0; xx <= x1; xx++) cell(xx, y0, U'─', ls), cell(xx, y1, U'─', ls);
    for (int yy = y0; yy <= y1; yy++) cell(x0, yy, U'│', ls), cell(x1, yy, U'│', ls);
    cell(x0, y0, U'┌', ls), cell(x1, y0, U'┐', ls), cell(x0, y1, U'└', ls), cell(x1, y1, U'┘', ls);
    if (auto cb = box_title(T, x0, x1, y0, "Settings", hs); cb.first >= 0) xbtn = {y0, cb.first, cb.second, 's'};

    auto all = c.setting_rows();
    std::vector<std::string> cats;
    std::vector<int> counts;
    std::vector<const Core::SetRow*> rows;
    if (sw.to_servers) {
        sw.to_servers = false;
        if (sw.cat.empty())  // else /servers name set it
            for (auto& r : all)
                if (r.sec.rfind("server.", 0) == 0) { sw.cat = r.sec; break; }
        if (sw.cat.empty()) sw.msg = "No servers yet: n adds one";
        else sw.focus = 0, sw.sel = sw.top = 0;
    }
    int ci = set_view(all, lower(to_utf8(sw.q)), sw.cat, cats, counts, rows);
    sw.sel = std::max(0, std::min(sw.sel, (int)rows.size() - 1));

    // search line: "/" starts it, letters are commands otherwise
    int sx = text(x0 + 2, y0 + 1, x1, "/ search: ", sw.searching ? keyst : dim, false);
    if (sw.searching || !sw.q.empty()) fill(y0 + 1, sx, x1 - 1, sw.searching ? fs : bs);
    text(sx, y0 + 1, x1 - 1, sw.q.empty() && !sw.searching ? "type / to search every key and value" : to_utf8(sw.q), sw.q.empty() && !sw.searching ? dim : sw.searching ? fs : val, false);
    {  // the two keys that leave the window, at the right of the search line whatever the hint below says
        std::string bk = "b: backup \u00b7 R: restore";
        int bx = x1 - 2 - (int)from_utf8(bk).size();
        if (bx > sx + (sw.searching || !sw.q.empty() ? (int)sw.q.size() + 4 : 42)) text(bx, y0 + 1, x1 - 1, bk, dim, false);
    }
    if (sw.searching) put_cursor(sx + (int)sw.q.size(), y0 + 1);
    else if (!sw.editing) tb_hide_cursor();

    // categories
    int nw = 22, ly = y0 + 3, n = bh - 6;  // n rows for the lists
    sw.x0 = x0, sw.y0 = y0, sw.w = bw, sw.h = bh, sw.nav_w = nw, sw.list_y = ly;
    if (ci < sw.ctop) sw.ctop = ci;  // the chosen category in sight
    if (ci >= sw.ctop + n) sw.ctop = ci - n + 1;
    sw.ctop = std::max(0, std::min(sw.ctop, (int)cats.size() - n));
    for (int j = 0; j < n && sw.ctop + j < (int)cats.size(); j++) {
        int i = sw.ctop + j;
        const std::string& cn = cats[(size_t)i];
        std::string name = cat_label(cn);
        bool on = i == ci, uns = false;
        for (auto& r : all) uns |= r.unsaved && (cn.empty() || r.cat == cn || r.cat.rfind(cn + "/", 0) == 0);
        Style st = on ? sel : counts[(size_t)i] ? bs : dim;
        if (on && sw.focus != 0) st = keyst, st.attr |= A_BOLD | (st.fg < 0 ? A_UNDER : 0);  // chosen, but the keys have the focus (no color: underlined)
        if (cn == "general" || (cn.rfind("server.", 0) == 0 && cat_parent(cn).empty())) st.attr |= A_BOLD;  // a level's top
        fill(ly + j, x0 + 2, x0 + 2 + nw - 1, on && sw.focus == 0 ? sel : bs);
        text(x0 + 3, ly + j, x0 + nw - 5, name, st, false);
        std::string cnt = (uns ? "\u25CF " : "") + std::to_string(counts[(size_t)i]);
        int cl = (int)cnt.size() - (uns ? 2 : 0);
        text(x0 + 2 + nw - 2 - cl, ly + j, x0 + 2 + nw, cnt, on && sw.focus == 0 ? sel : dim, false);
    }
    for (int yy = ly - 1; yy < ly + n; yy++) cell(x0 + 2 + nw, yy, U'│', ls);

    // keys: with section headers when showing All (or a search across sections)
    struct DL { const Core::SetRow* r; std::string head; };
    std::vector<DL> lines;
    std::string last;
    int sel_line = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i]->head != last) lines.push_back({nullptr, rows[i]->head}), last = rows[i]->head;
        if ((int)i == sw.sel) sel_line = (int)lines.size();
        lines.push_back({rows[i], ""});
    }
    if (sel_line < sw.top + 1) sw.top = std::max(0, sel_line - 1);  // keep its header in sight
    if (sel_line >= sw.top + n) sw.top = sel_line - n + 1;
    int kx = x0 + nw + 4, kw = 0;
    for (auto* r : rows) kw = std::max(kw, (int)r->key.size());
    kw = std::min(kw + 3, 26);
    int vx = kx + kw, vmax = x1 - 1;
    const Core::SetRow* pr = rows.empty() ? nullptr : rows[(size_t)sw.sel];
    bool plist = sw.plist && pr && pr->key == "perform";
    if (plist) {  // perform's commands, the passwords in them as ********; the last row adds one
        auto pl = perform_lines(pr->val);
        sw.psel = std::max(0, std::min(sw.psel, (int)pl.size()));
        text(kx, ly, vmax, pr->head + " \u00b7 perform: run in order once connected, before autojoin", secst, false);
        int top = std::max(0, sw.psel - (n - 3));
        for (int i = 0; i + 1 < n && top + i <= (int)pl.size(); i++) {
            int j = top + i, yy = ly + 1 + i;
            bool on = j == sw.psel, last = j == (int)pl.size();
            if (on) fill(yy, kx - 1, vmax + 1, sel);
            int ex = text(kx + 1, yy, vmax, last ? "+  " : std::to_string(j + 1) + ". ", on ? sel : dim, false);
            if (on && sw.editing) {  // typed in clear, but the password as \u2022s
                fill(yy, ex, vmax, fs);
                std::u32string shown = sw.ed;
                std::string u = to_utf8(sw.ed);
                if (size_t e, p = input_secret("", u, &e); p != std::string::npos) {
                    size_t a = from_utf8(u.substr(0, p)).size(), z = e == std::string::npos ? shown.size() : from_utf8(u.substr(0, e)).size();
                    for (size_t q = a; q < z && q < shown.size(); q++) shown[q] = U'\u2022';
                }
                int avail = vmax - ex - 1, off = std::max(0, (int)sw.ecur - avail);
                text(ex, yy, vmax, to_utf8(shown.substr((size_t)off)), fs, false);
                put_cursor(ex + (int)sw.ecur - off, yy);
                continue;
            }
            text(ex, yy, vmax, last ? (pl.empty() ? "add a command (or p: a preset for X, Q, an eggdrop, IdleRPG)" : "add a command") : perform_show(pl[(size_t)j]),
                 on ? sel : last ? dim : val, false);
        }
    }
    if (!plist && rows.empty()) text(kx, ly, vmax, "No key matches \u201c" + to_utf8(sw.q) + "\u201d (Esc clears the search)", dim, false);
    for (int i = 0; !plist && i < n && sw.top + i < (int)lines.size(); i++) {
        auto& L = lines[(size_t)(sw.top + i)];
        int yy = ly + i;
        if (!L.r) { text(kx, yy, vmax, L.head, secst, false); continue; }
        bool on = L.r == (rows.empty() ? nullptr : rows[(size_t)sw.sel]) && sw.focus == 1;
        Style ks = on ? sel : keyst;
        if (on) fill(yy, kx - 1, vmax + 1, sel);
        if (L.r->unsaved) text(kx - 1, yy, kx, "\u25CF", on ? sel : mark, false);
        text(kx + 1, yy, vx - 1, L.r->key, ks, false);
        if (on && sw.editing) {  // the value being edited, with its cursor
            fill(yy, vx, vmax, fs);
            std::u32string shown = L.r->secret ? std::u32string(sw.ed.size(), U'\u2022') : sw.ed;
            int avail = vmax - vx - 1, off = std::max(0, (int)sw.ecur - avail);
            text(vx, yy, vmax, to_utf8(shown.substr((size_t)off)), fs, false);
            put_cursor(vx + (int)sw.ecur - off, yy);
            continue;
        }
        std::string eff = L.r->val.empty() ? L.r->def : L.r->val;
        // where the value comes from: a network's own (overrides Global), Global's (inherited), the default
        bool inherits = L.r->shared && L.r->val.empty(), overrides = L.r->shared && !L.r->val.empty();
        std::string from = inherits ? "  (Global)" : overrides ? "  \u2190 overrides Global" : L.r->val.empty() ? "  (default)" : "";
        if (!L.r->over.empty()) from += "  (" + L.r->over + ")";
        if (L.r->choices.size() == 2 && (L.r->choices[0] == "on" || L.r->choices[0] == "true" || L.r->choices[0] == "yes")) {  // on/off: ● on, ○ off, and where it comes from
            bool yes = eff == L.r->choices[0];
            Style ts = on ? sel : yes && !inherits ? val : dim;
            int ex = text(vx, yy, vmax, std::string(yes ? "\u25CF " : "\u25CB ") + eff, ts, false);
            ex = text(ex, yy, vmax, from, on ? sel : dim, false);
            if (on) text(ex, yy, vmax, inherits ? "   Space: override" : overrides ? "   Space: switch · d: Global's" : "   Space: switch", sel, false);
            continue;
        }
        // the unit only after a bare number (or none yet): "3m" is already its own unit, "3m days" read wrong
        bool bare = L.r->val.find_first_not_of("0123456789") == std::string::npos;
        std::string over = (L.r->unit.empty() || !bare ? "" : " " + L.r->unit) + (L.r->over.empty() ? "" : "  (" + L.r->over + ")");
        bool cmds = L.r->key == "perform" || L.r->sec == "aliases";  // commands: their passwords as ********
        std::string v = !L.r->val.empty() ? (L.r->secret ? "********" : cmds ? perform_show(L.r->val) : L.r->val) + (overrides ? "  \u2190 overrides Global" : "") + over
                        : L.r->secret || L.r->def.empty() ? "(not set)" + over
                                                          : (inherits ? "inherits: " : "default: ") + L.r->def + over;
        int ex = text(vx, yy, vmax, v, on ? sel : L.r->val.empty() ? dim : val, false);
        if (on && !L.r->choices.empty()) {  // the themes: where this one is among them
            std::string list;
            for (auto& ch : L.r->choices) list += (list.empty() ? "" : " ") + (ch == eff ? "[" + ch + "]" : ch);
            text(ex, yy, vmax, "   Space: next  " + list, sel, false);
        }
    }
    // what happened, or what the keys do
    const Core::SetRow* hr = rows.empty() ? nullptr : rows[(size_t)std::max(0, std::min(sw.sel, (int)rows.size() - 1))];
    bool on_srv = !set_server(sw.cat, sw.focus == 1 ? hr : nullptr).empty();
    std::string hint = plist && sw.editing ? "Enter: keep it · Esc: cancel · $nick: your nick · /msg x@channels.undernet.org login user pass"
                       : plist ? "\u2191\u2193: move · Enter e: edit · a: add · d: delete · [ ]: up/down · p: preset · s: save · Esc \u2190: back"
                       : sw.editing     ? "Enter: set it · Esc: cancel · empty = the default" + (hr && !hr->unit.empty() ? " · in " + hr->unit : "")
                       : hr && hr->key == "perform" && sw.focus == 1 ? "Enter: its commands · p: a preset (X, Q, eggdrop, IdleRPG) · s: save · q: close"
                       : sw.searching ? "type to search · Enter or \u2193: to the list · Esc: clear it"
                       : on_srv ? "c: connect · l: login · r: rename · x: delete · n: new server · e Enter: edit · s: save · q: close"
                       : sw.focus == 0 ? "\u2191\u2193: category · \u2192 Tab Enter: its keys · n: new server · /: search · q Esc: close"
                                       : "\u2191\u2193: move · e Enter: edit · Space: on/off, next choice · d: default · /: search · s: save · q: close";
    if (!c.set_denied.empty()) sw.msg = c.set_denied + " (unchanged)", c.set_denied.clear();  // a guarded /set the lock password refused: the line said so under the window
    text(x0 + 2, y1 - 1, x1 - 1, sw.msg.empty() ? hint : sw.msg, sw.msg.empty() ? dim : mark, false);
}

// the ways to log in as a menu: each item runs cmd + " " + method (/login sasl, /server host -login sasl), the last one is no login
static std::vector<std::pair<std::string, std::string>> login_items(const std::string& cmd, const std::string& none_label, const std::string& none_cmd) {
    return {{"Account (SASL)", cmd + " sasl"}, {"NickServ password", cmd + " nickserv"}, {"Certificate (CertFP)", cmd + " cert"},
            {"Server password (bouncer)", cmd + " password"}, {"", ""}, {none_label, none_cmd}};
}

void Tui::settings_key(int mod, int k, uint32_t ch) {
    auto all = c.setting_rows();
    std::vector<std::string> cats;
    std::vector<int> counts;
    std::vector<const Core::SetRow*> rows;
    int ci = set_view(all, lower(to_utf8(sw.q)), sw.cat, cats, counts, rows);
    const Core::SetRow* r = rows.empty() ? nullptr : rows[(size_t)std::max(0, std::min(sw.sel, (int)rows.size() - 1))];
    bool bs = k == TB_KEY_BACKSPACE || k == TB_KEY_BACKSPACE2;
    if (sw.plist && !(r && r->key == "perform")) sw.plist = false;
    // perform's list: every change is one /set of the whole list (s saves it, like the rest here)
    auto pl = sw.plist ? perform_lines(r->val) : std::vector<std::string>{};
    auto pset = [&](const std::vector<std::string>& v, const std::string& said) {
        std::string j = perform_join(v);
        c.exec(c.active, "/set " + full_key(*r) + " " + (j.empty() ? "\"\"" : j));
        sw.msg = said + "  (not saved yet: s)";
    };
    if (sw.plist && sw.editing) {
        if (k == TB_KEY_ESC) { sw.editing = false; return; }
        if (k == TB_KEY_ENTER) {
            std::string v = trim(to_utf8(sw.ed));
            sw.editing = false;
            if (v.empty()) return;
            if (v[0] != '/') v = "/" + v;
            if (v.find("&&") != std::string::npos) { sw.msg = "One command per row, without && in it"; return; }
            bool add = sw.psel >= (int)pl.size();
            if (add) pl.push_back(v), sw.psel = (int)pl.size() - 1;
            else pl[(size_t)sw.psel] = v;
            pset(pl, (add ? "Added " : "Changed ") + perform_show(v));
            return;
        }
    }
    if (sw.plist && !sw.editing) {
        sw.msg.clear();
        int np = (int)pl.size();
        if (k == TB_KEY_ESC || k == TB_KEY_ARROW_LEFT || ch == 'q') { sw.plist = false; return; }
        if (k == TB_KEY_ARROW_UP) sw.psel = std::max(0, sw.psel - 1);
        else if (k == TB_KEY_ARROW_DOWN) sw.psel = std::min(np, sw.psel + 1);
        else if (k == TB_KEY_HOME) sw.psel = 0;
        else if (k == TB_KEY_END) sw.psel = np;
        else if (ch == 'a' || ((k == TB_KEY_ENTER || ch == 'e') && sw.psel >= np)) sw.psel = np, sw.editing = true, sw.ed.clear(), sw.ecur = 0;
        else if ((k == TB_KEY_ENTER || ch == 'e') && sw.psel < np) sw.editing = true, sw.ed = from_utf8(pl[(size_t)sw.psel]), sw.ecur = sw.ed.size();
        else if ((ch == 'd' || k == TB_KEY_DELETE) && sw.psel < np) {  // asks first: y/n over the window
            std::string gone = perform_show(pl[(size_t)sw.psel]);
            pl.erase(pl.begin() + sw.psel);
            std::string j = perform_join(pl);
            confirm = "Delete command " + std::to_string(sw.psel + 1) + " (" + gone + ") from " + r->sec.substr(7) +
                      "'s on-connect commands?\n/set " + full_key(*r) + " " + (j.empty() ? "\"\"" : j);
            confirm_title = "Delete command";
        } else if ((ch == '[' || ch == ']') && sw.psel < np) {
            int to = sw.psel + (ch == '[' ? -1 : 1);
            if (to < 0 || to >= np) return;
            std::swap(pl[(size_t)sw.psel], pl[(size_t)to]);
            sw.psel = to;
            pset(pl, "Moved to " + std::to_string(to + 1));
        } else if (ch == 's' || k == TB_KEY_CTRL_S) c.exec(c.active, "/save"), sw.msg = "Saved to " + c.cfg_path;
        else if (ch == 'p') settings_preset(r->sec.substr(7));
        return;
    }
    if (sw.editing) {
        if (k == TB_KEY_ESC) { sw.editing = false; return; }
        if (k == TB_KEY_ENTER && r) {
            std::string v = to_utf8(sw.ed);
            if (std::string m = v.empty() ? "" : bad_value(r->sec, r->key, v); !m.empty()) { sw.msg = m; return; }  // still editing: fix it or Esc
            c.exec(c.active, "/set " + full_key(*r) + " " + (v.empty() ? "\"\"" : v));
            sw.editing = false;
            sw.msg = r->key == "name" && r->sec.rfind("server.", 0) == 0 ? (v.empty() ? "A server needs a name" : "Renamed to " + v + " (saved)")
                     : full_key(*r) + (v.empty() ? (r->shared ? " uses Global's again" : " is back to its default") : " = " + (r->secret ? std::string("********") : v)) + "  (not saved yet: s)";
            return;
        }
        if (ch || k == TB_KEY_SPACE) sw.ed.insert(sw.ecur++, 1, ch ? (char32_t)ch : U' ');
        else if (bs && sw.ecur) sw.ed.erase(--sw.ecur, 1);
        else if (k == TB_KEY_DELETE && sw.ecur < sw.ed.size()) sw.ed.erase(sw.ecur, 1);
        else if (k == TB_KEY_ARROW_LEFT && sw.ecur) sw.ecur--;
        else if (k == TB_KEY_ARROW_RIGHT && sw.ecur < sw.ed.size()) sw.ecur++;
        else if (k == TB_KEY_HOME) sw.ecur = 0;
        else if (k == TB_KEY_END || k == TB_KEY_CTRL_E) sw.ecur = sw.ed.size();
        else if (k == TB_KEY_CTRL_U) sw.ed.erase(0, sw.ecur), sw.ecur = 0;
        return;
    }
    sw.msg.clear();
    int page = std::max(1, sw.h - 7);
    if (mod & (TB_MOD_ALT | TB_MOD_SUPER)) return;
    if (sw.searching) {  // after "/": the keys go to the search
        if (k == TB_KEY_ESC) sw.q.clear(), sw.searching = false;
        else if (k == TB_KEY_ENTER || k == TB_KEY_ARROW_DOWN || k == TB_KEY_TAB) sw.searching = false, sw.focus = 1;
        else if (bs) { if (!sw.q.empty()) sw.q.pop_back(); else sw.searching = false; }
        else if (ch || k == TB_KEY_SPACE) sw.q += ch ? (char32_t)ch : U' ';
        sw.sel = sw.top = 0;
        return;
    }
    if (k == TB_KEY_ESC) { if (!sw.q.empty()) sw.q.clear(); else sw.open = false; return; }
    if (ch == 'q') { sw.open = false; return; }
    if (ch == '/') { sw.searching = true; return; }
    if (k == TB_KEY_CTRL_S || ch == 's') {
        c.exec(c.active, "/save");
        sw.msg = "Saved to " + c.cfg_path;
        return;
    }
    if (ch == 'b' || ch == 'R') {  // the passphrase boxes open over the chat: the window gives way, as for a new server
        sw.open = false;
        c.exec(c.active, ch == 'b' ? "/backup" : "/restore");
        return;
    }
    // servers, like the webchat's Servers window: c connects the selected one, x deletes it (asks first), n adds one
    std::string srv = set_server(sw.cat, r);
    if (ch == 'c' && !srv.empty()) { c.exec(c.active, "/connect " + srv); sw.msg = "Connecting " + srv; return; }
    if (ch == 'r' && !srv.empty()) { sw.open = false; c.exec(c.active, "/server rename " + srv); return; }
    if (ch == 'x' && !srv.empty()) {
        sw.open = false;
        confirm = "Delete " + srv + ": disconnect, close its tabs and remove it from the config?\n/server del " + srv, confirm_title = "Delete server";
        return;
    }
    if (ch == 'n') {
        sw.open = false;
        // the host line, then how to log in: the login is asked before it connects (a password goes out with the connection).
        // It is kept in the config (-save), as the webchat's Servers window does; "save only" adds it without connecting
        // and comes back to /servers on it
        c.ask_pass(c.active, "server", [this](const std::string& v) {
            std::string nm = new_server_name(v), add = "/server " + v + " -save";
            if (nm.empty()) return;
            auto items = login_items(add + " -login", "No login: save and connect", add);
            items.push_back({"No login: save only", add + " -noconnect\n/servers " + nm});
            menu = Menu{std::max(0, tb_width() / 2 - 14), std::max(0, tb_height() / 2 - 4), 0, "Add " + nm + "?", items};
        }, false, "New server", {"host [port] [-tls] [-noverify] [-name name]", "[-nick nick] [-host vhost] [-noconnect]", "e.g. irc.sekurnet.org 6697 -tls"});  // the box is 44 wide: a longer line is cut
        return;
    }
    if (ch == 'l' && !srv.empty()) { settings_login(srv); return; }
    if (ch == 'p' && !srv.empty()) { settings_preset(srv); return; }
    if (ch == 'e') k = TB_KEY_ENTER, ch = 0;
    if (ch == 'd') k = TB_KEY_DELETE, ch = 0;
    if ((k == TB_KEY_SPACE || ch == ' ') && sw.focus == 1 && r && !r->choices.empty()) {  // on <-> off, the next theme: right away
        std::string eff = r->val.empty() ? r->def : r->val;
        auto it = std::find(r->choices.begin(), r->choices.end(), eff);
        std::string next = it == r->choices.end() || it + 1 == r->choices.end() ? r->choices[0] : *(it + 1);
        c.exec(c.active, "/set " + full_key(*r) + " " + next);
        sw.msg = full_key(*r) + " = " + next + "  (not saved yet: s)";
        return;
    }
    if (ch || k == TB_KEY_SPACE) return;  // other letters do nothing here
    if (bs) return;
    if (k == TB_KEY_TAB) { sw.focus = !sw.focus; return; }
    if (k == TB_KEY_ARROW_LEFT) { sw.focus = 0; return; }
    if (k == TB_KEY_ARROW_RIGHT) { sw.focus = 1; return; }
    if (sw.focus == 0) {  // categories: the empty ones are skipped
        int d = k == TB_KEY_ARROW_DOWN ? 1 : k == TB_KEY_ARROW_UP ? -1 : 0, nc = (int)cats.size();
        for (int i = ci + d; d && i >= 0 && i < nc; i += d)
            if (counts[(size_t)i]) { sw.cat = cats[(size_t)i], sw.sel = sw.top = 0; break; }
        if (k == TB_KEY_ENTER) sw.focus = 1;
        return;
    }
    int n = (int)rows.size();
    if (k == TB_KEY_ARROW_DOWN) sw.sel = std::min(n - 1, sw.sel + 1);
    else if (k == TB_KEY_ARROW_UP) sw.sel = std::max(0, sw.sel - 1);
    else if (k == TB_KEY_PGDN) sw.sel = std::min(n - 1, sw.sel + page);
    else if (k == TB_KEY_PGUP) sw.sel = std::max(0, sw.sel - page);
    else if (k == TB_KEY_HOME) sw.sel = 0;
    else if (k == TB_KEY_END) sw.sel = std::max(0, n - 1);
    else if (k == TB_KEY_ENTER && r && r->key == "perform") sw.plist = true, sw.psel = 0;
    else if (k == TB_KEY_ENTER && r) sw.editing = true, sw.ed = r->secret ? U"" : from_utf8(r->val), sw.ecur = sw.ed.size();
    else if (k == TB_KEY_DELETE && r && r->key == "name" && r->sec.rfind("server.", 0) == 0) sw.msg = "A server needs a name: r renames it, x deletes it";
    else if (k == TB_KEY_DELETE && r) {
        c.exec(c.active, "/set " + full_key(*r) + " \"\"");
        sw.msg = full_key(*r) + (r->shared ? " uses Global's again" : " is back to its default") + "  (not saved yet: s)";
    }
}

// l on a network: how it logs in, in a menu, in that network's tab (/login's dialogs ask there)
void Tui::settings_login(const std::string& srv) {
    for (auto& sv : c.servers)
        if (sv->cfg.name == srv) {
            sw.open = false;
            c.select(sv->status);
            menu = Menu{std::max(0, tb_width() / 2 - 14), std::max(0, tb_height() / 2 - 4), 0, srv + " login", login_items("/login", "No login: clear it", "/login none")};
            return;
        }
}

// p on a network: the presets in a menu, in that network's tab (the dialogs that follow ask there)
void Tui::settings_preset(const std::string& srv) {
    for (auto& sv : c.servers)
        if (sv->cfg.name == srv) {
            sw.open = false;
            c.select(sv->status);
            menu = Menu{std::max(0, tb_width() / 2 - 14), std::max(0, tb_height() / 2 - 4), 0, srv + " on connect",
                        {{"Undernet: log in to X, +x", "/perform preset undernet"}, {"QuakeNet: log in to Q, +x", "/perform preset quakenet"},
                         {"Eggdrop: ident to the bot", "/perform preset eggdrop"}, {"IdleRPG: log in to the bot", "/perform preset idlerpg"}}};
            return;
        }
}

// ---- the theme picker (/theme alone) -------------------------------------------
//   ┌─ Theme ────────────────────────────────┐
//   │ filter: mi                             │
//   │ ▸ mirc                                 │
//   │   mirc-dark                            │
//   │ ↑↓ preview · Enter keep · Esc back     │
//   └────────────────────────────────────────┘
std::vector<std::string> Tui::theme_list() const {
    std::string q = lower(to_utf8(tw.q));
    std::vector<std::string> out;
    for (auto& n : theme_names())
        if (lower(n).find(q) != std::string::npos) out.push_back(n);
    return out;
}

void Tui::draw_themes(int W, int H) {
    const Theme& T = c.look(false);
    Style bs = sty(T, "menu", "nicklist"), ls = sty(T, "menu_border", "separator"), hs = bs, dim = sty(T, "timestamp", "menu"),
          sel = sty(T, "menu_sel", "tab_active"), fs = T.get("input");
    for (Style* st : {&ls, &dim}) st->bg = bs.bg;
    hs.attr |= A_BOLD;
    auto l = theme_list();
    int n = (int)l.size();
    tw.rows = std::max(1, std::min({std::max(n, 1), H - 6, 14}));
    tw.w = std::min(W - 2, 56), tw.x0 = (W - tw.w) / 2;
    int bh = tw.rows + 4, y0 = std::max(0, (H - bh) / 2 - 1), x1 = tw.x0 + tw.w;
    tw.list_y = y0 + 2;
    if (tw.sel >= n) tw.sel = std::max(0, n - 1);
    if (tw.sel < tw.top) tw.top = tw.sel;
    if (tw.sel >= tw.top + tw.rows) tw.top = tw.sel - tw.rows + 1;
    for (int yy = y0; yy < y0 + bh; yy++) fill(yy, tw.x0, x1, bs);
    for (int xx = tw.x0; xx < x1; xx++) cell(xx, y0, U'─', ls), cell(xx, y0 + bh - 1, U'─', ls);
    for (int yy = y0; yy < y0 + bh; yy++) cell(tw.x0, yy, U'│', ls), cell(x1 - 1, yy, U'│', ls);
    cell(tw.x0, y0, U'┌', ls), cell(x1 - 1, y0, U'┐', ls), cell(tw.x0, y0 + bh - 1, U'└', ls), cell(x1 - 1, y0 + bh - 1, U'┘', ls);
    if (auto cb = box_title(T, tw.x0, x1 - 1, y0, "Theme", hs); cb.first >= 0) xbtn = {y0, cb.first, cb.second, 't'};
    int fx = text(tw.x0 + 2, y0 + 1, x1 - 2, "filter: ", hs, false);
    fill(y0 + 1, fx, x1 - 2, fs);
    int ex = text(fx, y0 + 1, x1 - 2, to_utf8(tw.q), fs, false);
    put_cursor(ex, y0 + 1);
    if (!n) text(tw.x0 + 2, tw.list_y, x1 - 2, "no theme matches", dim, false);
    for (int i = 0; i < tw.rows && tw.top + i < n; i++) {
        const std::string& name = l[(size_t)(tw.top + i)];
        bool on = tw.top + i == tw.sel;
        Style st = on ? sel : bs;
        fill(tw.list_y + i, tw.x0 + 1, x1 - 1, st);
        int nx = text(tw.x0 + 2, tw.list_y + i, x1 - 2, (on ? "▸ " : "  ") + name, st, false);
        if (name == tw.orig) { Style d = dim; on_row(d, st); text(nx + 1, tw.list_y + i, x1 - 2, "(was)", d, false); }
    }
    text(tw.x0 + 2, y0 + bh - 2, x1 - 2, "↑↓ preview · type: filter · Enter keep · Esc back", dim, false);
}

void Tui::theme_key(int k, uint32_t ch) {
    auto l = theme_list();
    int n = (int)l.size();
    bool bs = k == TB_KEY_BACKSPACE || k == TB_KEY_BACKSPACE2;
    if (k == TB_KEY_ESC) {  // the filter first, then back to the theme it had
        if (!tw.q.empty()) { tw.q.clear(); l = theme_list(); tw.sel = std::max(0, (int)(std::find(l.begin(), l.end(), c.theme.name) - l.begin())); return; }
        std::string err;
        if (c.theme.name != tw.orig) c.use_theme(tw.orig, err);
        tw.open = false;
        return;
    }
    if (k == TB_KEY_ENTER) {
        tw.open = false;
        if (c.theme.name != tw.orig) c.exec(c.active, "/set theme " + c.theme.name);  // /save keeps it
        return;
    }
    if (ch || k == TB_KEY_SPACE) tw.q += ch ? (char32_t)ch : U' ', tw.sel = tw.top = 0;
    else if (bs) { if (tw.q.empty()) return; tw.q.pop_back(), tw.sel = tw.top = 0; }
    else if (k == TB_KEY_ARROW_DOWN) tw.sel = std::min(n - 1, tw.sel + 1);
    else if (k == TB_KEY_ARROW_UP) tw.sel = std::max(0, tw.sel - 1);
    else if (k == TB_KEY_PGDN) tw.sel = std::min(n - 1, tw.sel + tw.rows);
    else if (k == TB_KEY_PGUP) tw.sel = std::max(0, tw.sel - tw.rows);
    else if (k == TB_KEY_HOME) tw.sel = 0;
    else if (k == TB_KEY_END) tw.sel = n - 1;
    else return;
    l = theme_list(), n = (int)l.size();
    tw.sel = std::max(0, std::min(tw.sel, n - 1));
    std::string err;
    if (n && l[(size_t)tw.sel] != c.theme.name) c.use_theme(l[(size_t)tw.sel], err);  // the live preview
}

// /keys: the key list. A row is a group header (desc empty) or the keys of one action ("M-Right C-n") and what it does
struct KeyRow { std::string head, specs, desc; };
static std::vector<KeyRow> key_rows(const std::map<std::string, std::string>& keymap) {
    static const std::vector<std::pair<const char*, std::vector<std::pair<const char*, const char*>>>> GROUPS = {
        {"Tabs", {{"next_tab", "next tab"}, {"prev_tab", "previous tab"}, {"next_active", "the busiest tab"}, {"mark_all_read", "mark every tab read"},
                  {"next_server", "next server or console tab"}, {"prev_server", "previous server or console tab"}, {"find_tab", "find a tab by name"},
                  {"other_pane", "the other /split pane"}, {"close_tab", "close the tab"}}},
        {"View", {{"scroll_up", "scroll up half a page"}, {"scroll_down", "scroll down half a page"}, {"toggle_nicklist", "show or hide the nick list"},
                  {"toggle_topic", "show or hide the topic bar"}, {"toggle_mouse", "mouse on / off"}, {"keys", "this list"}}},
        {"Session", {{"lock", "lock the screen now (/lock)"}, {"detach", "detach (stays connected)"}, {"quit", "exit iSekur (asks first)"}}},
    };
    std::map<std::string, std::string> by;  // action -> its keys
    for (auto& [sp, act] : keymap) by[act] += (by[act].empty() ? "" : " ") + sp;
    std::vector<KeyRow> out;
    for (auto& [g, acts] : GROUPS) {
        out.push_back({g, "", ""});
        if (std::string(g) == "Tabs") {  // Alt+1..0 in one row; a tab_N bound elsewhere gets its own
            out.push_back({"", "M-1…0", "go to tab 1 to 10"});
            for (int i = 1; i <= 10; i++) {
                std::string a = "tab_" + std::to_string(i), own;
                for (auto& sp : split(by[a], ' ')) if (sp != "M-" + std::to_string(i % 10)) own += (own.empty() ? "" : " ") + sp;
                if (!own.empty()) out.push_back({"", own, "go to tab " + std::to_string(i)});
            }
        }
        for (auto& [a, d] : acts)
            if (!by[a].empty()) out.push_back({"", by[a], d});
    }
    out.push_back({"Mouse", "", ""});  // not keymap actions: fixed
    out.push_back({"", "Double-click C-Click M-Click", "open a link (copied over SSH)"});
    out.push_back({"", "Right-click", "a link: Open, Copy URL; a tab, a nick: menu"});
    out.push_back({"", "Drag", "copy the text"});
    out.push_back({"Typing", "", ""});
    for (auto& r : std::vector<KeyRow>{
             {"", "Enter", "send the line"}, {"", "Tab", "complete nick or command"}, {"", "Up Down", "earlier lines you typed"},
             {"", "Home End", "start / end of the line"}, {"", "C-u C-k", "delete to start / end"}, {"", "M-BS", "delete the previous word"},
             {"", by["copy_input"], "copy the line (or drag over it)"},
             {"", "C-b C-c C-o", "bold, color, reset"}, {"", "C-v C-] C-_", "reverse, italic, underline"}})
        if (!r.specs.empty()) out.push_back(r);  // copy_input unbound in [keys]: no row
    return out;
}

// "M-Right" -> {"Alt", "→"}: the keycaps of one key
static std::vector<std::string> key_caps(const std::string& sp) {
    static const std::map<std::string, std::string> NAMES = {{"Right", "→"}, {"Left", "←"}, {"Up", "↑"}, {"Down", "↓"}, {"BS", "⌫"}, {"Esc", "Esc"}};
    std::vector<std::string> caps;
    size_t i = 0;
    for (; i + 1 < sp.size() && sp[i + 1] == '-' && sp.size() > i + 2; i += 2)
        caps.push_back(sp[i] == 'M' ? "Alt" : sp[i] == 'C' ? "Ctrl" : sp[i] == 'S' ? "Shift" : sp[i] == 'D' ? "Cmd" : std::string(1, sp[i]));
    std::string k = sp.substr(i);
    auto n = NAMES.find(k);
    caps.push_back(n != NAMES.end() ? n->second : k.size() == 1 ? std::string(1, (char)toupper((unsigned char)k[0])) : k);
    return caps;
}

void Tui::draw_keys(int W, int H) {
    const Theme& T = c.look(false);
    Style bs = sty(T, "menu", "nicklist"), ls = sty(T, "menu_border", "separator"), hs = bs, dim = sty(T, "timestamp", "menu"),
          cap = sty(T, "menu_sel", "tab_active"), grp = sty(T, "hilight", "menu");
    for (Style* st : {&ls, &dim, &grp}) st->bg = bs.bg;
    grp.fg = readable(grp.fg, bs.bg), dim.fg = readable(dim.fg, bs.bg);
    hs.attr |= A_BOLD, grp.attr |= A_BOLD, cap.attr |= A_BOLD;
    auto rows = key_rows(keymap);
    kw.n = (int)rows.size();
    kw.rows = std::max(1, std::min(kw.n, H - 5));
    auto caps_w = [&](const std::string& sp, bool first) {  // " Alt "+" → ", " / " before an alternative
        int w = first ? -1 : 2;
        for (auto& k : key_caps(sp)) { Row rr; cells(rr, k, cap, false); w += width(rr) + 3; }
        return w;
    };
    int need = 0, dw = 0;  // the widest keycaps and description, for the box's width
    for (auto& r : rows) {
        int w = 0;
        bool first = true;
        for (auto& sp : split(r.specs, ' ')) w += caps_w(sp, first), first = false;
        need = std::max(need, w), dw = std::max(dw, (int)from_utf8(r.desc).size());
    }
    kw.w = std::min(W - 2, need + dw + 7), kw.x0 = (W - kw.w) / 2;
    kw.top = std::max(0, std::min(kw.top, kw.n - kw.rows));
    int bh = kw.rows + 3, y0 = std::max(0, (H - bh) / 2 - 1), x1 = kw.x0 + kw.w, capx = kw.x0 + 3,
        descx = capx + std::min(need, std::max(12, kw.w - 5 - dw)) + 2;
    kw.list_y = y0 + 1;
    gutters(W, kw.x0, x1, y0, y0 + bh, T.get("default"));
    for (int yy = y0; yy < y0 + bh; yy++) fill(yy, kw.x0, x1, bs);
    for (int xx = kw.x0; xx < x1; xx++) cell(xx, y0, U'─', ls), cell(xx, y0 + bh - 1, U'─', ls);
    for (int yy = y0; yy < y0 + bh; yy++) cell(kw.x0, yy, U'│', ls), cell(x1 - 1, yy, U'│', ls);
    cell(kw.x0, y0, U'┌', ls), cell(x1 - 1, y0, U'┐', ls), cell(kw.x0, y0 + bh - 1, U'└', ls), cell(x1 - 1, y0 + bh - 1, U'┘', ls);
    if (auto cb = box_title(T, kw.x0, x1 - 1, y0, "Keys", hs); cb.first >= 0) xbtn = {y0, cb.first, cb.second, 'k'};
    for (int i = 0; i < kw.rows && kw.top + i < kw.n; i++) {
        auto& r = rows[(size_t)(kw.top + i)];
        int y = kw.list_y + i;
        if (r.specs.empty()) {  // a group: its name and a rule to the edge
            int ex = text(kw.x0 + 2, y, x1 - 2, r.head, grp, false);
            for (int xx = ex + 1; xx < x1 - 2; xx++) cell(xx, y, U'─', ls);
            continue;
        }
        // the keycaps: " Alt "+" → ", alternatives apart; what doesn't fit before the description is left out
        int x = capx;
        bool first = true;
        for (auto& sp : split(r.specs, ' ')) {
            auto caps = key_caps(sp);
            if (x + caps_w(sp, first) + (first ? 1 : 0) > descx - 1) break;
            if (!first) x = text(x, y, descx, " / ", dim, false);
            for (size_t j = 0; j < caps.size(); j++) {
                if (j) x = text(x, y, descx, "+", dim, false);
                x = text(x, y, descx, " " + caps[j] + " ", cap, false);
            }
            first = false;
        }
        text(descx, y, x1 - 2, r.desc, bs, false);
    }
    std::string more = kw.n > kw.rows ? "↑↓ scroll · " : "";
    text(kw.x0 + 2, y0 + bh - 2, x1 - 2, more + "Esc close · /help keys", dim, false);
    tb_hide_cursor();
}

void Tui::keys_key(int k, uint32_t ch) {
    int last = std::max(0, kw.n - kw.rows);
    if (k == TB_KEY_ARROW_DOWN) kw.top = std::min(last, kw.top + 1);
    else if (k == TB_KEY_ARROW_UP) kw.top = std::max(0, kw.top - 1);
    else if (k == TB_KEY_PGDN || k == TB_KEY_SPACE) kw.top = std::min(last, kw.top + kw.rows);
    else if (k == TB_KEY_PGUP) kw.top = std::max(0, kw.top - kw.rows);
    else if (k == TB_KEY_HOME) kw.top = 0;
    else if (k == TB_KEY_END) kw.top = last;
    else kw.open = false;  // Esc, q, Enter, F1 or Alt+K again: anything else closes
}

// every word typed must be in the tab's name, its network, or its kind ("server", "query"...)
std::vector<Buffer*> Tui::find_list() const {
    auto words = split(irc_lower(to_utf8(fw.q)), ' ');
    std::vector<Buffer*> out;
    for (auto& bp : c.bufs) {
        Buffer* b = bp.get();
        static const char* KIND[] = {"server", "channel", "query"};
        std::string hay = irc_lower(buf_label(b) + " " + b->name + " " + (b->srv ? b->srv->cfg.name + " " + b->srv->network : "") +
                                    " " + (b->type == BT::STATUS && !b->srv ? "cmd console" : KIND[(int)b->type]));
        bool ok = true;
        for (auto& w : words) ok = ok && (w.empty() || hay.find(w) != std::string::npos);
        if (ok) out.push_back(b);
    }
    std::stable_partition(out.begin(), out.end(), [&](Buffer* b) {  // the tab's own name first, then network-only matches
        std::string n = irc_lower(buf_label(b));
        for (auto& w : words) if (n.find(w) == std::string::npos) return false;
        return true;
    });
    return out;
}

void Tui::draw_find(int W, int H) {
    const Theme& T = c.look(false);
    Style bs = sty(T, "menu", "nicklist"), ls = sty(T, "menu_border", "separator"), hs = bs, dim = sty(T, "timestamp", "menu"),
          sel = sty(T, "menu_sel", "tab_active"), fs = T.get("input");
    for (Style* st : {&ls, &dim}) st->bg = bs.bg;
    hs.attr |= A_BOLD;
    auto l = find_list();
    int n = (int)l.size();
    fw.rows = std::max(1, std::min({std::max(n, 1), H - 6, 14}));
    fw.w = std::min(W - 2, 56), fw.x0 = (W - fw.w) / 2;
    int bh = fw.rows + 4, y0 = std::max(0, (H - bh) / 2 - 1), x1 = fw.x0 + fw.w;
    fw.list_y = y0 + 2;
    if (fw.sel >= n) fw.sel = std::max(0, n - 1);
    if (fw.sel < fw.top) fw.top = fw.sel;
    if (fw.sel >= fw.top + fw.rows) fw.top = fw.sel - fw.rows + 1;
    for (int yy = y0; yy < y0 + bh; yy++) fill(yy, fw.x0, x1, bs);
    for (int xx = fw.x0; xx < x1; xx++) cell(xx, y0, U'─', ls), cell(xx, y0 + bh - 1, U'─', ls);
    for (int yy = y0; yy < y0 + bh; yy++) cell(fw.x0, yy, U'│', ls), cell(x1 - 1, yy, U'│', ls);
    cell(fw.x0, y0, U'┌', ls), cell(x1 - 1, y0, U'┐', ls), cell(fw.x0, y0 + bh - 1, U'└', ls), cell(x1 - 1, y0 + bh - 1, U'┘', ls);
    if (auto cb = box_title(T, fw.x0, x1 - 1, y0, "Go to tab", hs); cb.first >= 0) xbtn = {y0, cb.first, cb.second, 'f'};
    int fx = text(fw.x0 + 2, y0 + 1, x1 - 2, "find: ", hs, false);
    fill(y0 + 1, fx, x1 - 2, fs);
    int ex = text(fx, y0 + 1, x1 - 2, to_utf8(fw.q), fs, false);
    put_cursor(ex, y0 + 1);
    if (!n) text(fw.x0 + 2, fw.list_y, x1 - 2, "no tab matches", dim, false);
    for (int i = 0; i < fw.rows && fw.top + i < n; i++) {
        Buffer* b = l[(size_t)(fw.top + i)];
        bool on = fw.top + i == fw.sel;
        Style st = on ? sel : b->activity == 3 ? T.get("tab_hilight") : b->activity == 2 ? T.get("tab_msg") : bs;
        if (!on) st.bg = bs.bg;
        fill(fw.list_y + i, fw.x0 + 1, x1 - 1, on ? sel : bs);
        std::string where = !b->srv ? "" : b->type == BT::STATUS ? "server" : b->srv->network.empty() ? b->srv->cfg.name : b->srv->network;
        int wx = x1 - 2 - (int)where.size();
        text(fw.x0 + 2, fw.list_y + i, wx - 1, (on ? "▸ " : "  ") + std::to_string(c.index_of(b) + 1) + ":" + buf_label(b), st, false);
        Style d = dim;
        on_row(d, on ? sel : bs);
        text(wx, fw.list_y + i, x1 - 2, where, d, false);
    }
    text(fw.x0 + 2, y0 + bh - 2, x1 - 2, "type: filter · ↑↓ · Enter go · Esc close", dim, false);
}

void Tui::find_key(int k, uint32_t ch) {
    auto l = find_list();
    int n = (int)l.size();
    if (k == TB_KEY_ESC) { if (!fw.q.empty()) fw.q.clear(), fw.sel = fw.top = 0; else fw.open = false; return; }
    if (k == TB_KEY_ENTER) { fw.open = false; if (n) c.select(l[(size_t)std::min(fw.sel, n - 1)]); return; }
    if (ch || k == TB_KEY_SPACE) fw.q += ch ? (char32_t)ch : U' ', fw.sel = fw.top = 0;
    else if (k == TB_KEY_BACKSPACE || k == TB_KEY_BACKSPACE2) { if (!fw.q.empty()) fw.q.pop_back(), fw.sel = fw.top = 0; }
    else if (k == TB_KEY_CTRL_U) fw.q.clear(), fw.sel = fw.top = 0;
    else if (k == TB_KEY_ARROW_DOWN || k == TB_KEY_TAB) fw.sel = n ? (fw.sel + 1) % n : 0;
    else if (k == TB_KEY_ARROW_UP) fw.sel = n ? (fw.sel - 1 + n) % n : 0;
    else if (k == TB_KEY_PGDN) fw.sel = std::min(n - 1, fw.sel + fw.rows);
    else if (k == TB_KEY_PGUP) fw.sel = std::max(0, fw.sel - fw.rows);
    else if (k == TB_KEY_CTRL_G) fw.open = false;  // the same key closes it
}

// a click goes there; the wheel moves
void Tui::find_mouse(int k, int x, int y) {
    if (k == TB_KEY_MOUSE_WHEEL_UP || k == TB_KEY_MOUSE_WHEEL_DOWN) { find_key(k == TB_KEY_MOUSE_WHEEL_UP ? TB_KEY_ARROW_UP : TB_KEY_ARROW_DOWN, 0); return; }
    int i = y - fw.list_y;
    if (k != TB_KEY_MOUSE_LEFT) return;
    if (i < 0 || i >= fw.rows || x <= fw.x0 || x >= fw.x0 + fw.w - 1) { fw.open = false; return; }  // outside: close
    if (fw.top + i >= (int)find_list().size()) return;
    fw.sel = fw.top + i;
    find_key(TB_KEY_ENTER, 0);
}

// ---- the plugin window (/plugin alone) --------------------------------------------------
//   ┌─ Plugins ──────────────────────────────────────────────────────┐
//   │  Installed 3    Available 4 ↑1                                  │
//   │     PLUGIN           STATUS        VERSION      DESCRIPTION     │
//   │ ▸ ✓ autoaway         on            1.2 ↑ 1.3    Away when you'… │
//   │   ○ seen             off           1.0          /seen nick: w…  │
//   ├─────────────────────────────────────────────────────────────────┤
//   │  autoaway 1.2 · on everywhere · 1.3 available: u updates        │
//   │  Away when you're idle or nobody's watching: sets /away after   │
//   │  a while without typing, and comes back when you do             │
//   │  Commands: /autoaway                                            │
//   │                                                                 │
//   │  Space turn off  c configure  u update  d remove  r reload  ... │
//   └─────────────────────────────────────────────────────────────────┘
// c: the plugin's settings at one scope (everywhere, a network, a channel; ←→), Enter edits one
static std::string scope_name(const std::string& sc) { return sc.empty() ? "everywhere" : sc; }

// the configure view's rows: enabled, then what the plugin declared that can be set at this scope
static std::vector<PluginSet> cfg_rows(const PluginInfo& pi, const std::string& scope) {
    int lvl = scope.empty() ? 0 : scope.find('/') == std::string::npos ? 1 : 2;
    std::vector<PluginSet> out{{"enabled", "off", 'c', true, "on or off here"}};
    for (auto& st : pi.sets)
        if (lvl == 0 || (lvl == 1 && st.scope != 'g') || (lvl == 2 && st.scope == 'c')) out.push_back(st);
    return out;
}

void Tui::plug_run(const std::string& cmd) {
    pw.mb = c.bufs.empty() ? nullptr : c.bufs.front().get();  // /plugin answers in the console tab: its newest line is the message
    pw.mark = pw.mb ? pw.mb->added : -1;
    if (c.active) c.exec(c.active, cmd);
}

void Tui::draw_plugins(int W, int H) {
    const Theme& T = c.look(false);
    Style bs = sty(T, "menu", "nicklist"), ls = sty(T, "menu_border", "separator"), hs = bs, dim = sty(T, "timestamp", "menu"),
          sel = sty(T, "menu_sel", "tab_active"), fs = T.get("input"), es = T.get("error"), keyst = sty(T, "set_key", "info"),
          ok = T.get("join"), up = sty(T, "input_prompt", "hilight");
    for (Style* st : {&ls, &dim, &es, &keyst, &ok, &up}) st->bg = bs.bg, st->attr = 0;
    for (Style* st : {&dim, &es, &keyst, &ok, &up}) st->fg = readable(st->fg, bs.bg);  // a theme's join green on its menu gray
    hs.attr |= A_BOLD, keyst.attr |= A_BOLD;
    auto info = c.plugin_info();
    auto scopes = c.plugin_scopes();
    auto cat = c.catalog_rows();
    const PluginInfo* cp = nullptr;
    for (auto& pi : info) if (pi.name == pw.cfg) cp = &pi;
    if (!pw.cfg.empty() && !cp) pw.cfg.clear(), pw.editing = false;  // removed meanwhile
    pw.scope = std::max(0, std::min(pw.scope, (int)scopes.size() - 1));
    std::string scope = scopes[(size_t)pw.scope];
    std::vector<PluginSet> cfg;
    if (cp) cfg = cfg_rows(*cp, scope);
    int n = cp ? (int)cfg.size() : pw.avail ? (int)cat.size() : (int)info.size();
    int ups = 0;
    for (auto& r : cat) ups += r.update;

    const int DET = 4;  // the selected one's details under the list
    const int PH = cp && cp->has_panel() ? 5 : 0;  // the plugin's panel (isekur.panel) above its settings: three lines and a sky, its buttons, a rule
    pw.rows = std::max(1, std::min({std::max(n, 1), H - 13 - PH, 12}));
    pw.w = std::min(W - 2, 88), pw.x0 = (W - pw.w) / 2;
    int bh = pw.rows + DET + 7 + PH, y0 = std::max(0, (H - bh) / 2 - 1), x1 = pw.x0 + pw.w, xm = x1 - 2;
    pw.tab_y = y0 + 1 + PH, pw.list_y = y0 + 3 + PH;
    int sep_y = pw.list_y + pw.rows, det_y = sep_y + 1, msg_y = det_y + DET, hint_y = msg_y + 1;
    if (pw.sel >= n) pw.sel = std::max(0, n - 1);
    if (pw.sel < pw.top) pw.top = pw.sel;
    if (pw.sel >= pw.top + pw.rows) pw.top = pw.sel - pw.rows + 1;
    gutters(W, pw.x0, x1, y0, y0 + bh, T.get("default"));
    for (int yy = y0; yy < y0 + bh; yy++) fill(yy, pw.x0, x1, bs);
    for (int xx = pw.x0; xx < x1; xx++) cell(xx, y0, U'─', ls), cell(xx, y0 + bh - 1, U'─', ls), cell(xx, sep_y, U'─', ls);
    for (int yy = y0; yy < y0 + bh; yy++) cell(pw.x0, yy, U'│', ls), cell(x1 - 1, yy, U'│', ls);
    cell(pw.x0, y0, U'┌', ls), cell(x1 - 1, y0, U'┐', ls), cell(pw.x0, y0 + bh - 1, U'└', ls), cell(x1 - 1, y0 + bh - 1, U'┘', ls);
    cell(pw.x0, sep_y, U'├', ls), cell(x1 - 1, sep_y, U'┤', ls);
    if (auto cb = box_title(T, pw.x0, x1 - 1, y0, cp ? "Plugins · " + cp->name : "Plugins", hs); cb.first >= 0) xbtn = {y0, cb.first, cb.second, 'g'};
    pw.pbtn.clear();
    if (PH) {  // its status lines (the first in the title color), the sun or moon at the right, the buttons, a rule under them
        for (int i = 0; i < 3 && i < (int)cp->panel_lines.size(); i++)
            text(pw.x0 + 2, y0 + 1 + i, cp->panel_sky >= 0 ? xm - 11 : xm, cp->panel_lines[(size_t)i], i ? dim : keyst, false);
        if (cp->panel_sky >= 0) draw_sky(xm - 9, y0 + 1, cp->panel_sky, keyst, dim);
        int bx = pw.x0 + 2;
        for (size_t i = 0; i < cp->panel_actions.size(); i++) {
            std::string lab = " " + cp->panel_actions[i].first + " ";
            int ex = text(bx, y0 + 4, xm, lab, sel, false);
            pw.pbtn.push_back({y0 + 4, bx, ex});
            bx = ex + 2;
        }
        for (int xx = pw.x0 + 1; xx < x1 - 1; xx++) cell(xx, y0 + 5, U'─', ls);
        cell(pw.x0, y0 + 5, U'├', ls), cell(x1 - 1, y0 + 5, U'┤', ls);
    }

    // tabs (or the scope), then the column titles
    int ca = pw.x0 + 6, cb2 = ca + 17, cc = cb2 + (cp ? 33 : 14), cd = cc + (cp ? 0 : 13);  // cp: value, where from (cc - 11), what it is (cc)
    if (cp) {
        int x = text(pw.x0 + 2, pw.tab_y, xm, "Scope ", hs, false);
        x = text(x, pw.tab_y, xm, " ◂ " + scope_name(scope) + " ▸ ", sel, false);
        x = text(x + 1, pw.tab_y, xm, std::to_string(pw.scope + 1) + "/" + std::to_string(scopes.size()), dim, false);
        text(x + 2, pw.tab_y, xm, scope.empty() ? "all networks and channels" : scope.find('/') == std::string::npos ? "this network's channels" : "this channel only", dim, false);
        text(ca, pw.tab_y + 1, xm, "SETTING", dim, false), text(cb2, pw.tab_y + 1, xm, "VALUE", dim, false), text(cc, pw.tab_y + 1, xm, "WHAT IT DOES", dim, false);
    } else {
        Style ti = pw.avail ? bs : sel, ta = pw.avail ? sel : bs;
        int x = text(pw.x0 + 2, pw.tab_y, xm, " Installed ", ti, false);
        x = text(x, pw.tab_y, xm, std::to_string(info.size()) + " ", ti, false);
        pw.tab_mid = x + 1;
        x = text(x + 2, pw.tab_y, xm, " Available ", ta, false);
        x = text(x, pw.tab_y, xm, (c.catalog_have ? std::to_string(cat.size()) : std::string("?")) + " ", ta, false);
        if (ups) { Style u = up; if (pw.avail) on_row(u, sel); text(x, pw.tab_y, xm, "↑" + std::to_string(ups) + " ", u, false); }
        text(ca, pw.tab_y + 1, xm, "PLUGIN", dim, false), text(cb2, pw.tab_y + 1, xm, pw.avail ? "VERSION" : "STATUS", dim, false);
        text(cc, pw.tab_y + 1, xm, pw.avail ? "" : "VERSION", dim, false), text(pw.avail ? cc : cd, pw.tab_y + 1, xm, "DESCRIPTION", dim, false);
    }

    // the list
    if (!n) {
        std::string empty = cp ? "" : !pw.avail ? "None installed yet: Tab shows the catalog's"
                          : c.catalog ? "Fetching the catalog..." : !c.catalog_err.empty() ? "Catalog: " + c.catalog_err + " (r retries)"
                          : c.catalog_have ? "The catalog is empty" : "r fetches the catalog";
        text(ca, pw.list_y, xm, empty, dim, false);
    }
    auto onstr = [](const PluginInfo& pi) {
        if (pi.state == "failed") return std::string("didn't load");
        if (pi.state != "on") return std::string("off");
        if (std::find(pi.on_in.begin(), pi.on_in.end(), "") != pi.on_in.end()) return std::string("on");
        std::string s = "on in";
        for (size_t i = 0; i < pi.on_in.size(); i++) s += (i ? ", " : " ") + pi.on_in[i];
        return s;
    };
    for (int i = 0; i < pw.rows && pw.top + i < n; i++) {
        int k = pw.top + i, y = pw.list_y + i;
        bool on = k == pw.sel;
        Style st = on ? sel : bs, d = dim, g = dim;
        auto mk = [&](Style s) { if (on) on_row(s, sel), s.fg = sel.fg; else s.bg = bs.bg; return s; };  // the selected row in one color: it must read
        fill(y, pw.x0 + 1, x1 - 1, st);
        text(pw.x0 + 2, y, xm, on ? "▸" : " ", st, false);
        if (cp) {
            const PluginSet& s = cfg[(size_t)k];
            std::string at = c.plugin_at(cp->name, s.key, scope), eff = c.plugin_eff(cp->name, s.key, scope, s.def, s.scope);
            text(pw.x0 + 4, y, xm, at.empty() ? " " : "●", mk(keyst), false);  // set at this scope
            text(ca, y, cb2 - 1, s.key, on ? st : keyst, false);
            if (on && pw.editing) {
                fill(y, cb2, xm, fs);
                int ex = text(cb2, y, xm, to_utf8(pw.ed), fs, false);
                put_cursor(std::min(ex, x1 - 3), y);
                continue;
            }
            std::string v = s.flag ? std::string(truthy_str(eff) ? "● on" : "○ off") : eff.empty() ? "(empty)" : !s.choices.empty() && on ? "◂ " + eff + " ▸" : eff;
            text(cb2, y, cc - 12, v, at.empty() ? mk(d) : st, false);
            text(cc - 11, y, cc - 1, at.empty() ? (scope.empty() ? "default" : "inherited") : "set here", at.empty() ? mk(d) : mk(keyst), false);
            if (!s.help.empty()) text(cc, y, xm, s.help, mk(d), false);
            continue;
        }
        if (!pw.avail) {
            const PluginInfo& pi = info[(size_t)k];
            bool fail = pi.state == "failed", off = pi.state == "off";
            g = fail ? es : off ? dim : ok;
            text(pw.x0 + 4, y, xm, fail ? "✗" : off ? "○" : "✓", mk(g), false);
            text(ca, y, cb2 - 1, pi.name, on ? st : hs, false);
            text(cb2, y, cc - 1, onstr(pi), mk(g), false);
            int vx = text(cc, y, cd - 1, pi.local.empty() ? "?" : pi.local, st, false);
            if (pi.update) text(vx + 1, y, cd - 1, "↑ " + pi.version, mk(up), false);
            else if (pi.edited) text(vx + 1, y, cd - 1, "edited", mk(d), false);
            std::string ds = pi.desc;
            if (ds.empty()) for (auto& x : pi.cmds) ds += (ds.empty() ? "/" : " /") + x;
            text(cd, y, xm, ds, mk(d), false);
        } else {
            const CatalogRow& r = cat[(size_t)k];
            g = r.too_new ? es : r.update ? up : r.installed ? ok : dim;
            text(pw.x0 + 4, y, xm, r.too_new ? "✗" : r.update ? "↑" : r.installed ? "✓" : "·", mk(g), false);
            text(ca, y, cb2 - 1, r.name, on ? st : hs, false);
            text(cb2, y, cc - 1, r.version, st, false);
            text(cc, y, xm, r.desc, mk(d), false);
        }
    }

    // the selected one in full: what it is, its state, what to do next
    std::vector<std::pair<std::string, Style>> det;  // lines; the first is the headline
    auto add_wrapped = [&](const std::string& s, Style st) { for (auto& l : wrap_words(s, pw.w - 6)) det.push_back({l, st}); };
    std::string head, state;
    Style state_st = dim;
    if (cp && n) {
        const PluginSet& s = cfg[(size_t)pw.sel];
        std::string at = c.plugin_at(cp->name, s.key, scope), eff = c.plugin_eff(cp->name, s.key, scope, s.def, s.scope);
        head = s.key;
        state = at.empty() ? (scope.empty() ? "default " : "inherits ") + (eff.empty() ? "(empty)" : eff) : "set to " + at + " " + (scope.empty() ? "everywhere" : "for " + scope);
        state_st = at.empty() ? dim : keyst;
        if (!s.help.empty()) add_wrapped(s.help, bs);
        if (!s.choices.empty()) {
            std::string all;
            for (auto& v : s.choices) all += (all.empty() ? "" : ", ") + v;
            add_wrapped("One of: " + all + " · ←→, Space or Enter: change it", dim);
        }
        add_wrapped(std::string("Applies ") + (s.scope == 'g' ? "everywhere only" : s.scope == 'n' ? "everywhere or per network" : "everywhere, per network or per channel")
                    + (at.empty() ? "" : " · Del clears it here"), dim);
    } else if (!cp && !pw.avail && n) {
        const PluginInfo& pi = info[(size_t)pw.sel];
        head = pi.name + (pi.local.empty() ? "" : " " + pi.local);
        state = pi.state == "failed" ? "didn't load: the console tab says why" : pi.state == "off" ? "off" : onstr(pi) == "on" ? "on everywhere" : onstr(pi);
        state_st = pi.state == "failed" ? es : pi.state == "off" ? dim : ok;
        if (!pi.desc.empty()) add_wrapped(pi.desc, bs);
        std::string cmds;
        for (auto& x : pi.cmds) cmds += " /" + x;
        if (!cmds.empty()) det.push_back({"Commands:" + cmds, dim});
        if (pi.update) det.push_back({pi.version + " is available: u updates it", up});
        else if (pi.edited) det.push_back({"Edited here: u restores the catalog's " + pi.version, dim});
    } else if (!cp && n) {
        const CatalogRow& r = cat[(size_t)pw.sel];
        head = r.name + " " + r.version;
        state = r.too_new ? "needs a newer iSekur" : r.update ? "update: you have " + (r.local.empty() ? "?" : r.local) : r.edited ? "installed, edited here" : r.installed ? "installed" : "not installed";
        state_st = r.too_new ? es : r.update ? up : r.installed ? ok : dim;
        add_wrapped(r.desc, bs);
        if (!r.too_new && (r.update || !r.installed)) det.push_back({r.update ? "Enter updates it" : "Enter installs it, then Tab to turn it on and configure it", dim});
    }
    if (!head.empty()) {
        int x = text(pw.x0 + 3, det_y, xm, head, hs, false);
        x = text(x, det_y, xm, " · ", dim, false);
        text(x, det_y, xm, state, state_st, false);
    }
    for (int i = 0; i < DET - 1 && i < (int)det.size(); i++) {
        bool cut = i == DET - 2 && (int)det.size() > DET - 1;
        text(pw.x0 + 3, det_y + 1 + i, xm, det[(size_t)i].first + (cut ? " …" : ""), det[(size_t)i].second, false);
    }

    // the message: the newest line of the tab the last action ran in, once something came
    bool live = false;
    for (auto& b : c.bufs) live |= b.get() == pw.mb;
    if (live && pw.mark >= 0 && pw.mb->added > pw.mark && !pw.mb->lines.empty()) {
        auto& l = pw.mb->lines.back();
        text(pw.x0 + 3, msg_y, xm, "» " + mirc_strip(l.text), l.kind == LK::ERROR ? es : keyst, false);
    } else if (c.catalog) text(pw.x0 + 3, msg_y, xm, "Fetching the catalog...", dim, false);
    else if (pw.avail && !cp && c.catalog_have)
        text(pw.x0 + 3, msg_y, xm, "Catalog from " + fmt_dur(time(nullptr) - c.catalog_at) + " ago" + (c.catalog_err.empty() ? "" : " (refresh failed: " + c.catalog_err + ")"), dim, false);

    // the keys for what's selected: the key bold, what it does dim
    std::vector<std::pair<std::string, std::string>> keys;
    if (pw.editing) keys = {{"Enter", "set"}, {"empty", "inherit or the default"}, {"Esc", "cancel"}};
    else if (cp) {
        bool flag = n && cfg[(size_t)pw.sel].flag, pick = n && !cfg[(size_t)pw.sel].choices.empty();
        for (auto& a : cp->panel_actions) {  // the plugin's buttons: its first letter
            std::string k1(1, (char)tolower((unsigned char)a.first[0])), w1 = a.first;
            for (char& ch : w1) ch = (char)tolower((unsigned char)ch);
            keys.push_back({k1, w1});
        }
        keys.push_back({"Enter", flag ? "switch" : pick ? "next value" : "edit"});
        if (flag) keys.push_back({"Space", "on/off"});
        if (pick) keys.push_back({"←→", "value"});
        keys.insert(keys.end(), {{"Del", "clear here"}, {pick ? "Tab" : "←→", "scope"}, {"Esc", "back"}});
    } else if (pw.avail) {
        if (n && !cat[(size_t)pw.sel].too_new && (cat[(size_t)pw.sel].update || !cat[(size_t)pw.sel].installed))
            keys.push_back({"Enter", cat[(size_t)pw.sel].update ? "update" : "install"});
        if (n) keys.push_back({"a", "about"});
        keys.insert(keys.end(), {{"r", "refresh"}, {"Tab", "installed"}, {"Esc", "close"}});
    } else {
        if (n) {
            const PluginInfo& pi = info[(size_t)pw.sel];
            keys = {{"Space", pi.state == "off" ? "turn on" : "turn off"}, {"c", "configure"}};
            if (pi.update || pi.edited) keys.push_back({"u", pi.update ? "update" : "restore"});
            keys.insert(keys.end(), {{"r", "reload"}, {"d", "remove"}, {"a", "about"}});
        }
        keys.insert(keys.end(), {{"Tab", "catalog"}, {"Esc", "close"}});
    }
    int hx = pw.x0 + 3;
    for (auto& [kk, what] : keys) {
        hx = text(hx, hint_y, xm, kk, keyst, false);
        hx = text(hx + 1, hint_y, xm, what, dim, false) + 2;
    }
    if (!pw.about.empty()) {  // a: who made it and how to reach them, from the catalog or the file's own header
        std::string name = pw.about, ver, desc, author, contact, from;
        for (auto& pi : info)
            if (pi.name == name)
                ver = pi.local.empty() ? pi.version : pi.local, desc = pi.desc, author = pi.author, contact = pi.contact,
                from = pi.official ? "Official iSekur plugin, from the catalog" : pi.in_catalog ? "From the catalog" : "Not from the catalog: a file you added";
        for (auto& r : cat)
            if (r.name == name && from.empty())
                ver = r.version, desc = r.desc, author = r.author, contact = r.contact,
                from = r.official ? "Official iSekur plugin, from the catalog" : "From the catalog";
        std::vector<std::string> l = {name + (ver.empty() ? "" : " " + ver), from, ""};
        l.push_back("Author:  " + (author.empty() ? std::string("unknown") : author));
        l.push_back("Contact: " + (contact.empty() ? std::string("none given") : contact));
        if (!desc.empty()) {
            l.push_back("");
            for (auto& w : wrap_words(desc, 44)) l.push_back(w);
        }
        draw_dialog(W, H, "About " + name, l, "", false, U"", 0, "Esc or Enter: close", 0, 0, 'A');
    }
    if (!pw.ask.empty())
        draw_dialog(W, H, "Remove plugin", wrap_words("Remove " + pw.ask + " with its settings and saved data?", 44), "", false, U"", 0,
                    "y: yes · n or Esc: no", 0, 0, 'P');
}

void Tui::plugin_key(int k, uint32_t ch) {
    if (ch == ' ') k = TB_KEY_SPACE, ch = 0;  // termbox gives Space as a character
    if (!pw.about.empty()) { if (k == TB_KEY_ESC || k == TB_KEY_ENTER || ch == 'a' || ch == 'q') pw.about.clear(); return; }
    if (!pw.ask.empty()) {  // remove it? (y/n)
        std::string n = pw.ask;
        pw.ask.clear();
        if (ch == 'y' || ch == 'Y') plug_run("/plugin remove " + n), pw.sel = 0;
        return;
    }
    auto info = c.plugin_info();
    auto scopes = c.plugin_scopes();
    pw.scope = std::max(0, std::min(pw.scope, (int)scopes.size() - 1));
    std::string scope = scopes[(size_t)pw.scope];
    bool bs = k == TB_KEY_BACKSPACE || k == TB_KEY_BACKSPACE2;
    const PluginInfo* cp = nullptr;
    for (auto& pi : info) if (pi.name == pw.cfg) cp = &pi;
    std::string at = cp ? cp->name + (scope.empty() ? "" : "@" + scope) : "";
    if (cp) {
        auto rows = cfg_rows(*cp, scope);
        int n = (int)rows.size();
        pw.sel = std::max(0, std::min(pw.sel, n - 1));
        const PluginSet& st = rows[(size_t)pw.sel];
        auto set = [&](const std::string& v) {
            if (st.key == "enabled") plug_run(v.empty() ? "/plugin set " + at + " enabled" : std::string("/plugin ") + (truthy_str(v) ? "enable " : "disable ") + at);
            else plug_run("/plugin set " + at + " " + st.key + (v.empty() ? "" : " " + v));
        };
        if (pw.editing) {
            if (k == TB_KEY_ESC) pw.editing = false;
            else if (k == TB_KEY_ENTER) pw.editing = false, set(to_utf8(pw.ed));
            else if (bs) { if (!pw.ed.empty()) pw.ed.pop_back(); }
            else if (ch || k == TB_KEY_SPACE) pw.ed += ch ? (char32_t)ch : U' ';
            return;
        }
        std::string eff = c.plugin_eff(cp->name, st.key, scope, st.def, st.scope);
        for (auto& a : cp->panel_actions)  // a button of the plugin's panel, by its first letter
            if (ch && ch < 128 && ch != 'q' && tolower((int)ch) == tolower((unsigned char)a.first[0])) { plug_run(a.second); return; }
        if (k == TB_KEY_ESC || ch == 'q') pw.cfg.clear(), pw.sel = 0;
        else if (k == TB_KEY_ARROW_DOWN) pw.sel = std::min(n - 1, pw.sel + 1);
        else if (k == TB_KEY_ARROW_UP) pw.sel = std::max(0, pw.sel - 1);
        else if ((k == TB_KEY_ARROW_RIGHT || k == TB_KEY_ARROW_LEFT) && !st.choices.empty()) {  // its values, round; Tab: the scope
            int m = (int)st.choices.size(), i = (int)(std::find(st.choices.begin(), st.choices.end(), eff) - st.choices.begin());
            set(st.choices[(size_t)(i >= m ? 0 : (i + (k == TB_KEY_ARROW_RIGHT ? 1 : m - 1)) % m)]);
        } else if (k == TB_KEY_ARROW_RIGHT || k == TB_KEY_TAB) pw.scope = (pw.scope + 1) % (int)scopes.size();
        else if (k == TB_KEY_ARROW_LEFT) pw.scope = (pw.scope + (int)scopes.size() - 1) % (int)scopes.size();
        else if (k == TB_KEY_SPACE && st.flag) set(truthy_str(eff) ? "off" : "on");
        else if (k == TB_KEY_DELETE || bs) set("");
        else if ((k == TB_KEY_ENTER || k == TB_KEY_SPACE) && !st.choices.empty()) {  // the next of its values, round
            auto it = std::find(st.choices.begin(), st.choices.end(), eff);
            set(it == st.choices.end() || ++it == st.choices.end() ? st.choices.front() : *it);
        } else if (k == TB_KEY_ENTER) {
            if (st.flag) set(truthy_str(eff) ? "off" : "on");
            else pw.editing = true, pw.ed = from_utf8(c.plugin_at(cp->name, st.key, scope));
        }
        return;
    }
    if (k == TB_KEY_ESC || ch == 'q') { pw.open = false; return; }
    if (k == TB_KEY_TAB || k == TB_KEY_ARROW_LEFT || k == TB_KEY_ARROW_RIGHT) {
        pw.avail = !pw.avail, pw.sel = pw.top = 0;
        if (pw.avail && !c.catalog_have && !c.catalog) c.plugin_refresh(c.active);
        return;
    }
    int n = pw.avail ? (int)c.catalog_rows().size() : (int)info.size();
    if (k == TB_KEY_ARROW_DOWN) pw.sel = std::min(n - 1, pw.sel + 1);
    else if (k == TB_KEY_ARROW_UP) pw.sel = std::max(0, pw.sel - 1);
    else if (k == TB_KEY_PGDN) pw.sel = std::min(n - 1, pw.sel + pw.rows);
    else if (k == TB_KEY_PGUP) pw.sel = std::max(0, pw.sel - pw.rows);
    else if (pw.avail) {
        auto cat = c.catalog_rows();
        if (ch == 'r') plug_run("/plugin refresh");
        else if (ch == 'a' && pw.sel < n) pw.about = cat[(size_t)pw.sel].name;
        else if ((k == TB_KEY_ENTER || ch == 'i') && pw.sel < n) plug_run((cat[(size_t)pw.sel].update ? "/plugin update " : "/plugin install ") + cat[(size_t)pw.sel].name);
    } else if (pw.sel < n) {
        const PluginInfo& pi = info[(size_t)pw.sel];
        if (k == TB_KEY_SPACE) plug_run(std::string("/plugin ") + (pi.state == "off" ? "enable " : "disable ") + pi.name);
        else if (ch == 'c' || k == TB_KEY_ENTER) pw.cfg = pi.name, pw.sel = pw.top = 0, pw.scope = 0;
        else if (ch == 'u') plug_run("/plugin update " + pi.name);
        else if (ch == 'r') plug_run("/plugin reload " + pi.name);
        else if (ch == 'd' || k == TB_KEY_DELETE) pw.ask = pi.name;
        else if (ch == 'a') pw.about = pi.name;
    }
    pw.sel = std::max(0, std::min(pw.sel, n - 1));
}

// a click picks a tab or a row (again, or a double click: Enter); the wheel moves
void Tui::plugin_mouse(int k, int x, int y) {
    if (!pw.ask.empty() || !pw.about.empty()) return;
    if (k == TB_KEY_MOUSE_WHEEL_UP || k == TB_KEY_MOUSE_WHEEL_DOWN) { plugin_key(k == TB_KEY_MOUSE_WHEEL_UP ? TB_KEY_ARROW_UP : TB_KEY_ARROW_DOWN, 0); return; }
    if (k != TB_KEY_MOUSE_LEFT || x <= pw.x0 || x >= pw.x0 + pw.w - 1) return;
    for (size_t i = 0; i < pw.pbtn.size(); i++)  // a button of the plugin's panel
        if (y == pw.pbtn[i][0] && x >= pw.pbtn[i][1] && x < pw.pbtn[i][2]) {
            for (auto& pi : c.plugin_info())
                if (pi.name == pw.cfg && i < pi.panel_actions.size()) { plug_run(pi.panel_actions[i].second); return; }
        }
    if (y == pw.tab_y) {
        if (pw.cfg.empty()) { if ((x >= pw.tab_mid) != pw.avail) plugin_key(TB_KEY_TAB, 0); }
        else plugin_key(x < pw.x0 + pw.w / 2 ? TB_KEY_ARROW_LEFT : TB_KEY_ARROW_RIGHT, 0);
        return;
    }
    int i = y - pw.list_y;
    if (i < 0 || i >= pw.rows || pw.editing) return;
    long long now = now_ms();
    bool again = pw.sel == pw.top + i && now - pw.click_ms < 500;
    pw.click_ms = now, pw.sel = pw.top + i;
    if (again) plugin_key(TB_KEY_ENTER, 0);
}

// a plugin's window (isekur.window): a display (the first line in the title color, with a cassette's reels when the plugin asks), a
// filter (/), the list, the buttons. The cursor is ours (Core::pwin.sel only moves it when the plugin changes it); Enter or a double
// click picks the row, a button is pressed by its first letter or a click
std::vector<int> Tui::pwin_vis() const {
    std::string q = c.pwin.search.empty() ? to_utf8(pu.q) : "";  // a search field is for the plugin: the rows are what it found, none are hidden here
    for (char& ch : q) ch = (char)tolower((unsigned char)ch);
    std::vector<int> v;
    for (size_t i = 0; i < c.pwin.items.size(); i++) {
        std::string t = c.pwin.items[i];
        for (char& ch : t) ch = (char)tolower((unsigned char)ch);
        if (q.empty() || t.find(q) != std::string::npos) v.push_back((int)i + 1);
    }
    return v;
}

void Tui::draw_pwin(int W, int H) {
    const Core::PWin& w = c.pwin;
    const Theme& T = c.look(false);
    Style bs = sty(T, "menu", "nicklist"), ls = sty(T, "menu_border", "separator"), hs = bs, dim = sty(T, "timestamp", "menu"),
          sel = sty(T, "menu_sel", "tab_active"), keyst = sty(T, "set_key", "info"), ok = T.get("join"), fs = T.get("input");
    for (Style* st : {&ls, &dim, &keyst, &ok}) st->bg = bs.bg, st->attr = 0;
    for (Style* st : {&dim, &keyst, &ok}) st->fg = readable(st->fg, bs.bg);
    hs.attr |= A_BOLD, keyst.attr |= A_BOLD;
    if (!pu.live) pu = PWinUi{}, pu.live = true;
    if (w.sel != pu.last_sel) pu.last_sel = w.sel, pu.cur = w.sel ? w.sel : pu.cur;
    if (w.list != pu.last_list) pu.last_list = w.list, pu.cur = w.sel ? w.sel : w.items.empty() ? 0 : 1, pu.top = 0, pu.q.clear(), pu.filtering = false;  // other rows: from the top, with no filter on them
    if (w.tab != pu.last_tab) pu.last_tab = w.tab, pu.q.clear(), pu.filtering = false;  // another tab: its own filter
    if (w.query != pu.last_query) pu.last_query = w.query, pu.q = from_utf8(w.query);  // the plugin changed what the search field holds
    auto vis = pwin_vis();
    int n = (int)vis.size(), at = (int)(std::find(vis.begin(), vis.end(), pu.cur) - vis.begin());
    if (at >= n) at = 0, pu.cur = n ? vis[0] : 0;  // the cursor's row is filtered out: the first one
    int fr = w.items.size() >= 6 || !w.search.empty() ? 1 : 0;  // the filter row (a search field is always there)
    int tr = w.tabs.empty() ? 0 : 1;  // the tab strip
    int rh = w.subs.empty() ? 1 : 2;  // a row with a second line takes two
    int nd = std::max(w.sky >= 0 ? 3 : 1, std::min((int)w.display.size(), 4));  // a sky is three rows tall
    int sr = w.slider >= 0 ? 1 : 0;  // the slider row (volume)
    pu.rowh = rh;
    pu.rows = n ? std::max(1, std::min({n, (H - nd - 9 - fr - sr - tr) / rh, rh == 1 ? 12 : 7})) : 0;
    pu.w = std::min(W - 2, w.tabs.empty() ? 64 : 72), pu.x0 = (W - pu.w) / 2;  // tabs and six buttons need the room
    int bh = nd + pu.rows * rh + 6 + fr + sr + tr, y0 = std::max(0, (H - bh) / 2 - 1), x1 = pu.x0 + pu.w, xm = x1 - 2;
    int ly = y0 + 1;  // the display's top border
    int ty = ly + nd + 2;  // the tabs
    int fy = ty + tr;
    pu.ty = tr ? ty : -1;
    pu.fy = fr ? fy : -1;
    pu.list_y = fy + fr, pu.sy = sr ? pu.list_y + pu.rows * rh : -1, pu.btn_y = pu.list_y + pu.rows * rh + sr;
    int hint_y = pu.btn_y + 1;
    if (n) {
        if (at < pu.top) pu.top = at;
        if (at >= pu.top + pu.rows) pu.top = at - pu.rows + 1;
        pu.top = std::max(0, std::min(pu.top, n - pu.rows));
    } else pu.top = 0;
    gutters(W, pu.x0, x1, y0, y0 + bh, T.get("default"));
    for (int yy = y0; yy < y0 + bh; yy++) fill(yy, pu.x0, x1, bs);
    for (int xx = pu.x0; xx < x1; xx++) cell(xx, y0, U'─', ls), cell(xx, y0 + bh - 1, U'─', ls);
    for (int yy = y0; yy < y0 + bh; yy++) cell(pu.x0, yy, U'│', ls), cell(x1 - 1, yy, U'│', ls);
    cell(pu.x0, y0, U'┌', ls), cell(x1 - 1, y0, U'┐', ls), cell(pu.x0, y0 + bh - 1, U'└', ls), cell(x1 - 1, y0 + bh - 1, U'┘', ls);
    if (auto cb = box_title(T, pu.x0, x1 - 1, y0, w.title, hs); cb.first >= 0) xbtn = {y0, cb.first, cb.second, 'w'};
    // the display: a panel of its own
    int lx0 = pu.x0 + 2, lx1 = x1 - 2;
    for (int xx = lx0; xx < lx1; xx++) cell(xx, ly, U'─', ls), cell(xx, ly + nd + 1, U'─', ls);
    for (int yy = ly; yy <= ly + nd + 1; yy++) cell(lx0, yy, U'│', ls), cell(lx1 - 1, yy, U'│', ls);
    cell(lx0, ly, U'┌', ls), cell(lx1 - 1, ly, U'┐', ls), cell(lx0, ly + nd + 1, U'└', ls), cell(lx1 - 1, ly + nd + 1, U'┘', ls);
    for (int i = 0; i < nd; i++)
        if (i < (int)w.display.size()) text(lx0 + 2, ly + 1 + i, w.sky >= 0 && i < 3 ? lx1 - 13 : i || w.reels < 0 ? lx1 - 2 : lx1 - 10, w.display[(size_t)i], i ? dim : keyst, false);
    if (w.sky >= 0) draw_sky(lx1 - 12, ly + 1, w.sky, keyst, dim);
    if (w.reels >= 0) {  // a cassette's two reels, turning ~8 times a second while it plays
        static const char32_t arc[] = {U'◜', U'◝', U'◞', U'◟'};
        char32_t r = w.reels ? arc[(now_ms() / 120) % 4] : U'○';
        int rx = lx1 - 9;
        cell(rx, ly + 1, U'[', ls), cell(rx + 1, ly + 1, r, keyst), cell(rx + 2, ly + 1, U'═', ls), cell(rx + 3, ly + 1, U'═', ls), cell(rx + 4, ly + 1, U'═', ls), cell(rx + 5, ly + 1, r, keyst), cell(rx + 6, ly + 1, U']', ls);
    }
    pu.tx.clear();
    if (tr) {  // the tabs: the active one in the cursor's colors, the others plain; Tab or its number picks one
        int x = pu.x0 + 2;
        for (size_t ti = 0; ti < w.tabs.size(); ti++) {
            bool on = (int)ti + 1 == w.tab;
            int xs = x;
            x = text(x, ty, xm, " " + std::to_string(ti + 1) + " " + w.tabs[ti] + " ", on ? sel : hs, false);
            pu.tx.push_back({xs, x - 1});
            x += 1;
        }
    }
    if (fr) {  // the filter, or the search field
        std::string f = "/ " + to_utf8(pu.q);
        fill(fy, pu.x0 + 2, x1 - 2, fs);  // an input field, even while it isn't typing
        if (pu.filtering) {
            int ex = text(pu.x0 + 3, fy, xm - 1, f, fs, false);
            put_cursor(std::min(ex, x1 - 4), fy);
        } else {
            Style ph = fs;
            ph.fg = readable(dim.fg, fs.bg), ph.attr = 0;
            text(pu.x0 + 3, fy, xm - 1, pu.q.empty() ? "/ " + (w.search.empty() ? std::string("search stations: press / or click here") : w.search + ": press /") : f, pu.q.empty() ? ph : fs, false);
        }
    }
    for (int i = 0; i < pu.rows && pu.top + i < n; i++) {
        int k = vis[(size_t)(pu.top + i)], y = pu.list_y + i * rh;
        bool on = k == pu.cur;
        Style st = on ? sel : bs;
        for (int l = 0; l < rh; l++) fill(y + l, pu.x0 + 1, x1 - 1, st);
        text(pu.x0 + 2, y, xm, k == w.mark ? "▶" : " ", on ? st : ok, false);  // ▶ playing, ★ favorite: two narrow columns before the name
        text(pu.x0 + 3, y, xm, k == w.star ? "★" : " ", on ? st : keyst, false);
        text(pu.x0 + 5, y, xm, w.items[(size_t)k - 1], st, false);
        if (rh == 2 && (size_t)k <= w.subs.size()) text(pu.x0 + 5, y + 1, xm, w.subs[(size_t)k - 1], on ? st : dim, false);
    }
    if (!n && !w.items.empty()) text(pu.x0 + 5, pu.list_y, xm, "Nothing matches", dim, false);
    if (sr) {  // the slider: an icon, a bar, the value
        int x = text(pu.x0 + 2, pu.sy, xm, w.slider_icon.empty() ? " " : w.slider_icon, keyst, false);
        pu.bar0 = x + 1, pu.barn = std::max(8, xm - 7 - pu.bar0);
        int filled = w.slider_max ? (int)((long long)w.slider * pu.barn / w.slider_max) : 0;
        for (int i = 0; i < pu.barn; i++) cell(pu.bar0 + i, pu.sy, i < filled ? U'█' : U'░', i < filled ? keyst : dim);
        char pct[16];
        snprintf(pct, sizeof pct, "%3d%%", w.slider);
        text(pu.bar0 + pu.barn + 1, pu.sy, xm, pct, hs, false);
    }
    pu.bx.clear();
    int x = pu.x0 + 2;
    for (size_t bi = 0; bi < w.buttons.size(); bi++) {
        std::u32string u = from_utf8(w.buttons[bi]);
        int xs = x;
        x = text(x, pu.btn_y, xm, "[ " + (bi < w.icons.size() && !w.icons[bi].empty() ? w.icons[bi] + " " : ""), hs, false);
        x = text(x, pu.btn_y, xm, to_utf8(u.substr(0, 1)), keyst, false);
        x = text(x, pu.btn_y, xm, to_utf8(u.substr(std::min<size_t>(1, u.size()))) + " ]", hs, false);
        pu.bx.push_back({xs, x - 1});
        x += 1;
    }
    std::string keys;  // the buttons' first letters
    for (auto& b : w.buttons) keys += (keys.empty() ? "" : " ") + b.substr(0, 1);
    bool srch = !w.search.empty();
    text(pu.x0 + 2, hint_y, xm, pu.filtering ? (srch ? "type what to look for · Enter searches · Esc clears" : "type to filter · Enter keeps it · Esc clears") :
         std::string(tr ? "↑↓ Enter · Tab tabs" : "↑↓ Enter picks") + (sr ? (tr ? " · ←→ vol" : " · ←→ volume") : "") + (fr ? (srch ? " · / search" : " · / filter") : "") + (keys.empty() ? "" : " · " + keys) + " · Esc", dim, false);
}

void Tui::pwin_key(int k, uint32_t ch) {
    const Core::PWin& w = c.pwin;
    auto vis = pwin_vis();
    int n = (int)vis.size(), at = (int)(std::find(vis.begin(), vis.end(), pu.cur) - vis.begin());
    auto go = [&](int to) { if (n) pu.cur = vis[(size_t)std::max(0, std::min(n - 1, to))]; };
    auto fix = [&] {  // the filter changed: the cursor stays on its row if it still shows, else the first
        auto v = pwin_vis();
        if (v.empty()) pu.cur = 0;
        else if (std::find(v.begin(), v.end(), pu.cur) == v.end()) pu.cur = v[0], pu.top = 0;
    };
    bool bs = k == TB_KEY_BACKSPACE || k == TB_KEY_BACKSPACE2, arrow = k == TB_KEY_ARROW_DOWN || k == TB_KEY_ARROW_UP || k == TB_KEY_PGDN || k == TB_KEY_PGUP;
    if (pu.filtering && !arrow) {
        if (k == TB_KEY_ESC) pu.q.clear(), pu.filtering = false, fix();
        else if (k == TB_KEY_ENTER) {
            pu.filtering = false;
            if (!w.search.empty()) c.pwin_act("search", 0, 0, to_utf8(pu.q));  // a search field: the plugin looks it up
        }
        else if (bs) { if (!pu.q.empty()) pu.q.pop_back(); fix(); }
        else if (ch || k == TB_KEY_SPACE) pu.q += ch ? (char32_t)ch : U' ', fix();
        return;
    }
    if (ch == ' ') k = TB_KEY_SPACE, ch = 0;
    if (k == TB_KEY_ESC) {
        if (!pu.q.empty()) pu.q.clear(), fix();
        else c.pwin_act("close", 0), pu = Tui::PWinUi{};
    }
    else if (k == TB_KEY_ARROW_DOWN) go(at >= n ? 0 : at + 1);
    else if (k == TB_KEY_ARROW_UP) go(at >= n ? 0 : at - 1);
    else if (k == TB_KEY_PGDN) go(at + std::max(1, pu.rows));
    else if (k == TB_KEY_PGUP) go(at - std::max(1, pu.rows));
    else if (w.slider >= 0 && (k == TB_KEY_ARROW_LEFT || k == TB_KEY_ARROW_RIGHT || ch == '+' || ch == '=' || ch == '-'))
        c.pwin_act("slider", w.slider + (k == TB_KEY_ARROW_RIGHT || ch == '+' || ch == '=' ? 1 : -1) * w.slider_step);
    else if (k == TB_KEY_ENTER) { if (pu.cur) c.pwin_act("pick", pu.cur); }
    else if (k == TB_KEY_TAB && !w.tabs.empty()) c.pwin_act("tab", w.tab % (int)w.tabs.size() + 1);
    else if (bs && !w.tabs.empty()) c.pwin_act("back", 0);  // one level up, for a plugin that has levels
    else if (ch >= '1' && ch <= '9' && (int)(ch - '0') <= (int)w.tabs.size()) c.pwin_act("tab", (int)(ch - '0'));
    else if (ch == '/' && (w.items.size() >= 6 || !w.search.empty())) pu.filtering = true;
    else if (ch) {  // a button's first letter
        for (size_t i = 0; i < w.buttons.size(); i++) {
            std::u32string u = from_utf8(w.buttons[i]);
            if (!u.empty() && (char32_t)tolower((int)u[0]) == (char32_t)tolower((int)ch)) { c.pwin_act("button", pu.cur, (int)i + 1); break; }
        }
    }
}

void Tui::pwin_mouse(int k, int x, int y) {
    const Core::PWin& w = c.pwin;
    if (w.slider >= 0 && y == pu.sy) {  // the slider row: the wheel steps it, a click sets it
        if (k == TB_KEY_MOUSE_WHEEL_UP || k == TB_KEY_MOUSE_WHEEL_DOWN) c.pwin_act("slider", w.slider + (k == TB_KEY_MOUSE_WHEEL_UP ? 1 : -1) * w.slider_step);
        else if (k == TB_KEY_MOUSE_LEFT && x >= pu.bar0 && x < pu.bar0 + pu.barn) {
            int v = (int)((long long)(x - pu.bar0 + 1) * w.slider_max / pu.barn);
            c.pwin_act("slider", (v + w.slider_step / 2) / w.slider_step * w.slider_step);
        }
        return;
    }
    if (k == TB_KEY_MOUSE_WHEEL_UP || k == TB_KEY_MOUSE_WHEEL_DOWN) { pwin_key(k == TB_KEY_MOUSE_WHEEL_UP ? TB_KEY_ARROW_UP : TB_KEY_ARROW_DOWN, 0); return; }
    if (k != TB_KEY_MOUSE_LEFT) return;
    if (y == pu.ty)  // the tabs
        for (size_t i = 0; i < pu.tx.size(); i++)
            if (x >= pu.tx[i].first && x <= pu.tx[i].second) { c.pwin_act("tab", (int)i + 1); return; }
    if (y == pu.fy && x > pu.x0 && x < pu.x0 + pu.w - 1) { pu.filtering = true; return; }  // the filter field
    if (y == pu.btn_y) {
        for (size_t i = 0; i < pu.bx.size(); i++)
            if (x >= pu.bx[i].first && x <= pu.bx[i].second) { c.pwin_act("button", pu.cur, (int)i + 1); return; }
        return;
    }
    auto vis = pwin_vis();
    int i = y < pu.list_y ? -1 : (y - pu.list_y) / std::max(1, pu.rowh);
    if (i < 0 || i >= pu.rows || pu.top + i >= (int)vis.size() || x <= pu.x0 || x >= pu.x0 + pu.w - 1) return;
    int row = vis[(size_t)(pu.top + i)];
    long long now = now_ms();
    bool again = pu.cur == row && now - pu.click_ms < 500;
    pu.click_ms = now, pu.cur = row;
    if (again) pwin_key(TB_KEY_ENTER, 0);
}

// a click previews a theme (again, or a double click: keeps it); the wheel moves
void Tui::theme_mouse(int k, int x, int y) {
    if (k == TB_KEY_MOUSE_WHEEL_UP || k == TB_KEY_MOUSE_WHEEL_DOWN) { theme_key(k == TB_KEY_MOUSE_WHEEL_UP ? TB_KEY_ARROW_UP : TB_KEY_ARROW_DOWN, 0); return; }
    int i = y - tw.list_y;
    if (k != TB_KEY_MOUSE_LEFT || i < 0 || i >= tw.rows || x <= tw.x0 || x >= tw.x0 + tw.w - 1) return;
    int j = tw.top + i;
    if (j >= (int)theme_list().size()) return;
    long long now = now_ms();
    bool again = tw.sel == j && now - tw.click_ms < 500;
    tw.click_ms = now;
    if (again) { theme_key(TB_KEY_ENTER, 0); return; }
    int d = j - tw.sel;
    tw.sel = j - (d > 0 ? 1 : d < 0 ? -1 : 0);  // one step from it, so theme_key's move lands there and previews
    if (d) theme_key(d > 0 ? TB_KEY_ARROW_DOWN : TB_KEY_ARROW_UP, 0);
}


// ---- /ban, /bans: the list box ----------------------------------------------------------------------------
struct BanRow { const BanEntry* e; time_t until; };  // until: when our /tempban lifts it (0: never)

// the rows the box shows: the filter's matches, in the sort order (t newest first, m mask, s who set it)
static std::vector<BanRow> ban_view(const Server* s, char sort, bool rev, const std::string& q) {
    std::vector<BanRow> v;
    for (auto& e : s->bl.v) {
        if (!q.empty() && lower(e.mask + " " + e.setter).find(q) == std::string::npos) continue;
        time_t until = 0;
        for (auto& t : s->tempbans)
            if (s->bl.mode == 'b' && t.mask == e.mask && s->lower(t.chan) == s->lower(s->bl.chan)) until = t.until;
        v.push_back({&e, until});
    }
    auto who = [](const BanEntry* e) { return lower(e->setter.substr(0, e->setter.find('!'))); };
    std::stable_sort(v.begin(), v.end(), [&](const BanRow& a, const BanRow& z) {
        bool lt = sort == 'm' ? lower(a.e->mask) < lower(z.e->mask) : sort == 's' ? who(a.e) < who(z.e) : a.e->ts > z.e->ts;
        bool gt = sort == 'm' ? lower(z.e->mask) < lower(a.e->mask) : sort == 's' ? who(z.e) < who(a.e) : z.e->ts > a.e->ts;
        return rev ? gt : lt;
    });
    return v;
}

static std::string ban_ago(long s) { return s < 60 ? "<1m" : s < 3600 ? std::to_string(s / 60) + "m" : s < 86400 ? std::to_string(s / 3600) + "h" : std::to_string(s / 86400) + "d"; }
static const char* ban_kind_name(char k) { return k == 'e' ? "Exceptions" : k == 'I' ? "Invites" : k == 'q' ? "Quiets" : "Bans"; }
static const char* ban_noun(char k) { return k == 'e' ? "exception" : k == 'I' ? "invite exception" : k == 'q' ? "quiet" : "ban"; }

//   ┌ Bans · #linux ─────────────────────────────────────────────────────┐
//   │  Bans   Exceptions   Invites                                       │
//   │ filter:                                                            │
//   │ Mask ▼                                    Set by         Set       │
//   │ ────────────────────────────────────────────────────────────────── │
//   │ ▸ *!*@bad.host                            alice          3d ago    │
//   │   troll!*@*  (lifts in 9m)                bob            2h ago    │
//   │ 2 bans · u unban · a add ...                                       │
void Tui::draw_bans(int W, int H) {
    const Theme& T = c.look(false);
    Buffer* b = nullptr;
    for (auto& x : c.bufs)
        if (x->id == bw.buf) b = x.get();
    if (!b || !b->srv) { bw.open = false, c.dirty = true; return; }
    Server* s = b->srv;
    const BanList& bl = s->bl;
    bool mine = s->lower(bl.chan) == s->lower(b->name), can = c.ban_rank(b);
    Style bs = sty(T, "menu", "nicklist"), ls = sty(T, "menu_border", "separator"), hs = bs, dim = sty(T, "timestamp", "menu"),
          sel = sty(T, "menu_sel", "tab_active"), fs = T.get("input"), es = T.get("error");
    for (Style* st : {&ls, &dim, &es}) st->bg = bs.bg;
    hs.attr |= A_BOLD;
    std::string q = to_utf8(bw.q);
    auto v = mine ? ban_view(s, bw.sort, bw.rev, lower(q)) : std::vector<BanRow>{};
    int n = (int)v.size(), idx = -1;
    for (int i = 0; i < n; i++)
        if (v[(size_t)i].e->mask == bw.sel) idx = i;
    if (idx < 0 && n) idx = 0, bw.sel = v[0].e->mask;
    if (!n) bw.sel.clear();
    bw.rows = std::max(1, std::min({std::max(n, 1), H - 10, 16}));
    bw.w = std::min(W - 2, 80), bw.x0 = (W - bw.w) / 2;
    int bh = bw.rows + 8, y0 = std::max(0, (H - bh) / 2 - 1), x1 = bw.x0 + bw.w, inner = bw.w - 4;
    bw.tab_y = y0 + 1, bw.filter_y = y0 + 2, bw.head_y = y0 + 3, bw.list_y = y0 + 5;
    if (idx >= 0 && idx < bw.top) bw.top = idx;
    if (idx >= bw.top + bw.rows) bw.top = idx - bw.rows + 1;
    bw.top = std::max(0, std::min(bw.top, std::max(0, n - bw.rows)));
    gutters(W, bw.x0, x1, y0, y0 + bh, T.get("default"));
    for (int yy = y0; yy < y0 + bh; yy++) fill(yy, bw.x0, x1, bs);
    for (int xx = bw.x0; xx < x1; xx++) cell(xx, y0, U'─', ls), cell(xx, y0 + bh - 1, U'─', ls), cell(xx, y0 + 4, U'─', ls);
    for (int yy = y0; yy < y0 + bh; yy++) cell(bw.x0, yy, U'│', ls), cell(x1 - 1, yy, U'│', ls);
    cell(bw.x0, y0, U'┌', ls), cell(x1 - 1, y0, U'┐', ls), cell(bw.x0, y0 + bh - 1, U'└', ls), cell(x1 - 1, y0 + bh - 1, U'┘', ls);
    cell(bw.x0, y0 + 4, U'├', ls), cell(x1 - 1, y0 + 4, U'┤', ls);
    if (auto cb = box_title(T, bw.x0, x1 - 1, y0, ban_kind_name(bl.mode) + (" · " + b->name), hs); cb.first >= 0) xbtn = {y0, cb.first, cb.second, 'b'};
    // the lists this server has, one is open
    bw.kinds = ban_kinds(s), bw.tabs.clear();
    int tx = bw.x0 + 2;
    for (char kd : bw.kinds) {
        std::string label = std::string(" ") + ban_kind_name(kd) + " ";
        int ex = text(tx, bw.tab_y, x1 - 2, label, kd == bl.mode ? sel : bs, false);
        bw.tabs.push_back({tx, ex});
        tx = ex + 1;
    }
    int fx = text(bw.x0 + 2, bw.filter_y, x1 - 2, "filter: ", hs, false);
    fill(bw.filter_y, fx, x1 - 2, fs);
    int ex = text(fx, bw.filter_y, x1 - 2, q.empty() && !bw.filtering ? "" : q, fs, false);
    if (q.empty() && !bw.filtering) text(fx, bw.filter_y, x1 - 2, "/ to filter by mask or who set it", Style{dim.fg, fs.bg, 0}, false);
    // the columns: mask, set by, set
    const int setter_w = 14, age_w = 9;
    bw.mask_w = inner - setter_w - age_w - 2;
    int sx = bw.x0 + 2 + bw.mask_w + 1, ax = sx + setter_w + 1;
    auto arrow = [&](char col) { return bw.sort == col ? (bw.rev ? " ▲" : " ▼") : ""; };
    text(bw.x0 + 2, bw.head_y, sx - 1, std::string("Mask") + arrow('m'), hs, false);
    text(sx, bw.head_y, ax - 1, std::string("Set by") + arrow('s'), hs, false);
    text(ax, bw.head_y, x1 - 2, std::string("Set") + arrow('t'), hs, false);
    time_t now = time(nullptr);
    for (int i = 0; i < bw.rows && bw.top + i < n; i++) {
        const BanRow& r = v[(size_t)(bw.top + i)];
        bool on = bw.top + i == idx;
        Style st = on ? sel : bs, dm = on ? sel : dim;
        int y = bw.list_y + i;
        fill(y, bw.x0 + 1, x1 - 1, st);
        int mx = text(bw.x0 + 2, y, bw.x0 + 2 + bw.mask_w, (on ? "▸ " : "  ") + r.e->mask, st, false);
        if (r.until && mx + 1 < sx - 1) text(mx + 1, y, sx - 1, "(lifts in " + ban_ago((long)(r.until - now)) + ")", dm, false);
        text(sx, y, ax - 1, r.e->setter.substr(0, r.e->setter.find('!')), dm, false);
        if (r.e->ts > 0) text(ax, y, x1 - 2, ban_ago((long)(now - r.e->ts)) + " ago", dm, false);
    }
    int sy = bw.list_y + bw.rows;
    if (!mine) text(bw.x0 + 2, sy, x1 - 2, "Another window is using the list: r loads this one", es, false);
    else if (bl.loading) text(bw.x0 + 2, sy, x1 - 2, "Loading…", dim, false);
    else if (!bl.err.empty()) text(bw.x0 + 2, sy, x1 - 2, bl.err, es, false);
    else {
        std::string what = std::to_string(n) + " " + ban_noun(bl.mode) + (n == 1 ? "" : "s");
        if (n != (int)bl.v.size()) what += " of " + std::to_string(bl.v.size());
        text(bw.x0 + 2, sy, x1 - 2, what + (can ? "" : " · read only: changing a list takes halfop or more"), dim, false);
    }
    text(bw.x0 + 2, sy + 1, x1 - 2,
         can ? "↑↓ · u unban · a add · r reload · s sort · Tab list · / filter · Esc close" : "↑↓ · r reload · s sort · Tab list · / filter · Esc close",
         dim, false);
    if (bw.filtering) put_cursor(ex, bw.filter_y);
    else tb_hide_cursor();
    if (!bw.ask.empty())
        draw_dialog(W, H, std::string(bl.mode == 'b' ? "Unban" : "Remove"), wrap_words((bl.mode == 'b' ? "Unban " : "Remove ") + bw.ask + " on " + b->name + "?", 44), "", false, U"", 0, "y: yes · n or Esc: no", 0, 0, 'u');
    if (bw.adding)
        draw_dialog(W, H, std::string("Add to the ") + ban_noun(bl.mode) + " list", wrap_words("A nick gives nick!*@*; or a mask like *!*@host.", 44), "Mask or nick", false, bw.ed,
                    bw.ed.size(), "Enter: add · Esc: cancel", 0, 0, 'm');
}

void Tui::bans_key(int k, uint32_t ch) {
    Buffer* b = nullptr;
    for (auto& x : c.bufs)
        if (x->id == bw.buf) b = x.get();
    if (!b || !b->srv) { bw.open = false; return; }
    Server* s = b->srv;
    const char mode = s->bl.mode;
    bool can = c.ban_rank(b);
    if (ch == ' ') k = TB_KEY_SPACE, ch = 0;  // termbox gives Space as a character
    bool bs = k == TB_KEY_BACKSPACE || k == TB_KEY_BACKSPACE2;
    auto edit = [&](std::u32string& t) {
        if (bs) { if (!t.empty()) t.pop_back(); }
        else if (k == TB_KEY_SPACE) t += U' ';
        else if (ch) t += ch;
    };
    if (!bw.ask.empty()) {  // unban it? (y/n)
        std::string m = bw.ask;
        bw.ask.clear();
        if (ch == 'y' || ch == 'Y') c.ban_edit(b, mode, false, m);
        return;
    }
    if (bw.adding) {
        if (k == TB_KEY_ESC) bw.adding = false;
        else if (k == TB_KEY_ENTER) {
            std::string m = trim(to_utf8(bw.ed));
            bw.adding = false;
            if (!m.empty()) c.ban_edit(b, mode, true, m);
        } else if (k != TB_KEY_SPACE) edit(bw.ed);  // a mask has no spaces
        return;
    }
    if (bw.filtering) {
        if (k == TB_KEY_ESC) bw.q.clear(), bw.filtering = false;
        else if (k == TB_KEY_ENTER) bw.filtering = false;
        else edit(bw.q), bw.top = 0;
        return;
    }
    auto v = s->lower(s->bl.chan) == s->lower(b->name) ? ban_view(s, bw.sort, bw.rev, lower(to_utf8(bw.q))) : std::vector<BanRow>{};
    int n = (int)v.size(), idx = -1;
    for (int i = 0; i < n; i++)
        if (v[(size_t)i].e->mask == bw.sel) idx = i;
    auto go = [&](int to) { if (n) bw.sel = v[(size_t)std::max(0, std::min(n - 1, to))].e->mask; };
    auto kind = [&](size_t to) {
        if (to < bw.kinds.size()) c.ban_list(b, bw.kinds[to], 3), bw.sel.clear(), bw.top = 0;
    };
    if (k == TB_KEY_ESC || ch == 'q') bw.open = false;
    else if (k == TB_KEY_ARROW_DOWN) go(idx + 1);
    else if (k == TB_KEY_ARROW_UP) go(idx - 1);
    else if (k == TB_KEY_PGDN) go(idx + bw.rows);
    else if (k == TB_KEY_PGUP) go(idx - bw.rows);
    else if (k == TB_KEY_HOME) go(0);
    else if (k == TB_KEY_END) go(n - 1);
    else if ((ch == 'u' || k == TB_KEY_ENTER || k == TB_KEY_DELETE) && can && idx >= 0) bw.ask = bw.sel;
    else if (ch == 'a' && can) bw.adding = true, bw.ed.clear();
    else if (ch == 'r') c.ban_list(b, mode, 3);
    else if (ch == 's') bw.sort = bw.sort == 't' ? 'm' : bw.sort == 'm' ? 's' : 't', bw.rev = false;
    else if (k == TB_KEY_TAB) kind((bw.kinds.find(mode) + 1) % std::max<size_t>(1, bw.kinds.size()));
    else if (ch == '/') bw.filtering = true;
}

// a click picks a list, a column to sort by, or a row (again, or a double click: unban); the wheel moves
void Tui::bans_mouse(int k, int x, int y) {
    if (!bw.ask.empty() || bw.adding) return;
    if (k == TB_KEY_MOUSE_WHEEL_UP || k == TB_KEY_MOUSE_WHEEL_DOWN) { bans_key(k == TB_KEY_MOUSE_WHEEL_UP ? TB_KEY_ARROW_UP : TB_KEY_ARROW_DOWN, 0); return; }
    if (k != TB_KEY_MOUSE_LEFT || x <= bw.x0 || x >= bw.x0 + bw.w - 1) return;
    Buffer* b = nullptr;
    for (auto& e : c.bufs)
        if (e->id == bw.buf) b = e.get();
    if (!b || !b->srv) return;
    Server* s = b->srv;
    if (y == bw.tab_y) {
        for (size_t i = 0; i < bw.tabs.size(); i++)
            if (x >= bw.tabs[i].first && x < bw.tabs[i].second && bw.kinds[i] != s->bl.mode) c.ban_list(b, bw.kinds[i], 3), bw.sel.clear(), bw.top = 0;
        return;
    }
    if (y == bw.filter_y) { bw.filtering = true; return; }
    int sx = bw.x0 + 2 + bw.mask_w + 1, ax = sx + 15;
    if (y == bw.head_y) {
        char col = x < sx ? 'm' : x < ax ? 's' : 't';
        bw.rev = col == bw.sort && !bw.rev, bw.sort = col;
        return;
    }
    int i = y - bw.list_y;
    if (i < 0 || i >= bw.rows || bw.filtering) return;
    auto v = s->lower(s->bl.chan) == s->lower(b->name) ? ban_view(s, bw.sort, bw.rev, lower(to_utf8(bw.q))) : std::vector<BanRow>{};
    if (bw.top + i >= (int)v.size()) return;
    long long now = now_ms();
    bool again = bw.sel == v[(size_t)(bw.top + i)].e->mask && now - bw.click_ms < 500;
    bw.sel = v[(size_t)(bw.top + i)].e->mask, bw.click_ms = now;
    if (again) bans_key(TB_KEY_ENTER, 0);
}

// a click picks a category or a key (again, or a double click: edits it); the wheel moves
void Tui::settings_mouse(int k, int x, int y) {
    if (k == TB_KEY_MOUSE_WHEEL_UP || k == TB_KEY_MOUSE_WHEEL_DOWN) {
        if (!sw.editing) settings_key(0, k == TB_KEY_MOUSE_WHEEL_UP ? TB_KEY_ARROW_UP : TB_KEY_ARROW_DOWN, 0), settings_key(0, k == TB_KEY_MOUSE_WHEEL_UP ? TB_KEY_ARROW_UP : TB_KEY_ARROW_DOWN, 0);
        return;
    }
    if (k != TB_KEY_MOUSE_LEFT || sw.editing || sw.plist) return;  // perform's list: the keys
    if (y == sw.y0 + 1) { sw.searching = true; return; }  // the search line
    int i = y - sw.list_y;
    if (i < 0 || i >= sw.h - 6 || x <= sw.x0 || x >= sw.x0 + sw.w - 1) return;
    auto all = c.setting_rows();
    std::vector<std::string> cats;
    std::vector<int> counts;
    std::vector<const Core::SetRow*> rows;
    set_view(all, lower(to_utf8(sw.q)), sw.cat, cats, counts, rows);
    if (x < sw.x0 + 2 + sw.nav_w) {  // a category
        if (sw.ctop + i < (int)cats.size() && counts[(size_t)(sw.ctop + i)]) sw.cat = cats[(size_t)(sw.ctop + i)], sw.sel = sw.top = 0, sw.focus = 0;
        return;
    }
    // the row under the pointer, counting the section headers the way draw_settings lays them out
    std::string last;
    int line = 0, want = sw.top + i;
    for (int j = 0; j < (int)rows.size(); j++) {
        if (rows[(size_t)j]->head != last) last = rows[(size_t)j]->head, line++;
        if (line == want) {
            long long now = now_ms();
            bool again = sw.focus == 1 && sw.sel == j && now - sw.click_ms < 500;
            sw.sel = j, sw.focus = 1, sw.click_ms = now;
            if (again) settings_key(0, TB_KEY_ENTER, 0);
            return;
        }
        line++;
    }
}

// ---- keyboard --------------------------------------------------------------
// vendor/termbox2.c: CSI u / modifyOtherKeys decoding, TB_MOD_SUPER = Cmd
extern "C" int isekur_extract_pre(struct tb_event*, size_t*);
extern "C" int isekur_extract_post(struct tb_event*, size_t*);

static std::string keyspec(int mod, int key, uint32_t ch) {
    std::string s = std::string(mod & TB_MOD_SUPER ? "D-" : "") + (mod & TB_MOD_ALT ? "M-" : "");
    if (ch) { utf8_put(s, ch); return s; }
    if (key >= TB_KEY_ARROW_RIGHT && key <= TB_KEY_ARROW_UP) {
        if (mod & TB_MOD_CTRL) s += "C-";
        if (mod & TB_MOD_SHIFT) s += "S-";
    }
    switch (key) {
    case TB_KEY_ARROW_UP: return s + "Up";
    case TB_KEY_ARROW_DOWN: return s + "Down";
    case TB_KEY_ARROW_LEFT: return s + "Left";
    case TB_KEY_ARROW_RIGHT: return s + "Right";
    case TB_KEY_PGUP: return s + "PgUp";
    case TB_KEY_PGDN: return s + "PgDn";
    case TB_KEY_HOME: return s + "Home";
    case TB_KEY_END: return s + "End";
    case TB_KEY_INSERT: return s + "Insert";
    case TB_KEY_DELETE: return s + "Delete";
    case TB_KEY_ENTER: return s + "Enter";
    case TB_KEY_TAB: return s + "Tab";
    case TB_KEY_BACKSPACE: case TB_KEY_BACKSPACE2: return s + "Backspace";
    case TB_KEY_ESC: return s + "Esc";
    case TB_KEY_SPACE: return s + " ";
    }
    if (key >= TB_KEY_F12 && key <= TB_KEY_F1) return s + "F" + std::to_string(TB_KEY_F1 - key + 1);
    if (key >= 1 && key <= 26) return s + "C-" + (char)('a' + key - 1);
    return s + "?";
}

bool Tui::guide_tab() const { return c.active && !c.active->srv && c.active->name == "*guide"; }

bool Tui::init(std::string& err, int fd) {
    int r = fd < 0 ? tb_init() : tb_init_fd(fd);
    if (r != TB_OK) { err = tb_strerror(r) + std::string(" (") + std::to_string(r) + ")"; return false; }
    tb_set_func(TB_FUNC_EXTRACT_PRE, isekur_extract_pre);
    tb_set_func(TB_FUNC_EXTRACT_POST, isekur_extract_post);
    nicklist = c.theme.lay("nicklist", "on") != "off";
    apply_cfg();
    return true;
}

// mouse, colors and keys from the config: at start, and after /set keys.* or /reload
void Tui::apply_cfg() {
    mouse_on = ini_get(c.cfg, "general", "mouse", "on") != "off";
    std::string th = ini_get(c.cfg, "general", "timestamp_hover", "off");
    ts_hover = th == "term" || th == "both" || th == "on";  // on: what it was before web/term/both
    set_mouse();
    std::string cs = ini_get(c.cfg, "general", "cursor", "blink");
    g_cur_style = cs == "off" ? 0 : cs == "block" ? 1 : 2;
    std::string mode = ini_get(c.cfg, "general", "colors", "auto");
    const char* ct = getenv("COLORTERM");
    truecolor = mode == "truecolor" ||
                (mode == "auto" && ct && (std::string(ct) == "truecolor" || std::string(ct) == "24bit"));
    g_true = truecolor;
    tb_set_output_mode(truecolor ? TB_OUTPUT_TRUECOLOR : TB_OUTPUT_256);
    c.no_color_check();
    load_keys();
}

void Tui::load_keys() {
    keymap.clear();
    for (int i = 1; i <= 10; i++) keymap["M-" + std::to_string(i % 10)] = "tab_" + std::to_string(i);
    keymap["M-Down"] = keymap["C-n"] = "next_tab";
    keymap["M-Up"] = keymap["C-p"] = "prev_tab";
    keymap["M-Right"] = "next_server";
    keymap["M-Left"] = "prev_server";
    keymap["M-f"] = "next_server";  // Ghostty/Terminal.app/iTerm send Option+Right/Left as Esc f / Esc b
    keymap["M-b"] = "prev_server";
    keymap["C-a"] = keymap["M-a"] = "next_active";
    keymap["C-r"] = keymap["M-r"] = keymap["D-r"] = "mark_all_read";  // D-r: Cmd+R, where the terminal reports Cmd (CSI u)
    keymap["C-x"] = keymap["M-x"] = "next_server";
    keymap["C-w"] = keymap["M-w"] = "close_tab";
    keymap["M-c"] = "copy_input";
    keymap["PgUp"] = "scroll_up";
    keymap["PgDn"] = "scroll_down";
    // Alt is the same key in the webchat. A Mac terminal sends Option+L as "¬" unless Option is set to be Alt:
    // no "¬"-style twins (they'd stop ø, ß, å... being typed), a note says what to set instead (option_note)
    keymap["C-l"] = keymap["M-l"] = keymap["F2"] = "toggle_nicklist";
    keymap["C-t"] = keymap["M-t"] = "toggle_topic";
    keymap["F3"] = "toggle_mouse";
    keymap["C-g"] = keymap["M-j"] = "find_tab";
    keymap["F6"] = keymap["M-o"] = "other_pane";  // /split
    keymap["M-q"] = "quit";  // it asks first
    keymap["F1"] = keymap["M-k"] = "keys";
    keymap["M-s"] = "lock";
    keymap["M-d"] = keymap["C-z"] = "detach";  // C-z: detach (still connected) where a shell would suspend
    // [keys] action = spec [spec...]
    auto ks = c.cfg.find("keys");
    if (ks != c.cfg.end())
        for (auto& [act, specs] : ks->second)
            for (auto& sp : split(specs, ' ')) keymap[sp] = act;
}

void Tui::shutdown() { tb_shutdown(); }

void Tui::set_mouse() {
    tb_set_input_mode(TB_INPUT_ALT | (mouse_on ? TB_INPUT_MOUSE : 0));
    // every move (1003): the link under the pointer is underlined, and timestamp_hover shows the time of the line under it
    if (mouse_on) tb_sendf("\x1b[?1003h");
    if (!mouse_on) g_hover_x = g_hover_y = -1, fold_hov = false;
    if (!mouse_on || !ts_hover) hover_x = hover_y = -1, hover_line = fade_line = nullptr;
}

// timestamp_hover: the line under the mouse changed; the one left fades out
void Tui::hover_set(const void* l) {
    if (l == hover_line) return;
    long long t = now_ms();
    if (hover_line) fade_line = hover_line, fade_at = t;
    hover_line = l, hover_at = t, ts_anim_until = t + 1500;
}

// how much of a line's time shows: in over 150ms under the mouse, out over 1.5s after
double Tui::hover_alpha(const void* l) const {
    long long t = now_ms();
    if (l == hover_line) return std::min(1.0, (t - hover_at) / 150.0);
    if (l == fade_line) return std::max(0.0, 1 - (t - fade_at) / 1500.0);
    return 0;
}

void Tui::fds(int& tty, int& resize) { tb_get_fds(&tty, &resize); }

void Tui::sync_input() {
    int id = c.active ? c.active->id : 0;
    if (id == input_id) return;
    if (!input.empty()) drafts[input_id] = {input, cur};
    else drafts.erase(input_id);
    auto it = drafts.find(id);
    if (it != drafts.end()) input = it->second.first, cur = it->second.second, drafts.erase(it);
    else input.clear(), cur = 0;
    if (!hist.empty()) hists[input_id] = std::move(hist);
    auto h = hists.find(id);
    if (h != hists.end()) hist = std::move(h->second), hists.erase(h);
    else hist.clear();
    hpos = hist.size(), input_id = id;
}

void Tui::on_readable() {
    tb_event ev;
    // An Enter followed by more input in the same read is a paste: it becomes
    // a newline in the input line instead of sending each line as it comes.
    bool enter = false, keyed = false;  // keyed: a key came (typing goes out only then, or on a tab change)
    int nkeys = 0;
    while (tb_peek_event(&ev, 0) == TB_OK) {
        nkeys += ev.type == TB_EVENT_KEY;
        sync_input();  // a key may have switched tabs: the next one types into that tab's line
        if ((ev.type == TB_EVENT_KEY || ev.type == TB_EVENT_MOUSE) && c.locked && !c.screen_by.empty() && !peek_at) {  // locked: the password box over the screensaver
            c.wake(), peek_at = now_ms(), lock_in.clear(), c.dirty = true;
            continue;
        }
        if ((ev.type == TB_EVENT_KEY || ev.type == TB_EVENT_MOUSE) && c.wake()) { c.dirty = true; continue; }  // it only ends the screensaver
        keyed |= ev.type == TB_EVENT_KEY;
        if (enter) {  // an Enter with more coming is a paste's line break: a newline in the line, a space in a question's field
            if (!c.choices.empty()) cu.text.insert(std::min(cu.cur, cu.text.size()), 1, U' '), cu.cur++;
            else input.insert(cur++, 1, U'\n');
            enter = false;
        }
        if ((c.locked || sw.open || tw.open || fw.open || kw.open || pw.open || c.pwin.open || lw.open || bw.open) && ev.type == TB_EVENT_KEY) { key(ev.mod, ev.key, ev.ch); c.dirty = true; continue; }  // a paste is keys too (the settings window: Enter edits)
        bool nl = ev.type == TB_EVENT_KEY && !(ev.mod & TB_MOD_ALT) && (ev.key == TB_KEY_ENTER || ev.key == TB_KEY_CTRL_J);
        if (nl && (!c.choices.empty() || (paste.empty() && confirm.empty() && menu.items.empty() && sugg_sel < 0))) { enter = true; continue; }  // a highlighted suggestion: key() takes it
        if (ev.type == TB_EVENT_KEY) key(ev.mod, ev.key, ev.ch);
        else if (ev.type == TB_EVENT_MOUSE) mouse(ev.mod, ev.key, ev.x, ev.y);
        c.dirty = true;
    }
    if (enter) {
        if (!c.choices.empty()) choice_key(0, TB_KEY_ENTER, 0);  // the last Enter of a read is a key: it answers the question
        else submit();
        c.dirty = true;
    }
    // a file dropped on the terminal is its path typed in one go (a few characters at a time can't be a hand): when the whole line is the path
    // of a file and a plugin takes files, it goes to it without a second Enter
    if (nkeys >= 6 && !enter && !input.empty() && c.active && !c.locked && c.choices.empty() && confirm.empty() && paste.empty() &&
        !(sw.open || tw.open || fw.open || kw.open || pw.open || c.pwin.open || lw.open || bw.open) && !c.asking(c.active) &&
        c.plugin_hooks("drop", c.active->srv, c.active->type == BT::CHANNEL || c.active->type == BT::QUERY ? c.active->name : "")) {
        std::string line = to_utf8(input);
        if (!dropped_paths(line).empty()) input.clear(), cur = 0, c.upload_cmd(c.active, line), c.dirty = true;
    }
    sync_input();
    // +typing: the input line has text for the tab on screen; a tab left behind is told "done"
    int id = c.active ? c.active->id : 0;
    if (id != typed_id)
        for (auto& b : c.bufs)
            if (b->id == typed_id) c.typed(b.get(), "");
    bool moved = id != typed_id;
    typed_id = id;
    // only after keys here (or a tab change): a mouse move or a resize must not say "done" while you type in the webchat
    if (c.active && (keyed || moved)) c.typed(c.active, c.locked || sw.open || tw.open || fw.open || pw.open || c.pwin.open || lw.open || bw.open ? "" : to_utf8(input));
}

bool Tui::action(const std::string& a) {
    int n = c.index_of(c.active), sz = (int)c.bufs.size();
    auto at = [&](int i) { return c.bufs[(size_t)((i % sz + sz) % sz)].get(); };
    if (a == "copy_input") { copy_input(0, input.size()); return true; }
    if (a == "next_tab") c.select(at(n + 1));
    else if (a == "prev_tab") c.select(at(n - 1));
    else if (a.rfind("tab_", 0) == 0) {
        int t = atoi(a.c_str() + 4) - 1;
        if (t >= 0 && t < sz) c.select(at(t));
    } else if (a == "next_active") {
        Buffer* best = nullptr;
        for (int i = 1; i < sz; i++)
            if (at(n + i)->activity > (best ? best->activity : 0)) best = at(n + i);
        if (best) c.select(best);
    } else if (a == "mark_all_read") c.mark_all_read();
    else if (a == "next_server" || a == "prev_server") {  // the top level: server tabs and the ones of no network (cmd)
        int d = a == "next_server" ? 1 : -1;
        for (int i = 1; i < sz; i++)
            if (at(n + d * i)->type == BT::STATUS || !at(n + d * i)->srv) { c.select(at(n + d * i)); break; }
    } else if (a == "close_tab") c.close(c.active);
    else if (a == "scroll_up") c.active->scroll += chat_height() / 2;
    else if (a == "scroll_down") c.active->scroll = std::max(0, c.active->scroll - chat_height() / 2);
    else if (a == "toggle_nicklist") nicklist = !nicklist;
    else if (a == "toggle_topic") topicbar = !topicbar;
    else if (a == "other_pane") { if (c.split_buf) c.select(c.split_buf); }
    else if (a == "detach") c.detach_req = true;
    else if (a == "lock") c.lock_now(c.active);
    else if (a == "quit") ask_exit("");  // asks first, in the yes/no box (y quits; n, Esc or × don't)
    else if (a == "keys") kw = KeysWin{}, kw.open = true;
    else if (a == "find_tab") fw = FindWin{}, fw.open = true, fw.sel = std::max(0, c.index_of(c.active));
    else if (a == "toggle_mouse") {  // off = the terminal's own text selection works again
        mouse_on = !mouse_on;
        set_mouse();
        c.print(c.active, LK::INFO, std::string("Mouse ") + (mouse_on ? "on (drag copies, Shift+drag is the terminal's selection)" : "off"));
    }
    else return false;
    return true;
}

void Tui::key(int mod, int k, uint32_t ch) {
    if (c.locked) {  // only the password; Alt+D still detaches
        auto it = keymap.find(keyspec(mod, k, ch));
        if (it != keymap.end() && it->second == "detach") { action("detach"); return; }
        if (mod & (TB_MOD_ALT | TB_MOD_SUPER)) return;
        if (peek_at) {  // over the screensaver: Esc puts the box away, a key keeps it up
            if (k == TB_KEY_ESC) { peek_at = 0, lock_in.clear(); return; }
            peek_at = now_ms();
        }
        if (ch) lock_in += (char32_t)ch;
        else if (k == TB_KEY_SPACE) lock_in += U' ';
        else if ((k == TB_KEY_BACKSPACE || k == TB_KEY_BACKSPACE2) && !lock_in.empty()) lock_in.pop_back();
        else if (k == TB_KEY_ESC && lock_in.empty()) c.lock_saver();  // nothing typed: the screensaver, as Alt+S
        else if (k == TB_KEY_ESC || k == TB_KEY_CTRL_U) lock_in.clear();
        else if (k == TB_KEY_ENTER) c.unlock(to_utf8(lock_in)), lock_in.clear();
        return;
    }
    if (!c.choices.empty()) { choice_key(mod, k, ch); return; }  // a plugin's question has the keys
    if (sw.open && confirm.empty() && !c.asking(c.active)) { settings_key(mod, k, ch); return; }  // asking: the password box has the keys, below
    if (tw.open) { if (!(mod & (TB_MOD_ALT | TB_MOD_SUPER))) theme_key(k, ch); return; }
    if (fw.open) { if (!(mod & (TB_MOD_ALT | TB_MOD_SUPER))) find_key(k, ch); return; }
    if (kw.open) { keys_key(k, ch); return; }
    if (pw.open) { if (!(mod & (TB_MOD_ALT | TB_MOD_SUPER))) plugin_key(k, ch); return; }
    if (c.pwin.open) { if (!(mod & (TB_MOD_ALT | TB_MOD_SUPER))) pwin_key(k, ch); return; }
    if (lw.open) { if (!(mod & (TB_MOD_ALT | TB_MOD_SUPER))) logs_key(k, ch); return; }
    if (bw.open) { if (!(mod & (TB_MOD_ALT | TB_MOD_SUPER))) bans_key(k, ch); return; }
    if (!menu.items.empty()) {  // the nick menu has the keys until it closes
        int n = (int)menu.items.size(), d = k == TB_KEY_ARROW_DOWN ? 1 : k == TB_KEY_ARROW_UP ? -1 : 0;
        if (d) {
            do menu.sel = (menu.sel + d + n) % n;
            while (menu.items[(size_t)menu.sel].first.empty());
            return;
        }
        std::string cmd = k == TB_KEY_ENTER ? menu.items[(size_t)menu.sel].second : "";  // other keys close it
        std::string it = menu.items[(size_t)menu.sel].first;
        int mb = menu.buf;
        menu.items.clear();
        if (!cmd.empty() && (cmd[0] == '\x01' || cmd[0] == '\x02')) {  // a link's menu: open it, or copy it
            if (cmd[0] == '\x01') open_link(cmd.substr(1));
            else copy_text(cmd.substr(1));
            return;
        }
        if (!cmd.empty() && cmd[0] == '?') {  // asks first, titled like the item ("Delete server...": "Delete server")
            confirm = cmd.substr(1), confirm_title = it, confirm_buf = mb;
            while (!confirm_title.empty() && confirm_title.back() == '.') confirm_title.pop_back();
        }
        else if (!cmd.empty() && (cmd.back() == ' ' || cmd.back() == ':')) input = from_utf8(cmd), cur = input.size();  // ends in a space: you finish it
        else if (!cmd.empty()) { if (Buffer* t = tab_by_id(mb)) for (auto& l : split(cmd, '\n')) c.exec(t, l); }  // one command a line
        return;
    }
    if (!confirm.empty()) {  // a menu's question: y runs it, anything else drops it
        std::string q;
        q.swap(confirm);
        int cb = confirm_buf;
        confirm_buf = 0;
        if (ch == 'y' || ch == 'Y')
            if (Buffer* t = tab_by_id(cb)) c.exec(t, q.substr(q.find('\n') + 1));
        return;
    }
    if (!paste.empty()) {  // "Send N lines?" - y sends, anything else puts it back to edit
        std::string p;
        p.swap(paste);
        if (ch == 'y' || ch == 'Y') c.exec(c.active, p);
        else input = from_utf8(p), cur = input.size();
        return;
    }
    if (c.asking(c.active)) {  // the password dialog
        if ((k == TB_KEY_ESC || k == TB_KEY_CTRL_C) && !ch && !(mod & TB_MOD_ALT)) {  // Esc (or Ctrl+C) cancels
            input.clear(), cur = 0;
            c.exec(c.active, "");
            return;
        }
        if (c.pass_hidden) cur = input.size();  // no moving inside what you can't see
        if (k == TB_KEY_ARROW_UP || k == TB_KEY_ARROW_DOWN || k == TB_KEY_TAB) return;  // no history or completion in it
    }
    // the suggestion list under the input line takes Up/Down while it shows;
    // Enter or Tab on the highlight puts it in the input line without sending
    // (only Alt/Cmd are excluded: termbox reports Enter and Tab as Ctrl+M / Ctrl+I)
    if (!(mod & (TB_MOD_ALT | TB_MOD_SUPER)) && (k == TB_KEY_ARROW_UP || k == TB_KEY_ARROW_DOWN || (sugg_sel >= 0 && (k == TB_KEY_ENTER || k == TB_KEY_TAB)))) {
        std::vector<Sugg> sg;
        int from = suggest(sg), n = (int)sg.size();
        if (from >= 0 && n > 0) {
            if (k == TB_KEY_ARROW_DOWN) sugg_sel = std::min(sugg_sel + 1, n - 1);
            else if (k == TB_KEY_ARROW_UP) sugg_sel = sugg_sel < 0 ? n - 1 : sugg_sel - 1;  // above the first: none
            else if (sugg_sel < n) {
                std::u32string w = from_utf8(sg[(size_t)sugg_sel].word + (sg[(size_t)sugg_sel].last ? "" : " "));
                input.replace((size_t)from, cur - (size_t)from, w);
                cur = (size_t)from + w.size(), sugg_sel = -1, completing = false;
            }
            return;
        }
    }
    sugg_sel = -1;
    auto it = keymap.find(keyspec(mod, k, ch));
    if (it != keymap.end() && action(it->second)) return;
    if (k != TB_KEY_TAB) completing = false;
    bool bs = k == TB_KEY_BACKSPACE || k == TB_KEY_BACKSPACE2;
    if (mod & TB_MOD_SUPER) {  // Cmd, when the terminal reports it (CSI u)
        if (bs) input.erase(0, cur), cur = 0;
        else if (k == TB_KEY_ARROW_LEFT) cur = 0;
        else if (k == TB_KEY_ARROW_RIGHT) cur = input.size();
        return;
    }
    if ((mod & TB_MOD_ALT) && bs) {  // Option/Alt+Backspace: previous word
        size_t st = cur;
        while (st > 0 && input[st - 1] == U' ') st--;
        while (st > 0 && input[st - 1] != U' ') st--;
        input.erase(st, cur - st), cur = st;
        return;
    }
    if (mod & TB_MOD_ALT) return;  // unbound Alt combos do nothing
    if (input.empty() && guide_tab() && ((ch == 'q' || ch == 'Q') || (k == TB_KEY_ESC && !ch))) { c.guide(c.active, "close"); return; }  // q or Esc closes the guide
    if (input.empty() && k == TB_KEY_ESC && !ch && c.active->srv && c.active->name == "*channels") { c.list_filter(c.active, ""); return; }  // Esc clears the /list filter
    if (input.empty() && k == TB_KEY_ESC && !ch && zen) { c.view_end(); return; }  // Esc leaves the focus view (the zen plugin's)
    if (ch) option_note(ch);
    if (ch || k == TB_KEY_SPACE) { input.insert(cur++, 1, ch ? (char32_t)ch : U' '); return; }
    switch (k) {
    case TB_KEY_ENTER: submit(); break;
    case TB_KEY_BACKSPACE: case TB_KEY_BACKSPACE2: if (cur) input.erase(--cur, 1); break;
    case TB_KEY_DELETE: if (cur < input.size()) input.erase(cur, 1); break;
    case TB_KEY_ARROW_LEFT: if (input.empty() && guide_tab()) c.guide(c.active, "prev"); else if (cur) cur--; break;
    case TB_KEY_ARROW_RIGHT: if (input.empty() && guide_tab()) c.guide(c.active, "next"); else if (cur < input.size()) cur++; break;
    case TB_KEY_HOME: cur = 0; break;  // Ctrl+A is next_active
    // Ctrl+U, and Cmd+Backspace (Ghostty and others send it as ^U)
    case TB_KEY_CTRL_U: input.erase(0, cur); cur = 0; break;
    case TB_KEY_END: case TB_KEY_CTRL_E: cur = input.size(); break;
    case TB_KEY_CTRL_K: input.erase(cur); break;
    case TB_KEY_ARROW_UP:
        if (hpos > 0) input = from_utf8(hist[--hpos]), cur = input.size();
        break;
    case TB_KEY_ARROW_DOWN:
        if (hpos < hist.size()) {
            ++hpos;
            input = hpos == hist.size() ? U"" : from_utf8(hist[hpos]);
            cur = input.size();
        }
        break;
    case TB_KEY_TAB: complete(); break;
    // mIRC formatting: bold, color, reset, reverse, italic, underline
    case TB_KEY_CTRL_C:  // on an empty line it's the "how do I quit?" key: say so, once in a row
        if (input.empty()) {
            const std::string hint = "Ctrl+C doesn't quit iSekur (it's the color key while typing): /exit closes iSekur, Ctrl+Z, Alt+D or /detach leave it running";
            if (c.active->lines.empty() || c.active->lines.back().text != hint) {
                Line l{time(nullptr), LK::INFO, "", hint};
                l.nolog = true;
                c.add(c.active, l);
            }
            break;
        }
        input.insert(cur++, 1, (char32_t)k);
        break;
    case TB_KEY_CTRL_B: case TB_KEY_CTRL_O: case TB_KEY_CTRL_V:
    case TB_KEY_CTRL_RSQ_BRACKET: case TB_KEY_CTRL_UNDERSCORE:
        input.insert(cur++, 1, (char32_t)k);
        break;
    }
}

// vendor/termbox2.c: horizontal wheel
static const int TB_KEY_MOUSE_WHEEL_LEFT = 0xffff - 29, TB_KEY_MOUSE_WHEEL_RIGHT = 0xffff - 30;

bool Tui::in_sel(int x, int y) const {
    int a = sel_y0 * 10000 + sel_x0, z = sel_y1 * 10000 + sel_x1, p = y * 10000 + x;
    return p >= std::min(a, z) && p <= std::max(a, z);
}

// Option+A typed "å": a Mac terminal not sending Option as Alt. Typed as is (it may be meant), and once
// a run a note says how to make it Alt, when that letter's Alt key does something
void Tui::option_note(uint32_t ch) {
    // ponytail: US/ABC layout's Option symbols only; other layouts type the letter they type
    static const std::map<uint32_t, char> OPT = {{U'å', 'a'}, {U'∫', 'b'}, {U'ç', 'c'}, {U'∂', 'd'}, {U'ƒ', 'f'}, {U'∆', 'j'}, {U'˚', 'k'},
        {U'¬', 'l'}, {U'ø', 'o'}, {U'œ', 'q'}, {U'®', 'r'}, {U'ß', 's'}, {U'†', 't'}, {U'∑', 'w'}, {U'≈', 'x'}};
    auto it = OPT.find(ch);
    if (option_noted || it == OPT.end() || !keymap.count(std::string("M-") + it->second)) return;
    option_noted = true;
    c.info_note(c.active, std::string("Option+") + (char)toupper(it->second) + " typed a symbol: for Option as Alt, set macos-option-as-alt = left in Ghostty "
                "(iTerm2: Left Option key Esc+; Terminal.app: Use Option as Meta key). Right Option keeps typing symbols");
}

// the input line as drawn: a NickServ password being typed is *s from where it starts
std::u32string Tui::shown_input() const {
    std::u32string in = input;
    std::string u = to_utf8(input);
    size_t e, p = c.active ? input_secret(c.active->name, u, &e) : std::string::npos;
    if (p != std::string::npos)
        for (size_t i = from_utf8(u.substr(0, p)).size(), n = e == std::string::npos ? in.size() : from_utf8(u.substr(0, e)).size(); i < n; i++) in[i] = U'*';
    return in;
}

// input[a, b) as text for the clipboard: masked as drawn, formatting codes left out, a pasted newline kept
void Tui::copy_input(size_t a, size_t b) {
    std::u32string in = shown_input(), out;
    for (size_t i = a; i < b && i < in.size(); i++)
        if (in[i] >= 0x20 || in[i] == U'\n') out += in[i];
    copy_text(to_utf8(out));
}

// a drag over the input line held at its left or right edge: one character further that way, the view following
void Tui::input_edge() {
    if (!in_selecting) return;
    // first to what shows at that edge (a fast drag gets there in one jump), then past it
    if (in_drag_x <= in_px) isel_b = std::min(isel_b, input_at(in_px)), isel_b -= isel_b > 0;
    else if (in_drag_x >= in_px + in_w) isel_b = std::max(isel_b, input_at(in_px + in_w)), isel_b += isel_b < input.size();
    else return;
    cur = isel_b, c.dirty = true;
}

// the input line's character under column x (the end of the line past it)
size_t Tui::input_at(int x) const {
    std::u32string in = shown_input();
    int col = 0, want = x - in_px + in_off;
    size_t i = 0;
    for (; i < in.size(); i++) {
        int w = cwidth(in[i]);
        if (col + w > want) break;
        col += w;
    }
    return i;
}

// the selection to the terminal's clipboard: chat lines as their text, "00:08 nick text" (the time first even when
// drawn at the right, no padding, a wrapped line joined back up); other rows as their cells
void Tui::copy_sel() {
    tb_cell* cb = tb_cell_buffer();
    int W = tb_width();
    auto take = [&](int y, int x0, int x1) {
        std::string s;
        for (int x = x0; x < x1 && x < W; x++)
            if (cb[y * W + x].ch && in_sel(x, y)) utf8_put(s, cb[y * W + x].ch);
        return s;
    };
    auto any = [&](int y, int x0, int x1) {
        for (int x = x0; x < x1; x++)
            if (in_sel(x, y)) return true;
        return false;
    };
    std::vector<std::string> lines;
    int last = -1;
    for (int y = std::min(sel_y0, sel_y1); y <= std::max(sel_y0, sel_y1); y++) {
        bool chat = false;
        for (auto& r : crows) {
            if (r.y != y) continue;
            chat = true;
            if (r.id == -2 || !any(y, r.x0, std::max(r.bend, r.tsx1))) continue;
            std::string text = take(y, r.bx, r.bend);
            if (r.id >= 0 && r.id == last && !r.first) { lines.back() += text; continue; }  // the rest of a wrapped line
            std::string head;
            if (r.tsx0 >= 0 && any(y, r.tsx0, r.tsx1)) head = r.ts + " ";
            int nx = r.tsx0 == r.x0 ? r.tsx1 : r.x0;  // the nick column, past a time drawn at the left
            if (r.first && !r.marker.empty() && any(y, nx, r.bx)) head += r.marker + " ";
            lines.push_back(head + text), last = r.id;
        }
        if (!chat) lines.push_back(take(y, sel_lo, sel_hi)), last = -1;
    }
    std::string out;
    for (auto& l : lines) {
        while (!l.empty() && l.back() == ' ') l.pop_back();
        out += (&l == &lines[0] ? "" : "\n") + l;
    }
    copy_text(out);
}

// to the terminal's clipboard (OSC 52), "Copied N characters" in the status bar
void Tui::copy_text(const std::string& out) {
    if (out.empty()) return;
    std::string osc = "\x1b]52;c;" + b64(out) + "\x07";
    tb_send(osc.data(), osc.size());
    size_t n = std::count_if(out.begin(), out.end(), [](char b) { return (b & 0xc0) != 0x80; });  // characters, not UTF-8 bytes
    c.flash = "Copied " + std::to_string(n) + (n == 1 ? " character" : " characters"), c.flash_until = now_ms() + 1000;
}

// A link (http(s)://, from linkify) in this machine's browser. Over SSH, with no display or no opener
// it is copied instead: the browser would open on the wrong machine (the terminal's own Cmd/Ctrl+click works there).
void Tui::open_link(const std::string& url) {
#if defined(__APPLE__)
    const char* opener = "open";
    bool gui = true;
#elif defined(__CYGWIN__)
    const char* opener = "cygstart";
    bool gui = true;
#else
    const char* opener = "xdg-open";
    bool gui = getenv("DISPLAY") || getenv("WAYLAND_DISPLAY");
#endif
    bool found = false;
    std::string path = getenv("PATH") ? getenv("PATH") : "";
    for (size_t a = 0; a <= path.size() && !found;) {
        size_t b = path.find(':', a);
        if (b == std::string::npos) b = path.size();
        found = access((path.substr(a, b - a) + "/" + opener).c_str(), X_OK) == 0;
        a = b + 1;
    }
    if (getenv("SSH_CONNECTION") || getenv("SSH_TTY") || !gui || !found) {
        copy_text(url);
        c.flash = "Link copied (this machine can't open it): paste it in a browser", c.flash_until = now_ms() + 3000;
        return;
    }
    pid_t p = fork();  // twice: nothing of the browser is left to reap, and its output stays off our screen
    if (p == 0) {
        if (fork() == 0) {
            int nul = ::open("/dev/null", O_RDWR);
            dup2(nul, 0), dup2(nul, 1), dup2(nul, 2);
            for (int fd = 3; fd < 1024; fd++) close(fd);
            setsid();
            execlp(opener, opener, url.c_str(), (char*)nullptr);  // url starts with http:// or https://: never an option
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0) waitpid(p, nullptr, 0);
    c.flash = "Opening " + url, c.flash_until = now_ms() + 2000;
}

// the word drawn under x,y in the chat, without the punctuation around it
std::string Tui::word_at(int x, int y) const {
    tb_cell* cb = tb_cell_buffer();
    int W = tb_width(), cw = sel_hi;
    if (y < 0 || y >= tb_height() || x < sel_lo || x >= cw) return "";
    auto ch = [&](int xx) { return cb[y * W + xx].ch; };
    if (!ch(x) || ch(x) == ' ') return "";
    int a = x, z = x;
    while (a > sel_lo && ch(a - 1) && ch(a - 1) != ' ') a--;
    while (z + 1 < cw && ch(z + 1) && ch(z + 1) != ' ') z++;
    std::string w;
    for (int xx = a; xx <= z; xx++) utf8_put(w, ch(xx));
    while (!w.empty() && strchr(",.;:!?)]}'\"", w.back())) w.pop_back();
    while (!w.empty() && strchr("([{'\"", w.front())) w.erase(0, 1);
    return w;
}

// Wheel scrolls the chat (or the nicklist, or the topic, under the pointer);
// left click picks a tab or an activity number (double click on a nick: a query), or
// unfolds the topic (double click: edit it); a drag in the chat copies text.
// the × (or the red dot) of the box on screen: closes it like Esc does
void Tui::close_box() {
    switch (xbtn.kind) {
    case 's': sw.open = false; break;
    case 't': tw.q.clear(), theme_key(TB_KEY_ESC, 0); break;  // back to the theme it had
    case 'f': fw.open = false; break;
    case 'k': kw.open = false; break;
    case 'g': pw.open = false; break;
    case 'w': c.pwin_act("close", 0); break;  // a plugin's window: its plugin is told
    case 'l': lw.open = false; break;
    case 'b': bw.open = false; break;
    case 'u': bw.ask.clear(); break;
    case 'm': bw.adding = false; break;
    case 'C': lw.focus = lw.back; break;
    case 'P': pw.ask.clear(); break;
    case 'A': pw.about.clear(); break;
    case 'a': input.clear(), cur = 0, c.exec(c.active, ""); break;  // a question: cancelled
    case 'c': confirm.clear(), confirm_buf = 0; break;
    case 'q': c.choice_act(0); break;  // a plugin's question: cancelled
    case 'p': input = from_utf8(paste), cur = input.size(), paste.clear(); break;  // back to edit it
    }
    xbtn = XBtn{};
    c.dirty = true;
}

void Tui::mouse(int mod, int k, int x, int y) {
    if (c.locked) {  // over the screensaver, the box's X puts it away
        if (k == TB_KEY_MOUSE_LEFT && !(mod & TB_MOD_MOTION) && xbtn.kind == 'L' && y == xbtn.y && x >= xbtn.x0 && x <= xbtn.x1) peek_at = 0, lock_in.clear();
        return;
    }
    if (k == TB_KEY_MOUSE_RELEASE && (mod & TB_MOD_MOTION)) {  // 1003: a move, no button held
        hover_x = x, hover_y = y, g_hover_x = x, g_hover_y = y;
        if (isekur_link_at(x, y) != g_hover_id) c.dirty = true;  // onto a link or off one: its underline
        bool onf = false;
        for (auto& fh : fhits) onf = onf || (fh.y == y && x >= fh.x0 && x < fh.x1);
        if (onf != fold_hov) fold_hov = onf, c.dirty = true;  // onto an eventshidemgr row or off one: its highlight
        return;
    }
    if (k == TB_KEY_MOUSE_LEFT && !(mod & TB_MOD_MOTION) && xbtn.kind && y == xbtn.y && x >= xbtn.x0 && x <= xbtn.x1) { close_box(); return; }
    if (!c.choices.empty()) { choice_mouse(mod, k, x, y); return; }
    if (sw.open) { if (!(mod & TB_MOD_MOTION) && !c.asking(c.active)) settings_mouse(k, x, y); return; }
    if (tw.open) { if (!(mod & TB_MOD_MOTION)) theme_mouse(k, x, y); return; }
    if (fw.open) { if (!(mod & TB_MOD_MOTION)) find_mouse(k, x, y); return; }
    if (pw.open) { if (!(mod & TB_MOD_MOTION)) plugin_mouse(k, x, y); return; }
    if (c.pwin.open) { if (!(mod & TB_MOD_MOTION)) pwin_mouse(k, x, y); return; }
    if (lw.open) { if (!(mod & TB_MOD_MOTION)) logs_mouse(k, x, y); return; }
    if (bw.open) { if (!(mod & TB_MOD_MOTION)) bans_mouse(k, x, y); return; }
    if (kw.open) {  // the wheel scrolls, a click outside closes
        if (k == TB_KEY_MOUSE_WHEEL_UP || k == TB_KEY_MOUSE_WHEEL_DOWN) keys_key(k == TB_KEY_MOUSE_WHEEL_UP ? TB_KEY_ARROW_UP : TB_KEY_ARROW_DOWN, 0);
        else if (k == TB_KEY_MOUSE_LEFT && !(mod & TB_MOD_MOTION) && (x < kw.x0 || x >= kw.x0 + kw.w || y < kw.list_y - 1 || y > kw.list_y + kw.rows + 1)) kw.open = false;
        return;
    }
    int top = top_row(), ch = chat_height();
    bool over_topic = topicbar && !zen && y < topic_h, over_chat = y >= top && y < top + ch && x >= chat_x && x < chat_end && (nl_x < 0 || x < std::min(nl_x, nl_x0) || x >= std::max(nl_x + 1, nl_x1));  // not on the nicklist or its │
    if (in_selecting && (k == TB_KEY_MOUSE_LEFT || k == TB_KEY_MOUSE_RELEASE)) {  // a drag over the input line
        in_drag_x = x;
        if (x > in_px && x < in_px + in_w) isel_b = cur = input_at(x);
        else input_edge();  // at an edge: input_edge() keeps scrolling while the mouse stays there
        if (k == TB_KEY_MOUSE_RELEASE) {
            in_selecting = false;
            if (isel_a != isel_b) copy_input(std::min(isel_a, isel_b), std::max(isel_a, isel_b));
        }
        c.dirty = true;
        return;
    }
    if (selecting && (k == TB_KEY_MOUSE_LEFT || k == TB_KEY_MOUSE_RELEASE)) {
        sel_x1 = std::max(sel_lo, std::min(x, sel_hi - 1));
        sel_y1 = std::max(top, std::min(y, top + ch - 1));
        if (k == TB_KEY_MOUSE_RELEASE) {
            selecting = false;
            if (sel_x1 != sel_x0 || sel_y1 != sel_y0) copy_sel();
            sel_x0 = -1;  // copied, the highlight goes (as in tmux): it's screen cells, new lines would slide under it
        }
        c.dirty = true;
        return;
    }
    if (div_drag && (k == TB_KEY_MOUSE_LEFT || k == TB_KEY_MOUSE_RELEASE)) {  // /split: moving the divider
        int AW = chat_end - chat_x, at = div_x >= 0 ? std::max(10, std::min(AW - 11, x - chat_x)) * 1000 / std::max(1, AW - 1)
                                            : std::max(3, std::min(ch - 3, y - top)) * 1000 / std::max(1, ch);
        if (at != c.split_pm) c.split_pm = at, div_moved = true, c.dirty = true;
        if (k == TB_KEY_MOUSE_RELEASE) {
            div_drag = false;
            if (!div_moved && other.in(x, y)) c.select(c.split_buf);  // a plain click on the other pane's title
        }
        return;
    }
    if (wdrag && (k == TB_KEY_MOUSE_LEFT || k == TB_KEY_MOUSE_RELEASE)) {  // the side tab list's or the nicklist's │: its width
        // the side that stays put: the screen edge (tab list), the pane's edge (nicklist)
        int w = wdrag == 't' ? std::clamp(side_x ? tb_width() - 1 - x : x, 10, std::min(60, tb_width() - 41))
                             : std::clamp(nl_left ? x - nl_x0 : nl_x1 - 1 - x, 8, 40);
        std::string key = wdrag == 't' ? "tab_bar_width" : "nicklist_width";
        if (std::to_string(w) != c.cfg["general"][key]) c.cfg["general"][key] = std::to_string(w), c.dirty = true, wdrag_moved = true;
        if (k == TB_KEY_MOUSE_RELEASE) {  // saved like /set + /save of that one key; a click without a move: nothing
            bool moved = wdrag_moved;
            wdrag = 0, wdrag_moved = false;
            if (!moved) return;
            std::string err, v = c.cfg["general"][key];
            if (c.cfg_path.empty() || !ini_save(c.cfg_path, Ini{{"general", {{key, v}}}}, err)) c.unsaved["general"][key] = v;
        }
        return;
    }
    if (tab_drag >= 0 && (mod & TB_MOD_MOTION)) {  // dragging a tab: the one it would land on, reversed
        auto [moved, at] = drop_at(x, y);
        int over = -1;
        for (auto& h : hits)
            if (moved && h.y == y && x >= h.x0 && x < h.x1 && h.tab >= 0 && h.tab < (int)c.bufs.size()) over = c.bufs[(size_t)h.tab]->id;
        if (over != tab_drop) tab_drop = over, c.dirty = true;
        return;
    }
    if (mod & TB_MOD_MOTION) return;
    if (k == TB_KEY_MOUSE_LEFT && menu.items.empty() && !nick_sel.empty()) {  // a click off the nicklist unselects
        bool on = false;
        for (auto& h : hits) on |= h.y == y && x >= h.x0 && x < h.x1 && !h.nick.empty();
        if (!on) nick_sel.clear(), c.dirty = true;
    }
    if (k == TB_KEY_MOUSE_LEFT && menu.items.empty() && y >= top && y < top + ch && ((side_w && x == sep_x) || (nl_x >= 0 && x == nl_x))) {
        wdrag = side_w && x == sep_x ? 't' : 'n', wdrag_moved = false;
        return;
    }
    if (k == TB_KEY_MOUSE_LEFT && menu.items.empty() && ((div_x >= 0 && x == div_x && y >= top && y < top + ch) || (div_y >= 0 && y == div_y))) {
        div_drag = true, div_moved = false;
        return;
    }
    if (menu.items.empty() && c.split_buf && other.in(x, y)) {  // /split: the other pane
        if (k == TB_KEY_MOUSE_WHEEL_UP) c.split_buf->scroll += 3;
        else if (k == TB_KEY_MOUSE_WHEEL_DOWN) c.split_buf->scroll = std::max(0, c.split_buf->scroll - 3);
        else if (k == TB_KEY_MOUSE_LEFT) c.select(c.split_buf);
        c.dirty = true;
        return;
    }
    if (!menu.items.empty() && (k == TB_KEY_MOUSE_LEFT || k == TB_KEY_MOUSE_RIGHT || k == TB_KEY_MOUSE_MIDDLE || (k >= TB_KEY_MOUSE_WHEEL_RIGHT && k <= TB_KEY_MOUSE_WHEEL_UP))) {  // pick, or close it (the middle button and the wheel only close)
        int i = y - menu.y - 1;
        int mw = 0;
        for (auto& it : menu.items) mw = std::max(mw, (int)it.first.size() + 2);
        mw = std::max(mw, (int)menu.nick.size() + 2);
        bool in = (k == TB_KEY_MOUSE_LEFT || k == TB_KEY_MOUSE_RIGHT) && x >= menu.x && x < menu.x + mw && i >= 0 && i < (int)menu.items.size() && !menu.items[(size_t)i].first.empty();
        if (in) menu.sel = i, key(0, TB_KEY_ENTER, 0);
        else menu.items.clear();
        c.dirty = true;
        return;
    }
    if (k == TB_KEY_MOUSE_RIGHT) {
        if (int id = isekur_link_at(x, y); id > 0 && id <= (int)frame_urls.size() && (over_chat || over_topic)) {  // a link: Open, Copy URL
            const std::string& u = frame_urls[(size_t)id - 1];
            menu = Menu{x, y, 0, u.size() > 38 ? u.substr(0, 35) + "..." : u, {{"Open", "\x01" + u}, {"Copy URL", "\x02" + u}}};  // the command: \x01 open, \x02 copy, then the URL
            c.dirty = true;
            return;
        }
        for (auto& h : hits) {
            if (h.y != y || x < h.x0 || x >= h.x1) continue;
            if (!h.nick.empty()) {  // on the selection: a menu for all of it; else just this one
                if (std::find(nick_sel.begin(), nick_sel.end(), h.nick) == nick_sel.end()) nick_sel = {h.nick};
                std::string who;
                for (auto& n : nick_sel) who += (who.empty() ? "" : " ") + n;
                nick_click_ms = 0, open_menu(who, x, y);
                continue;
            }
            if ((side_w ? x < side_x || x >= side_x + side_w || y < top_row() || y >= top_row() + chat_height() : y < top_row() - tab_h || y >= top_row()) || h.tab < 0 || h.tab >= (int)c.bufs.size()) continue;
            Buffer* tb = c.bufs[(size_t)h.tab].get();
            if ((tb->type == BT::STATUS && tb->srv) || tb->type == BT::CHANNEL || tb->type == BT::QUERY)  // its commands run there
                menu = Menu{x, y, 0, tb->type == BT::STATUS ? tb->srv->cfg.name : tb->name, c.tab_menu(tb), tb->id}, c.dirty = true;  // you stay on your tab: its commands run on this one
        }
        return;
    }
    if (over_topic && k >= TB_KEY_MOUSE_WHEEL_RIGHT && k <= TB_KEY_MOUSE_WHEEL_UP) {
        bool back = k == TB_KEY_MOUSE_WHEEL_LEFT || k == TB_KEY_MOUSE_WHEEL_UP;
        topic_off = std::max(0, topic_off + (back ? -8 : 8));
        c.dirty = true;
        return;
    }
    if (k == TB_KEY_MOUSE_WHEEL_UP || k == TB_KEY_MOUSE_WHEEL_DOWN) {
        int d = k == TB_KEY_MOUSE_WHEEL_UP ? 3 : -3;
        bool over_nicks = nl_x >= 0 && x >= nl_x0 && x < nl_x1 && y >= top_row() && y < top_row() + chat_height();
        if (over_nicks) nl_scroll = std::max(0, nl_scroll - d);
        else if (side_w && x >= side_x && x < side_x + side_w && y >= top_row()) side_top = std::max(0, side_top - d);  // clamped by draw()
        else c.active->scroll = std::max(0, c.active->scroll + d);
        c.dirty = true;
        return;
    }
    if (k == TB_KEY_MOUSE_RELEASE && tab_drag >= 0) {  // a dragged tab let go: it takes the place of the one under it
        auto [moved, at] = drop_at(x, y);
        tab_drag = tab_drop = -1, c.dirty = true;
        if (moved) c.exec(moved, "/move " + std::to_string(at));
        return;
    }
    if (k != TB_KEY_MOUSE_LEFT) return;
    if (in_px >= 0 && y == tb_height() - 1 && x >= in_px && !(mod & TB_MOD_MOTION)) {  // the input line: the cursor goes there, a drag selects
        isel_a = isel_b = cur = input_at(x), in_selecting = true, completing = false, c.dirty = true;
        return;
    }
    if (over_topic) {
        Buffer* b = c.active;
        long long now = now_ms();
        bool dbl = now - click_ms < 400;
        click_ms = dbl ? 0 : now;
        if (!dbl) { topic_open = !topic_open; return; }
        // double click: the topic into the input line, if we may set it (+t needs ops)
        topic_open = false;
        if (b->type != BT::CHANNEL || !b->srv) return;
        auto me = b->nicks.find(b->srv->lower(b->srv->nick));
        std::string my = me == b->nicks.end() ? "" : me->second.prefix;
        if (b->modes.find('t') != std::string::npos && !rank_ok(b->srv, my, '%')) {
            c.print(b, LK::ERROR, b->name + " is +t: only channel operators can change the topic");
            return;
        }
        // a dialog with the topic in it to edit; Enter sets it, Esc (or emptying it) leaves it be
        // (closing the tab drops the dialog and this with it, so b is still there when it runs)
        c.ask_pass(b, "topic", [this, b](const std::string& t) { c.exec(b, "/topic " + t); },
                   false, "Topic of " + b->name, {"Enter sets it · Esc leaves it as it is"});
        input = from_utf8(b->topic), cur = input.size(), completing = false;
        return;
    }
    for (auto& h : hits) {
        if (h.y != y || x < h.x0 || x >= h.x1) continue;
        if (h.tab == -2) c.about(c.active);
        else if (h.tab >= 0 && h.tab < (int)c.bufs.size()) c.select(c.bufs[(size_t)h.tab].get()), tab_drag = c.bufs[(size_t)h.tab]->id;
        else if (!h.nick.empty()) {
            long long now = now_ms();
            bool dbl = h.nick == nick_click && now - nick_click_ms < 400;
            nick_click = h.nick, nick_click_ms = dbl ? 0 : now, c.dirty = true;
            auto at = std::find(nick_sel.begin(), nick_sel.end(), h.nick);
            if (dbl) nick_sel = {h.nick}, c.exec(c.active, "/query " + h.nick);
            else if (mod & (TB_MOD_SHIFT | TB_MOD_CTRL)) { if (at == nick_sel.end()) nick_sel.push_back(h.nick); else nick_sel.erase(at); }
            else if (at != nick_sel.end()) nick_sel.clear();  // a selected one: unselects
            else nick_sel = {h.nick};
        }
        return;
    }
    if (over_chat) {
        for (auto& fh : fhits)  // eventshidemgr's row: a click opens or folds the run
            if (fh.y == y && x >= fh.x0 && x < fh.x1) {
                if (!fold_open.erase({fh.bid, fh.key})) fold_open.insert({fh.bid, fh.key});
                c.dirty = true;
                return;
            }
        Buffer* b = c.active;
        std::string w = word_at(x, y);
        // *channels: a click on the Channel or Users header sorts by it
        if (b->srv && b->name == "*channels" && (w == "Channel" || w == "Users")) { c.list_sort(b, w == "Channel" ? "name" : "users"); return; }
        if (guide_tab()) {  // its buttons: ◀ Prev, Next ▶ / ✔ Done, and the box from its ☐ on
            tb_cell* cb = tb_cell_buffer();
            int box = -1;
            for (int xx = 0; xx < tb_width() && box < 0; xx++)
                if (cb[y * tb_width() + xx].ch == 0x2610 || cb[y * tb_width() + xx].ch == 0x2611) box = xx;
            if (box >= 0 && x >= box) { c.guide(b, c.opt_on(nullptr, "show_guide", false) ? "off" : "on"); return; }
            if (w == "◀" || w == "Prev") { c.guide(b, "prev"); return; }
            if (w == "Next" || w == "▶") { c.guide(b, "next"); return; }
            if (w == "Done" || w == "✔") { c.guide(b, "close"); return; }
        }
        // double click a #channel (any tab of a server) joins it; in *channels anywhere on its row
        long long now = now_ms();
        bool dbl = now - chat_click_ms < 400 && chat_click_y == y;
        chat_click_ms = dbl ? 0 : now, chat_click_y = y;
        if (int id = isekur_link_at(x, y); id > 0 && id <= (int)frame_urls.size() && (dbl || (mod & (TB_MOD_CTRL | TB_MOD_ALT)))) {
            open_link(frame_urls[(size_t)id - 1]);  // a link: double click, or Ctrl/Alt+click
            return;
        }
        if (dbl && b->srv) {
            if (!b->srv->is_chan(w) && b->name == "*channels")
                for (int xx = 0; xx < x && !b->srv->is_chan(w); xx++) w = word_at(xx, y);  // the row's first #word: its name
            if (b->srv->is_chan(w)) { c.exec(b, "/join " + w); return; }
        }
        selecting = true, sel_x0 = sel_x1 = x, sel_y0 = sel_y1 = y;
    }
}

// a tab dropped on another of its server's (a server tab: on any tab of another server) takes that place;
// a channel on its server tab goes first
std::pair<Buffer*, int> Tui::drop_at(int x, int y) {
    Buffer* b = nullptr;
    for (auto& p : c.bufs) if (p->id == tab_drag) b = p.get();
    for (auto& h : hits) {
        if (h.y != y || x < h.x0 || x >= h.x1 || h.tab < 0 || h.tab >= (int)c.bufs.size() || !h.nick.empty()) continue;
        Buffer* to = c.bufs[(size_t)h.tab].get();
        if (!b || b == to || !b->srv || !to->srv) return {nullptr, 0};
        bool srv = b->type == BT::STATUS;  // a server tab: among the servers
        if (srv == (to->srv == b->srv)) return {nullptr, 0};
        int n = 0, at = 1;
        for (auto& p : c.bufs)
            if (srv ? p->srv && p->type == BT::STATUS : p->srv == to->srv && p->type != BT::STATUS) {
                n++;
                if (srv ? p->srv == to->srv : p.get() == to) at = n;
            }
        return {b, at};
    }
    return {nullptr, 0};
}

Buffer* Tui::tab_by_id(int id) const {
    if (!id) return c.active;
    for (auto& b : c.bufs)
        if (b->id == id) return b.get();
    return nullptr;
}

void Tui::open_menu(const std::string& nick, int x, int y) {
    if (!c.active->srv) return;
    size_t n = std::count(nick.begin(), nick.end(), ' ') + 1;
    menu = Menu{x, y, 0, n > 1 ? std::to_string(n) + " users" : nick, c.nick_menu(c.active, nick)};
    c.dirty = true;
}

void Tui::submit() {
    std::string line = to_utf8(input);
    input.clear(), cur = 0;
    if (c.asking(c.active)) { c.exec(c.active, line); return; }  // a password: no history
    if (line.empty()) return;
    // a path (a file dropped on the terminal, then Enter) goes to the plugin that takes files, when there is one
    if (line.find('\n') == std::string::npos && !dropped_paths(line).empty() &&
        c.plugin_hooks("drop", c.active->srv, c.active->type == BT::CHANNEL || c.active->type == BT::QUERY ? c.active->name : "")) {
        c.upload_cmd(c.active, line);
        return;
    }
    // paste_confirm = N (default 5, 0 = off): ask before sending more lines than that
    size_t lines = 0;
    for (auto& l : split(line, '\n')) lines += !l.empty();
    int max = atoi(c.opt(c.active->srv, "paste_confirm", "5").c_str());
    if (max > 0 && lines > (size_t)max && (line[0] == '/' || line.find("\n/") != std::string::npos || !c.plugin_hooks("paste", c.active->srv, c.active->name))) {  // chat lines a plugin takes (pasteguard): no question
        paste = line;
        return;
    }
    // a password: not kept for Up (perform's lines often have one)
    bool secret = input_secret(c.active->name, line) != std::string::npos || line.rfind("/perform ", 0) == 0 ||
                  (line.rfind("/set ", 0) == 0 && line.find(".perform ") != std::string::npos);
    if (!secret && (hist.empty() || hist.back() != line)) hist.push_back(line);
    if (hist.size() > 500) hist.erase(hist.begin());
    hpos = hist.size();
    c.active->scroll = 0;
    if (line == "/exit" || line.rfind("/exit ", 0) == 0) { ask_exit(line.substr(5)); return; }
    if (line == "/restart" || line.rfind("/restart ", 0) == 0) { ask_exit(line.substr(8), true); return; }
    c.exec(c.active, line);
}

// /exit, /restart and Alt+Q: the yes/no box while a network is connected, else straight out
void Tui::ask_exit(std::string msg, bool restart) {
    while (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
    bool on = false;
    for (auto& sv : c.servers) on |= sv->state != SS::DISCONNECTED;
    std::string cmd = restart ? "/restart " : "/exit ";
    if (msg == "-y" || msg.rfind("-y ", 0) == 0) { c.exec(c.active, cmd + msg); return; }
    if (!on) { c.exec(c.active, cmd + "-y " + msg); return; }
    confirm = (restart ? "Restart iSekur? You leave every network, it starts again and the webchat is back in a moment; settings not saved with /save are lost. Alt+D detaches instead: iSekur stays connected.\n"
                       : "Exit iSekur? You leave every network and the webchat closes. Alt+D detaches instead: iSekur stays connected.\n") + cmd + "-y " + msg;
    confirm_title = restart ? "Restart" : "Exit";
}

int Tui::suggest(std::vector<Sugg>& out) const {
    std::string s = to_utf8(input.substr(0, cur));
    if (c.asking(c.active)) return -1;  // a password is not a command
    if (s.empty() || s[0] != '/' || s.rfind("//", 0) == 0) return -1;
    size_t sp = s.find(' ');
    auto cmds = [&](const std::string& pfx, bool slash) {
        for (auto& k : commands())
            if (std::string(k.name).rfind(irc_lower(pfx), 0) == 0)
                out.push_back({(slash ? "/" : "") + std::string(k.name), (*k.usage ? " " + std::string(k.usage) : "") + "  ", k.summary});
        for (auto& [k, u, pl] : c.plugin_commands())  // the plugins that are on
            if (k.rfind(irc_lower(pfx), 0) == 0) out.push_back({(slash ? "/" : "") + k, (u.empty() ? "" : " " + u) + "  ", "plugin: " + pl});
        if (c.cfg.count("aliases"))  // yours (/alias), after iSekur's
            for (auto& [k, v] : c.cfg.at("aliases"))
                if (!v.empty() && k.rfind(irc_lower(pfx), 0) == 0) out.push_back({(slash ? "/" : "") + k, "  ", "alias: " + perform_show(v)});
    };
    if (sp == std::string::npos) { cmds(s.substr(1), true); return 0; }  // "/jo"
    std::string cmd = irc_lower(s.substr(1, sp - 1)), arg = s.substr(sp + 1);
    // "/msg al", "/op alice bo": nicks for the word being typed, the tab's own; in a tab without a nicklist
    // (the server tab, a query) the network's, with the tab they're in
    size_t ws = s.rfind(' ') + 1;
    std::string word = s.substr(ws);
    const CmdInfo* nk = find_command(cmd);
    std::string usage = nk ? nk->usage : "";  // iSekur's own or a plugin's
    if (!nk)
        for (auto& [k, u, pl] : c.plugin_commands())
            if (k == cmd) usage = u;
    Buffer* b = c.active;
    size_t wi = (size_t)std::count(arg.begin(), arg.end(), ' ');  // the argument being typed, 0 the first
    std::string first = arg.substr(0, arg.find(' ')), name = nk ? nk->name : "";
    // "/part ", "/mode #", "/msg #is": the network's channels, this tab's first. /join: those you're not in; /mode: those
    // where you can set modes (more than voice); /part and /cycle: any tab. web/index.html chanRows() is the same
    if (b && b->srv && !usage.empty()) {
        Server* sv = b->srv;
        auto u = split(usage.substr(0, usage.find(" | ")), ' ');
        bool want = word.empty() ? (name == "mode" ? !wi : name != "kick" && wi < u.size() && u[wi].find("#channel") != std::string::npos)
                                 : sv->is_chan(word) && (usage.find("#channel") != std::string::npos || usage.find("target") != std::string::npos);
        auto chan = [&](Buffer* x) {
            if (x->srv != sv || x->type != BT::CHANNEL || sv->lower(x->name).rfind(sv->lower(word), 0) != 0) return;
            if (name == "join" ? x->joined : !x->joined && name != "part" && name != "cycle") return;
            if (name == "mode" && !wi) {
                auto me = x->nicks.find(sv->lower(sv->nick));
                if (me == x->nicks.end() || me->second.prefix.empty() || me->second.prefix[0] == '+') return;
            }
            out.push_back({x->name, "  ", x == b ? "(this tab)" : ""});
        };
        if (want) {
            chan(b);
            for (auto& x : c.bufs)
                if (x.get() != b) chan(x.get());
            if (out.size() == 1 && sv->lower(out[0].word) == sv->lower(word)) out.clear();  // typed out already: Up is history's
            if (!out.empty()) return (int)from_utf8(s.substr(0, ws)).size();
        }
    }
    // "/mode +o al", "/mode #chan +b al": nicks after modes that take one
    bool mode_nick = false;
    if (name == "mode") {
        auto a = split(arg, ' ');
        size_t m = !first.empty() && (first[0] == '+' || first[0] == '-') ? 0 : 1;  // where the modes are
        mode_nick = wi > m && m < a.size() && a[m].find_first_of("qaohvb") != std::string::npos;
    }
    // "/kick #chan al", "/mode #chan +o al": that channel's nicks
    if (wi && b && b->srv && b->srv->is_chan(first))
        if (Buffer* x = c.find(b->srv, first)) b = x;
    if (!word.empty() && !usage.empty() && b && b->srv && (mode_nick || nick_arg(usage, wi, first))) {
        std::string pfx = irc_lower(word), me = b->srv->lower(b->srv->nick);
        std::set<std::string> seen;
        auto add = [&](const std::string& n, const std::string& where) {
            std::string l = b->srv->lower(n);
            if (l.rfind(pfx, 0) == 0 && l != me && seen.insert(l).second) out.push_back({n, "  ", where});
        };
        auto scan = [&](Buffer* x) {
            for (auto& kv : x->nicks) add(kv.second.nick, x == b ? "" : x->name);
            if (x->type == BT::QUERY && x->name[0] != '*') add(x->name, x == b ? "" : "query");
        };
        scan(b);
        if (b->type != BT::CHANNEL)
            for (auto& x : c.bufs)
                if (x->srv == b->srv && x.get() != b) scan(x.get());
        if (!out.empty()) return (int)from_utf8(s.substr(0, ws)).size();  // Tab and the list count in characters
    }
    if (cmd == "set" && arg.find(' ') != std::string::npos && arg.find(' ') == arg.rfind(' ')) {  // "/set cursor b": the values it takes, else what it is
        std::string key = arg.substr(0, arg.find(' ')), sec = "general";
        if (size_t dot = key.rfind('.'); dot != std::string::npos) sec = key.substr(0, dot), key = key.substr(dot + 1);
        if (!secret_key(key)) {
            std::string pfx = irc_lower(word), cur_v = ini_get(c.cfg, sec, key);
            for (auto& o : c.setting_choices(sec, key))
                if (irc_lower(o).rfind(pfx, 0) == 0) out.push_back({o, "  ", o == cur_v ? "(current)" : "", true});
            if (!out.empty()) return (int)from_utf8(s.substr(0, ws)).size();
            if (std::string h = c.setting_hint(sec, key); !h.empty() && pfx.empty()) { out.push_back({key, "  ", h}); return -2; }
        }
    }
    if (arg.find(' ') == std::string::npos) {
        if (cmd == "set") {  // config keys containing what's typed
            for (auto& st : c.settings())
                if (irc_lower(st.key).find(irc_lower(arg)) != std::string::npos) out.push_back({st.key, " = ", st.val});
            return (int)sp + 1;
        }
        if (cmd == "theme") {  // installed themes containing what's typed
            for (auto& n : theme_names())
                if (irc_lower(n).find(irc_lower(arg)) != std::string::npos) out.push_back({n, "  ", n == c.theme.name ? "(current)" : ""});
            return (int)sp + 1;
        }
        if (cmd == "help") { cmds(arg, false); return (int)sp + 1; }
        if (const CmdInfo* k = find_command(cmd)) {  // "/kickban al": the arguments, until the 2nd word
            out.push_back({"/" + std::string(k->name), (*k->usage ? " " + std::string(k->usage) : "") + "  ", k->summary});
            return -2;
        }
        for (auto& [k, u, pl] : c.plugin_commands())  // "/radio ": its usage, until the 2nd word
            if (k == cmd) { out.push_back({"/" + k, (u.empty() ? "" : " " + u) + "  ", "plugin: " + pl}); return -2; }
    }
    return -1;
}

void Tui::complete() {
    Buffer* b = c.active;
    if (!completing) {
        size_t st = cur;
        while (st > 0 && input[st - 1] != U' ') st--;
        comp.clear();
        std::vector<Sugg> sg;
        int from = suggest(sg);
        if (from >= 0) {  // a command, config key or nick
            st = (size_t)from;
            for (auto& g : sg) comp.push_back(g.word + (g.last ? "" : " "));
        } else {
            std::string pfx = irc_lower(to_utf8(input.substr(st, cur - st)));
            if (pfx.empty()) return;
            for (auto& kv : b->nicks)
                if (kv.first.rfind(pfx, 0) == 0) comp.push_back(kv.second.nick);
            if (b->type == BT::QUERY && irc_lower(b->name).rfind(pfx, 0) == 0) comp.push_back(b->name);
            std::sort(comp.begin(), comp.end(),
                      [](auto& x, auto& y) { return irc_lower(x) < irc_lower(y); });
            for (auto& n : comp) n += st == 0 ? c.complete_suffix() + " " : " ";  // "bob: " at the line's start
        }
        if (comp.empty()) return;
        completing = true, comp_i = 0, comp_start = st;
    } else {
        comp_i = (comp_i + 1) % comp.size();
    }
    std::u32string w = from_utf8(comp[comp_i]);
    input.replace(comp_start, cur - comp_start, w);
    cur = comp_start + w.size();
}

// ---- /logs: the log browser ------------------------------------------------------------------
static const char* MONTHS[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
static std::string thousands(size_t n) {
    std::string s = std::to_string(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return s;
}
static int month_days(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : d[m - 1];
}
static std::string ymd(int y, int m, int d) {
    char b[40];  // room for any int: gcc's -Wformat-truncation can't tell they're a date
    snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d);
    return b;
}
// "Thu 2026-11-05"
static std::string day_label(const std::string& day) {
    tm t{};
    if (!strptime(day.c_str(), "%Y-%m-%d", &t)) return day;
    t.tm_hour = 12, t.tm_isdst = -1;
    return fmt_time(mktime(&t), "%a ") + day;
}
// the find line's words in a row (any case, within the row): reversed, like a terminal's search
static void mark_words(Row& r, const std::vector<std::u32string>& words) {
    auto low = [](char32_t c) { return c < 128 ? (char32_t)tolower((int)c) : c; };
    for (auto& w : words) {
        if (w.empty()) continue;
        for (size_t i = 0; i + w.size() <= r.size(); i++) {
            size_t j = 0;
            while (j < w.size() && low(r[i + j].cp) == low(w[j])) j++;
            if (j < w.size()) continue;
            for (j = 0; j < w.size(); j++) r[i + j].st.attr |= A_REV;
            i += w.size() - 1;
        }
    }
}
// the log rows' buffer: no server, so no nick prefixes; a channel, so events keep their marker
static const Buffer* log_buf() {
    static Buffer b{};
    b.type = BT::CHANNEL, b.srv = nullptr;
    return &b;
}

std::vector<Tui::LogNode> Tui::log_tree() const {
    std::vector<LogNode> out;
    auto& f = lw.files;
    for (size_t i = 0; i < f.size();) {
        size_t j = i;
        std::set<std::string> names;
        while (j < f.size() && f[j].net == f[i].net) names.insert(f[j++].name);
        out.push_back({0, f[i].net, "", -1, (int)names.size()});
        if (lw.unfolded.count(f[i].net))
            for (size_t a = i; a < j;) {
                size_t z = a;
                while (z < j && f[z].name == f[a].name) z++;
                out.push_back({1, f[a].net, f[a].name, -1, (int)(z - a)});
                if (lw.unfolded.count(f[a].net + "\t" + f[a].name))
                    for (size_t k = a; k < z; k++) out.push_back({2, f[k].net, f[k].name, (int)k, 0});
                a = z;
            }
        i = j;
    }
    return out;
}

void Tui::logs_select(int file) {
    if (file < 0 || file >= (int)lw.files.size()) return;
    auto& f = lw.files[(size_t)file];
    lw.unfolded.insert(f.net), lw.unfolded.insert(f.net + "\t" + f.name);
    auto t = log_tree();
    for (size_t i = 0; i < t.size(); i++)
        if (t[i].file == file) lw.sel = (int)i;
}

void Tui::logs_view() {
    lw.view.clear();
    if (lw.searched) {
        for (size_t i = 0; i < lw.hits.size(); i++) {
            if (!i || lw.hits[i].file != lw.hits[i - 1].file) lw.view.push_back({(int)lw.hits[i].file, std::string::npos, nullptr});
            lw.view.push_back({(int)lw.hits[i].file, lw.hits[i].line, &lw.hits[i].l});
        }
    } else if (lw.file >= 0) {
        LogQuery q = log_query(to_utf8(lw.q), lw.events);
        for (size_t i = 0; i < lw.lines.size(); i++)
            if (log_match(lw.lines[i], q)) lw.view.push_back({lw.file, i, &lw.lines[i]});
    }
    lw.cur = std::max(0, std::min(lw.cur, (int)lw.view.size() - 1));
}

void Tui::logs_open(int file, long line) {
    if (file < 0 || file >= (int)lw.files.size()) return;
    if (file != lw.file || lw.lines.empty()) lw.lines = log_read(lw.files[(size_t)file].path, &lw.cut), lw.file = file;
    lw.searched = false, lw.hits.clear();
    if (line >= 0) lw.q.clear();  // a line found: shown among the rest of its day
    logs_view();
    lw.cur = 0, lw.ltop = 0;
    if (line == -2) lw.cur = (int)lw.view.size() - 1, lw.ltop = std::max(0, lw.cur - 400);
    if (line >= 0) {
        for (size_t i = 0; i < lw.view.size(); i++)
            if (lw.view[i].line >= (size_t)line) { lw.cur = (int)i; break; }
        lw.ltop = std::max(0, lw.cur - lw.rows / 3);
    }
    logs_select(file);
}

// the tree's network or channel: every day of it; a day: its filter only
void Tui::logs_search() {
    auto t = log_tree();
    if (t.empty() || lw.q.empty()) { lw.searched = false, logs_view(); return; }
    const LogNode& n = t[(size_t)std::max(0, std::min(lw.sel, (int)t.size() - 1))];
    if (n.lvl == 2) { logs_open(n.file, -1); return; }
    std::vector<size_t> which;
    std::set<std::string> days;
    for (size_t i = 0; i < lw.files.size(); i++)
        if (lw.files[i].net == n.net && (n.lvl == 0 || lw.files[i].name == n.name)) which.push_back(i), days.insert(lw.files[i].day);
    lw.hits = log_search(lw.files, which, log_query(to_utf8(lw.q), lw.events), 2000, &lw.more);
    lw.searched = true, lw.found_in = n.lvl == 0 ? n.net : n.name, lw.found_days = days.size();
    logs_view();
    lw.cur = 0, lw.ltop = 0;
}

void Tui::logs_init() {
    lw = LogWin{};
    lw.open = true;
    lw.files = c.log_files();
    lw.q = from_utf8(c.logs_q);
    std::string at = c.logs_at, net = at.substr(0, at.find('\t')), name = at.find('\t') == std::string::npos ? "" : at.substr(at.find('\t') + 1);
    c.logs_at.clear(), c.logs_q.clear();
    int newest = -1;
    for (size_t i = 0; i < lw.files.size(); i++)
        if (irc_lower(lw.files[i].net) == irc_lower(net) && (name.empty() || lw.files[i].name == name)) { newest = (int)i; break; }
    if (newest < 0 && !lw.files.empty()) lw.unfolded.insert(lw.files[0].net);
    if (newest < 0) return;
    auto& f = lw.files[(size_t)newest];
    lw.unfolded.insert(f.net);
    if (name.empty() || !lw.q.empty()) {  // a network, or a search: the tree's network or channel row
        if (!name.empty()) lw.unfolded.insert(f.net + "\t" + f.name);
        auto t = log_tree();
        for (size_t i = 0; i < t.size(); i++)
            if (t[i].net == f.net && t[i].lvl == (name.empty() ? 0 : 1) && (name.empty() || t[i].name == f.name)) lw.sel = (int)i;
        if (!lw.q.empty()) logs_search(), lw.focus = 1;
        return;
    }
    lw.rows = std::max(1, tb_height() - 7);
    logs_open(newest, -2);
    lw.focus = 1;
}

// the calendar of the channel picked in the tree (or shown), at its day shown or its newest
void Tui::logs_cal() {
    auto t = log_tree();
    std::string net, name, day;
    if (lw.focus == 1 && lw.file >= 0 && !lw.searched) {
        auto& f = lw.files[(size_t)lw.file];
        net = f.net, name = f.name, day = f.day;
    } else if (!t.empty()) {
        const LogNode& n = t[(size_t)std::max(0, std::min(lw.sel, (int)t.size() - 1))];
        net = n.net, name = n.name;
        if (n.file >= 0) day = lw.files[(size_t)n.file].day;
    }
    if (name.empty()) { lw.msg = "Pick a channel or query first: the calendar shows its days"; return; }
    if (day.empty())
        for (auto& f : lw.files)
            if (f.net == net && f.name == name) { day = f.day; break; }  // newest first
    lw.cnet = net, lw.cname = name;
    lw.cy = atoi(day.substr(0, 4).c_str()), lw.cm = atoi(day.substr(5, 2).c_str()), lw.cd = atoi(day.substr(8, 2).c_str());
    lw.back = lw.focus == 3 ? lw.back : lw.focus, lw.focus = 3;
}

void Tui::draw_logs(int W, int H) {
    const Theme& T = c.look(false);
    Style bs = sty(T, "menu", "nicklist"), ls = sty(T, "menu_border", "separator"), hs = bs, dim = sty(T, "timestamp", "menu"),
          sel = sty(T, "menu_sel", "tab_active"), fs = T.get("input"), es = T.get("error"), keyst = sty(T, "set_key", "info"),
          ok = T.get("join"), def = T.get("default"), mark = sty(T, "marker", "separator");
    for (Style* st : {&ls, &dim, &es, &keyst, &ok}) st->bg = bs.bg, st->attr = 0;
    for (Style* st : {&dim, &es, &keyst, &ok}) st->fg = readable(st->fg, bs.bg);
    hs.attr |= A_BOLD, keyst.attr |= A_BOLD;
    if (W < 50 || H < 14) { text(0, 0, W, "terminal too small for /logs", def, false); return; }
    lw.w = std::min(W - 2, 170), lw.x0 = (W - lw.w) / 2, lw.y0 = 0;
    int h = H, x1 = lw.x0 + lw.w, y0 = lw.y0, tw = std::min(30, lw.w / 3);
    lw.body_y = y0 + 3, lw.rows = h - 6, lw.xd = lw.x0 + 1 + tw;
    int sep2 = y0 + h - 3, hint_y = y0 + h - 2;
    gutters(W, lw.x0, x1, y0, y0 + h, T.get("default"));
    for (int yy = y0; yy < y0 + h; yy++) fill(yy, lw.x0, x1, bs);
    for (int xx = lw.x0; xx < x1; xx++) cell(xx, y0, U'─', ls), cell(xx, y0 + h - 1, U'─', ls), cell(xx, y0 + 2, U'─', ls), cell(xx, sep2, U'─', ls);
    for (int yy = y0; yy < y0 + h; yy++) cell(lw.x0, yy, U'│', ls), cell(x1 - 1, yy, U'│', ls);
    for (int yy = y0 + 3; yy < sep2; yy++) cell(lw.xd, yy, U'│', ls);
    cell(lw.x0, y0, U'┌', ls), cell(x1 - 1, y0, U'┐', ls), cell(lw.x0, y0 + h - 1, U'└', ls), cell(x1 - 1, y0 + h - 1, U'┘', ls);
    cell(lw.x0, y0 + 2, U'├', ls), cell(x1 - 1, y0 + 2, U'┤', ls), cell(lw.x0, sep2, U'├', ls), cell(x1 - 1, sep2, U'┤', ls);
    cell(lw.xd, y0 + 2, U'┬', ls), cell(lw.xd, sep2, U'┴', ls);
    if (auto cb = box_title(T, lw.x0, x1 - 1, y0, "Logs", hs); cb.first >= 0) xbtn = {y0, cb.first, cb.second, 'l'};

    // the find line, and the switches
    int fx = text(lw.x0 + 2, y0 + 1, x1 - 2, "Find ", hs, false), fw_ = std::min(44, lw.w - 50);
    lw.find_x = fx;
    fill(y0 + 1, fx, fx + fw_, fs);
    std::string qs = to_utf8(lw.q);
    if (qs.empty() && lw.focus != 2) text(fx + 1, y0 + 1, fx + fw_, "/ to type: words, \"a phrase\", nick:alice", sty(T, "timestamp", "input"), false);
    else text(fx + 1, y0 + 1, fx + fw_ - 1, qs, fs, false);
    if (lw.focus == 2) put_cursor(std::min(fx + 1 + (int)lw.q.size(), fx + fw_ - 1), y0 + 1);
    int sx = text(fx + fw_ + 2, y0 + 1, x1 - 2, "e", keyst, false);
    sx = text(sx, y0 + 1, x1 - 2, lw.events ? " events shown  " : " events hidden  ", dim, false);
    sx = text(sx, y0 + 1, x1 - 2, "c", keyst, false);
    lw.cal_x = text(sx, y0 + 1, x1 - 2, " calendar", dim, false);
    // what every log on disk takes (the files this window lists): on the line under the find line, at the right,
    // like a title on the border (a row of its own would cost the log a line); left out if it doesn't fit
    unsigned long long used = 0;
    for (auto& f : lw.files) used += (unsigned long long)f.size;
    std::u32string tot = from_utf8(" " + human_bytes(used) + " on disk · " + thousands(lw.files.size()) + (lw.files.size() == 1 ? " file " : " files "));
    int tx = x1 - 2 - (int)tot.size();
    if (tx > lw.xd + 1) text(tx, y0 + 2, x1 - 2, to_utf8(tot), hs, false);

    // the tree
    auto t = log_tree();
    lw.sel = std::max(0, std::min(lw.sel, (int)t.size() - 1));
    if (lw.sel < lw.ttop) lw.ttop = lw.sel;
    if (lw.sel >= lw.ttop + lw.rows) lw.ttop = lw.sel - lw.rows + 1;
    if (t.empty()) {
        bool on = false;
        for (auto& s : c.servers) on |= c.opt_on(s.get(), "log", false);
        text(lw.x0 + 2, lw.body_y, lw.xd, "No logs yet", hs, false);
        if (!on) text(lw.x0 + 2, lw.body_y + 2, lw.xd, "Logging is off:", dim, false), text(lw.x0 + 2, lw.body_y + 3, lw.xd, "/set log on, /save", keyst, false);
    }
    for (int i = 0; i < lw.rows && lw.ttop + i < (int)t.size(); i++) {
        const LogNode& n = t[(size_t)(lw.ttop + i)];
        int y = lw.body_y + i;
        bool on = lw.ttop + i == lw.sel, focus = on && lw.focus == 0;
        Style st = focus ? sel : bs, d = dim;
        if (focus) d.fg = sel.fg, on_row(d, sel);
        else if (on) st.attr |= A_BOLD | A_UNDER;
        fill(y, lw.x0 + 1, lw.xd, st);
        std::string label, right;
        if (n.lvl == 0) label = std::string(lw.unfolded.count(n.net) ? "▾ " : "▸ ") + n.net, right = std::to_string(n.n);
        else if (n.lvl == 1) label = std::string(lw.unfolded.count(n.net + "\t" + n.name) ? "  ▾ " : "  ▸ ") + n.name, right = std::to_string(n.n) + (n.n == 1 ? " day" : " days");
        else {
            auto& f = lw.files[(size_t)n.file];
            label = std::string(n.file == lw.file && !lw.searched ? "    ● " : "      ") + f.day, right = human_bytes((unsigned long long)f.size);
        }
        int rx = lw.xd - 1 - (int)right.size();
        text(lw.x0 + 2, y, rx - 1, label, n.lvl == 1 && !focus ? (on ? st : hs) : st, false);
        text(rx, y, lw.xd - 1, right, d, false);
    }

    // the log: a heading row, then the lines
    int lx = lw.xd + 2, lxe = x1 - 2, lwid = lxe - lx;
    for (int yy = lw.body_y; yy < sep2; yy++) fill(yy, lw.xd + 1, x1 - 1, def);
    Style head = def;
    head.attr |= A_BOLD;
    Style hd = sty(T, "timestamp", "default");
    hd.bg = def.bg;
    int hx = lx;
    if (lw.searched) {
        hx = text(hx, lw.body_y, lxe, "\"" + to_utf8(lw.q) + "\" in " + lw.found_in, head, false);
        size_t n = lw.hits.size();
        text(hx, lw.body_y, lxe, ": " + (n ? thousands(n) + (lw.more ? "+" : "") + (n == 1 ? " line" : " lines") : std::string("nothing")) +
                                      " in " + std::to_string(lw.found_days) + (lw.found_days == 1 ? " day" : " days") + (lw.more ? " (the first 2,000)" : ""), hd, false);
    } else if (lw.file >= 0) {
        auto& f = lw.files[(size_t)lw.file];
        hx = text(hx, lw.body_y, lxe, f.name + " · " + f.net + " · " + day_label(f.day) + (f.current ? " (today's file)" : ""), head, false);
        std::string cnt = thousands(lw.lines.size()) + " lines";
        if (lw.view.size() != lw.lines.size()) cnt = thousands(lw.view.size()) + " of " + cnt;
        if (lw.cut) cnt += " (its last 32 MB)";
        text(hx + 2, lw.body_y, lxe, cnt, hd, false);
    } else if (!t.empty()) {
        const LogNode& n = t[(size_t)lw.sel];
        std::vector<std::string> info;
        if (n.lvl == 0) info = {n.net + ": " + std::to_string(n.n) + (n.n == 1 ? " channel or query" : " channels and queries") + " with logs", "", "/ then Enter searches all of them", "→ or Enter lists them"};
        else if (n.lvl == 1) {
            std::string a, z;
            for (auto& f : lw.files) if (f.net == n.net && f.name == n.name) { if (z.empty()) z = f.day; a = f.day; }
            info = {n.name + " · " + n.net + ": " + std::to_string(n.n) + (n.n == 1 ? " day" : " days") + " of logs, " + a + " to " + z, "",
                    "→ or Enter lists the days", "c picks one on a calendar", "/ then Enter searches all of them"};
        }
        for (size_t i = 0; i < info.size(); i++) text(lx, lw.body_y + (int)i, lxe, info[i], i ? hd : head, false);
    }
    // keep the cursor's line on screen
    int lrows = lw.rows - 1, ly = lw.body_y + 1;
    bool rot = c.opt_on(nullptr, "log_rotate", true);
    std::vector<std::u32string> words;  // what the find line looks for: shown reversed in the lines
    for (auto& w : log_query(to_utf8(lw.q)).words) words.push_back(from_utf8(w));
    auto rows_of = [&](int i) {
        const LogItem& it = lw.view[(size_t)i];
        if (!it.l) {
            Row r;
            auto& f = lw.files[(size_t)it.file];
            cells(r, "── " + day_label(f.day) + (lw.found_in == f.net ? " · " + f.name : "") + " ", mark, false);
            while (width(r) < lwid) r.push_back({U'─', mark, 1});
            return std::vector<Row>{r};
        }
        bool long_ts = lw.files[(size_t)it.file].current && !rot;  // one file for every day
        auto rows = wrap(c, *it.l, log_buf(), lwid, true, long_ts ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S");
        for (auto& row : rows) mark_words(row, words);
        return rows;
    };
    int nv = (int)lw.view.size();
    lw.cur = std::max(0, std::min(lw.cur, nv - 1));
    lw.ltop = std::max(0, std::min(lw.ltop, std::max(0, nv - 1)));
    if (lw.cur < lw.ltop) lw.ltop = lw.cur;
    for (;;) {
        int used = 0;
        for (int i = lw.ltop; i <= lw.cur && i < nv && used <= lrows; i++) used += (int)rows_of(i).size();
        if (used <= lrows || lw.ltop >= lw.cur) break;
        lw.ltop++;
    }
    lw.row_item.assign((size_t)std::max(0, lrows), -1);
    if (nv == 0 && (lw.searched || lw.file >= 0))
        text(lx, ly + 1, lxe, lw.searched ? "Nothing found. Search a whole network: pick it in the tree" : "No line here matches the filter", hd, false);
    int r = 0;
    for (int i = lw.ltop; i < nv && r < lrows; i++)
        for (auto& row : rows_of(i)) {
            if (r >= lrows) break;
            lw.row_item[(size_t)r] = i;
            if (i == lw.cur && lw.focus == 1) cell(lw.xd + 1, ly + r, U'▌', sel);  // the cursor's line
            draw_row(lx, ly + r, lxe, row);
            r++;
        }
    if (nv > lrows) {  // where in the day: a scroll mark on the right edge
        int y = ly + (int)((long long)lw.cur * (lrows - 1) / std::max(1, nv - 1));
        cell(x1 - 2, y, U'┃', dim);
    }

    // the hint, or what just went wrong
    std::string hint = lw.focus == 0 ? "↑↓ move · → ← unfold, fold · Enter opens · / find · c calendar · e events · Tab the log · Esc closes"
                     : lw.focus == 1 ? "↑↓ PgUp PgDn Home End · [ ] older, newer day · Enter: a found line in its day · / find · c calendar · Tab the tree"
                     : lw.focus == 2 ? "Enter searches the tree's network or channel (a day: filters it) · nick:alice · \"a phrase\" · Ctrl+U clears · Esc leaves"
                                     : "←→↑↓ a day · PgUp PgDn a month · [ ] the last, next day with a log · Enter opens it · Esc back";
    if (!lw.msg.empty()) text(lw.x0 + 2, hint_y, x1 - 2, lw.msg, es, false);
    else text(lw.x0 + 2, hint_y, x1 - 2, hint, dim, false);

    // the calendar, over the log
    if (lw.focus == 3) {
        std::set<int> have;  // the month's days with a log
        for (auto& f : lw.files)
            if (f.net == lw.cnet && f.name == lw.cname && atoi(f.day.substr(0, 4).c_str()) == lw.cy && atoi(f.day.substr(5, 2).c_str()) == lw.cm) have.insert(atoi(f.day.substr(8, 2).c_str()));
        int cw = 30, ch = 13, cx = lw.xd + 1 + std::max(0, (x1 - 1 - lw.xd - cw) / 2), cy = lw.body_y + std::max(0, (lw.rows - ch) / 2);
        lw.cal_x = cx, lw.cal_y = cy;
        for (int yy = cy; yy < cy + ch; yy++) fill(yy, cx, cx + cw, bs);
        for (int xx = cx; xx < cx + cw; xx++) cell(xx, cy, U'─', ls), cell(xx, cy + ch - 1, U'─', ls);
        for (int yy = cy; yy < cy + ch; yy++) cell(cx, yy, U'│', ls), cell(cx + cw - 1, yy, U'│', ls);
        cell(cx, cy, U'┌', ls), cell(cx + cw - 1, cy, U'┐', ls), cell(cx, cy + ch - 1, U'└', ls), cell(cx + cw - 1, cy + ch - 1, U'┘', ls);
        if (auto cb = box_title(T, cx, cx + cw - 1, cy, "Calendar · " + lw.cname, hs); cb.first >= 0) xbtn = {cy, cb.first, cb.second, 'C'};
        std::string mt = std::string(MONTHS[lw.cm - 1]) + " " + std::to_string(lw.cy);
        text(cx + 2, cy + 2, cx + cw, "◂", keyst, false), text(cx + cw - 3, cy + 2, cx + cw, "▸", keyst, false);
        text(cx + (cw - (int)mt.size()) / 2, cy + 2, cx + cw - 3, mt, hs, false);
        const char* wd[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
        for (int i = 0; i < 7; i++) text(cx + 4 + i * 3, cy + 3, cx + cw, wd[i], dim, false);
        tm t1{};
        t1.tm_year = lw.cy - 1900, t1.tm_mon = lw.cm - 1, t1.tm_mday = 1, t1.tm_hour = 12, t1.tm_isdst = -1;
        mktime(&t1);
        int off = (t1.tm_wday + 6) % 7, nd = month_days(lw.cy, lw.cm);
        std::string today = fmt_time(time(nullptr), "%Y-%m-%d");
        for (int d = 1; d <= nd; d++) {
            int k = off + d - 1, x = cx + 4 + (k % 7) * 3, y = cy + 4 + k / 7;
            bool has = have.count(d), at = d == lw.cd;
            Style st = at ? sel : has ? ok : dim;
            if (has) st.attr |= A_BOLD;
            if (ymd(lw.cy, lw.cm, d) == today) st.attr |= A_UNDER;
            char b[16];
            snprintf(b, sizeof b, "%2d", d);
            if (at) cell(x - 1, y, ' ', st), cell(x + 2, y, ' ', st);
            text(x, y, x + 2, b, st, false);
        }
        std::string n = std::to_string(have.size()) + (have.size() == 1 ? " day" : " days") + " with a log";
        text(cx + 2, cy + ch - 2, cx + cw - 1, n, have.empty() ? dim : ok, false);
    }
}

void Tui::logs_key(int k, uint32_t ch) {
    if (ch == ' ') k = TB_KEY_SPACE, ch = 0;
    lw.msg.clear();
    bool bs = k == TB_KEY_BACKSPACE || k == TB_KEY_BACKSPACE2;
    auto t = log_tree();
    int nt = (int)t.size(), nv = (int)lw.view.size(), page = std::max(1, lw.rows - 2);
    if (lw.focus == 2) {  // the find line
        if (k == TB_KEY_ESC || k == TB_KEY_TAB) { lw.focus = lw.back; return; }
        if (k == TB_KEY_ENTER) {
            lw.focus = 1;
            if (nt && t[(size_t)lw.sel].lvl == 2 && lw.file == t[(size_t)lw.sel].file && !lw.searched) logs_view();
            else logs_search();
            return;
        }
        if (bs) { if (!lw.q.empty()) lw.q.pop_back(); }
        else if (k == TB_KEY_CTRL_U) lw.q.clear();
        else if (ch || k == TB_KEY_SPACE) lw.q += ch ? (char32_t)ch : U' ';
        else return;
        if (!lw.searched && lw.file >= 0) logs_view(), lw.cur = 0, lw.ltop = 0;  // a day: filtered as you type
        if (lw.searched && lw.q.empty()) lw.searched = false, logs_view();
        return;
    }
    if (lw.focus == 3) {  // the calendar
        auto to = [&](int y, int m, int d) {
            while (m < 1) m += 12, y--;
            while (m > 12) m -= 12, y++;
            lw.cy = y, lw.cm = m, lw.cd = std::max(1, std::min(d, month_days(y, m)));
        };
        auto step = [&](int days) {
            tm x{};
            x.tm_year = lw.cy - 1900, x.tm_mon = lw.cm - 1, x.tm_mday = lw.cd + days, x.tm_hour = 12, x.tm_isdst = -1;
            mktime(&x);
            to(x.tm_year + 1900, x.tm_mon + 1, x.tm_mday);
        };
        std::vector<std::pair<std::string, int>> days;  // newest first
        for (size_t i = 0; i < lw.files.size(); i++)
            if (lw.files[i].net == lw.cnet && lw.files[i].name == lw.cname) days.push_back({lw.files[i].day, (int)i});
        std::string at = ymd(lw.cy, lw.cm, lw.cd);
        auto go = [&](const std::string& d) { to(atoi(d.substr(0, 4).c_str()), atoi(d.substr(5, 2).c_str()), atoi(d.substr(8, 2).c_str())); };
        if (k == TB_KEY_ESC || ch == 'c' || ch == 'q') lw.focus = lw.back;
        else if (k == TB_KEY_ARROW_LEFT) step(-1);
        else if (k == TB_KEY_ARROW_RIGHT) step(1);
        else if (k == TB_KEY_ARROW_UP) step(-7);
        else if (k == TB_KEY_ARROW_DOWN) step(7);
        else if (k == TB_KEY_PGUP || ch == '<') to(lw.cy, lw.cm - 1, lw.cd);
        else if (k == TB_KEY_PGDN || ch == '>') to(lw.cy, lw.cm + 1, lw.cd);
        else if (ch == '[') { for (auto& d : days) if (d.first < at) { go(d.first); break; } }
        else if (ch == ']') { for (auto it = days.rbegin(); it != days.rend(); ++it) if (it->first > at) { go(it->first); break; } }
        else if (k == TB_KEY_HOME && !days.empty()) go(days.back().first);
        else if (k == TB_KEY_END && !days.empty()) go(days.front().first);
        else if (k == TB_KEY_ENTER || k == TB_KEY_SPACE) {
            for (auto& d : days)
                if (d.first == at) { logs_open(d.second, -1), lw.focus = 1; return; }
            lw.msg = "No log on " + day_label(at) + ": the days with one are in color, [ and ] go to them";
        }
        return;
    }
    if (k == TB_KEY_ESC || ch == 'q') { lw.open = false; return; }
    if (k == TB_KEY_TAB) { lw.focus = lw.focus == 0 && (nv || lw.file >= 0 || lw.searched) ? 1 : 0; return; }
    if (ch == '/' || k == TB_KEY_CTRL_F) { lw.back = lw.focus, lw.focus = 2; return; }
    if (ch == 'c') { logs_cal(); return; }
    if (ch == 'e') {
        lw.events = !lw.events;
        if (lw.searched) logs_search();
        else logs_view();
        return;
    }
    if ((ch == '[' || ch == ']') && lw.file >= 0) {  // the day before or after, of the same channel
        int f = lw.file + (ch == '[' ? 1 : -1);
        if (f >= 0 && f < (int)lw.files.size() && lw.files[(size_t)f].net == lw.files[(size_t)lw.file].net && lw.files[(size_t)f].name == lw.files[(size_t)lw.file].name)
            logs_open(f, ch == '[' ? -2 : -1);
        else lw.msg = ch == '[' ? "That's its oldest log" : "That's its newest log";
        return;
    }
    if (lw.focus == 0) {
        if (!nt) return;
        const LogNode n = t[(size_t)lw.sel];
        std::string key = n.lvl == 0 ? n.net : n.net + "\t" + n.name;
        if (k == TB_KEY_ARROW_UP) lw.sel = std::max(0, lw.sel - 1);
        else if (k == TB_KEY_ARROW_DOWN) lw.sel = std::min(nt - 1, lw.sel + 1);
        else if (k == TB_KEY_PGUP) lw.sel = std::max(0, lw.sel - page);
        else if (k == TB_KEY_PGDN) lw.sel = std::min(nt - 1, lw.sel + page);
        else if (k == TB_KEY_HOME) lw.sel = 0;
        else if (k == TB_KEY_END) lw.sel = nt - 1;
        else if (k == TB_KEY_ARROW_RIGHT && n.lvl < 2) {
            if (!lw.unfolded.count(key)) lw.unfolded.insert(key);
            else lw.sel = std::min(nt - 1, lw.sel + 1);
        } else if (k == TB_KEY_ARROW_LEFT) {
            if (n.lvl < 2 && lw.unfolded.count(key)) lw.unfolded.erase(key);
            else if (n.lvl > 0)
                for (int i = lw.sel; i >= 0; i--)
                    if (t[(size_t)i].lvl == n.lvl - 1) { lw.sel = i; break; }
        } else if (k == TB_KEY_ENTER || k == TB_KEY_SPACE) {
            if (n.lvl == 2) logs_open(n.file, -1), lw.focus = 1;
            else if (lw.unfolded.count(key)) lw.unfolded.erase(key);
            else lw.unfolded.insert(key);
        }
        return;
    }
    // the log
    if (k == TB_KEY_ARROW_UP) lw.cur = std::max(0, lw.cur - 1);
    else if (k == TB_KEY_ARROW_DOWN) lw.cur = std::min(nv - 1, lw.cur + 1);
    else if (k == TB_KEY_PGUP) lw.cur = std::max(0, lw.cur - page), lw.ltop = std::max(0, lw.ltop - page);
    else if (k == TB_KEY_PGDN) lw.cur = std::min(nv - 1, lw.cur + page), lw.ltop = std::min(std::max(0, nv - 1), lw.ltop + page);
    else if (k == TB_KEY_HOME) lw.cur = 0, lw.ltop = 0;
    else if (k == TB_KEY_END) lw.cur = nv - 1;
    else if (k == TB_KEY_ENTER && lw.cur < nv) {  // a found or filtered line: shown in the whole day
        const LogItem& it = lw.view[(size_t)lw.cur];
        bool filtered = lw.searched || !lw.q.empty();
        if (!it.l) logs_open(it.file, -1);
        else if (filtered) logs_open(it.file, (long)it.line);
    }
}

void Tui::logs_mouse(int k, int x, int y) {
    bool wheel = k == TB_KEY_MOUSE_WHEEL_UP || k == TB_KEY_MOUSE_WHEEL_DOWN;
    int up = k == TB_KEY_MOUSE_WHEEL_UP;
    if (lw.focus == 3) {  // the calendar: a day opens it, the arrows turn the month, the wheel too
        int cx = lw.cal_x, cy = lw.cal_y;
        if (wheel) { logs_key(up ? TB_KEY_PGUP : TB_KEY_PGDN, 0); return; }
        if (k != TB_KEY_MOUSE_LEFT) return;
        if (y == cy + 2 && x <= cx + 3) { logs_key(TB_KEY_PGUP, 0); return; }
        if (y == cy + 2 && x >= cx + 26) { logs_key(TB_KEY_PGDN, 0); return; }
        if (y >= cy + 4 && y < cy + 10 && x >= cx + 3 && x < cx + 24) {
            tm t1{};
            t1.tm_year = lw.cy - 1900, t1.tm_mon = lw.cm - 1, t1.tm_mday = 1, t1.tm_hour = 12, t1.tm_isdst = -1;
            mktime(&t1);
            int d = (y - cy - 4) * 7 + (x - cx - 3) / 3 - (t1.tm_wday + 6) % 7 + 1;
            if (d >= 1 && d <= month_days(lw.cy, lw.cm)) lw.cd = d, logs_key(TB_KEY_ENTER, 0);
            return;
        }
        if (x < cx || x >= cx + 30 || y < cy || y >= cy + 13) lw.focus = lw.back;  // a click outside closes it
        return;
    }
    if (y == lw.y0 + 1 && k == TB_KEY_MOUSE_LEFT) {  // the find line, e, c
        int fe = lw.find_x + std::min(44, lw.w - 50);
        if (x >= lw.cal_x - 10 && x < lw.cal_x) logs_cal();
        else if (x >= lw.find_x && x < fe) lw.back = lw.focus == 2 ? lw.back : lw.focus, lw.focus = 2;
        else if (x > fe && x < lw.cal_x - 10) logs_key(0, 'e');
        return;
    }
    int i = y - lw.body_y;
    if (i < 0 || i >= lw.rows) return;
    if (x < lw.xd) {  // the tree
        if (wheel) { lw.sel = std::max(0, lw.sel + (up ? -3 : 3)); return; }
        if (k != TB_KEY_MOUSE_LEFT) return;
        auto t = log_tree();
        int j = lw.ttop + i;
        if (j >= (int)t.size()) return;
        lw.sel = j, lw.focus = 0;
        logs_key(TB_KEY_ENTER, 0);
        return;
    }
    if (wheel) { lw.cur = std::max(0, std::min((int)lw.view.size() - 1, lw.cur + (up ? -3 : 3))); lw.focus = 1; return; }
    if (k != TB_KEY_MOUSE_LEFT || i < 1 || i - 1 >= (int)lw.row_item.size() || lw.row_item[(size_t)(i - 1)] < 0) return;
    long long now = now_ms();
    int it = lw.row_item[(size_t)(i - 1)];
    bool again = lw.cur == it && now - lw.click_ms < 500;
    lw.click_ms = now, lw.cur = it, lw.focus = 1;
    if (again) logs_key(TB_KEY_ENTER, 0);  // a double click: the line in its day
}
