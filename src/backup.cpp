// /backup and /restore: the config folder's state (settings, servers, passwords with their secret.key, plugins and their data,
// your themes, the BNC's certificate) in one file, <config dir>/backups/<name>.isebak. The file is compressed (zlib) and
// sealed with a passphrase (scrypt + AES-256-GCM, the same as /e2e), so the passwords inside stay private on a USB stick.
//
//   "iSEBAK1" 'z'|'p'  salt(16)  nonce|ciphertext|tag
//
// The ciphertext holds records "<size>\t<path>\n<bytes>", zlib-compressed behind a 4-byte size when the first letter is z.
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <openssl/rand.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include "core.h"
#include "plugin/internal.h"
#ifndef ISEKUR_NO_ZLIB
#include <zlib.h>
#endif

static const char MAGIC[] = "iSEBAK1";  // 7 bytes, then z (compressed) or p
static const size_t HEAD = 8, SALT = 16;
[[maybe_unused]] static const size_t MAX_RAW = 64u << 20;  // a backup is a few KB: 64 MB is a corrupt (or hostile) file

static bool stem_ok(const std::string& s) {  // a backup's or theme's name: letters, digits, . _ - (never a path)
    if (s.empty() || s[0] == '.' || s.find("..") != std::string::npos) return false;
    for (unsigned char ch : s)
        if (!isalnum(ch) && ch != '_' && ch != '-' && ch != '.') return false;
    return true;
}

// the only files a backup holds, and so the only ones /restore writes (a crafted backup can't reach anything else)
bool backup_path_ok(const std::string& p) {
    if (p == "isekur.conf" || p == "secret.key" || p == "bnc.crt" || p == "bnc.key") return true;
    auto ext = [&](const char* pre, const char* e) {
        size_t a = strlen(pre), n = strlen(e);
        return p.rfind(pre, 0) == 0 && p.size() > a + n && p.compare(p.size() - n, n, e) == 0 ? p.substr(a, p.size() - a - n) : std::string();
    };
    for (const char* e : {".lua", ".data"})
        if (std::string s = ext("plugins/", e); !s.empty()) return plugin_name_ok(s);
    if (std::string s = ext("themes/", ".theme"); !s.empty()) return stem_ok(s);
    return false;
}

std::string backup_pack(const std::vector<BackupFile>& files, const std::string& pass, bool zip) {
    std::string body;
    for (auto& f : files) body += std::to_string(f.data.size()) + "\t" + f.path + "\n" + f.data;
    char kind = 'p';
#ifndef ISEKUR_NO_ZLIB
    if (zip) {
        uLongf n = compressBound((uLong)body.size());
        std::string c(n, '\0');
        if (compress2((Bytef*)&c[0], &n, (const Bytef*)body.data(), (uLong)body.size(), 9) == Z_OK) {
            uint32_t raw = (uint32_t)body.size();
            body = std::string{(char)(raw >> 24), (char)(raw >> 16), (char)(raw >> 8), (char)raw} + c.substr(0, n);
            kind = 'z';
        }
    }
#else
    (void)zip;
#endif
    std::string head = std::string(MAGIC) + kind;
    unsigned char r[SALT];
    if (RAND_bytes(r, sizeof r) != 1) return "";
    std::string salt((char*)r, sizeof r), key = e2e_key(pass, salt);
    if (key.empty()) return "";
    return head + salt + unb64(e2e_seal(key, body, head));  // the header is authenticated too: it can't be switched to "plain"
}

bool backup_unpack(const std::string& in, const std::string& pass, std::vector<BackupFile>& out, std::string& err) {
    out.clear();
    if (in.size() < HEAD + SALT + 28 || in.compare(0, 7, MAGIC) != 0) { err = "not an iSekur backup"; return false; }
    char kind = in[7];
    if (kind != 'z' && kind != 'p') { err = "a backup this iSekur doesn't know"; return false; }
#ifdef ISEKUR_NO_ZLIB
    if (kind == 'z') { err = "this backup is compressed and this iSekur was built without zlib: install its headers and build again"; return false; }
#endif
    std::string key = e2e_key(pass, in.substr(HEAD, SALT)), body;
    if (key.empty() || !e2e_open(key, b64(in.substr(HEAD + SALT)), body, in.substr(0, HEAD))) {
        err = "wrong passphrase, or the file is damaged";
        return false;
    }
#ifndef ISEKUR_NO_ZLIB
    if (kind == 'z') {
        if (body.size() < 5) { err = "the backup is damaged"; return false; }
        uLongf n = (uLongf)((unsigned char)body[0] << 24 | (unsigned char)body[1] << 16 | (unsigned char)body[2] << 8 | (unsigned char)body[3]);
        if (n > MAX_RAW) { err = "the backup is damaged"; return false; }
        std::string raw(n, '\0');
        if (uncompress((Bytef*)&raw[0], &n, (const Bytef*)body.data() + 4, (uLong)body.size() - 4) != Z_OK || n != raw.size()) {
            err = "the backup is damaged";
            return false;
        }
        body = std::move(raw);
    }
#endif
    for (size_t at = 0; at < body.size();) {
        size_t tab = body.find('\t', at), nl = body.find('\n', at);
        if (tab == std::string::npos || nl == std::string::npos || tab >= nl || tab == at ||
            body.find_first_not_of("0123456789", at) != tab || tab - at > 12) { err = "the backup is damaged"; return false; }
        size_t sz = (size_t)strtoull(body.c_str() + at, nullptr, 10), data = nl + 1;
        if (sz > body.size() - data) { err = "the backup is damaged"; return false; }
        out.push_back({body.substr(tab + 1, nl - tab - 1), body.substr(data, sz)});
        at = data + sz;
    }
    return true;
}

// ---- the Core side ----------------------------------------------------------------------------
static bool slurp(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

static std::vector<std::string> ls(const std::string& dir) {  // the plain files in a folder, A-Z
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.') out.push_back(e->d_name);
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

static bool write_atomic(const std::string& path, const std::string& data, std::string& err) {  // 0600, whole or not at all
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool ok = fd >= 0;
    for (size_t at = 0; ok && at < data.size();) {
        ssize_t n = write(fd, data.data() + at, data.size() - at);
        if (n <= 0) ok = false;
        else at += (size_t)n;
    }
    if (fd >= 0) ok = (close(fd) == 0) && ok;
    if (ok && rename(tmp.c_str(), path.c_str()) != 0) ok = false;
    if (!ok) err = path + ": " + strerror(errno), unlink(tmp.c_str());
    return ok;
}

// the warning /restore shows in the chat before it asks: the dialog's own lines say the same, in the box
static const char* const WIPE = "\xe2\x9a\xa0 /restore WIPES your current configuration (settings, servers, passwords, plugins, your themes) and replaces it with the backup's";

static std::string kb(size_t n) { return n < 1024 ? std::to_string(n) + " B" : std::to_string((n + 512) / 1024) + " KB"; }

static std::string mval(const std::string& man, const std::string& key) {  // MANIFEST's key=value line
    for (auto& l : split(man, '\n'))
        if (l.rfind(key + "=", 0) == 0) return l.substr(key.size() + 1);
    return "";
}

std::string Core::backups_dir() const { return cfg_path.substr(0, cfg_path.rfind('/') + 1) + "backups"; }

std::vector<BackupFile> Core::backup_files() const {  // what is on disk now (unsaved /set changes aren't)
    std::string dir = cfg_path.substr(0, cfg_path.rfind('/') + 1);
    std::vector<BackupFile> out;
    auto add = [&](const std::string& rel, const std::string& path) {
        if (std::string d; slurp(path, d)) out.push_back({rel, d});
    };
    add("isekur.conf", cfg_path);
    for (const char* f : {"secret.key", "bnc.crt", "bnc.key"}) add(f, dir + f);
    for (const char* sub : {"plugins", "themes"})
        for (auto& n : ls(dir + sub))
            if (backup_path_ok(std::string(sub) + "/" + n)) add(std::string(sub) + "/" + n, dir + sub + "/" + n);
    return out;
}

struct BakStat { size_t files = 0, raw = 0, packed = 0; };
static bool make_backup(const Core& c, const std::string& path, const std::string& pass, BakStat& st, std::string& err) {
    auto files = c.backup_files();
    if (files.empty() || files[0].path != "isekur.conf") { err = "no config at " + c.cfg_path; return false; }
    char host[256] = "";
    gethostname(host, sizeof host - 1);
    st.files = files.size();
    for (auto& f : files) st.raw += f.data.size();
    files.insert(files.begin(), {"MANIFEST", "created=" + std::to_string((long long)time(nullptr)) + "\nhost=" + host + "\nversion=" + version_string() +
                                                 "\ndir=" + c.cfg_path.substr(0, c.cfg_path.rfind('/') + 1) + "\n"});
    std::string bytes = backup_pack(files, pass);
    if (bytes.empty()) { err = "couldn't lock the file (no random numbers, or not enough memory for the key)"; return false; }
    mkdir_p(path.substr(0, path.rfind('/')));
    if (!write_atomic(path, bytes, err)) return false;
    st.packed = bytes.size();
    return true;
}

static bool is_bak(const std::string& n) { return n.size() > 7 && n.compare(n.size() - 7, 7, ".isebak") == 0; }

// /backup [name]: asks a passphrase twice, then writes <config dir>/backups/<name>.isebak
void Core::backup_cmd(Buffer* b, const std::string& args) {
    auto argv = split(args, ' ');
    std::string name = argv.empty() ? "" : argv[0];
    if (is_bak(name)) name.resize(name.size() - 7);
    if (name.empty()) name = "isekur-" + fmt_time(time(nullptr), "%Y%m%d-%H%M%S");
    else if (!stem_ok(name)) { print(b, LK::ERROR, "A backup's name takes letters, digits and . _ - : /backup [name]"); return; }
    std::string path = backups_dir() + "/" + name + ".isebak";
    if (access(path.c_str(), F_OK) == 0) { print(b, LK::ERROR, path + " is there already: /backup with another name (a backup is never overwritten)"); return; }
    if (access(cfg_path.c_str(), R_OK) != 0) { print(b, LK::ERROR, "No config file at " + cfg_path + " to back up"); return; }
    if (!unsaved.empty()) info_note(b, "Unsaved /set changes aren't in the backup, only the saved file is: /save first to include them");
    ask_pass(b, "passphrase", [this, b, path, name](const std::string& p1) {
        if (index_of(b) < 0) return;
        if (p1.size() < 8) { print(b, LK::ERROR, "A passphrase takes 8 characters or more: /backup to try again"); return; }
        ask_pass(b, "again", [this, b, path, name, p1](const std::string& p2) {
            if (index_of(b) < 0) return;
            if (p1 != p2) { print(b, LK::ERROR, "The passphrases don't match, nothing was saved: /backup to try again"); return; }
            BakStat st;
            std::string err;
            if (!make_backup(*this, path, p1, st, err)) { print(b, LK::ERROR, "Backup failed: " + err); return; }
            print(b, LK::INFO, "Backup saved: " + path);
            print(b, LK::INFO, std::to_string(st.files) + (st.files == 1 ? " file, " : " files, ") + (st.packed < st.raw ? kb(st.raw) + " packed into " : "") + kb(st.packed) + ", locked with your passphrase (iSekur can't recover it)");
            info_note(b, "To move to another computer: install iSekur there, start it once, copy " + name + ".isebak into ~/.config/isekur/backups/ "
                         "(make the folder if it isn't there), and type /restore " + name + " in iSekur. /restore /any/folder/" + name + ".isebak works too");
        }, true, "Backup", {"Type it again"});
    }, true, "Backup", {"Settings, servers, passwords, plugins", "and themes go into one file.", "The passphrase locks it: 8+ characters.", "Lose it and nobody can open the backup."});
}

// /restore [name | path]: the backup replaces what is in the config folder
void Core::restore_cmd(Buffer* b, const std::string& args) {
    std::string dir = backups_dir(), a = trim(args);  // a path may have spaces in it
    if (a.empty()) {  // what is there, then which one
        struct Have { time_t at; std::string name, line; };
        std::vector<Have> have;
        for (auto& n : ls(dir))
            if (struct stat sb; is_bak(n) && stat((dir + "/" + n).c_str(), &sb) == 0)
                have.push_back({sb.st_mtime, n.substr(0, n.size() - 7), n.substr(0, n.size() - 7) + "  " + kb((size_t)sb.st_size) + "  " + fmt_time(sb.st_mtime, "%Y-%m-%d %H:%M")});
        std::sort(have.begin(), have.end(), [](auto& x, auto& y) { return x.at > y.at; });
        print(b, LK::INFO, have.empty() ? "No backups in " + dir + " yet: /backup makes one, or give /restore the path of a .isebak file" : "Backups in " + dir + ", newest first:");
        for (auto& h : have) print(b, LK::INFO, "  " + h.line);
        print(b, LK::ERROR, WIPE);
        std::vector<std::string> info = {"!\xe2\x9a\xa0 This WIPES your current configuration", "!and replaces it with the backup's.",
                                         have.empty() ? "No backups in the backups folder yet." : "Backups here, newest first:"};
        for (size_t i = 0; i < have.size() && i < 4; i++) info.push_back("  " + have[i].name.substr(0, 40));
        info.push_back("Or the full path of a .isebak file.");
        ask_pass(b, "backup (name or path)", [this, b](const std::string& n) { if (index_of(b) >= 0) exec(b, "/restore " + n); }, false, "Restore backup", info);
        return;
    }
    std::string path, bytes;
    if (a.rfind("~/", 0) == 0)
        if (const char* h = getenv("HOME")) a = h + a.substr(1);
    path = a.find('/') != std::string::npos ? a : dir + "/" + (is_bak(a) ? a : a + ".isebak");
    if (!slurp(path, bytes)) { print(b, LK::ERROR, "Can't read " + path + (a.find('/') == std::string::npos ? ": /restore lists the backups in " + dir : "")); return; }
    if (bytes.compare(0, 7, MAGIC) != 0) { print(b, LK::ERROR, path + " isn't an iSekur backup"); return; }
    std::string snap = dir + "/before-restore-" + fmt_time(time(nullptr), "%Y%m%d-%H%M%S") + ".isebak";
    print(b, LK::ERROR, WIPE);
    ask_pass(b, "passphrase of the backup", [this, b, path, bytes, snap](const std::string& pass) { restore_apply(b, path, bytes, snap, pass); }, true,
             "Restore backup",
             {"!\xe2\x9a\xa0 This WIPES your current configuration:", "!settings, servers, passwords, plugins,", "!themes. The backup's replaces them.", "A copy of what you have now is saved to",
              snap.substr(snap.rfind('/') + 1), "with the same passphrase first."});
}

void Core::restore_apply(Buffer* b, const std::string& path, const std::string& bytes, const std::string& snap, const std::string& pass) {
    if (index_of(b) < 0) return;
    std::vector<BackupFile> files, put;
    std::string err, man;
    if (!backup_unpack(bytes, pass, files, err)) { print(b, LK::ERROR, "Nothing changed: " + err); return; }
    for (auto& f : files) {
        if (f.path == "MANIFEST") { man = f.data; continue; }
        if (!backup_path_ok(f.path)) { print(b, LK::ERROR, "Nothing changed: the backup holds a file iSekur doesn't restore (" + f.path + ")"); return; }
        put.push_back(f);
    }
    if (std::none_of(put.begin(), put.end(), [](auto& f) { return f.path == "isekur.conf"; })) { print(b, LK::ERROR, "Nothing changed: there is no config in that backup"); return; }
    if (access(cfg_path.c_str(), F_OK) == 0) {  // what you have now, first: the way back
        BakStat st;
        if (!make_backup(*this, snap, pass, st, err)) { print(b, LK::ERROR, "Nothing changed: couldn't save what you have now first (" + err + ")"); return; }
    }
    std::string dir = cfg_path.substr(0, cfg_path.rfind('/') + 1);
    auto rank = [](const std::string& p) { return p == "secret.key" ? 0 : p == "isekur.conf" ? 2 : 1; };  // the config's passwords need the key: key first, config last
    std::stable_sort(put.begin(), put.end(), [&](auto& x, auto& y) { return rank(x.path) < rank(y.path); });
    mkdir_p(dir + "plugins"), mkdir_p(dir + "themes");
    std::set<std::string> kept;
    std::vector<std::string> luas;
    for (auto& f : put) {
        if (!write_atomic(f.path == "isekur.conf" ? cfg_path : dir + f.path, f.data, err)) {
            print(b, LK::ERROR, "Stopped half way: " + err + ". What you had before is in " + snap + ": /restore " + snap.substr(snap.rfind('/') + 1) + " puts it back");
            return;
        }
        kept.insert(f.path);
        if (f.path.compare(0, 8, "plugins/") == 0 && f.path.compare(f.path.size() - 4, 4, ".lua") == 0) luas.push_back(f.path.substr(8, f.path.size() - 12));
    }
    size_t gone = 0;  // plugins and themes the backup doesn't have: not part of that configuration
    for (const char* sub : {"plugins", "themes"})
        for (auto& n : ls(dir + sub))
            if (std::string rel = std::string(sub) + "/" + n; backup_path_ok(rel) && !kept.count(rel) && unlink((dir + rel).c_str()) == 0) gone++;
    unsaved.clear(), set_was.clear();  // the unsaved changes belonged to the configuration that is gone
    std::vector<std::string> running;
    for (auto& p : plugins)
        if (!p->dead && std::find(luas.begin(), luas.end(), p->name) != luas.end()) running.push_back(p->name);
    reload(b);  // settings, theme, servers, which plugins are on
    for (auto& n : running) plugin_cmd(b, "reload " + n);  // the file may have changed under a plugin that was already running
    long long made = atoll(mval(man, "created").c_str());
    std::string host = mval(man, "host");
    print(b, LK::INFO, "Restored " + std::to_string(put.size()) + (put.size() == 1 ? " file from " : " files from ") + path + (made ? ", made " + fmt_time((time_t)made, "%Y-%m-%d %H:%M") : "") +
                           (host.empty() ? "" : " on " + host) + (gone ? "; " + std::to_string(gone) + " plugin/theme files it doesn't have were removed" : ""));
    print(b, LK::INFO, "What you had before is in " + snap + " (same passphrase): /restore " + snap.substr(snap.rfind('/') + 1) + " undoes this");
    if (std::string from = mval(man, "dir"); !from.empty() && from != dir)
        info_note(b, "This backup was made in " + from + ": folders it names (log_dir, dcc_dir, certificates) may not exist here, /checkconf lists what doesn't");
    info_note(b, "Restart iSekur to apply everything: connections that are open keep their old settings until then");
}
