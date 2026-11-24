// Files the user gives iSekur (API 16) and the boxes a plugin asks with: Core::file_drop, /upload, isekur.choose's queue.
// The Lua side is in api_core.cpp (isekur.upload) and api_ui.cpp (isekur.choose, isekur.copy).
#include "internal.h"
#include <openssl/rand.h>

using namespace plug;

static const time_t DROP_KEEP = 1800;                      // a dropped file nobody sent is forgotten (and a webchat's copy deleted) after this

// extension -> content type; the kind is its first part (image, video, audio, text), else "other"
static std::string mime_of(const std::string& name) {
    static const std::map<std::string, std::string> M = {
        {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"gif", "image/gif"}, {"webp", "image/webp"}, {"avif", "image/avif"},
        {"bmp", "image/bmp"}, {"svg", "image/svg+xml"}, {"heic", "image/heic"}, {"mp4", "video/mp4"}, {"m4v", "video/mp4"}, {"webm", "video/webm"},
        {"mov", "video/quicktime"}, {"mkv", "video/x-matroska"}, {"ogv", "video/ogg"}, {"mp3", "audio/mpeg"}, {"ogg", "audio/ogg"},
        {"oga", "audio/ogg"}, {"opus", "audio/ogg"}, {"wav", "audio/wav"}, {"m4a", "audio/mp4"}, {"aac", "audio/aac"}, {"flac", "audio/flac"},
        {"txt", "text/plain"}, {"md", "text/plain"}, {"log", "text/plain"}, {"csv", "text/plain"}, {"json", "text/plain"}, {"pdf", "application/pdf"}};
    size_t dot = name.rfind('.');
    std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
    to_lower(ext);
    auto it = M.find(ext);
    return it == M.end() ? "application/octet-stream" : it->second;
}

std::string Core::upload_dir() const { return cfg_path.substr(0, cfg_path.rfind('/') + 1) + "uploads"; }

Core::Drop* Core::drop_find(const std::string& id) {
    for (auto& d : drops)
        if (d.id == id) return &d;
    return nullptr;
}

void Core::drop_forget(const std::string& id) {
    for (auto it = drops.begin(); it != drops.end(); ++it)
        if (it->id == id) {
            if (it->own) unlink(it->path.c_str());
            drops.erase(it);
            return;
        }
}

// every tick: the forgotten ones; the first time, what an iSekur that crashed left in the folder
void Core::drops_gc(time_t now) {
    static bool swept = false;
    if (!swept && !cfg_path.empty()) {
        swept = true;
        if (DIR* d = opendir(upload_dir().c_str())) {
            while (dirent* e = readdir(d))
                if (e->d_name[0] != '.') unlink((upload_dir() + "/" + e->d_name).c_str());
            closedir(d);
        }
    }
    for (size_t i = 0; i < drops.size();)
        if (now - drops[i].at > DROP_KEEP) drop_forget(drops[i].id);
        else i++;
}

// a file the user gave: from the webchat (path: its copy in uploads/, own: deleted when done), /upload or a path pasted in the terminal
// (the user's own file, never touched). A plugin's `drop` event gets its id and what it is, and nothing of where it is. "" when a plugin
// has it, else why not
std::string Core::file_drop(Buffer* b, const std::string& path, const std::string& name, bool own) {
    auto fail = [&](const std::string& why) {
        if (own) unlink(path.c_str());
        return why;
    };
    struct stat st;
    if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode)) return fail("not a file");
    if (st.st_size <= 0) return fail("the file is empty");
    if ((unsigned long long)st.st_size > UPLOAD_MAX) return fail("too big: " + std::to_string(UPLOAD_MAX >> 20) + " MB at most");
    Server* s = b ? b->srv : nullptr;
    std::string target = b && (b->type == BT::CHANNEL || b->type == BT::QUERY) ? b->name : "";
    if (!plugin_hooks("drop", s, target)) return fail("no plugin takes files: /plugin install dropzone");
    Drop d;
    unsigned char r[8];
    RAND_bytes(r, sizeof r);
    char hx[3];
    for (unsigned char ch : r) snprintf(hx, sizeof hx, "%02x", ch), d.id += hx;
    d.path = path, d.own = own, d.at = time(nullptr), d.size = (unsigned long long)st.st_size;
    d.name = name.empty() ? path.substr(path.rfind('/') + 1) : name;
    for (char& ch : d.name) if ((unsigned char)ch < 0x20 || ch == 0x7f) ch = '_';  // a file called "a\nb" must not break a line of the chat
    d.mime = mime_of(d.name);
    d.kind = d.mime.compare(0, 6, "image/") == 0 ? "image" : d.mime.compare(0, 6, "video/") == 0 ? "video" : d.mime.compare(0, 6, "audio/") == 0 ? "audio" : d.mime.compare(0, 5, "text/") == 0 ? "text" : "other";
    drops.push_back(d);
    plugin_emit("drop", s, target, {{"id", d.id}, {"name", d.name}, {"size", (int)d.size}, {"kind", d.kind}, {"type", d.mime}, {"e2e", b && e2e_on(b)}});
    return "";
}

// "a b" 'a b' a\ b file:///a%20b ~/a: what a terminal types when a file is dropped on it, as the paths it names. Empty unless every word is a
// regular file (a message that happens to be a path is just a message)
std::vector<std::string> dropped_paths(const std::string& text) {
    std::vector<std::string> out, words;
    std::string w;
    bool in = false;
    char q = 0;
    for (size_t i = 0; i < text.size(); i++) {
        char ch = text[i];
        if (q) { if (ch == q) q = 0; else w += ch; }
        else if (ch == '\'' || ch == '"') q = ch, in = true;
        else if (ch == '\\' && i + 1 < text.size()) w += text[++i], in = true;
        else if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') { if (in) words.push_back(w), w.clear(), in = false; }
        else w += ch, in = true;
    }
    if (q) return {};
    if (in) words.push_back(w);
    for (auto p : words) {
        if (p.rfind("file://", 0) == 0) {  // a file URL: percent-escapes, maybe a host
            p = p.substr(7);
            if (p[0] != '/') p = p.substr(std::min(p.find('/'), p.size()));
            std::string o;
            for (size_t i = 0; i < p.size(); i++)
                if (p[i] == '%' && i + 2 < p.size() && isxdigit((unsigned char)p[i + 1]) && isxdigit((unsigned char)p[i + 2])) o += (char)strtol(p.substr(i + 1, 2).c_str(), nullptr, 16), i += 2;
                else o += p[i];
            p = o;
        }
        if (p.rfind("~/", 0) == 0)
            if (const char* h = getenv("HOME")) p = h + p.substr(1);
        struct stat st;
        if (p.empty() || p[0] != '/' || stat(p.c_str(), &st) || !S_ISREG(st.st_mode)) return {};
        out.push_back(p);
    }
    return out;
}

// /upload <file>...: the same as dropping them
void Core::upload_cmd(Buffer* b, const std::string& args) {
    auto files = dropped_paths(args);
    if (files.empty()) {
        print(b, LK::INFO, trim(args).empty() ? "/upload <file>: gives a file to the plugin that takes dropped files (drag one onto the webchat, or onto this terminal). /plugin install dropzone"
                                              : "/upload: not a file: " + trim(args).substr(0, 200));
        return;
    }
    for (auto& f : files)
        if (std::string why = file_drop(b, f, "", false); !why.empty()) {
            print(b, LK::ERROR, "/upload: " + why);
            return;
        }
}

// isekur.choose's queue: the front one is on screen (terminal and webchat); an answer from either closes it on both and shows the next
void Core::choice_act(int index, const std::string& text) {
    if (choices.empty()) return;
    Choice q = choices.front();
    choices.pop_front();
    dirty = true;
    notify(Ev::CHOICE, nullptr);
    if (index < 0 || index > (int)q.items.size()) index = 0;
    std::string t;  // the field's text: one line, no control characters, trimmed, 300 bytes at most (never cut inside a character); nothing when cancelled
    if (!q.input.empty() && index)
        for (char ch : text) t += (unsigned char)ch < 0x20 || ch == 0x7f ? ' ' : ch;
    t = trim(t);
    if (t.size() > 300) {
        t.resize(300);
        while (!t.empty() && ((unsigned char)t.back() & 0xc0) == 0x80) t.pop_back();
        if (!t.empty() && (unsigned char)t.back() >= 0xc0) t.pop_back();
    }
    emit_only = q.by;
    plugin_emit("choice", nullptr, "", {{"id", q.id}, {"index", index}, {"text", t}});
    emit_only.clear();
}

// isekur.copy: the terminal's clipboard on its next frame (when one is attached), the webchat's through its message
void Core::copy_text_out(const std::string& text) {
    if (tty) copy_out = text, dirty = true;
    Line l{time(nullptr), LK::INFO, "", text};  // nick unused, the text is what to copy
    notify(Ev::COPY, nullptr, &l);
}
