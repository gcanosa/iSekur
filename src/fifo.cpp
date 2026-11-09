// The FIFO, like weechat's fifo pipe: a named pipe that scripts and shells write iSekur's lines into.
//   echo "/msg bob hi" > ~/.config/isekur/fifo
// Each line runs as if typed in the active tab (a line without a leading / is a message there). The pipe is mode 0600,
// so only your user can write it. Lines are dropped while iSekur is locked. POSIX only: Linux, macOS, the BSDs and
// Cygwin. Native Windows has no mkfifo, so there is no FIFO there.
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "core.h"

static const size_t MAX_FIFO_LINE = 8192;  // a longer line with no newline is dropped, not kept forever

void Core::fifo_open() {
    bool was = fifo_fd >= 0;
    fifo_close();
    if (!opt_on(nullptr, "fifo", false)) {
        if (was) print(bufs.front().get(), LK::INFO, "FIFO off");
        return;
    }
    std::string p = ini_get(cfg, "general", "fifo_path");
    if (p.empty()) p = "~/.config/isekur/fifo";
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
    Buffer* b = active && index_of(active) >= 0 ? active : bufs.front().get();
    exec(b, l);
}
