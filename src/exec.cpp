// /exec: run a shell command without blocking the client. Its output streams
// into the cmd tab, or with -o is said in the tab you typed it in once it ends.
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "core.h"

static const size_t MAX_JOB_LINES = 1000;  // then it's killed: `yes` must not fill memory

// one output line for a chat line: no ANSI escapes, tabs expanded, no CR
static std::string clean(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        char ch = s[i];
        if (ch == '\x1b') {  // ESC [ params letter, or ESC + one char
            if (i + 1 < s.size() && s[i + 1] == '[') {
                i += 2;
                while (i < s.size() && !isalpha((unsigned char)s[i])) i++;
            } else i++;
        } else if (ch == '\t') o.append(8 - o.size() % 8, ' ');
        else if (ch != '\r') o += ch;
    }
    return o;
}

// /exec [-o] [-y] command
void Core::exec_cmd(Buffer* b, const std::string& args) {
    if (from_web && !opt_on(nullptr, "exec_web", false)) {
        print(b, LK::ERROR, "/exec is off in the webchat: it runs commands on this machine (/set exec_web on to allow it)");
        return;
    }
    Job j;
    std::string cmd = args;
    for (;;) {
        if (cmd.rfind("-o ", 0) == 0) j.say = true;
        else if (cmd.rfind("-y ", 0) == 0) j.yes = true;
        else break;
        cmd = after_words(cmd, 1);
    }
    if (cmd.empty()) {
        if (jobs.empty()) print(b, LK::INFO, "Usage: /exec [-o] command   (no commands running)");
        for (auto& r : jobs) print(b, LK::INFO, "running " + std::to_string(time(nullptr) - r.started) + "s: " + r.cmd);
        return;
    }
    if (j.say && (b->type == BT::STATUS || !b->srv)) { print(b, LK::ERROR, "/exec -o sends to a channel or query: type it there"); return; }
    int p[2];
    if (pipe(p) < 0) { print(b, LK::ERROR, std::string("pipe: ") + strerror(errno)); return; }
    pid_t pid = fork();
    if (pid < 0) { ::close(p[0]), ::close(p[1]); print(b, LK::ERROR, std::string("fork: ") + strerror(errno)); return; }
    if (pid == 0) {  // child: own process group (the timeout kills it all), no terminal, no sockets
        setpgid(0, 0);
        for (int sig : {SIGPIPE, SIGHUP, SIGINT, SIGTERM}) signal(sig, SIG_DFL);
        int nul = ::open("/dev/null", O_RDONLY);
        dup2(nul, 0), dup2(p[1], 1), dup2(p[1], 2);
        for (int fd = 3; fd < 1024; fd++) ::close(fd);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    ::close(p[1]);
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    j.pid = pid, j.fd = p[0], j.cmd = cmd, j.started = time(nullptr), j.to = b;
    jobs.push_back(j);
    if (!j.say) print(bufs.front().get(), LK::INFO, "\x02$ " + cmd + "\x02");
}

void Core::job_line(Job& j, const std::string& raw) {
    if (j.nlines >= MAX_JOB_LINES) return;  // killed, the pipe is draining
    std::string l = clean(raw);
    if (j.say) { if (!l.empty()) j.out.push_back(l); }
    else print(bufs.front().get(), LK::INFO, "  " + l);
    if (++j.nlines == MAX_JOB_LINES) {
        kill(-j.pid, SIGKILL);
        j.note = "stopped after " + std::to_string(MAX_JOB_LINES) + " lines";
    }
}

void Core::job_read(Job& j) {
    char buf[4096];
    ssize_t n;
    while ((n = read(j.fd, buf, sizeof buf)) > 0) {
        j.partial.append(buf, (size_t)n);
        size_t nl;
        while ((nl = j.partial.find('\n')) != std::string::npos) {
            job_line(j, j.partial.substr(0, nl));
            j.partial.erase(0, nl + 1);
        }
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {  // EOF: output done
        if (!j.partial.empty()) job_line(j, j.partial), j.partial.clear();
        ::close(j.fd), j.fd = -1;
    }
}

// read the pipes poll() marked, reap what ended, kill what ran too long
void Core::jobs_poll(const std::vector<pollfd>& fds, size_t from) {
    for (size_t i = 0, k = from; i < jobs.size(); i++)
        if (jobs[i].fd >= 0 && k < fds.size()) {
            if (fds[k].revents) job_read(jobs[i]);
            k++;
        }
    int limit = atoi(opt(nullptr, "exec_timeout", "30").c_str());
    for (size_t i = 0; i < jobs.size();) {
        Job& j = jobs[i];
        if (limit > 0 && j.note.empty() && time(nullptr) - j.started >= limit) {
            kill(-j.pid, SIGKILL);
            j.note = "killed after " + std::to_string(limit) + "s (exec_timeout)";
        }
        int st = 0;
        if (j.fd >= 0 || waitpid(j.pid, &st, WNOHANG) == 0) { i++; continue; }  // still going
        std::string end = !j.note.empty() ? j.note
                          : WIFEXITED(st) && WEXITSTATUS(st) ? "exit " + std::to_string(WEXITSTATUS(st))
                          : WIFSIGNALED(st) ? "killed by signal " + std::to_string(WTERMSIG(st))
                                            : "";
        if (!j.say) {
            if (!end.empty()) print(bufs.front().get(), LK::INFO, "  [" + end + "]");
        } else if (index_of(j.to) < 0) {
            // the tab is gone: nothing to say it in
        } else {
            size_t cap = (size_t)std::max(1, atoi(opt(j.to->srv, "paste_confirm", "5").c_str()));
            if (j.out.size() > cap && !j.yes)
                print(j.to, LK::ERROR, j.cmd + ": " + std::to_string(j.out.size()) + " lines, more than paste_confirm (" +
                                           std::to_string(cap) + "), not sent. /exec -o -y sends them anyway");
            else
                for (auto& l : j.out) say(j.to, l);
            if (!end.empty()) print(j.to, LK::INFO, j.cmd + ": " + end);
        }
        jobs.erase(jobs.begin() + (long)i);
    }
}
