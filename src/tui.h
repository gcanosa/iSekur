// Terminal frontend (termbox2).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct Core;
struct Buffer;
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
    std::vector<std::string> hist;
    size_t hpos = 0;
    std::map<std::string, std::string> keymap;
    void load_keys();  // defaults + [keys]; again after /set keys.*  // "M-Right" -> "next_tab"
    bool nicklist = true, topicbar = true, truecolor = true, mouse_on = true;
    int top_row() const { return topicbar ? topic_h + 1 : 1; }  // first chat row, under the topic and tab bars
    // topic bar: wheel scrolls it sideways, a click unfolds it, a double click edits it
    int topic_h = 1, topic_off = 0;
    bool topic_open = false;
    const Buffer* topic_buf = nullptr;
    long long click_ms = 0;
    // drag in the chat selects text, copied on release (OSC 52)
    int sel_x0 = -1, sel_y0 = 0, sel_x1 = 0, sel_y1 = 0;
    bool selecting = false;
    bool in_sel(int x, int y) const;
    void copy_sel();
    // clickable regions from the last draw(): tabs, activity numbers, nicks
    struct Hit { int y, x0, x1; int tab; std::string nick; };
    std::vector<Hit> hits;
    int nl_x = -1, nl_scroll = 0;
    const Buffer* nl_buf = nullptr;
    // nick completion
    std::vector<std::string> comp;
    size_t comp_i = 0, comp_start = 0;
    bool completing = false;

    void key(int mod, int key, uint32_t ch);
    void mouse(int mod, int key, int x, int y);
    bool action(const std::string& a);
    void complete();
    // live list under the input line: config keys after "/set ", commands while
    // typing the first word or after "/help ", the usage of a typed command
    struct Sugg { std::string word, sep, val; };
    int suggest(std::vector<Sugg>& out) const;  // where Tab replaces from; -1 none, -2 shown only
    void submit();
    int chat_height() const;
};
