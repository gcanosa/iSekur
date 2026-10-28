// Terminal frontend: layout, rendering, keyboard.
#include "tui.h"
#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include "core.h"
#include "termbox2.h"

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

static void cell(int x, int y, uint32_t cp, const Style& s) {
    uintattr_t fg = tbc(s.fg), bg = tbc(s.bg);
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

struct Cell { uint32_t cp; Style st; int w; };
using Row = std::vector<Cell>;

static void cells(Row& out, const std::string& s, Style st, bool mirc = true) {
    auto put = [&](uint32_t cp, const Style& cs) {
        int w = cwidth(cp);
        if (w) out.push_back({cp, cs, w});
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

// theme element with a fallback element instead of "default"
static Style sty(const Theme& t, const std::string& k, const std::string& fb) {
    return t.el.count(k) ? t.get(k) : t.get(fb);
}

static Style nick_style(const Theme& t, const std::string& nick) {
    unsigned h = 5381;
    for (char ch : irc_lower(nick)) h = h * 33 + (unsigned char)ch;
    Style s = t.get("default");
    s.fg = t.nicks[h % t.nicks.size()];
    return s;
}

static std::string buf_label(const Buffer* b) { return b->name; }  // status = config name

// ---- line layout -----------------------------------------------------------
static std::vector<Row> wrap(const Core& c, const Line& l, const Buffer* b, int width_) {
    const Theme& T = c.theme;
    int nw = atoi(T.lay("nick_width", "12").c_str());
    std::string tsfmt = ini_get(c.cfg, "general", "timestamp", T.lay("timestamp", "%H:%M"));
    Row pre;
    cells(pre, fmt_time(l.ts, tsfmt) + " ", T.get("timestamp"), false);

    std::string marker;
    Style ms, body = T.get("default");
    std::string txt = l.text;
    switch (l.kind) {
    case LK::MSG: {
        std::string pfx;
        auto it = b->nicks.find(irc_lower(l.nick));
        if (it != b->nicks.end() && !it->second.prefix.empty()) pfx = it->second.prefix.substr(0, 1);
        marker = pfx + l.nick;
        ms = l.self ? T.get("msg_self") : nick_style(T, l.nick);
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
    default: marker = "--", ms = body = T.get("info"); break;
    }
    Row nick;
    cells(nick, marker, ms, false);
    if (width(nick) > nw) {
        while (!nick.empty() && width(nick) > nw - 1) nick.pop_back();
        nick.push_back({'+', T.get("separator"), 1});
    }
    for (int pad = nw - width(nick); pad > 0; pad--) pre.push_back({' ', T.get("default"), 1});
    pre.insert(pre.end(), nick.begin(), nick.end());
    cells(pre, " " + T.lay("separator", "│") + " ", T.get("separator"), false);

    int indent = width(pre), avail = std::max(10, width_ - indent);
    Row body_cells;
    cells(body_cells, txt, body);

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
        rows.push_back(cur);
        cur.assign((size_t)indent, Cell{' ', T.get("default"), 1});
        i = j;
    }
    if (rows.empty()) rows.push_back(cur);
    return rows;
}

// ---- drawing ---------------------------------------------------------------
int Tui::chat_height() const { return std::max(1, tb_height() - 2 - top_row()); }

void Tui::draw() {
    const Theme& T = c.theme;
    Style def = T.get("default");
    tb_set_clear_attrs(tbc(def.fg), tbc(def.bg));
    tb_clear();
    int W = tb_width(), H = tb_height();
    Buffer* b = c.active;
    c.dirty = false;
    hits.clear(), nl_x = -1;
    if (H < 6 || W < 30 || !b) {
        text(0, 0, W, "terminal too small", def, false);
        tb_present();
        return;
    }

    // topic bar
    if (topicbar) {
        Style ts = T.get("topic");
        fill(0, 0, W, ts);
        std::string topic;
        if (b->type == BT::CHANNEL) topic = b->topic;
        else if (b->type == BT::QUERY) topic = "Query with " + b->name;
        else if (b->srv) {
            static const char* st[] = {"disconnected", "connecting", "registering", "connected"};
            topic = b->srv->cfg.host + ":" + std::to_string(b->srv->cfg.port) +
                    (b->srv->cfg.tls ? " (TLS)" : "") + " - " + st[(int)b->srv->state];
        } else topic = "iSekur - multi-server IRC with a built-in webchat";
        text(1, 0, W - 1, topic, ts);
    }

    // tab bar
    Style tbs = T.get("tabbar");
    int ty = top_row() - 1;
    fill(ty, 0, W, tbs);
    std::vector<Row> labels;
    int ai = c.index_of(b);
    for (size_t i = 0; i < c.bufs.size(); i++) {
        Buffer* t = c.bufs[i].get();
        Style s = t == b ? T.get("tab_active")
                : t->activity == 3 ? T.get("tab_hilight")
                : t->activity == 2 ? T.get("tab_msg")
                : t->activity == 1 ? T.get("tab_data")
                : t->type == BT::STATUS ? sty(T, "tab_server", "tabbar") : tbs;
        Row r;
        cells(r, " " + std::to_string(i + 1) + ":" + buf_label(t) + " ", s, false);
        labels.push_back(r);
    }
    int first = 0, span = 0;
    for (int i = 0; i <= ai; i++) span += width(labels[(size_t)i]);
    while (span > W && first < ai) span -= width(labels[(size_t)first++]);
    for (int i = first, x = 0; i < (int)labels.size() && x < W; i++) {
        int x0 = x;
        x = draw_row(x, ty, W, labels[(size_t)i]);
        hits.push_back({ty, x0, x, i, ""});
    }

    // nick list
    int top = top_row(), ch = chat_height();
    int nlw = nicklist && b->type == BT::CHANNEL && W >= 60 ? atoi(T.lay("nicklist_width", "18").c_str()) : 0;
    int cw = W - (nlw ? nlw + 1 : 0);
    nl_x = nlw ? cw : -1;
    if (nlw) {
        Style bs = sty(T, "nicklist_border", "separator");
        for (int y = top; y < top + ch; y++) text(cw, y, W, T.lay("separator", "│"), bs, false);
        std::vector<const NickEntry*> v;
        for (auto& kv : b->nicks) v.push_back(&kv.second);
        Server* s = b->srv;
        std::sort(v.begin(), v.end(), [&](const NickEntry* x, const NickEntry* y) {
            int rx = s->prefix_rank(x->prefix), ry = s->prefix_rank(y->prefix);
            return rx != ry ? rx < ry : irc_lower(x->nick) < irc_lower(y->nick);
        });
        if (nl_buf != b) nl_buf = b, nl_scroll = 0;
        nl_scroll = std::max(0, std::min(nl_scroll, (int)v.size() - (ch - 1)));
        v.erase(v.begin(), v.begin() + nl_scroll);
        Style hs = sty(T, "nicklist_title", "nicklist");
        fill(top, cw + 1, W, hs);
        text(cw + 2, top, W, std::to_string(v.size() + (size_t)nl_scroll) + " users" + (nl_scroll ? " ↑" : ""), hs, false);
        for (size_t i = 0; i < v.size() && (int)i < ch - 1; i++) {
            const NickEntry* e = v[i];
            hits.push_back({top + 1 + (int)i, cw + 1, W, -1, e->nick});
            Style ns = e->prefix.empty() ? T.get("nicklist")
                     : e->prefix[0] == '+' ? sty(T, "nicklist_voice", "nicklist")
                                           : sty(T, "nicklist_op", "nicklist");
            if (e->away) ns.attr |= A_DIM;
            fill(top + 1 + (int)i, cw + 1, W, T.get("nicklist"));
            text(cw + 2, top + 1 + (int)i, W,
                 (e->prefix.empty() ? " " : e->prefix.substr(0, 1)) + e->nick, ns, false);
        }
        for (int y = top + 1 + (int)v.size(); y < top + ch; y++) fill(y, cw + 1, W, T.get("nicklist"));
    }

    // chat
    std::vector<Row> rows;  // bottom-up
    int need = ch + b->scroll;
    size_t n = 0;
    for (auto it = b->lines.rbegin(); it != b->lines.rend() && (int)rows.size() < need; ++it, ++n) {
        if (b->unread && n == (size_t)b->unread) {  // read marker above the new lines
            Row mk;
            std::string bar = "── new ";
            for (int k = 7; k < cw; k++) bar += "─";
            cells(mk, bar, sty(T, "marker", "separator"), false);
            rows.push_back(mk);
        }
        auto r = wrap(c, *it, b, cw);
        rows.insert(rows.end(), r.rbegin(), r.rend());
    }
    if ((int)rows.size() < need) b->scroll = std::max(0, (int)rows.size() - ch);
    for (int k = 0; k < ch; k++) {
        size_t idx = (size_t)(b->scroll + k);
        if (idx >= rows.size()) break;
        draw_row(0, top + ch - 1 - k, cw, rows[idx]);
    }

    // live suggestions ("/set key", "/jo", "/help k"), Tab completes
    std::vector<Sugg> sk;
    suggest(sk);
    if (!sk.empty()) {
        int n = std::min((int)sk.size(), std::max(1, ch / 2));
        Style ks = T.get("input"), kk = sty(T, "set_key", "info"), kv = sty(T, "set_value", "input"), ke = T.get("separator");
        for (Style* st : {&kk, &kv, &ke})
            if (st->bg < 0) st->bg = ks.bg;
        for (int i = 0; i < n; i++) {
            int yy = top + ch - n + i;
            fill(yy, 0, W, ks);
            if (i == n - 1 && (int)sk.size() > n) {
                text(1, yy, W, "... " + std::to_string(sk.size() - (size_t)n + 1) + " more", ks, false);
                break;
            }
            const Sugg& g = sk[(size_t)i];
            int x2 = text(1, yy, W, g.word, kk, false);
            x2 = text(x2, yy, W, g.sep, ke, false);
            text(x2, yy, W, g.val, kv, false);
        }
    }

    // status bar
    int y = H - 2, x = 0;
    Style ss = T.get("status"), sh = sty(T, "status_hi", "status");
    fill(y, 0, W, ss);
    auto seg = [&](const std::string& inner, Style is) {
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
    std::string acts;
    std::vector<std::pair<int, int>> act;
    for (size_t i = 0; i < c.bufs.size(); i++)
        if (c.bufs[i]->activity) act.push_back({(int)i + 1, c.bufs[i]->activity});
    if (!act.empty()) {
        x = text(x, y, W, "[Act: ", ss, false);
        for (size_t i = 0; i < act.size(); i++) {
            Style as = act[i].second == 3 ? T.get("tab_hilight") : act[i].second == 2 ? T.get("tab_msg") : T.get("tab_data");
            as.bg = ss.bg;
            if (i) x = text(x, y, W, ",", as, false);
            int x0 = x;
            x = text(x, y, W, std::to_string(act[i].first), as, false);
            hits.push_back({y, x0, x, act[i].first - 1, ""});
        }
        x = text(x, y, W, "] ", ss, false);
    }
    if (b->scroll) seg("-- MORE --", T.get("hilight"));

    // input line
    y = H - 1;
    Style is = T.get("input");
    fill(y, 0, W, is);
    if (!paste.empty()) {
        size_t n = 0;
        for (auto& l : split(paste, '\n')) n += !l.empty();
        int px = text(0, y, W, "Send " + std::to_string(n) + " lines to " + buf_label(b) + "? (y/n) ",
                      T.get("hilight"), false);
        tb_set_cursor(px, y);
        tb_present();
        return;
    }
    int px = text(0, y, W, "[" + buf_label(b) + "] ", sty(T, "input_prompt", "input"), false);
    int avail = std::max(1, W - px - 1);
    int curw = 0;
    for (size_t i = 0; i < cur; i++) curw += cwidth(input[i]);
    int off = std::max(0, curw - avail);
    int col = 0;
    for (char32_t ch32 : input) {
        int w = cwidth(ch32);
        if (col >= off && col - off + w <= avail) {
            if (ch32 < 0x20) {  // formatting codes show as reverse letters, like irssi
                Style r = is;
                r.attr |= A_REV;
                cell(px + col - off, y, ch32 == U'\n' ? U'↵' : '@' + ch32, r);
            } else cell(px + col - off, y, ch32, is);
        }
        col += w;
    }
    tb_set_cursor(px + curw - off, y);
    tb_present();
}

// ---- keyboard --------------------------------------------------------------
// vendor/termbox2.c: CSI u / modifyOtherKeys decoding, TB_MOD_SUPER = Cmd
extern "C" int isekur_extract_pre(struct tb_event*, size_t*);
extern "C" int isekur_extract_post(struct tb_event*, size_t*);
static const int TB_MOD_SUPER = 16;

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

bool Tui::init(std::string& err, int fd) {
    int r = fd < 0 ? tb_init() : tb_init_fd(fd);
    if (r != TB_OK) { err = tb_strerror(r) + std::string(" (") + std::to_string(r) + ")"; return false; }
    mouse_on = ini_get(c.cfg, "general", "mouse", "on") != "off";
    tb_set_input_mode(TB_INPUT_ALT | (mouse_on ? TB_INPUT_MOUSE : 0));
    tb_set_func(TB_FUNC_EXTRACT_PRE, isekur_extract_pre);
    tb_set_func(TB_FUNC_EXTRACT_POST, isekur_extract_post);
    std::string mode = ini_get(c.cfg, "general", "colors", "auto");
    const char* ct = getenv("COLORTERM");
    truecolor = mode == "truecolor" ||
                (mode == "auto" && ct && (std::string(ct) == "truecolor" || std::string(ct) == "24bit"));
    g_true = truecolor;
    tb_set_output_mode(truecolor ? TB_OUTPUT_TRUECOLOR : TB_OUTPUT_256);
    nicklist = c.theme.lay("nicklist", "on") != "off";

    load_keys();
    return true;
}

void Tui::load_keys() {
    keymap.clear();
    for (int i = 1; i <= 10; i++) keymap["M-" + std::to_string(i % 10)] = "tab_" + std::to_string(i);
    keymap["M-Right"] = keymap["C-n"] = "next_tab";
    keymap["M-Left"] = keymap["C-p"] = "prev_tab";
    keymap["M-a"] = "next_active";
    keymap["C-x"] = "next_server";
    keymap["C-w"] = "close_tab";
    keymap["PgUp"] = "scroll_up";
    keymap["PgDn"] = "scroll_down";
    keymap["M-u"] = keymap["F2"] = "toggle_nicklist";
    keymap["M-t"] = keymap["†"] = "toggle_topic";  // †: Option+T on a Mac without "Option as Meta"
    keymap["F3"] = "toggle_mouse";
    keymap["M-d"] = keymap["∂"] = "detach";  // ∂: Option+D on a Mac without "Option as Meta"
    // [keys] action = spec [spec...]
    auto ks = c.cfg.find("keys");
    if (ks != c.cfg.end())
        for (auto& [act, specs] : ks->second)
            for (auto& sp : split(specs, ' ')) keymap[sp] = act;
}

void Tui::shutdown() { tb_shutdown(); }

void Tui::fds(int& tty, int& resize) { tb_get_fds(&tty, &resize); }

void Tui::on_readable() {
    tb_event ev;
    // An Enter followed by more input in the same read is a paste: it becomes
    // a newline in the input line instead of sending each line as it comes.
    bool enter = false;
    while (tb_peek_event(&ev, 0) == TB_OK) {
        if (enter) input.insert(cur++, 1, U'\n'), enter = false;
        bool nl = ev.type == TB_EVENT_KEY && !(ev.mod & TB_MOD_ALT) && (ev.key == TB_KEY_ENTER || ev.key == TB_KEY_CTRL_J);
        if (nl && paste.empty()) { enter = true; continue; }
        if (ev.type == TB_EVENT_KEY) key(ev.mod, ev.key, ev.ch);
        else if (ev.type == TB_EVENT_MOUSE) mouse(ev.mod, ev.key, ev.x, ev.y);
        c.dirty = true;
    }
    if (enter) submit(), c.dirty = true;
}

bool Tui::action(const std::string& a) {
    int n = c.index_of(c.active), sz = (int)c.bufs.size();
    auto at = [&](int i) { return c.bufs[(size_t)((i % sz + sz) % sz)].get(); };
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
    } else if (a == "next_server") {
        for (int i = 1; i < sz; i++)
            if (at(n + i)->type == BT::STATUS) { c.select(at(n + i)); break; }
    } else if (a == "close_tab") c.close(c.active);
    else if (a == "scroll_up") c.active->scroll += chat_height() / 2;
    else if (a == "scroll_down") c.active->scroll = std::max(0, c.active->scroll - chat_height() / 2);
    else if (a == "toggle_nicklist") nicklist = !nicklist;
    else if (a == "toggle_topic") topicbar = !topicbar;
    else if (a == "detach") c.detach_req = true;
    else if (a == "toggle_mouse") {  // off = the terminal's own text selection works again
        mouse_on = !mouse_on;
        tb_set_input_mode(TB_INPUT_ALT | (mouse_on ? TB_INPUT_MOUSE : 0));
        c.print(c.active, LK::INFO, std::string("Mouse ") + (mouse_on ? "on (Shift+drag selects text)" : "off"));
    }
    else return false;
    return true;
}

void Tui::key(int mod, int k, uint32_t ch) {
    if (c.keys_changed) c.keys_changed = false, load_keys();
    if (!paste.empty()) {  // "Send N lines?" - y sends, anything else puts it back to edit
        std::string p;
        p.swap(paste);
        if (ch == 'y' || ch == 'Y') c.exec(c.active, p);
        else input = from_utf8(p), cur = input.size();
        return;
    }
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
    if (ch || k == TB_KEY_SPACE) { input.insert(cur++, 1, ch ? (char32_t)ch : U' '); return; }
    switch (k) {
    case TB_KEY_ENTER: submit(); break;
    case TB_KEY_BACKSPACE: case TB_KEY_BACKSPACE2: if (cur) input.erase(--cur, 1); break;
    case TB_KEY_DELETE: if (cur < input.size()) input.erase(cur, 1); break;
    case TB_KEY_ARROW_LEFT: if (cur) cur--; break;
    case TB_KEY_ARROW_RIGHT: if (cur < input.size()) cur++; break;
    case TB_KEY_HOME: case TB_KEY_CTRL_A: cur = 0; break;
    case TB_KEY_END: case TB_KEY_CTRL_E: cur = input.size(); break;
    case TB_KEY_CTRL_U: input.erase(0, cur); cur = 0; break;
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
    case TB_KEY_CTRL_B: case TB_KEY_CTRL_C: case TB_KEY_CTRL_O: case TB_KEY_CTRL_V:
    case TB_KEY_CTRL_RSQ_BRACKET: case TB_KEY_CTRL_UNDERSCORE:
        input.insert(cur++, 1, (char32_t)k);
        break;
    }
}

// Wheel scrolls the chat (or the nicklist under the pointer); left click picks
// a tab or an activity number, or opens a query with a nick.
void Tui::mouse(int mod, int k, int x, int y) {
    if (mod & TB_MOD_MOTION) return;
    if (k == TB_KEY_MOUSE_WHEEL_UP || k == TB_KEY_MOUSE_WHEEL_DOWN) {
        int d = k == TB_KEY_MOUSE_WHEEL_UP ? 3 : -3;
        bool over_nicks = nl_x >= 0 && x > nl_x && y >= top_row() && y < top_row() + chat_height();
        if (over_nicks) nl_scroll = std::max(0, nl_scroll - d);
        else c.active->scroll = std::max(0, c.active->scroll + d);
        c.dirty = true;
        return;
    }
    if (k != TB_KEY_MOUSE_LEFT) return;
    for (auto& h : hits) {
        if (h.y != y || x < h.x0 || x >= h.x1) continue;
        if (h.tab >= 0 && h.tab < (int)c.bufs.size()) c.select(c.bufs[(size_t)h.tab].get());
        else if (!h.nick.empty()) c.exec(c.active, "/query " + h.nick);
        return;
    }
}

void Tui::submit() {
    std::string line = to_utf8(input);
    input.clear(), cur = 0;
    if (line.empty()) return;
    // paste_confirm = N (default 5, 0 = off): ask before sending more lines than that
    size_t lines = 0;
    for (auto& l : split(line, '\n')) lines += !l.empty();
    int max = atoi(c.opt(c.active->srv, "paste_confirm", "5").c_str());
    if (max > 0 && lines > (size_t)max) {
        paste = line;
        return;
    }
    if (hist.empty() || hist.back() != line) hist.push_back(line);
    if (hist.size() > 500) hist.erase(hist.begin());
    hpos = hist.size();
    c.active->scroll = 0;
    c.exec(c.active, line);
}

int Tui::suggest(std::vector<Sugg>& out) const {
    std::string s = to_utf8(input.substr(0, cur));
    if (s.empty() || s[0] != '/' || s.rfind("//", 0) == 0) return -1;
    size_t sp = s.find(' ');
    auto cmds = [&](const std::string& pfx, bool slash) {
        for (auto& k : commands())
            if (std::string(k.name).rfind(irc_lower(pfx), 0) == 0)
                out.push_back({(slash ? "/" : "") + std::string(k.name), (*k.usage ? " " + std::string(k.usage) : "") + "  ", k.summary});
    };
    if (sp == std::string::npos) { cmds(s.substr(1), true); return 0; }  // "/jo"
    std::string cmd = irc_lower(s.substr(1, sp - 1)), arg = s.substr(sp + 1);
    if (arg.find(' ') == std::string::npos) {
        if (cmd == "set") {  // config keys containing what's typed
            for (auto& [sec, kv] : c.cfg)
                for (auto& [k, v] : kv) {
                    std::string full = (sec == "general" ? "" : sec + ".") + k;
                    bool secret = k.find("pass") != std::string::npos || k.find("hash") != std::string::npos;
                    if (irc_lower(full).find(irc_lower(arg)) != std::string::npos)
                        out.push_back({full, " = ", secret ? "********" : v});
                }
            return (int)sp + 1;
        }
        if (cmd == "help") { cmds(arg, false); return (int)sp + 1; }
        if (const CmdInfo* k = find_command(cmd)) {  // "/kickban al": the arguments, until the 2nd word
            out.push_back({"/" + std::string(k->name), (*k->usage ? " " + std::string(k->usage) : "") + "  ", k->summary});
            return -2;
        }
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
        if (from >= 0) {  // a command or config key, counted in UTF-8 bytes: these are ASCII
            st = (size_t)from;
            for (auto& g : sg) comp.push_back(g.word + " ");
        } else {
            std::string pfx = irc_lower(to_utf8(input.substr(st, cur - st)));
            if (pfx.empty()) return;
            for (auto& kv : b->nicks)
                if (kv.first.rfind(pfx, 0) == 0) comp.push_back(kv.second.nick);
            if (b->type == BT::QUERY && irc_lower(b->name).rfind(pfx, 0) == 0) comp.push_back(b->name);
            std::sort(comp.begin(), comp.end(),
                      [](auto& x, auto& y) { return irc_lower(x) < irc_lower(y); });
            for (auto& n : comp) n += st == 0 ? ": " : " ";
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
