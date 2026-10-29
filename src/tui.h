// Terminal frontend (termbox2).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

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

private:
    std::u32string input;
    size_t cur = 0;
    std::string paste;  // a multi-line paste waiting for y/n
    std::string confirm;  // "question\n/command" from a menu, waiting for y/n
    std::vector<std::string> hist;
    size_t hpos = 0;
    std::map<std::string, std::string> keymap;
    void apply_cfg();  // mouse, colors, keys
    void load_keys();  // defaults + [keys]; again after /set keys.*  // "M-Right" -> "next_tab"
    bool nicklist = true, topicbar = true, truecolor = true, mouse_on = true;
    int top_row() const { return topicbar ? topic_h + 1 : 1; }  // first chat row, under the topic and tab bars
    // topic bar: wheel scrolls it sideways, a click unfolds it, a double click edits it
    int topic_h = 1, topic_off = 0;
    bool topic_open = false;
    const Buffer* topic_buf = nullptr;
    long long click_ms = 0;
    long long chat_click_ms = 0;
    int chat_click_y = -1;
    std::string word_at(int x, int y) const;
    // drag in the chat selects text, copied on release (OSC 52)
    int sel_x0 = -1, sel_y0 = 0, sel_x1 = 0, sel_y1 = 0;
    bool selecting = false;
    bool in_sel(int x, int y) const;
    void copy_sel();
    // clickable regions from the last draw(): tabs, activity numbers, nicks
    struct Hit { int y, x0, x1; int tab; std::string nick; };
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
    bool shell_keys() const;  // a live shell tab: keys go to it
    void shell_key(int mod, int k, uint32_t ch);
    void draw_term(Term* t, int top, int rows, int cols);
    // a prompt box over the middle of the screen (/shell login, passwords, the lock screen)
    void draw_dialog(int W, int H, const std::string& title, const std::vector<std::string>& info, const std::string& label,
                     bool hidden, const std::u32string& in, size_t at, const std::string& hint);
    std::u32string lock_in;  // the lock screen's password, apart from the input line
    std::vector<uint8_t> pass_dots;  // how many dots each typed password character shows
    void mouse(int mod, int key, int x, int y);
    // /set alone: every setting in a window over the chat; categories left, keys right, a search on top
    struct SetWin {
        bool open = false, editing = false;
        int focus = 1, cat = 0, sel = 0, top = 0;  // focus: 0 categories, 1 keys
        std::u32string q, ed;                      // the search; the value being edited
        size_t ecur = 0;
        std::string msg;                           // the last thing done, under the list
        int x0 = 0, y0 = 0, w = 0, h = 0, nav_w = 0, list_y = 0;  // where it was drawn, for the mouse
        long long click_ms = 0;
    };
    SetWin sw;
    struct SetView { std::vector<std::string> cats; std::vector<int> counts; std::vector<const void*> rows; };
    void draw_settings(int W, int H);
    void settings_key(int mod, int k, uint32_t ch);
    void settings_mouse(int k, int x, int y);
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
