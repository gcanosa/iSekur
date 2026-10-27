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
    bool init(std::string& err);
    void shutdown();
    void fds(int& tty, int& resize);
    void on_readable();  // drain pending terminal events
    void draw();

private:
    std::u32string input;
    size_t cur = 0;
    std::vector<std::string> hist;
    size_t hpos = 0;
    std::map<std::string, std::string> keymap;  // "M-Right" -> "next_tab"
    bool nicklist = true, truecolor = true;
    // nick completion
    std::vector<std::string> comp;
    size_t comp_i = 0, comp_start = 0;
    bool completing = false;

    void key(int mod, int key, uint32_t ch);
    bool action(const std::string& a);
    void complete();
    void submit();
    int chat_height() const;
};
