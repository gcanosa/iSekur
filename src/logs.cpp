// /logs: the log browser's side of the core. What's on disk (<log_dir>/<network>/<name>[.YYYY-MM-DD].log), one file
// read back into Lines, and the filter/search both windows (terminal and webchat) use.
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <set>
#include <sys/stat.h>
#include "core.h"

static bool is_day(const std::string& d) {  // YYYY-MM-DD
    if (d.size() != 10 || d[4] != '-' || d[7] != '-') return false;
    for (int i : {0, 1, 2, 3, 5, 6, 8, 9})
        if (d[(size_t)i] < '0' || d[(size_t)i] > '9') return false;
    return true;
}

// the files of one network folder: "<name>.log" (the current one, its day from when it was last written) and the
// rotated "<name>.YYYY-MM-DD.log"
static void scan(const std::string& dir, const std::string& net, std::vector<LogFile>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string f = e->d_name;
        if (f[0] == '.' || f.size() <= 4 || f.compare(f.size() - 4, 4, ".log") != 0) continue;
        LogFile lf;
        lf.net = net, lf.path = dir + "/" + f;
        struct stat st;
        if (stat(lf.path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        lf.size = (long long)st.st_size;
        std::string stem = f.substr(0, f.size() - 4);
        if (stem.size() > 11 && stem[stem.size() - 11] == '.' && is_day(stem.substr(stem.size() - 10)))
            lf.name = stem.substr(0, stem.size() - 11), lf.day = stem.substr(stem.size() - 10);
        else lf.name = stem, lf.day = fmt_time(st.st_mtime, "%Y-%m-%d"), lf.current = true;
        out.push_back(lf);
    }
    closedir(d);
}

std::vector<LogFile> Core::log_files() const {
    std::vector<LogFile> out;
    std::set<std::string> seen;  // folders
    // every configured network's folder (log_dir can be its own), then whatever else is under the main log_dir:
    // networks since removed from the config
    for (auto& s : servers) {
        std::string dir = log_folder(s.get());
        if (seen.insert(dir).second) scan(dir, s->cfg.name, out);
    }
    std::string base = opt(nullptr, "log_dir", cfg_path.substr(0, cfg_path.rfind('/') + 1) + "logs");
    if (base.rfind("~/", 0) == 0)
        if (const char* h = getenv("HOME")) base = h + base.substr(1);
    if (DIR* d = opendir(base.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name, dir = base + "/" + n;
            struct stat st;
            if (n[0] == '.' || stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || !seen.insert(dir).second) continue;
            scan(dir, n, out);
        }
        closedir(d);
    }
    // network, then name (channels before queries: '#' sorts first), then the newest day first
    std::sort(out.begin(), out.end(), [](const LogFile& a, const LogFile& b) {
        std::string an = irc_lower(a.net), bn = irc_lower(b.net);
        if (an != bn) return an < bn;
        if (a.name != b.name) return a.name < b.name;
        if (a.day != b.day) return a.day > b.day;
        return a.current < b.current;
    });
    return out;
}

// "-!- ..." rows come back as INFO: their text tells joins, parts, quits, nick changes, modes and topics apart again
static void classify(Line& l) {
    if (l.kind != LK::INFO) return;
    const std::string& t = l.text;
    auto has = [&](const char* s) { return t.find(s) != std::string::npos; };
    if (t.rfind("mode/", 0) == 0) {
        l.kind = LK::MODE;
        if (size_t by = t.rfind(" by "); by != std::string::npos) l.nick = t.substr(by + 4);
        return;
    }
    std::string first = t.substr(0, t.find(' '));
    if (has(" has joined ") || t.rfind("You have joined ", 0) == 0) l.kind = LK::JOIN;
    else if (has(" has left ") || has(" was kicked by ")) l.kind = LK::PART;
    else if (has(" has quit")) l.kind = LK::QUIT;
    else if (has(" is now known as ") || t.rfind("You are now known as ", 0) == 0) l.kind = LK::NICK;
    else if (has(" changed the topic to: ") || has(" removed the topic") || t.rfind("Topic: ", 0) == 0) l.kind = LK::TOPIC;
    else return;
    if (first != "You" && first != "Topic:") l.nick = first;
}

// ponytail: a whole file in memory, its last 32 MB at most (log_rotate = off grows one forever); stream it if that bites
std::vector<Line> log_read(const std::string& path, bool* cut) {
    std::vector<Line> out;
    if (cut) *cut = false;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return out;
    const long CAP = 32L << 20;
    fseek(f, 0, SEEK_END);
    long size = ftell(f), from = std::max(0L, size - CAP);
    fseek(f, from, SEEK_SET);
    std::string data((size_t)(size - from), '\0');
    data.resize(fread(&data[0], 1, data.size(), f));
    fclose(f);
    size_t at = 0;
    if (from > 0) {  // a partial first row
        at = data.find('\n');
        at = at == std::string::npos ? data.size() : at + 1;
        if (cut) *cut = true;
    }
    while (at < data.size()) {
        size_t nl = data.find('\n', at);
        if (nl == std::string::npos) nl = data.size();
        Line l;
        if (parse_log_line(data.substr(at, nl - at), l)) classify(l), out.push_back(l);
        at = nl + 1;
    }
    return out;
}

// "hello world": both words, anywhere in the nick or the text, any case. nick:alice (or from:alice): only alice's
// lines, her joins and quits too. "a phrase" in quotes: as written.
LogQuery log_query(const std::string& q, bool events) {
    LogQuery r;
    r.events = events;
    size_t i = 0;
    while (i < q.size()) {
        while (i < q.size() && q[i] == ' ') i++;
        if (i >= q.size()) break;
        std::string w;
        if (q[i] == '"') {
            size_t e = q.find('"', i + 1);
            w = q.substr(i + 1, e == std::string::npos ? std::string::npos : e - i - 1);
            i = e == std::string::npos ? q.size() : e + 1;
        } else {
            size_t e = q.find(' ', i);
            w = q.substr(i, e == std::string::npos ? std::string::npos : e - i);
            i = e == std::string::npos ? q.size() : e;
        }
        std::string lw = irc_lower(w);
        if (lw.rfind("nick:", 0) == 0 || lw.rfind("from:", 0) == 0) r.nick = lw.substr(5);
        else if (!lw.empty()) r.words.push_back(lw);
    }
    return r;
}

bool log_match(const Line& l, const LogQuery& q) {
    if (!q.events && noise(l)) return false;
    if (!q.nick.empty() && irc_lower(l.nick) != q.nick) return false;
    if (q.words.empty()) return true;
    std::string hay = irc_lower(l.nick + " " + mirc_strip(l.text));
    for (auto& w : q.words)
        if (hay.find(w) == std::string::npos) return false;
    return true;
}

// q's lines in files[which...], in that order (newest day first, as log_files() lists them), max at most
std::vector<LogHit> log_search(const std::vector<LogFile>& files, const std::vector<size_t>& which, const LogQuery& q,
                               size_t max, bool* more) {
    std::vector<LogHit> out;
    if (more) *more = false;
    for (size_t fi : which) {
        if (fi >= files.size()) continue;
        auto lines = log_read(files[fi].path);
        for (size_t i = 0; i < lines.size(); i++) {
            if (!log_match(lines[i], q)) continue;
            if (out.size() >= max) { if (more) *more = true; return out; }
            out.push_back({fi, i, lines[i]});
        }
    }
    return out;
}
