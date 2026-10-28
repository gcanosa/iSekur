// Detach / attach over a unix socket; see session.h.
#include "session.h"
#include <algorithm>
#include <cerrno>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>
#include "core.h"

static const char* HELLO = "isekur-attach 1\n";
// what the terminal's side decides: terminfo, truecolor, character widths
static const char* ENV_KEYS[] = {"TERM", "COLORTERM", "LANG", "LC_ALL", "LC_CTYPE"};

std::string sock_path_for(const std::string& cfg_path) {
    return cfg_path.substr(0, cfg_path.rfind('/') + 1) + "isekur.sock";
}

static bool sock_addr(const std::string& path, sockaddr_un& a) {
    a = sockaddr_un{};
    a.sun_family = AF_UNIX;
    if (path.size() >= sizeof a.sun_path) return false;
    memcpy(a.sun_path, path.c_str(), path.size() + 1);
    return true;
}

static void cloexec(int fd) { fcntl(fd, F_SETFD, FD_CLOEXEC); }

// The socket is in a 0700 directory and 0600 itself; this also covers
// systems that ignore socket file permissions.
static bool same_user(int fd) {
#if defined(__CYGWIN__)
    return false;  // listen() refuses there
#elif defined(__linux__)
    ucred cr{};
    socklen_t len = sizeof cr;
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) == 0 && cr.uid == geteuid();
#else
    uid_t u;
    gid_t g;
    return getpeereid(fd, &u, &g) == 0 && u == geteuid();
#endif
}

int Session::listen(const std::string& p, std::string& err) {
    path = p;
#ifdef __CYGWIN__
    err = "Cygwin's unix sockets can't pass a terminal";  // ponytail: the TUI stays in-process there
    return 0;
#endif
    sockaddr_un a;
    if (!sock_addr(p, a)) { err = "socket path too long: " + p; return 0; }
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { err = strerror(errno); return 0; }
    if (connect(s, (sockaddr*)&a, sizeof a) == 0) { ::close(s); return -1; }  // someone answers
    ::close(s);
    unlink(p.c_str());  // left over from a crash
    if (p.find('/') != std::string::npos) mkdir_p(p.substr(0, p.rfind('/')));  // "isekur.sock": the current dir
    if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) { err = strerror(errno); return 0; }
    mode_t old = umask(077);
    int r = bind(s, (sockaddr*)&a, sizeof a);
    umask(old);
    if (r < 0 || ::listen(s, 4) < 0) {
        err = p + ": " + strerror(errno);
        ::close(s);
        return 0;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    cloexec(s);
    lfd = s;
    return 1;
}

void Session::close() {
    for (int fd : pending) ::close(fd);
    pending.clear();
    if (lfd >= 0) ::close(lfd), unlink(path.c_str());
    lfd = -1;
}

void Session::add_fds(std::vector<pollfd>& fds) {
    first = fds.size();
    if (lfd >= 0) fds.push_back({lfd, POLLIN, 0});
    if (cfd >= 0) fds.push_back({cfd, POLLIN, 0});
    for (int fd : pending) fds.push_back({fd, POLLIN, 0});
    n = fds.size() - first;
}

// one message: the hello + environment, with the terminal fd attached
static int recv_tty(int s, std::string& data) {
    char buf[2048];
    iovec io{buf, sizeof buf};
    union { cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } ctl;
    msghdr m{};
    m.msg_iov = &io, m.msg_iovlen = 1;
    m.msg_control = ctl.b, m.msg_controllen = sizeof ctl.b;
    ssize_t r = recvmsg(s, &m, 0);
    if (r <= 0) return -1;
    data.assign(buf, (size_t)r);
    int fd = -1;
    for (cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(c), sizeof fd);
    return fd;
}

int Session::process(const std::vector<pollfd>& fds, bool busy) {
    int got = -1;
    for (size_t i = first; i < first + n && i < fds.size(); i++) {
        const pollfd& p = fds[i];
        if (!p.revents) continue;
        if (p.fd == lfd) {
            int c = accept(lfd, nullptr, nullptr);
            if (c < 0) continue;
            if (!same_user(c) || pending.size() >= 4) { ::close(c); continue; }
            cloexec(c);
            pending.push_back(c);
        } else if (p.fd == cfd) {  // 'W' = resized; nothing = the attach process is gone
            char b[64];
            ssize_t k = read(cfd, b, sizeof b);
            if (k <= 0) lost = true;
            else if (memchr(b, 'W', (size_t)k)) raise(SIGWINCH);  // termbox re-reads the size
        } else {
            pending.erase(std::remove(pending.begin(), pending.end(), p.fd), pending.end());
            std::string env;
            int t = recv_tty(p.fd, env);
            if (t < 0 || !isatty(t) || env.rfind(HELLO, 0) != 0) {
                if (t >= 0) ::close(t);
                ::close(p.fd);
                continue;
            }
            if (busy || cfd >= 0 || got >= 0) {
                if (write(p.fd, "B", 1) < 0) {}
                ::close(t), ::close(p.fd);
                continue;
            }
            cloexec(t);
            for (const char* k : ENV_KEYS) unsetenv(k);
            for (auto& l : split(env.substr(strlen(HELLO)), '\n')) {
                size_t eq = l.find('=');
                for (const char* k : ENV_KEYS)
                    if (eq != std::string::npos && l.compare(0, eq, k) == 0) setenv(k, l.c_str() + eq + 1, 1);
            }
            setlocale(LC_CTYPE, "");
            cfd = p.fd, tty = got = t, lost = false;
        }
    }
    return got;
}

void Session::release(char why) {
    if (cfd >= 0) {
        if (write(cfd, &why, 1) < 0) {}
        ::close(cfd);
    }
    if (tty >= 0) ::close(tty);
    cfd = tty = -1;
    lost = false;
}

// ---- the --attach client ---------------------------------------------------
static int g_winch[2] = {-1, -1};

int attach_main(const std::string& path, const std::string& how) {
    // the terminal's own device: on macOS and the BSDs /dev/tty means "my
    // controlling terminal" on every call, which is nothing to the receiver
    int tty = -1;
    for (int fd = 0; fd < 3 && tty < 0; fd++)
        if (const char* name = isatty(fd) ? ttyname(fd) : nullptr) tty = open(name, O_RDWR | O_NOCTTY);
    if (tty < 0) tty = open("/dev/tty", O_RDWR | O_NOCTTY);
    if (tty < 0) { fprintf(stderr, "isekur --attach needs a terminal\n"); return 1; }
    sockaddr_un a;
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (!sock_addr(path, a) || s < 0 || connect(s, (sockaddr*)&a, sizeof a) < 0) {
        fprintf(stderr, "No iSekur is running for this config (nothing at %s)\n", path.c_str());
        return 1;
    }
    std::string env = HELLO;
    for (const char* k : ENV_KEYS)
        if (const char* v = getenv(k)) env += std::string(k) + "=" + v + "\n";
    iovec io{&env[0], env.size()};
    union { cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } ctl;
    memset(&ctl, 0, sizeof ctl);
    msghdr m{};
    m.msg_iov = &io, m.msg_iovlen = 1;
    m.msg_control = ctl.b, m.msg_controllen = sizeof ctl.b;
    cmsghdr* c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET, c->cmsg_type = SCM_RIGHTS, c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &tty, sizeof tty);
    // iSekur puts the terminal in raw mode; if it dies without undoing that, we do
    termios saved{};
    bool have = tcgetattr(tty, &saved) == 0;
    auto restore = [&]() {
        if (have) tcsetattr(tty, TCSANOW, &saved);
        static const char reset[] = "\x1b[?1006l\x1b[?1015l\x1b[?1002l\x1b[?1000l\x1b[0m\x1b[?25h\x1b[?1049l";
        if (write(tty, reset, sizeof reset - 1) < 0) {}
    };
    if (sendmsg(s, &m, 0) < 0) { perror("isekur --attach"); return 1; }

    // resizes reach us (the terminal's foreground process); pass them on
    if (pipe(g_winch) == 0) {
        struct sigaction sa{};
        sa.sa_handler = [](int) { if (write(g_winch[1], "W", 1) < 0) {} };
        sigaction(SIGWINCH, &sa, nullptr);
    }
    for (;;) {
        pollfd p[2] = {{s, POLLIN, 0}, {g_winch[0], POLLIN, 0}};
        if (poll(p, g_winch[0] >= 0 ? 2 : 1, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        char ch;
        if (p[1].revents && read(g_winch[0], &ch, 1) == 1 && write(s, "W", 1) < 0) break;
        if (!p[0].revents) continue;
        if (read(s, &ch, 1) != 1) break;
        if (ch == 'B') {
            fprintf(stderr, "iSekur is on another terminal right now. Detach it there first (Alt+D or /detach)\n");
            return 1;
        }
        if (ch == 'D') { printf("Detached; iSekur keeps running. Back with: %s\n", how.c_str()); return 0; }
        if (ch == 'Q') { puts("iSekur quit."); return 0; }
        if (ch == 'F') {
            restore();
            fprintf(stderr, "iSekur couldn't use this terminal; it is still running (its first window says why)\n");
            return 1;
        }
    }
    restore();
    fprintf(stderr, "Lost the connection to iSekur\n");
    return 1;
}
