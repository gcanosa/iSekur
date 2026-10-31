// Detach / attach: a unix socket next to the config. `isekur --attach` passes
// its terminal over it (SCM_RIGHTS) and the running iSekur draws on it.
#pragma once
#include <poll.h>
#include <string>
#include <vector>

struct Session {
    std::string path;
    bool listening() const { return lfd >= 0; }
    // 1 listening, 0 failed (err says why), -1 another iSekur already owns the socket
    int listen(const std::string& sock_path, std::string& err);
    void close();  // stop listening, remove the socket
    void add_fds(std::vector<pollfd>& fds);
    // after poll(): a terminal handed over by `isekur --attach`, with its TERM
    // and locale applied, or -1. `busy`: already drawing on one, so refuse it.
    int process(const std::vector<pollfd>& fds, bool busy);
    void release(char why);  // 'D' detached / 'Q' quit / 'F' unusable terminal: tell the attach process, close its terminal
    int tty = -1;            // the attached terminal
    bool lost = false;       // the attach process went away (closed terminal, dropped SSH)

private:
    int lfd = -1, cfd = -1;    // listener; the attach process while attached
    std::vector<int> pending;  // accepted, terminal not received yet
    size_t first = 0, n = 0;   // our slots in the pollfd vector
};

std::string cfg_side(const std::string& cfg_path, const std::string& ext);  // <dir>/<name>.conf -> <dir>/<name>.ext
std::string sock_path_for(const std::string& cfg_path);  // <dir>/<name>.sock
// --list: the running ones whose sockets are in dirs, each with the command that attaches it
int list_main(const std::vector<std::string>& dirs, const std::string& default_cfg);
// the --attach client; `how` is the command that attaches again, for its messages
int attach_main(const std::string& sock_path, const std::string& how);
