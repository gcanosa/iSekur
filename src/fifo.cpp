// The FIFO, like weechat's fifo pipe: a named pipe that scripts and shells write iSekur's lines into.
//   echo "/msg bob hi" > ~/.config/isekur/fifo
// Each line runs as if typed in the active tab (a line without a leading / is a message there). The pipe is mode 0600,
// so only your user can write it. Lines are dropped while iSekur is locked. POSIX only: Linux, macOS, the BSDs and
// Cygwin. Native Windows has no mkfifo, so there is no FIFO there.
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "core.h"
#include "session.h"

static const size_t MAX_FIFO_LINE = 8192;  // a longer line with no newline is dropped, not kept forever

// no fifo_path: the main config gets ~/.config/isekur/fifo; any other config gets <name>.fifo beside it, like its socket
static std::string default_fifo(const std::string& cfg) {
    if (cfg.empty() || cfg == default_config_path()) return "~/.config/isekur/fifo";
    return cfg_side(cfg, "fifo");
}

// \n \r \t \e \\ and \xHH in a line stand for the bytes they name, so a message can carry color and control codes:
// \x02 bold, \x03 color, \x0f reset, \x1b (or \e) the escape. A decoded \n ends a line, like a real one.
std::vector<std::string> fifo_lines(const std::string& s) {
    std::vector<std::string> out{""};
    auto put = [&](char c) { if (c == '\n') out.push_back(""); else out.back() += c; };
    auto hex = [](char c) { return isdigit((unsigned char)c) ? c - '0' : isxdigit((unsigned char)c) ? tolower(c) - 'a' + 10 : -1; };
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { put(s[i]); continue; }
        char e = s[++i];
        switch (e) {
            case 'n': put('\n'); break;
            case 'r': put('\r'); break;
            case 't': put('\t'); break;
            case 'e': put('\x1b'); break;
            case '\\': put('\\'); break;
            case 'b': put('\x02'); break;  // bold
            case 'i': put('\x1d'); break;  // italics
            case 'u': put('\x1f'); break;  // underline
            case 's': put('\x1e'); break;  // strikethrough
            case 'm': put('\x11'); break;  // monospace
            case 'v': put('\x16'); break;  // reverse
            case 'o': put('\x0f'); break;  // reset all formatting
            case 'x':
                if (i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
                    put((char)(hex(s[i + 1]) * 16 + hex(s[i + 2])));
                    i += 2;
                    break;
                }
                put('\\'), put('x');  // not a \xHH: kept as typed
                break;
            default: put('\\'), put(e);  // unknown escape: kept as typed
        }
    }
    return out;
}

void Core::fifo_open() {
    bool was = fifo_fd >= 0;
    fifo_close();
    if (!opt_on(nullptr, "fifo", false)) {
        if (was) print(bufs.front().get(), LK::INFO, "FIFO off");
        return;
    }
    std::string p = ini_get(cfg, "general", "fifo_path");
    if (p.empty()) p = default_fifo(cfg_path);
    if (p[0] == '~' && p.size() > 1 && p[1] == '/')
        if (const char* h = getenv("HOME")) p = h + p.substr(1);
    struct stat st;
    if (::stat(p.c_str(), &st) == 0) {
        if (!S_ISFIFO(st.st_mode)) { print(bufs.front().get(), LK::ERROR, "FIFO: " + p + " exists and is not a named pipe"); return; }
        if (st.st_mode & 077) ::chmod(p.c_str(), 0600);  // only you can write it
    } else if (::mkfifo(p.c_str(), 0600) == 0) fifo_made = true;
    else { print(bufs.front().get(), LK::ERROR, "FIFO: cannot create " + p + ": " + strerror(errno)); return; }
    // read and write at once: a writer closing the pipe is never an end of file, so poll() never spins
    fifo_fd = ::open(p.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fifo_fd < 0) {
        print(bufs.front().get(), LK::ERROR, "FIFO: cannot open " + p + ": " + strerror(errno));
        fifo_close();
        return;
    }
    fifo_path = p;
    print(bufs.front().get(), LK::INFO, "FIFO: " + p + " (a line written to it runs here)");
}

void Core::fifo_close() {
    if (fifo_fd >= 0) ::close(fifo_fd), fifo_fd = -1;
    if (fifo_made) ::unlink(fifo_path.c_str()), fifo_made = false;  // one iSekur created: gone at exit
    fifo_path.clear(), fifo_buf.clear();
}

// the bytes poll() marked, one line at a time
void Core::fifo_poll(const std::vector<pollfd>& fds, size_t k) {
    if (fifo_fd < 0 || k >= fds.size() || fds[k].fd != fifo_fd || !fds[k].revents) return;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(fifo_fd, buf, sizeof buf)) > 0) {
        fifo_buf.append(buf, (size_t)n);
        size_t nl;
        while ((nl = fifo_buf.find('\n')) != std::string::npos) {
            std::string l = fifo_buf.substr(0, nl);
            fifo_buf.erase(0, nl + 1);
            fifo_line(l);
        }
        if (fifo_buf.size() > MAX_FIFO_LINE) fifo_buf.clear();
    }
}

void Core::fifo_line(const std::string& raw) {
    std::string l = raw;
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (l.empty()) return;
    if (locked) { print(bufs.front().get(), LK::ERROR, "FIFO: locked, a line was ignored"); return; }
    Buffer* act = active && index_of(active) >= 0 ? active : bufs.front().get();
    // the tab named before the first |: "net/#chan", "net" (its status tab), "#chan" (on the active network), "N" (tab N)
    auto target = [&](const std::string& t) -> Buffer* {
        if (t.size() < 7 && std::all_of(t.begin(), t.end(), [](unsigned char ch) { return isdigit(ch); })) {
            int n = atoi(t.c_str());
            return n >= 1 && n <= (int)bufs.size() ? bufs[(size_t)n - 1].get() : nullptr;
        }
        auto named = [&](const std::string& name) -> Server* {
            for (auto& sv : servers) if (irc_lower(sv->cfg.name) == irc_lower(name)) return sv.get();
            return nullptr;
        };
        size_t sl = t.find('/');
        if (sl == std::string::npos) {
            if (Server* s = named(t)) return s->status;
            return act->srv ? find(act->srv, t) : nullptr;
        }
        Server* s = named(t.substr(0, sl));
        return s ? find(s, t.substr(sl + 1)) : nullptr;
    };
    // a line is "target|text" when the part before the | names a tab or looks like one; with no such tab it is refused
    auto shaped = [&](const std::string& t) {
        if (t.find_first_not_of("0123456789") == std::string::npos || t.find('/') != std::string::npos || t[0] == '#' || t[0] == '&') return true;
        for (auto& sv : servers) if (irc_lower(sv->cfg.name) == irc_lower(t)) return true;
        return false;
    };
    for (auto& x : fifo_lines(l)) {  // escapes decoded; each resulting line is routed and runs
        if (x.empty()) continue;
        Buffer* b = act;
        std::string text = x;
        size_t bar = x.find('|');
        if (bar != std::string::npos && bar > 0 && x.substr(0, bar).find(' ') == std::string::npos) {
            std::string t = x.substr(0, bar);
            if (Buffer* tb = target(t)) b = tb, text = x.substr(bar + 1);
            else if (shaped(t)) { print(bufs.front().get(), LK::ERROR, "FIFO: no open tab " + t + ", the line was ignored"); continue; }
        }
        if (!text.empty()) exec(b, text);
    }
}
