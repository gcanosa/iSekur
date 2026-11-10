// Terminal frontend (termbox2).
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "core.h"

struct Core;
struct Buffer;
struct Term;
struct Tui {
    Core& c;
    explicit Tui(Core& core) : c(core) {}
    bool init(std::string& err, int fd = -1);  // fd: a terminal from isekur --attach
    void shutdown();
    void fds(int& tty, int& resize);
    void on_readable();  // drain pending terminal events
    void draw();
    long long ts_anim_until = 0;  // timestamp_hover: a time fading in or out, redraw ~30 times a second until then

private:
    std::u32string input;
    int typed_id = 0;  // the tab the input line was last typed in (+typing goes there)
    size_t cur = 0;
    std::map<int, std::pair<std::u32string, size_t>> drafts;  // tab id -> its unsent input line and cursor
    int input_id = 0;  // the tab input belongs to
    void sync_input();  // the active tab changed: park input under the old one, bring back the new one's
    std::string paste;  // a multi-line paste waiting for y/n
    std::string confirm;  // "question\n/command" from a menu, waiting for y/n
    std::string confirm_title;  // the menu item that asked
    std::vector<std::string> hist;  // the active tab's sent lines (Up/Down)
    std::map<int, std::vector<std::string>> hists;  // every other tab's, by id
    size_t hpos = 0;
    std::map<std::string, std::string> keymap;
    void apply_cfg();  // mouse, colors, keys
    void load_keys();  // defaults + [keys]; again after /set keys.*  // "M-Right" -> "next_tab"
    bool nicklist = true, topicbar = true, truecolor = true, mouse_on = true;
    // timestamp_hover = term | both: the time shows only on the line under the mouse (moves come with mode 1003)
    bool ts_hover = false, hover_in = false;
    int hover_x = -1, hover_y = -1;
    const void* hover_line = nullptr;  // only compared, never read: a line gone just stops matching
    const void* fade_line = nullptr;
    long long hover_at = 0, fade_at = 0;
    void hover_set(const void* l);
    double hover_alpha(const void* l) const;
    void set_mouse();  // the input mode, and 1003 (every move) while timestamp_hover wants it
    int top_row() const { return (topicbar ? topic_h : 0) + tab_h; }  // first chat row, under the topic and tab bars
    int tab_h = 1;  // tab bar rows, set by draw()
    // topic bar: wheel scrolls it sideways, a click unfolds it, a double click edits it
    int topic_h = 1, topic_off = 0;
    bool topic_open = false;
    const Buffer* topic_buf = nullptr;
    long long click_ms = 0;
    long long chat_click_ms = 0;
    long long nick_click_ms = 0;  // a nick opens a query on a double click: one stray click does nothing
    std::string nick_click;
    std::vector<std::string> nick_sel;  // the nicklist's selection: a click picks one, shift/ctrl+click adds or drops
    int chat_click_y = -1;
    std::string word_at(int x, int y) const;
    // drag in the chat selects text, copied on release (OSC 52)
    int sel_x0 = -1, sel_y0 = 0, sel_x1 = 0, sel_y1 = 0;
    bool selecting = false;
    bool in_sel(int x, int y) const;
    void copy_sel();
    // the chat rows of the last draw, so a copy is the lines' text ("00:08 nick text"), not the screen's padding.
    // id: the line it shows (rows of one id join up), -2 the "new" marker (never copied). bx..bend: its text
    struct CopyRow { int y, x0, bx, bend, id; bool first; std::string ts, marker; int tsx0, tsx1; };
    std::vector<CopyRow> crows;
    // clickable regions from the last draw(): tabs, activity numbers, nicks
    struct Hit { int y, x0, x1; int tab; std::string nick; };
    int tab_drag = -1;  // the id of the tab pressed on: released on another of its server's tabs, it moves there
    int tab_drop = -1;  // while dragging one: the id of the tab it would land on (drawn reversed)
    std::pair<Buffer*, int> drop_at(int x, int y);  // the dragged tab and its /move N if released there, else {nullptr, 0}
    std::vector<Hit> hits;
    int nl_x = -1, nl_scroll = 0;
    // right click on a nick or a channel tab: a menu of commands ("" label = separator); Up/Down/Enter or a click
    struct Menu { int x = 0, y = 0, sel = 0; std::string nick; std::vector<std::pair<std::string, std::string>> items; };
    Menu menu;  // items.empty() = closed
    void open_menu(const std::string& nick, int x, int y);
    const Buffer* nl_buf = nullptr;
    // nick completion
    std::vector<std::string> comp;
    size_t comp_i = 0, comp_start = 0;
    bool completing = false;

    void key(int mod, int key, uint32_t ch);
    bool shell_keys() const;
    bool guide_tab() const;  // the *guide tab is shown: Left/Right and clicks turn its pages  // a live shell tab: keys go to it
    void shell_key(int mod, int k, uint32_t ch);
    void draw_term(Term* t, int top, int rows, int cols, int x0 = 0);
    void draw_chat(Buffer* b, int x, int y, int w, int h);  // a tab's lines (or its shell) in that box
    void draw_nicks(Buffer* b, int sx, int top, int xend, int h, bool focus);
    struct Box { int x = 0, y = 0, w = 0, h = 0; bool in(int px, int py) const { return w && px >= x && px < x + w && py >= y && py < y + h; } };
    Box other;
    Box pane;   // where the focused tab was drawn (a shell tab's clicks go to its program by it)  // /split: where the unfocused pane was drawn (a click there focuses it, the wheel scrolls it)
    // /split: the first (left/top) pane's share in per mille; a drag on the divider (the │ column, or the lower
    // pane's title row) moves it. div_x / div_y: where the divider was drawn, -1 = none
    int div_x = -1, div_y = -1;
    bool div_drag = false, div_moved = false;
    // a prompt box over the middle of the screen (/shell login, passwords, the lock screen)
    void draw_dialog(int W, int H, const std::string& title, const std::vector<std::string>& info, const std::string& label,
                     bool hidden, const std::u32string& in, size_t at, const std::string& hint, int dx = 0, int dy = 0, char kind = 0);
    // the close button (× or the red dot) drawn this frame, and which box it closes: s settings, t theme, f find,
    // a a question, c a yes/no, p the paste check (0: none, the lock's are only the look)
    struct XBtn { int y = -1, x0 = 0, x1 = -1; char kind = 0; };
    XBtn xbtn;
    void close_box();
    void draw_lock(int W, int H, long long ms);  // the lock screen; ms: how far into Core::lock_anim
    std::u32string lock_in;  // the lock screen's password, apart from the input line
    std::vector<uint8_t> pass_dots;  // how many dots each typed password character shows
    void mouse(int mod, int key, int x, int y);
    // /set alone: every setting in a window over the chat; categories left, keys right, a search on top
    struct SetWin {
        bool open = false, editing = false, searching = false;  // searching: "/" was typed, keys go to the search
        int focus = 1, sel = 0, top = 0, ctop = 0;  // focus: 0 categories, 1 keys; ctop: the first category shown
        std::string cat;                           // the chosen category ("" All, "general/Look", "server.x"...)
        std::u32string q, ed;                      // the search; the value being edited
        size_t ecur = 0;
        std::string msg;                           // the last thing done, under the list
        int x0 = 0, y0 = 0, w = 0, h = 0, nav_w = 0, list_y = 0;  // where it was drawn, for the mouse
        long long click_ms = 0;
        bool to_servers = false;                   // /servers: opens at the first server's category
    };
    SetWin sw;
    void draw_settings(int W, int H);
    void settings_key(int mod, int k, uint32_t ch);
    void settings_mouse(int k, int x, int y);
    // /theme alone: the installed themes in a box, a filter on top; moving previews them (the webchat too)
    struct ThemeWin {
        bool open = false;
        std::u32string q;
        std::string orig;  // Esc goes back to it
        int sel = 0, top = 0, x0 = 0, w = 0, list_y = 0, rows = 0;
        long long click_ms = 0;
    };
    ThemeWin tw;
    std::vector<std::string> theme_list() const;  // the installed themes the filter matches
    void draw_themes(int W, int H);
    void theme_key(int k, uint32_t ch);
    void ask_exit(std::string msg);  // /exit, Alt+Q
    void theme_mouse(int k, int x, int y);
    // Ctrl+G / Alt+J: go to a tab by typing part of its name (channel, query, network)
    struct FindWin {
        bool open = false;
        std::u32string q;
        int sel = 0, top = 0, x0 = 0, w = 0, list_y = 0, rows = 0;
    };
    FindWin fw;
    std::vector<Buffer*> find_list() const;  // the tabs the filter matches: own name first, then by network
    void draw_find(int W, int H);
    void find_key(int k, uint32_t ch);
    void find_mouse(int k, int x, int y);
    // /keys, F1, Alt+K: every key binding as keycaps, by group, from the live keymap ([keys] included)
    struct KeysWin { bool open = false; int top = 0, rows = 0, n = 0, x0 = 0, w = 0, list_y = 0; };
    KeysWin kw;
    // /plugin alone: the installed plugins and the catalog's, a tab each; c configures one, per scope
    struct PlugWin {
        bool open = false, avail = false;  // avail: the Available tab
        int sel = 0, top = 0, rows = 0, x0 = 0, w = 0, list_y = 0, tab_y = 0, tab_mid = 0;  // tab_mid: Installed | Available
        std::string cfg;                   // configuring this plugin ("" = the lists)
        int scope = 0;                     // index into Core::plugin_scopes()
        bool editing = false;
        std::u32string ed;
        std::string ask;                   // remove this one? (y/n)
        std::string about;                 // the About box of this one (a)
        long long mark = -1;               // mb->added when an action ran: mb's newest line since then is the message
        Buffer* mb = nullptr;
        long long click_ms = 0;
    };
    PlugWin pw;
    void draw_plugins(int W, int H);
    void plugin_key(int k, uint32_t ch);
    void plugin_mouse(int k, int x, int y);
    void plug_run(const std::string& cmd);
    // /logs: the log browser. Left the tree (network > channel or query > days), right the day's lines or what a
    // search found; a find line on top, a calendar of the days a channel has logs (c)
    struct LogNode { int lvl; std::string net, name; int file, n; };  // lvl 0 network, 1 channel/query, 2 a day (file)
    struct LogItem { int file; size_t line; const Line* l; };       // l null: a day's heading (search results)
    struct LogWin {
        bool open = false;
        int focus = 0, back = 0;  // 0 the tree, 1 the log, 2 the find line, 3 the calendar; back: where Esc leaves the find line for
        std::vector<LogFile> files;
        std::set<std::string> unfolded;  // "net", "net\tname"
        int sel = 0, ttop = 0;           // the tree
        int file = -1;                   // the day shown
        std::vector<Line> lines;
        bool cut = false;                // only its last 32 MB
        std::u32string q;
        bool events = true;              // joins, parts, quits, nick changes shown
        bool searched = false, more = false;  // the right side lists a search's hits (more: there were more than shown)
        std::string found_in;            // what was searched: "#c" or "network"
        size_t found_days = 0;
        std::vector<LogHit> hits;
        std::vector<LogItem> view;
        int cur = 0, ltop = 0;           // the log's cursor and its first item on screen
        std::string msg;                 // the last thing that went wrong, on the hint row until the next key
        int cy = 0, cm = 0, cd = 0;      // the calendar's year, month (1-12) and day
        std::string cnet, cname;         // ...of this channel
        // where it was drawn, for the mouse
        int x0 = 0, y0 = 0, w = 0, body_y = 0, rows = 0, xd = 0, find_x = 0, cal_x = 0, cal_y = 0;
        std::vector<int> row_item;       // the log rows: which item each shows (-1 none)
        long long click_ms = 0;
    };
    LogWin lw;
    std::vector<LogNode> log_tree() const;
    void logs_init();
    void logs_open(int file, long line);  // line: its index, -1 the top, -2 the end
    void logs_view();                     // view from lines (filtered) or hits
    void logs_search();
    void logs_select(int file);           // the tree shows and selects that day
    void logs_cal();                      // the calendar, at the tree's channel
    void draw_logs(int W, int H);
    void logs_key(int k, uint32_t ch);
    void logs_mouse(int k, int x, int y);
    void draw_keys(int W, int H);
    void keys_key(int k, uint32_t ch);
    bool action(const std::string& a);
    void complete();
    // live list under the input line: config keys after "/set ", commands while
    // typing the first word or after "/help ", the usage of a typed command
    struct Sugg { std::string word, sep, val; };
    int suggest(std::vector<Sugg>& out) const;  // where Tab replaces from; -1 none, -2 shown only
    int sugg_sel = -1;  // Up/Down highlight in that list, -1 = none; Enter or Tab puts it in the input line
    void submit();
    int chat_height() const;
};
