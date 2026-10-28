// iSekur: colorful terminal IRC client with a built-in webchat.
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <algorithm>
#include "core.h"
#include "session.h"
#include "tui.h"
#include "web.h"

static volatile sig_atomic_t g_term = 0;

static int usage() {
    puts("usage: isekur [-c config] [-d|--daemon] [-a|--attach] [--web|--no-web] [--hash-password] [-v|--version]\n"
         "  -c FILE          config file (default ~/.config/isekur/isekur.conf)\n"
         "  -d, --daemon     run headless in the background; attach with --attach or the webchat\n"
         "  -a, --attach     bring a detached iSekur (Alt+D, /detach, --daemon) back to this terminal\n"
         "  --web, --no-web  start the webchat or not, whatever [web] autostart says\n"
         "  --hash-password  read a password and print a [web] password_hash value");
    return 1;
}

static int hash_pw_cmd() {
    termios old{}, quiet{};
    bool tty = tcgetattr(0, &old) == 0;
    if (tty) quiet = old, quiet.c_lflag &= ~(tcflag_t)ECHO, tcsetattr(0, TCSANOW, &quiet);
    fprintf(stderr, "Password: ");
    char buf[512] = "";
    bool got = fgets(buf, sizeof buf, stdin) != nullptr;
    if (tty) tcsetattr(0, TCSANOW, &old), fputc('\n', stderr);
    buf[strcspn(buf, "\r\n")] = 0;
    if (!got || !*buf) return 1;
    printf("password_hash = %s\n", hash_password(buf).c_str());
    return 0;
}

// Background iSekur in a new session, stdio on the log: 1 in it, 0 back in
// the caller when keep_parent (which then attaches to it), -1 if fork failed.
// Done before anything starts a thread: fork() keeps only the calling one,
// and on macOS a child of a threaded process can't use the resolver.
static int daemonize(const std::string& log, bool keep_parent) {
    fflush(stdout), fflush(stderr);
    pid_t p = fork();
    if (p < 0) return -1;
    if (p > 0) {
        if (!keep_parent) _exit(0);
        waitpid(p, nullptr, 0);  // the middle process exits right away
        return 0;
    }
    setsid();
    if ((p = fork()) < 0) _exit(1);
    if (p > 0) _exit(0);
    int in = open("/dev/null", O_RDONLY), out = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    dup2(in, 0);
    if (out >= 0) dup2(out, 1), dup2(out, 2);
    return 1;
}

// macOS poll() can't wait on terminals (POLLNVAL right away: a busy loop), so
// there it's select(), fine for the few dozen descriptors a client has
static int wait_fds(std::vector<pollfd>& fds, int timeout_ms) {
#ifdef __APPLE__
    fd_set r, w;
    FD_ZERO(&r), FD_ZERO(&w);
    int maxfd = -1;
    for (auto& p : fds) {
        if (p.fd >= FD_SETSIZE) return poll(fds.data(), (nfds_t)fds.size(), timeout_ms);
        if (p.events & POLLIN) FD_SET(p.fd, &r);
        if (p.events & POLLOUT) FD_SET(p.fd, &w);
        maxfd = std::max(maxfd, p.fd);
    }
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int n = select(maxfd + 1, &r, &w, nullptr, timeout_ms < 0 ? nullptr : &tv);
    for (auto& p : fds)
        p.revents = n <= 0 ? 0 : (short)((FD_ISSET(p.fd, &r) ? POLLIN : 0) | (FD_ISSET(p.fd, &w) ? POLLOUT : 0));
    return n;
#else
    return poll(fds.data(), (nfds_t)fds.size(), timeout_ms);
#endif
}

int main(int argc, char** argv) {
    setlocale(LC_ALL, "");
    signal(SIGPIPE, SIG_IGN);
    bool daemon = false, attach = false;
    int web_flag = -1;  // --web / --no-web; -1 = [web] autostart decides
    Core c;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-c" && i + 1 < argc) c.cfg_path = argv[++i];
        else if (a == "-d" || a == "--daemon") daemon = true;
        else if (a == "-a" || a == "--attach") attach = true;
        else if (a == "--web") web_flag = 1;
        else if (a == "--no-web") web_flag = 0;
        else if (a == "--hash-password") return hash_pw_cmd();
        else if (a == "-v" || a == "--version") { puts(version_string().c_str()); return 0; }
        else return usage();
    }
    bool own_cfg = !c.cfg_path.empty();
    if (c.cfg_path.empty()) c.cfg_path = default_config_path();
    std::string how = std::string("isekur --attach") + (own_cfg ? " -c " + c.cfg_path : "");
    std::string sock = sock_path_for(c.cfg_path);
    if (attach) return attach_main(sock, how);

    // One iSekur per config (a second one would join the same channels
    // twice): starting it again attaches to the one that runs.
    Session ses;
    std::string err;
    int ls = ses.listen(sock, err);
    if (ls < 0 && daemon) {
        fprintf(stderr, "iSekur is already running with %s. Attach to it: %s\n", c.cfg_path.c_str(), how.c_str());
        return 1;
    }
    if (ls < 0) return attach_main(sock, how);
    // Like tmux: the IRC side runs in the background from the start and this
    // process lends it the terminal, so detaching (Alt+D, a closed window, a
    // dropped SSH) only ends this side. Without the socket (`local`), the
    // terminal is driven here and there is no detaching.
    std::string log = c.cfg_path.substr(0, c.cfg_path.rfind('/') + 1) + "isekur.log";
    bool local = !daemon && ls == 0;
    if (!local) {
        int d = daemonize(log, !daemon);
        if (d < 0) { perror("fork"); return 1; }
        if (d == 0) return attach_main(sock, how);
        signal(SIGHUP, SIG_IGN);
    }
    signal(SIGTERM, [](int) { g_term = 1; });
    signal(SIGINT, [](int) { g_term = 1; });

    c.daemon = !local && !ses.listening();  // the webchat is the only way back in
    c.load_config();
    Buffer* home = c.bufs.front().get();
    if (ls == 0) c.print(home, LK::ERROR, "Detaching is not available: " + err);
    Web web(c);
    // --daemon always starts the webchat; it can also be attached from a terminal
    if (daemon || (web_flag < 0 ? Web::autostart(c.cfg) : web_flag == 1)) {
        if (!web.start(err)) {
            c.print(home, LK::ERROR, "Webchat not started: " + err);
            if (c.daemon) { fprintf(stderr, "daemon mode needs the webchat: %s\n", err.c_str()); return 1; }
        } else c.print(home, LK::INFO, "Webchat on " + web.url);
    }
    c.web_url = web.url;
    c.listener = [&](Ev e, Buffer* b, const Line* l) { web.on_event(e, b, l); };

    Tui tui(c);
    if (local && !tui.init(err)) { fprintf(stderr, "terminal: %s\n", err.c_str()); return 1; }
    c.tty = local;
    bool hinted = false;

    time_t last_min = 0;
    std::vector<pollfd> fds;
    std::vector<Server*> polled;
    while (!c.quit && !g_term) {
        fds.clear(), polled.clear();
        if (c.tty) {
            int tty = -1, rsz = -1;
            tui.fds(tty, rsz);
            fds.push_back({tty, POLLIN, 0});
            fds.push_back({rsz, POLLIN, 0});
        }
        size_t srv0 = fds.size();
        for (auto& s : c.servers)
            if (s->conn.fd >= 0) fds.push_back({s->conn.fd, s->conn.events(), 0}), polled.push_back(s.get());
        web.add_fds(fds);
        ses.add_fds(fds);

        if (wait_fds(fds, c.drain()) < 0 && errno != EINTR) break;
        // servers first: their handlers never delete servers, so `polled` stays valid
        for (size_t i = 0; i < polled.size(); i++)
            if (fds[srv0 + i].revents) c.on_readable(polled[i], fds[srv0 + i].revents);
        // the attach process quit (closed window, dropped SSH) or the terminal hung up
        bool hung = !local && (ses.lost || (c.tty && (fds[0].revents & (POLLHUP | POLLERR))));
        if (c.tty && !hung && (fds[0].revents || fds[1].revents)) tui.on_readable();
        web.process(fds);
        int t = ses.process(fds, c.tty);
        if (c.detach_req && local) c.print(c.active, LK::ERROR, "Detaching is not available: " + err);
        else if (c.tty && (hung || c.detach_req)) {
            tui.shutdown();
            ses.release('D');
            c.tty = false;
            c.print(home, LK::INFO, std::string(hung ? "Terminal gone" : "Detached") + "; " + how + " brings it back");
        }
        c.detach_req = false;
        if (t >= 0) {
            if (!tui.init(err, t)) {
                c.print(home, LK::ERROR, "Attach failed: " + err), ses.release('F');
                fprintf(stderr, "attach failed: %s\n", err.c_str());
            }
            else {
                c.tty = c.dirty = true;
                if (!hinted) c.print(home, LK::INFO, "Alt+D (Option+D on a Mac) or /detach leaves iSekur running in the background"), hinted = true;
            }
        }
        web.flush();
        if (c.web_req != Core::WebReq::NONE) {
            Buffer* to = c.index_of(c.web_req_buf) >= 0 ? c.web_req_buf : c.bufs.front().get();
            if (c.web_req == Core::WebReq::STOP && web.running()) {
                web.stop();
                c.print(to, LK::INFO, "Webchat stopped");
            } else if (c.web_req == Core::WebReq::STOP) {
                c.print(to, LK::INFO, "Webchat is not running");
            } else if (web.running()) {
                c.print(to, LK::INFO, "Webchat already on " + web.url);
            } else if (!web.start(err)) {
                c.print(to, LK::ERROR, "Webchat not started: " + err);
            } else {
                c.print(to, LK::INFO, "Webchat on " + web.url);
            }
            c.web_req = Core::WebReq::NONE, c.web_url = web.url;
        }
        c.drain();

        time_t now = time(nullptr);
        c.set_attached(c.tty || web.attached());
        c.tick(now);
        if (now / 60 != last_min) last_min = now / 60, c.dirty = true;  // status bar clock
        if (c.tty && c.dirty) tui.draw();
    }

    // flush QUITs for up to a second
    for (auto& s : c.servers)
        if (s->state != SS::DISCONNECTED && !s->user_quit) s->send_now("QUIT :" + c.quit_msg);
    long long until = now_ms() + 1000;
    while (now_ms() < until) {
        fds.clear(), polled.clear();
        for (auto& s : c.servers)
            if (s->conn.st == Conn::OPEN && !s->conn.out.empty())
                fds.push_back({s->conn.fd, POLLOUT, 0}), polled.push_back(s.get());
        if (fds.empty()) break;
        poll(fds.data(), (nfds_t)fds.size(), 100);
        for (size_t i = 0; i < polled.size(); i++)
            if (fds[i].revents) polled[i]->conn.pump(fds[i].revents, polled[i]->inbuf);
    }
    if (c.tty) tui.shutdown();
    ses.release('Q');
    ses.close();
    return 0;
}
