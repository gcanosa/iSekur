// The plugin catalog (index.json and <name>/<name>.lua from the plugins repo: fetched, checked against sha256, installed, updated,
// removed), /plugin, and the data the plugin windows show.
#include <openssl/evp.h>
#include "plugin/internal.h"

using namespace plug;

// ---- the catalog: <base>index.json and <base><name>/<name>.lua ---------------------------
// base: $ISEKUR_PLUGIN_URL (https://... or file:///dir/ for testing), else the repo's plugins/
static std::string catalog_base() {
    const char* e = getenv("ISEKUR_PLUGIN_URL");
    std::string b = e && *e ? e : "https://raw.githubusercontent.com/gcanosa/isekur-plugins/main/";
    return b.back() == '/' ? b : b + "/";
}
static const size_t FETCH_CAP = 1 << 20;

static bool fetch(const std::string& base, const std::string& rel, std::string& out) {
    if (base.rfind("file://", 0) == 0) {
        std::ifstream f(base.substr(7) + rel, std::ios::binary);
        if (!f) { out = "can't read " + base.substr(7) + rel; return false; }
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }
    if (base.rfind("https://", 0) != 0) { out = "ISEKUR_PLUGIN_URL must be https:// or file://"; return false; }
    size_t sl = base.find('/', 8);
    return https_get(base.substr(8, sl - 8), base.substr(sl) + rel, FETCH_CAP, out);
}

std::string sha256_of(const std::string& s) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    EVP_Digest(s.data(), s.size(), md, &n, EVP_sha256(), nullptr);
    std::string o;
    char b[3];
    for (unsigned i = 0; i < n; i++) snprintf(b, sizeof b, "%02x", md[i]), o += b;
    return o;
}

// index.json: an array of flat objects, string/number/true/false/null values; ok false on bad JSON
std::vector<std::map<std::string, std::string>> plugin_index(const std::string& j, bool* ok) {
    std::vector<std::map<std::string, std::string>> out;
    size_t i = 0;
    auto ws = [&] { while (i < j.size() && isspace((unsigned char)j[i])) i++; };
    auto eat = [&](char ch) { ws(); if (i < j.size() && j[i] == ch) { i++; return true; } return false; };
    auto str = [&](std::string& v) {
        if (!eat('"')) return false;
        v.clear();
        while (i < j.size() && j[i] != '"') {
            char ch = j[i++];
            if (ch != '\\') { v += ch; continue; }
            if (i >= j.size()) return false;
            ch = j[i++];
            if (ch == 'u' && i + 4 <= j.size()) utf8_put(v, (uint32_t)strtoul(j.substr(i, 4).c_str(), nullptr, 16)), i += 4;
            else v += ch == 'n' ? '\n' : ch == 't' ? '\t' : ch;  // \" \\ \/
        }
        return eat('"');
    };
    auto fail = [&] { if (ok) *ok = false; return std::vector<std::map<std::string, std::string>>{}; };
    if (ok) *ok = true;
    if (!eat('[')) return fail();
    if (eat(']')) return out;
    do {
        if (!eat('{')) return fail();
        std::map<std::string, std::string> o;
        if (!eat('}')) {
            do {
                std::string k, v;
                if (!str(k) || !eat(':')) return fail();
                ws();
                if (i < j.size() && j[i] == '"') { if (!str(v)) return fail(); }
                else {
                    size_t st = i;
                    while (i < j.size() && (isalnum((unsigned char)j[i]) || j[i] == '.' || j[i] == '-' || j[i] == '+')) i++;
                    if (st == i) return fail();
                    v = j.substr(st, i - st);
                }
                o[k] = v;
            } while (eat(','));
            if (!eat('}')) return fail();
        }
        out.push_back(o);
    } while (eat(','));
    if (!eat(']')) return fail();
    return out;
}

struct CatalogJob {
    std::string sub, base, index, err;     // sub: available, install, update
    std::vector<std::string> want;         // the plugin files to fetch with the index
    std::map<std::string, std::string> files, errs;
    int buf = 0;                           // the tab /plugin was typed in
    bool all = false;                      // update with no name: only newer versions, edited files left alone
    std::atomic<bool> done{false};
};

static void catalog_start(Core& c, Buffer* b, const std::string& sub, std::vector<std::string> want) {
    if (c.catalog) { if (sub != "refresh") c.plug_print(b, LK::ERROR, "Still fetching the plugin catalog: try again in a moment"); return; }
    auto j = std::make_shared<CatalogJob>();
    j->sub = sub, j->base = catalog_base(), j->want = std::move(want), j->buf = b->id;
    c.catalog = j;
    if (sub != "refresh") c.plug_print(b, LK::INFO, "Fetching the plugin catalog from " + j->base + "...");
    std::thread([j] {
        if (fetch(j->base, "index.json", j->index)) {
            for (auto& n : j->want) {
                std::string body;
                if (fetch(j->base, n + "/" + n + ".lua", body)) j->files[n] = body;
                else j->errs[n] = body;
            }
        } else j->err = j->index;
        j->done.store(true, std::memory_order_release);
    }).detach();
}

static std::string read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    if (f) ss << f.rdbuf();
    return ss.str();
}

static bool write_plugin(Core& c, const std::string& name, const std::string& body, std::string& err) {
    std::string dir = c.plugin_dir(), path = dir + "/" + name + ".lua", tmp = path + ".tmp";
    mkdir_p(dir);
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) { err = "can't write " + path; return false; }
    bool ok = fwrite(body.data(), 1, body.size(), f) == body.size();
    ok = fclose(f) == 0 && ok && rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) err = "can't write " + path, unlink(tmp.c_str());
    return ok;
}

// ponytail: one shared cache file per config; ETag/If-None-Match if a 12h fetch per user ever counts on GitHub's side
static const time_t CATALOG_TTL = 12 * 3600, CATALOG_RETRY = 600, CATALOG_FORCE = 60;
static std::string catalog_file(const Core& c) { return c.plugin_dir() + "/.catalog.json"; }

// "-- version: 1.2", "-- author: ...", "-- contact: ..." in the first 20 lines
std::string plugin_header(const std::string& lua, const std::string& key) {
    size_t at = 0;
    std::string tag = "-- " + key + ":";
    for (int i = 0; i < 20 && at < lua.size(); i++) {
        size_t nl = lua.find('\n', at);
        std::string l = lua.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? lua.size() : nl + 1;
        if (l.rfind(tag, 0) != 0) continue;
        std::string v = l.substr(tag.size());
        v.erase(0, v.find_first_not_of(" \t"));
        v.erase(v.find_last_not_of(" \t\r") + 1);
        return v;
    }
    return "";
}
// what the windows compare with the catalog's
std::string plugin_version(const std::string& lua) { return plugin_header(lua, "version"); }

// the installed file's sha256 and version, read again only when its size or time changed
// its header comment's first sentence ("-- name: what it does. More..."): the description until the catalog's comes
static std::string plugin_blurb(const std::string& lua, const std::string& name) {
    std::string out;
    bool in = false;
    for (auto& l : split(lua, '\n')) {
        if (l.rfind("--", 0) != 0) { if (in) break; continue; }
        std::string t = l.substr(2);
        t.erase(0, t.find_first_not_of(" \t")), t.erase(t.find_last_not_of(" \t\r") + 1);
        if (!in) { if (t.rfind(name + ":", 0) != 0) continue; in = true, t = t.substr(name.size() + 1); t.erase(0, t.find_first_not_of(' ')); }
        if (t.empty()) break;
        out += (out.empty() ? "" : " ") + t;
        if (size_t e = out.find(". "); e != std::string::npos) { out.resize(e + 1); break; }
    }
    if (!out.empty() && islower((unsigned char)out[0])) out[0] = (char)toupper((unsigned char)out[0]);
    return out;
}

struct FileInfo { std::string sha, version, blurb, author, contact; };
static FileInfo file_info(const std::string& path, const std::string& name) {  // name: for the blurb (cached by path)
    static std::map<std::string, std::pair<std::string, FileInfo>> cache;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return {};
    std::string key = std::to_string(st.st_size) + ":" + std::to_string((long long)st.st_mtime);
    auto& e = cache[path];
    if (e.first != key) {
        std::string t = read_all(path);
        e = {key, {sha256_of(t), plugin_version(t), plugin_blurb(t, name), plugin_header(t, "author"), plugin_header(t, "contact")}};
    }
    return e.second;
}
// newer: the catalog's version is above the installed one (or the file says none and isn't the catalog's)
static bool newer_in_catalog(const std::string& cat, const std::string& local, bool same) {
    return local.empty() ? !same : version_newer(cat, local);
}

// auto-away was in core: its [general] and [server.X] keys become [plugin.autoaway] and
// [plugin.autoaway@X], and leave the config. false: the old auto_away = off said it was off
static const std::multimap<std::string, std::string> AWAY_KEYS = {
    {"auto_away", "enabled"}, {"auto_away_idle", "idle"}, {"away_message", "message"},
    {"away_announce", "announce_away"}, {"away_announce", "announce_back"}};  // it did both
bool old_away_keys(const Core& c) {
    for (auto& [sec, kv] : c.cfg)
        if (sec == "general" || sec.rfind("server.", 0) == 0)
            for (auto& [k, v] : AWAY_KEYS) if (kv.count(k)) return true;
    return false;
}
static void migrate_autoaway(Core& c, Buffer* b) {
    Ini moved;
    int n = 0;
    for (auto& [sec, kv] : c.cfg) {
        std::string to = sec == "general" ? "plugin.autoaway" : sec.rfind("server.", 0) == 0 ? "plugin.autoaway@" + sec.substr(7) : "";
        if (to.empty()) continue;
        for (auto& [k, nk] : AWAY_KEYS) {
            auto it = kv.find(k);
            if (it == kv.end()) continue;
            if (!it->second.empty() && ini_get(c.cfg, to, nk).empty()) moved[to][nk] = it->second, n++;
        }
    }
    for (auto& [sec, kv] : c.cfg)
        for (auto& [k, nk] : AWAY_KEYS) kv.erase(k), c.unsaved[sec].erase(k);
    for (auto& [sec, kv] : moved)
        for (auto& [k, v] : kv) c.cfg[sec][k] = v;
    std::string err;
    std::set<std::string> old;
    for (auto& [k, nk] : AWAY_KEYS) old.insert(k);
    if (!c.cfg_path.empty() && ((!moved.empty() && !ini_save(c.cfg_path, moved, err)) || !ini_drop_keys_file(c.cfg_path, old, err))) {
        c.plug_print(b, LK::ERROR, "autoaway: " + err);
        return;
    }
    if (n) c.plug_print(b, LK::INFO, "autoaway: moved " + std::to_string(n) + " auto-away setting" + (n > 1 ? "s" : "") +
                                    " into [plugin.autoaway] sections; the old keys are gone from the config");
}

// /plugin available: the cached catalog, what's installed marked
static void list_available(Core& c, Buffer* b) {
    int newer = 0, shown = 0;
    for (auto& r : c.catalog_rows()) {
        if (r.too_new) { newer++; continue; }
        std::string st = !r.installed ? "" : r.update ? "  [update: " + (r.local.empty() ? "?" : r.local) + " → " + r.version + ", /plugin update " + r.name + "]"
                                       : r.edited ? "  [installed, edited here: /plugin update " + r.name + " restores it]" : "  [installed]";
        c.plug_print(b, LK::INFO, "\x02" + r.name + "\x02 " + r.version + "  " + r.desc + st);
        shown++;
    }
    if (!shown) c.plug_print(b, LK::INFO, "The catalog has no plugins for this iSekur yet");
    else c.plug_print(b, LK::INFO, "/plugin install name installs one · catalog from " + fmt_dur(time(nullptr) - c.catalog_at) + " ago, /plugin refresh fetches it again");
    if (newer) c.plug_print(b, LK::INFO, std::to_string(newer) + " more need a newer iSekur");
}

void Core::plugins_poll() {
    http_poll();
    time_t now = time(nullptr);
    for (auto& p : plugins)
        if (p->store_dirty && now - p->store_at >= STORE_EVERY) store_flush(p.get());
    if (!catalog || !catalog->done.load(std::memory_order_acquire)) return;
    auto j = catalog;
    catalog.reset();
    Buffer* b = nullptr;
    for (auto& x : bufs) if (x->id == j->buf) b = x.get();
    bool quiet = j->sub == "refresh";
    dirty = true;
    if (j->err.size()) { catalog_err = j->err; if (!quiet) plug_print(b, LK::ERROR, "Plugin catalog: " + j->err); return; }
    bool ok;
    auto idx = plugin_index(j->index, &ok);
    if (!ok) { catalog_err = "index.json isn't valid"; if (!quiet) plug_print(b, LK::ERROR, "Plugin catalog: index.json isn't valid"); return; }
    catalog_cache = idx, catalog_have = true, catalog_err.clear(), catalog_at = time(nullptr);
    if (!cfg_path.empty()) {  // kept for the next start and the windows: no fetch while it's fresh
        std::string path = catalog_file(*this), tmp = path + ".tmp";
        mkdir_p(plugin_dir());
        if (FILE* f = fopen(tmp.c_str(), "w")) {
            bool okw = fwrite(j->index.data(), 1, j->index.size(), f) == j->index.size();
            if (fclose(f) == 0 && okw) rename(tmp.c_str(), path.c_str());
            else unlink(tmp.c_str());
        }
    }
    if (quiet) return;
    if (j->sub == "fetch") {
        plug_print(b, LK::INFO, "Catalog fetched: " + std::to_string(catalog_rows().size()) + " plugin" + (catalog_rows().size() == 1 ? "" : "s") + " (/plugin available)");
        return;
    }
    std::map<std::string, std::map<std::string, std::string>> by;
    for (auto& e : idx) if (plugin_name_ok(e["name"])) by[e["name"]] = e;
    auto files = plugin_files(plugin_dir());
    auto have = [&](const std::string& n) { return std::find(files.begin(), files.end(), n) != files.end(); };
    auto local_sha = [&](const std::string& n) { return sha256_of(read_all(plugin_dir() + "/" + n + ".lua")); };
    if (j->sub == "available") { list_available(*this, b); return; }
    // install and update: checked against the index, written, (re)loaded
    int done = 0;
    for (auto& n : j->want) {
        auto e = by.find(n);
        if (e == by.end()) { if (j->sub == "install") plug_print(b, LK::ERROR, "No plugin named " + n + " in the catalog (/plugin available)"); continue; }
        auto& en = e->second;
        if (atoi(en["api"].c_str()) > PLUGIN_API) { plug_print(b, LK::ERROR, n + " " + en["version"] + " needs a newer iSekur"); continue; }
        std::string lv = have(n) ? plugin_version(read_all(plugin_dir() + "/" + n + ".lua")) : "";
        if (j->sub == "update" && have(n)) {
            bool same = local_sha(n) == en["sha256"];
            if (same) continue;  // already the catalog's
            if (j->all && !newer_in_catalog(en["version"], lv, same)) continue;  // edited here, same version: only by name
        }
        auto f = j->files.find(n);
        if (f == j->files.end()) { plug_print(b, LK::ERROR, n + ": " + (j->errs.count(n) ? j->errs[n] : "not fetched")); continue; }
        if (sha256_of(f->second) != en["sha256"]) { plug_print(b, LK::ERROR, n + ": refused, its checksum doesn't match the catalog's"); continue; }
        std::string err;
        if (!write_plugin(*this, n, f->second, err)) { plug_print(b, LK::ERROR, n + ": " + err); continue; }
        done++;
        plugins.erase(std::remove_if(plugins.begin(), plugins.end(), [&](auto& p) { return p->name == n; }), plugins.end());
        bool on = true;
        if (j->sub == "install") {
            if (n == "autoaway") migrate_autoaway(*this, b), on = !(cfg.count("plugin.autoaway") && cfg["plugin.autoaway"].count("enabled"));
            else on = !wanted(*this, n) && !(cfg.count("plugin." + n) && cfg["plugin." + n].count("enabled"));
            std::string err2;
            if (on) {  // installed means on, unless the config already says otherwise
                cfg["plugin." + n]["enabled"] = "on";
                if (!cfg_path.empty() && !ini_save(cfg_path, Ini{{"plugin." + n, {{"enabled", "on"}}}}, err2)) plug_print(b, LK::ERROR, err2);
            }
        }
        plugins_sync();
        bool loaded = std::any_of(plugins.begin(), plugins.end(), [&](auto& p) { return p->name == n; });
        plug_print(b, LK::INFO, n + " " + en["version"] + (j->sub == "install" ? " installed" : lv.empty() || lv == en["version"] ? " restored" : " updated from " + lv) +
                               (loaded ? " and on" : wanted(*this, n) ? " (it didn't load, see above)" : ": off in the config, /plugin enable " + n));
    }
    if (j->sub == "update" && !done) plug_print(b, LK::INFO, j->all ? "Your plugins are up to date with the catalog" : "Already the catalog's version");
}

static void remove_plugin(Core& c, Buffer* b, const std::string& name) {
    c.plugins.erase(std::remove_if(c.plugins.begin(), c.plugins.end(), [&](auto& p) { return p->name == name; }), c.plugins.end());
    view_gone(c);
    std::string dir = c.plugin_dir(), err;
    unlink((dir + "/" + name + ".lua").c_str()), unlink((dir + "/" + name + ".data").c_str());
    std::vector<std::string> secs;
    for (auto& [k, kv] : c.cfg)
        if (k == "plugin." + name || k.rfind("plugin." + name + "@", 0) == 0) secs.push_back(k);
    for (auto& k : secs) {
        c.cfg.erase(k), c.unsaved.erase(k);
        if (!c.cfg_path.empty() && !ini_drop_section(c.cfg_path, k, err)) c.plug_print(b, LK::ERROR, err);
    }
    c.plug_print(b, LK::INFO, name + " removed, with its settings and saved data");
}


// ---- what the plugin windows show -------------------------------------------------------
void Core::catalog_load() {
    if (catalog_have || cfg_path.empty()) return;
    struct stat st;
    std::string path = catalog_file(*this), text = read_all(path);
    bool ok;
    auto idx = plugin_index(text, &ok);
    if (text.empty() || !ok || stat(path.c_str(), &st) != 0) return;
    catalog_cache = idx, catalog_have = true, catalog_at = st.st_mtime;
}

void Core::plugin_refresh(Buffer* b, bool force) {
    if (!b || catalog) return;
    catalog_load();
    time_t now = time(nullptr);
    if (!force && catalog_have && now - catalog_at < CATALOG_TTL) return;  // fresh enough
    if (now - catalog_tried < (force ? CATALOG_FORCE : CATALOG_RETRY)) {
        if (force) plug_print(b, LK::INFO, "The catalog was last asked for " + fmt_dur(now - catalog_tried) + " ago: try again in a minute");
        return;
    }
    catalog_tried = now;
    catalog_start(*this, b, force ? "fetch" : "refresh", {});
}

std::string plugin_sec(const std::string& name, const std::string& scope) { return "plugin." + name + (scope.empty() ? "" : "@" + scope); }

std::string Core::plugin_at(const std::string& name, const std::string& key, const std::string& scope) const {
    return ini_get(cfg, plugin_sec(name, scope), key);
}

std::string Core::plugin_eff(const std::string& name, const std::string& key, const std::string& scope, const std::string& def, char sc) const {
    std::string net = scope.substr(0, scope.find('/'));
    if (sc == 'c' && scope.find('/') != std::string::npos)
        if (std::string v = plugin_at(name, key, scope); !v.empty()) return v;
    if (sc != 'g' && !net.empty())
        if (std::string v = plugin_at(name, key, net); !v.empty()) return v;
    return ini_get(cfg, plugin_sec(name, ""), key, def);
}

// the windows' scopes: everywhere and each network. A channel only when the config already has one for it
// (made with /plugin set name@net/#chan): enough for a window, and a channel's own can still be changed there
std::vector<std::string> Core::plugin_scopes() const {
    std::set<std::string> out{""};
    for (auto& s : servers) out.insert(s->cfg.name);
    for (auto& [k, kv] : cfg)
        if (size_t at = k.find('@'); k.rfind("plugin.", 0) == 0 && at != std::string::npos) out.insert(k.substr(at + 1));
    return {out.begin(), out.end()};
}

std::vector<PluginInfo> Core::plugin_info() const {
    std::map<std::string, const std::map<std::string, std::string>*> cat;
    for (auto& e : catalog_cache) if (e.count("name")) cat[e.at("name")] = &e;
    std::vector<PluginInfo> out;
    for (auto& n : plugin_files(plugin_dir())) {
        PluginInfo pi;
        pi.name = n;
        const Plugin* p = nullptr;
        for (auto& x : plugins) if (x->name == n && !x->dead) p = x.get();
        pi.state = p ? "on" : wanted(*this, n) ? "failed" : "off";
        for (auto& [k, kv] : cfg)
            if ((k == "plugin." + n || k.rfind("plugin." + n + "@", 0) == 0) && truthy(ini_get(cfg, k, "enabled")))
                pi.on_in.push_back(k.size() > n.size() + 7 ? k.substr(n.size() + 8) : "");
        if (p) {
            for (auto& [cmd, i] : p->cmds) pi.cmds.push_back(cmd);
            pi.panel_lines = p->panel.lines, pi.panel_sky = p->panel.sky;
            for (auto& a : p->panel.actions) {  // a button runs the plugin's own commands only
                std::string w = a.second.substr(1, a.second.find(' ') - 1);
                to_lower(w);
                if (p->cmds.count(w)) pi.panel_actions.push_back(a);
            }
            for (auto& k : p->order) {
                auto& o = p->opts.at(k);
                pi.sets.push_back({k, o.def, o.scope, o.type == LUA_TBOOLEAN, o.help, o.choices});
            }
        }
        auto fi = file_info(plugin_dir() + "/" + n + ".lua", n);
        const std::string& sha = fi.sha;
        std::string local = fi.version;
        pi.local = local, pi.desc = fi.blurb, pi.author = fi.author, pi.contact = fi.contact;
        if (auto e = cat.find(n); e != cat.end()) {
            auto get = [&](const char* k) { auto it = e->second->find(k); return it == e->second->end() ? std::string() : it->second; };
            pi.version = get("version"), pi.in_catalog = true, pi.official = get("official") == "true";
            if (!get("description").empty()) pi.desc = get("description");
            if (!get("author").empty()) pi.author = get("author");
            if (!get("contact").empty()) pi.contact = get("contact");
            bool same = sha == get("sha256");
            pi.update = newer_in_catalog(pi.version, local, same);
            pi.edited = !same && !pi.update;
        }
        out.push_back(pi);
    }
    return out;
}

std::vector<CatalogRow> Core::catalog_rows() const {
    auto files = plugin_files(plugin_dir());
    std::vector<CatalogRow> out;
    for (auto& e : catalog_cache) {
        auto get = [&](const char* k) { auto it = e.find(k); return it == e.end() ? std::string() : it->second; };
        std::string n = get("name");
        if (!plugin_name_ok(n)) continue;
        bool inst = std::find(files.begin(), files.end(), n) != files.end();
        FileInfo fi = inst ? file_info(plugin_dir() + "/" + n + ".lua", n) : FileInfo{};
        const std::string &sha = fi.sha, &local = fi.version;
        bool same = sha == get("sha256"), up = inst && newer_in_catalog(get("version"), local, same);
        out.push_back({n, get("version"), local, get("description"), inst, up, inst && !same && !up, atoi(get("api").c_str()) > PLUGIN_API,
                       get("author"), get("contact"), get("official") == "true"});
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.name < b.name; });
    return out;
}

// /plugin [list] | enable|disable name[@net[/#chan]] | set name[@net[/#chan]] key value | reload name
void Core::plugin_cmd(Buffer* b, const std::string& args) {
    if (plugin_depth) { plug_print(b, LK::ERROR, "/plugin can't be run by a plugin"); return; }
    auto argv = split(args, ' ');
    argv.erase(std::remove(argv.begin(), argv.end(), std::string()), argv.end());
    if (argv.empty() && tty && !from_web) { plugin_win = dirty = true; return; }  // the terminal's window (the webchat opens its own)
    std::string sub = argv.empty() ? "list" : argv[0];
    std::string dir = plugin_dir();
    auto files = plugin_files(dir);
    auto loaded = [&](const std::string& n) -> Plugin* {
        for (auto& p : plugins)
            if (p->name == n && !p->dead) return p.get();
        return nullptr;
    };
    if (sub == "list") {
        if (files.empty()) { plug_print(b, LK::INFO, "No plugins in " + dir + ": /plugin available lists the catalog's"); return; }
        for (auto& n : files) {
            std::string scopes;
            for (auto& [k, kv] : cfg)
                if (k.rfind("plugin." + n + "@", 0) == 0 && truthy(ini_get(cfg, k, "enabled"))) scopes += " " + k.substr(n.size() + 8);
            Plugin* p = loaded(n);
            std::string cmds;
            if (p) for (auto& [c, i] : p->cmds) cmds += " /" + c;
            plug_print(b, LK::INFO, n + "  " + (p ? "on" : wanted(*this, n) ? "failed (see above, /plugin reload " + n + ")" : "off") +
                                   (scopes.empty() ? "" : "  in:" + scopes) + (cmds.empty() ? "" : "  commands:" + cmds));
        }
        return;
    }
    if (sub == "available") {  // the cached catalog while it's fresh, else fetched (at most every 10 minutes after a failure)
        catalog_load();
        time_t now = time(nullptr);
        if (catalog_have && (now - catalog_at < CATALOG_TTL || now - catalog_tried < CATALOG_RETRY)) { list_available(*this, b); return; }
        if (now - catalog_tried < CATALOG_RETRY) { plug_print(b, LK::ERROR, "Plugin catalog: " + (catalog_err.empty() ? "fetched a moment ago" : catalog_err) + " (/plugin refresh tries again)"); return; }
        catalog_tried = now;
        catalog_start(*this, b, sub, {});
        return;
    }
    if (sub == "refresh") { plugin_refresh(b, true); return; }
    if (sub == "update") {  // update [name]: the installed ones whose file differs from the catalog's
        std::vector<std::string> want = argv.size() > 1 ? std::vector<std::string>{argv[1]} : files;
        if (want.empty()) { plug_print(b, LK::INFO, "No plugins installed: /plugin available"); return; }
        catalog_start(*this, b, sub, want);
        if (catalog && argv.size() == 1) catalog->all = true;
        return;
    }
    if (sub == "install") {
        if (argv.size() != 2 || !plugin_name_ok(argv[1])) { plug_print(b, LK::ERROR, "Usage: /plugin install name (/plugin available lists them)"); return; }
        catalog_start(*this, b, sub, {argv[1]});
        return;
    }
    const char* usage = "Usage: /plugin [list] | available | refresh | install|update|remove name | enable|disable name[@network[/#channel]] | set name[@network[/#channel]] key value | reload name";
    if (argv.size() < 2) { plug_print(b, LK::ERROR, usage); return; }
    // name@net or name@net/#chan: that scope; enable and disable also take it as a word after the name
    std::string name = argv[1].substr(0, argv[1].find('@'));
    std::string scope = argv[1].size() > name.size() ? argv[1].substr(name.size() + 1) : "";
    if (scope.empty() && argv.size() == 3 && (sub == "enable" || sub == "disable")) scope = argv[2];
    if (!plugin_name_ok(name) || std::find(files.begin(), files.end(), name) == files.end()) {
        plug_print(b, LK::ERROR, "No plugin named " + name + " in " + dir);
        return;
    }
    // [plugin.name], [plugin.name@net] or [plugin.name@net/#chan] (the channel lowercased): written to the config at once
    std::string sec = "plugin." + name;
    if (!scope.empty()) {
        size_t sl = scope.find('/');
        std::string net = scope.substr(0, sl);
        if (!server_name_ok(net)) { plug_print(b, LK::ERROR, "No network named " + net); return; }
        sec += "@" + net;
        if (sl != std::string::npos) {
            Server* sv = nullptr;
            for (auto& s : servers) if (s->cfg.name == net) sv = s.get();
            std::string ch = scope.substr(sl + 1);
            if (ch.empty()) { plug_print(b, LK::ERROR, usage); return; }
            sec += "/" + (sv ? sv->lower(ch) : irc_lower(ch));
        }
    }
    std::string where = scope.empty() ? "" : " in " + scope;
    auto write = [&](const std::string& key, const std::string& val) {
        cfg[sec][key] = val;
        std::string err;
        if (!cfg_path.empty() && !ini_save(cfg_path, Ini{{sec, {{key, val}}}}, err)) { plug_print(b, LK::ERROR, err); return false; }
        return true;
    };
    if (sub == "enable" || sub == "disable") {
        if (!write("enabled", sub == "enable" ? "on" : "off")) return;
        plugins_sync();
        plug_print(b, LK::INFO, name + (sub == "enable" ? " enabled" : " disabled") + where +
                               (loaded(name) ? "" : wanted(*this, name) ? " (it didn't load, see above)" : ""));
        return;
    }
    if (sub == "set") {
        if (argv.size() < 3) { plug_print(b, LK::ERROR, usage); return; }
        std::string val = after_words(args, 3);
        for (auto& p : plugins)  // a setting with choices takes only those (in any case)
            if (auto o = p->name == name ? p->opts.find(argv[2]) : p->opts.end(); o != p->opts.end() && !o->second.choices.empty() && !val.empty()) {
                auto& cs = o->second.choices;
                auto it = std::find_if(cs.begin(), cs.end(), [&](auto& v) { return strcasecmp(v.c_str(), val.c_str()) == 0; });
                if (it == cs.end()) {
                    std::string all;
                    for (auto& v : cs) all += (all.empty() ? "" : ", ") + v;
                    plug_print(b, LK::ERROR, name + " " + argv[2] + " is one of: " + all);
                    return;
                }
                val = *it;
            }
        if (!write(argv[2], val)) return;
        if (argv[2] == "enabled") plugins_sync();
        else plugins_reshow();  // lolcat's palette or lines: the backlog too
        plug_print(b, LK::INFO, name + " " + argv[2] + " = " + (val.empty() ? "(default)" : val) + where);
        return;
    }
    if (sub == "remove") { remove_plugin(*this, b, name), plugins_reshow(); return; }
    if (sub == "reload") {
        plugins.erase(std::remove_if(plugins.begin(), plugins.end(), [&](auto& p) { return p->name == name; }), plugins.end());
        if (view.by == name) view_end();  // a fresh start: its view too
        if (!wanted(*this, name)) { plugins_reshow(), plug_print(b, LK::INFO, name + " is off: /plugin enable " + name); return; }
        load_one(*this, name);
        plugins_reshow();
        if (loaded(name)) plug_print(b, LK::INFO, name + " reloaded");
        return;
    }
    plug_print(b, LK::ERROR, "Unknown /plugin " + sub + ": list, available, refresh, install, update, remove, enable, disable, set, reload");
}
