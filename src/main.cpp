// iSekur: colorful terminal IRC client with a built-in webchat.
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>
#include "core.h"
#include "tui.h"
#include "web.h"

static volatile sig_atomic_t g_term = 0;

static int usage() {
    puts("usage: isekur [-c config] [-d|--daemon] [--hash-password] [-v|--version]\n"
         "  -c FILE          config file (default ~/.config/isekur/isekur.conf)\n"
         "  -d, --daemon     run headless in the background; use the webchat to attach\n"
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

static bool daemonize(const std::string& log) {
    pid_t p = fork();
    if (p < 0) return false;
    if (p > 0) _exit(0);
    setsid();
    if ((p = fork()) < 0) return false;
    if (p > 0) _exit(0);
    int in = open("/dev/null", O_RDONLY), out = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    dup2(in, 0);
    if (out >= 0) dup2(out, 1), dup2(out, 2);
    return true;
}

int main(int argc, char** argv) {
    setlocale(LC_ALL, "");
    signal(SIGPIPE, SIG_IGN);
    bool daemon = false;
    Core c;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-c" && i + 1 < argc) c.cfg_path = argv[++i];
        else if (a == "-d" || a == "--daemon") daemon = true;
        else if (a == "--hash-password") return hash_pw_cmd();
        else if (a == "-v" || a == "--version") { puts("iSekur " ISEKUR_VERSION); return 0; }
        else return usage();
    }
    if (daemon) {
        std::string dir = c.cfg_path.empty() ? "" : c.cfg_path.substr(0, c.cfg_path.rfind('/') + 1);
        if (dir.empty()) {
            const char* h = getenv("HOME");
            dir = std::string(h ? h : ".") + "/.config/isekur/";
        }
        if (!daemonize(dir + "isekur.log")) { perror("fork"); return 1; }
        signal(SIGTERM, [](int) { g_term = 1; });
        signal(SIGINT, [](int) { g_term = 1; });
        signal(SIGHUP, SIG_IGN);
    }

    c.load_config();
    Web web(c);
    std::string err;
    if (!web.start(err)) {
        c.print(c.bufs.front().get(), LK::ERROR, "Webchat disabled: " + err);
        if (daemon) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    }
    c.listener = [&](Ev e, Buffer* b, const Line* l) { web.on_event(e, b, l); };
    if (daemon && !web.running()) { fprintf(stderr, "daemon mode needs [web] enabled\n"); return 1; }

    Tui tui(c);
    if (!daemon && !tui.init(err)) { fprintf(stderr, "terminal: %s\n", err.c_str()); return 1; }

    time_t last_min = 0;
    std::vector<pollfd> fds;
    std::vector<Server*> polled;
    while (!c.quit && !g_term) {
        fds.clear(), polled.clear();
        int tty = -1, rsz = -1;
        if (!daemon) {
            tui.fds(tty, rsz);
            fds.push_back({tty, POLLIN, 0});
            fds.push_back({rsz, POLLIN, 0});
        }
        for (auto& s : c.servers)
            if (s->conn.fd >= 0) fds.push_back({s->conn.fd, s->conn.events(), 0}), polled.push_back(s.get());
        size_t srv0 = daemon ? 0 : 2;
        web.add_fds(fds);

        if (poll(fds.data(), (nfds_t)fds.size(), c.drain()) < 0 && errno != EINTR) break;
        // servers first: their handlers never delete servers, so `polled` stays valid
        for (size_t i = 0; i < polled.size(); i++)
            if (fds[srv0 + i].revents) c.on_readable(polled[i], fds[srv0 + i].revents);
        if (!daemon && (fds[0].revents || fds[1].revents)) tui.on_readable();
        web.process(fds);
        web.flush();
        c.drain();

        time_t now = time(nullptr);
        c.set_attached(!daemon || web.attached());
        c.tick(now);
        if (now / 60 != last_min) last_min = now / 60, c.dirty = true;  // status bar clock
        if (!daemon && c.dirty) tui.draw();
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
    if (!daemon) tui.shutdown();
    return 0;
}
