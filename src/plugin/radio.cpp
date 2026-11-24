// A plugin's radio (isekur.radio, API 12): ffmpeg decodes one stream to MP3; iSekur fans it out to the webchat's <audio>
// (GET /radio/stream, web.cpp) and, when asked, to a player on this machine. A YouTube link (a 24/7 live) is resolved by
// yt-dlp first to its audio-only HLS URL. That URL works only from this machine's address, which is why the audio goes
// through iSekur instead of the browser fetching it. No shell anywhere: every program is run with a fixed argv.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include "core.h"
#include "net.h"

static const size_t PLAYER_CAP = 512 << 10;  // bytes the player may be behind: more is dropped (live audio skips ahead)
static const int RESOLVE_S = 60, SILENT_S = 45, STALL_S = 30, RETRIES = 3;

static std::string which_prog(const std::string& n) {
    const char* e = getenv("PATH");
    for (auto& d : split(e ? e : "/usr/bin:/bin", ':')) {
        struct stat st;
        std::string f = d + "/" + n;
        if (!d.empty() && stat(f.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(f.c_str(), X_OK) == 0) return f;
    }
    return "";
}

// the host of an http(s) URL we would fetch, lowercase; "" for anything else: no spaces or control characters, no user:pass@,
// and nothing on this machine or a private network (names that say so, and addresses written out)
static std::string url_host(const std::string& u) {
    size_t s = u.rfind("https://", 0) == 0 ? 8 : u.rfind("http://", 0) == 0 ? 7 : 0;
    if (!s || u.size() > 2000) return "";
    for (unsigned char ch : u)
        if (ch <= 32 || ch == 127) return "";
    size_t e = u.find_first_of("/?#", s);
    std::string auth = u.substr(s, e == std::string::npos ? std::string::npos : e - s);  // host[:port]; user:pass@ would hide the real host from the checks below
    if (auth.empty() || auth.find('@') != std::string::npos || auth[0] == '[' || auth[0] == '-') return "";  // ponytail: no [ipv6] literals
    std::string h = auth.substr(0, auth.find(':'));
    if (h.size() < auth.size() && (h.size() + 1 == auth.size() || auth.find_first_not_of("0123456789", h.size() + 1) != std::string::npos)) return "";  // a port is digits
    if (h.empty()) return "";
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char ch) { return (char)tolower(ch); });
    auto ends = [&](const char* x) { size_t n = strlen(x); return h.size() >= n && !h.compare(h.size() - n, n, x); };
    if (h == "localhost" || ends(".localhost") || ends(".local") || ends(".internal") || ends(".lan")) return "";
    in_addr a4;
    if (inet_pton(AF_INET, h.c_str(), &a4)) {
        sockaddr_in sa{};
        sa.sin_family = AF_INET, sa.sin_addr = a4;
        if (!public_addr((sockaddr*)&sa)) return "";
    }
    return h;
}
static bool is_yt(const std::string& h) { return h == "youtu.be" || h == "youtube.com" || (h.size() > 12 && h.compare(h.size() - 12, 12, ".youtube.com") == 0); }

// argv with stdin, stdout and stderr on in, out and err (-1: /dev/null); the pid, -1 if it couldn't fork
static pid_t spawn(const std::vector<std::string>& av, int in, int out, int err) {
    std::vector<char*> argv;
    for (auto& a : av) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid != 0) return pid;
    int nul = open("/dev/null", O_RDWR);
    dup2(in >= 0 ? in : nul, 0), dup2(out >= 0 ? out : nul, 1), dup2(err >= 0 ? err : nul, 2);
    for (int fd = 3; fd < 1024; fd++) close(fd);
    signal(SIGPIPE, SIG_DFL);
    execvp(argv[0], argv.data());
    _exit(127);
}
static bool mkpipe(int p[2]) {
    if (pipe(p)) return false;
    for (int i = 0; i < 2; i++) fcntl(p[i], F_SETFD, FD_CLOEXEC);
    return true;
}
static void nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

// ffmpeg on src, its MP3 on a pipe; the player (if asked and not already there) on another. "" or why not
static std::string start_ff(Core& c, const std::string& src) {
    auto& r = c.radio;
    int o[2], e[2], in[2];
    if (!mkpipe(o)) return strerror(errno);
    if (!mkpipe(e)) { close(o[0]), close(o[1]); return strerror(errno); }
    if (!mkpipe(in)) { close(o[0]), close(o[1]), close(e[0]), close(e[1]); return strerror(errno); }
    char gain[24];
    snprintf(gain, sizeof gain, "%.3f", r.volume / 100.0);
    // the protocols a stream (and a playlist inside it) may use: never file: or anything local
    // stdin is ours: "cvolume@v -1 volume 0.5" changes the volume while it plays (ffmpeg's interactive commands). The limiter keeps a boost above 100 from clipping
    r.ff = spawn({"ffmpeg", "-hide_banner", "-loglevel", "error", "-protocol_whitelist", "http,https,tcp,tls,crypto",
                  "-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "5", "-i", src,
                  "-vn", "-map", "a:0", "-af", std::string("volume@v=") + gain + ":eval=frame,alimiter=limit=0.95",
                  "-c:a", "libmp3lame", "-b:a", "128k", "-f", "mp3", "pipe:1"}, in[0], o[1], e[1]);
    close(o[1]), close(e[1]), close(in[0]);
    if (r.ff < 0) { close(o[0]), close(e[0]), close(in[1]); return "can't start ffmpeg"; }
    nonblock(o[0]), nonblock(e[0]), nonblock(in[1]);
    r.ff_fd = o[0], r.ff_err = e[0], r.ff_in = in[1], r.ff_msg.clear(), r.try_at = time(nullptr);
    if (r.host && r.pl < 0 && !r.player.empty()) {
        int i[2];
        if (mkpipe(i)) {
            static const std::map<std::string, std::vector<std::string>> av = {
                {"mpv", {"mpv", "--no-video", "--no-terminal", "--really-quiet", "-"}},
                {"ffplay", {"ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", "-i", "pipe:0"}},
                {"vlc", {"vlc", "-I", "dummy", "--no-video", "--play-and-exit", "-q", "fd://0"}}};
            r.pl = spawn(av.at(r.player), i[0], -1, -1);
            close(i[0]);
            if (r.pl < 0) close(i[1]);
            else nonblock(i[1]), r.pl_fd = i[1];
        }
    }
    r.st = Core::Radio::RESOLVING;
    return "";
}

// the first try or a restart: yt-dlp for a YouTube link, else ffmpeg straight away. "" or why not
static std::string begin(Core& c) {
    auto& r = c.radio;
    r.try_at = time(nullptr), r.res_out.clear();
    if (!is_yt(url_host(r.url))) return start_ff(c, r.url);
    int p[2];
    if (!mkpipe(p)) return strerror(errno);
    // 234/233: the audio-only HLS formats of a live; bestaudio for a video. "--" so a URL is never an option
    r.res = spawn({"yt-dlp", "--no-warnings", "--no-playlist", "--no-colors", "-f", "234/233/bestaudio", "-g", "--", r.url}, -1, p[1], p[1]);
    close(p[1]);
    if (r.res < 0) { close(p[0]); return "can't start yt-dlp"; }
    nonblock(p[0]);
    r.res_fd = p[0], r.st = Core::Radio::RESOLVING;
    return "";
}

std::string Core::radio_start(const std::string& by, const std::string& url, const std::string& name, bool host, bool direct) {
    std::string h = url_host(url);
    if (h.empty()) return "not a stream address (http or https, a host on the internet)";
    if (direct) {  // the browsers connect themselves: nothing runs here. A page on https only plays https, and YouTube's audio can't be had from a browser
        if (url.rfind("https://", 0) || is_yt(h)) return "direct needs a plain https stream: YouTube and http ones go through this machine";
        radio_stop();
        radio.by = by, radio.name = name, radio.url = url, radio.host = false, radio.player.clear(), radio.direct = true;
        radio.want = false, radio.fails = 0, radio.retry = 0, radio.err.clear();
        radio.st = Radio::PLAYING, radio.play_at = radio.data_at = time(nullptr), dirty = true;
        notify(Ev::RADIO, nullptr);
        return "";
    }
    if (which_prog("ffmpeg").empty()) return "ffmpeg isn't installed: it decodes the stream";
    if (is_yt(h) && which_prog("yt-dlp").empty()) return "yt-dlp isn't installed: it finds the audio of a YouTube channel";
    std::string player;
    if (host)
        for (const char* n : {"mpv", "ffplay", "vlc"})
            if (player.empty() && !which_prog(n).empty()) player = n;
    if (host && player.empty()) return "no player for this machine's speakers: install mpv (or ffplay or vlc)";
    radio_stop();
    radio.by = by, radio.name = name, radio.url = url, radio.host = host, radio.player = player, radio.direct = false;
    radio.want = true, radio.fails = 0, radio.retry = 0, radio.err.clear();
    if (std::string e = begin(*this); !e.empty()) { radio_stop(); return e; }
    dirty = true;
    notify(Ev::RADIO, nullptr);
    return "";
}

// what runs is told to stop and its pipes are closed; radio_poll reaps it
void Core::radio_drop() {
    time_t now = time(nullptr);
    for (pid_t* p : {&radio.res, &radio.ff, &radio.pl})
        if (*p > 0) kill(*p, SIGTERM), radio.dying.push_back({*p, now}), *p = -1;
    for (int* f : {&radio.res_fd, &radio.ff_fd, &radio.ff_err, &radio.ff_in, &radio.pl_fd})
        if (*f >= 0) ::close(*f), *f = -1;
    radio.res_out.clear(), radio.pl_buf.clear(), radio.ff_msg.clear();
}

void Core::radio_stop() {
    bool was = radio.st != Radio::OFF;
    radio.want = false;
    radio_drop();
    radio.st = Radio::OFF, radio.by.clear(), radio.fails = 0, radio.retry = 0, radio.direct = false;
    if (radio_sink) radio_sink(nullptr, 0);
    if (was) dirty = true, notify(Ev::RADIO, nullptr);
}

void Core::radio_mute(bool on) {
    if (radio.muted == on) return;
    radio.muted = on, radio.pl_buf.clear();
    if (radio_on()) dirty = true, notify(Ev::RADIO, nullptr);
}

// the volume in percent (0-150): ffmpeg's filter changes live; a direct stream's browsers set their own (the page's volume)
void Core::radio_volume(int percent) {
    percent = std::max(0, std::min(150, percent));
    if (radio.volume == percent) return;
    radio.volume = percent;
    if (radio.ff_in >= 0) {
        char cmd[64];
        int n = snprintf(cmd, sizeof cmd, "cvolume@v -1 volume %.3f\n", percent / 100.0);
        if (write(radio.ff_in, cmd, (size_t)n) < 0) {}  // ponytail: a full pipe drops it (ffmpeg stopped reading): the next change goes through
    }
    if (radio_on()) dirty = true, notify(Ev::RADIO, nullptr);
}

void Core::radio_fail(const std::string& why) {
    std::string by = radio.by;
    radio.muted = false;
    plug_print(nullptr, LK::ERROR, "radio: " + why, by.empty() ? "plugin" : by);
    radio_stop();
    radio.err = why;
}

// a direct stream runs in the browsers, so only they know it didn't play (an error, a codec or a format the browser lacks such as HLS, a
// network that blocks it, no sound): the first to say so stops the radio like a relayed one that died. A report about another stream (an older
// station's, a late one) is ignored
bool Core::radio_client_fail(const std::string& url, const std::string& why) {
    if (!radio.direct || radio.st == Radio::OFF || url != radio.url) return false;
    std::string w = why.substr(0, 160);
    for (char& ch : w) if ((unsigned char)ch < 32 || ch == 127) ch = ' ';
    radio_fail(w.empty() ? "the browser couldn't play it" : w);
    return true;
}

void Core::radio_fds(std::vector<pollfd>& fds) {
    radio_pf.clear();
    for (int f : {radio.res_fd, radio.ff_fd, radio.ff_err})
        if (f >= 0) fds.push_back({f, POLLIN, 0}), radio_pf.push_back(f);
}

static void feed_player(Core::Radio& r, const char* b, size_t n) {
    r.pl_buf.append(b, n);
    while (!r.pl_buf.empty()) {
        ssize_t w = write(r.pl_fd, r.pl_buf.data(), r.pl_buf.size());
        if (w > 0) { r.pl_buf.erase(0, (size_t)w); continue; }
        if (w < 0 && errno != EAGAIN && errno != EINTR) ::close(r.pl_fd), r.pl_fd = -1, r.pl_buf.clear();  // the player went away
        break;
    }
    if (r.pl_buf.size() > PLAYER_CAP) r.pl_buf.clear();
}

void Core::radio_poll(const std::vector<pollfd>& fds, size_t from) {
    time_t now = time(nullptr);
    char buf[65536];
    for (size_t i = 0; i < radio_pf.size() && from + i < fds.size(); i++) {
        if (!fds[from + i].revents) continue;
        int f = radio_pf[i];
        ssize_t n;
        if (f == radio.res_fd) {  // yt-dlp: its answer (the URL) and, on failure, an ERROR line
            while ((n = read(f, buf, sizeof buf)) > 0)
                if (radio.res_out.size() < 65536) radio.res_out.append(buf, (size_t)n);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                ::close(radio.res_fd), radio.res_fd = -1;
                if (radio.res > 0) {
                    if (waitpid(radio.res, nullptr, WNOHANG) == 0) radio.dying.push_back({radio.res, now});
                    radio.res = -1;
                }
                std::string url, err;
                for (auto& l : split(radio.res_out, '\n')) {
                    if (l.rfind("http", 0) == 0) url = l;
                    else if (err.empty() && l.rfind("ERROR", 0) == 0) err = l.substr(l.find(' ') == std::string::npos ? 0 : l.find(' ') + 1);
                }
                while (!url.empty() && (url.back() == '\r' || url.back() == ' ')) url.pop_back();
                if (url.empty() || url_host(url).empty()) { radio_fail(err.empty() ? "yt-dlp found no audio there" : "yt-dlp: " + err); return; }
                if (std::string e = start_ff(*this, url); !e.empty()) { radio_fail(e); return; }
            }
        } else if (f == radio.ff_err) {  // ffmpeg's complaints: the first lines, to say why it stopped
            while ((n = read(f, buf, sizeof buf)) > 0)
                if (radio.ff_msg.size() < 2000) radio.ff_msg.append(buf, (size_t)n);
            if (n == 0) ::close(radio.ff_err), radio.ff_err = -1;
        } else if (f == radio.ff_fd) {  // ffmpeg's MP3
            bool first = false;
            while ((n = read(f, buf, sizeof buf)) > 0) {
                if (radio.st == Radio::RESOLVING) radio.st = Radio::PLAYING, radio.play_at = now, first = true;
                radio.data_at = now;
                if (radio_sink) radio_sink(buf, (size_t)n);
                if (radio.pl_fd >= 0 && !radio.muted) feed_player(radio, buf, (size_t)n);
            }
            if (first) dirty = true, notify(Ev::RADIO, nullptr);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {  // the stream ended: again, a few times, then say why
                std::string said = radio.ff_msg.substr(0, radio.ff_msg.find('\n'));
                ::close(radio.ff_fd), radio.ff_fd = -1;
                if (radio.ff_in >= 0) ::close(radio.ff_in), radio.ff_in = -1;
                if (radio.ff_err >= 0) {
                    while ((n = read(radio.ff_err, buf, sizeof buf)) > 0)
                        if (said.empty()) said = std::string(buf, (size_t)n).substr(0, std::string(buf, (size_t)n).find('\n'));
                    ::close(radio.ff_err), radio.ff_err = -1;
                }
                if (radio.ff > 0) {
                    if (waitpid(radio.ff, nullptr, WNOHANG) == 0) radio.dying.push_back({radio.ff, now});
                    radio.ff = -1;
                }
                if (radio_sink) radio_sink(nullptr, 0);
                if (radio.st == Radio::PLAYING && now - radio.play_at >= 30) radio.fails = 0;  // it ran a while: not a loop of failures
                if (!radio.want || ++radio.fails > RETRIES) { radio_fail("the stream stopped" + (said.empty() ? "" : ": " + said)); return; }
                radio.st = Radio::RESOLVING, radio.retry = now + 2 + radio.fails, dirty = true;
                notify(Ev::RADIO, nullptr);
            }
        }
    }
    if (radio.st == Radio::OFF && radio.dying.empty()) return;
    // a stream that went quiet is stopped (it then starts again), one that never came is given up on
    if (radio.st == Radio::PLAYING && radio.ff > 0 && now - radio.data_at > STALL_S) kill(radio.ff, SIGTERM), radio.data_at = now;
    if (radio.st == Radio::RESOLVING && radio.retry && now >= radio.retry) {
        radio.retry = 0;
        if (std::string e = begin(*this); !e.empty()) { radio_fail(e); return; }
    }
    if (radio.st == Radio::RESOLVING && !radio.retry && now - radio.try_at > (radio.res > 0 ? RESOLVE_S : SILENT_S)) {
        radio_fail(radio.res > 0 ? "yt-dlp took too long" : "no sound came from the stream");
        return;
    }
    if (radio.pl > 0 && waitpid(radio.pl, nullptr, WNOHANG) != 0) {  // the player was closed: the web still plays
        radio.pl = -1;
        if (radio.pl_fd >= 0) ::close(radio.pl_fd), radio.pl_fd = -1;
        radio.pl_buf.clear();
    }
    for (size_t i = 0; i < radio.dying.size();) {
        auto& [pid, at] = radio.dying[i];
        if (waitpid(pid, nullptr, WNOHANG) != 0) radio.dying.erase(radio.dying.begin() + (long)i);
        else {
            if (now - at >= 3) kill(pid, SIGKILL);
            i++;
        }
    }
}
