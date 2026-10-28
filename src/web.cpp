// Built-in webchat: tiny HTTP/1.1 + WebSocket server mirroring the session.
#include "web.h"
#include <algorithm>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include "web_assets.h"

static const int PBKDF2_ITER = 200000;
static const time_t SESSION_TTL = 7 * 24 * 3600;
static const size_t MAX_CLIENTS = 64, MAX_OUT = 8 << 20, BACKLOG = 500;

static std::string random_hex(size_t n) {
    std::vector<unsigned char> r(n);
    RAND_bytes(r.data(), (int)n);
    std::string o;
    char b[3];
    for (auto c : r) snprintf(b, sizeof b, "%02x", c), o += b;
    return o;
}

static std::string sha256_hex(const std::string& s) {
    unsigned char h[32];
    SHA256((const unsigned char*)s.data(), s.size(), h);
    std::string o;
    char b[3];
    for (auto c : h) snprintf(b, sizeof b, "%02x", c), o += b;
    return o;
}

// Sessions survive restarts. Only token hashes are stored, so the file can't
// be replayed as a cookie.
void Web::load_sessions() {
    FILE* f = fopen(sessions_path.c_str(), "r");
    if (!f) return;
    char hash[80];
    long long exp;
    time_t now = time(nullptr);
    while (fscanf(f, "%64s %lld", hash, &exp) == 2)
        if (exp > now && strlen(hash) == 64) sessions[hash] = (time_t)exp;
    fclose(f);
}

void Web::save_sessions() {
    time_t now = time(nullptr);
    std::string tmp = sessions_path + ".tmp";
    mode_t old = umask(077);
    FILE* f = fopen(tmp.c_str(), "w");
    umask(old);
    if (!f) return;
    for (auto it = sessions.begin(); it != sessions.end();) {
        if (it->second <= now) { it = sessions.erase(it); continue; }
        fprintf(f, "%s %lld\n", it->first.c_str(), (long long)it->second);
        ++it;
    }
    if (fclose(f) == 0) rename(tmp.c_str(), sessions_path.c_str());
}

// ---- passwords -------------------------------------------------------------
std::string hash_password(const std::string& pw) {
    unsigned char salt[16], out[32];
    RAND_bytes(salt, sizeof salt);
    PKCS5_PBKDF2_HMAC(pw.data(), (int)pw.size(), salt, sizeof salt, PBKDF2_ITER, EVP_sha256(),
                      sizeof out, out);
    return "pbkdf2$sha256$" + std::to_string(PBKDF2_ITER) + "$" +
           b64(std::string((char*)salt, sizeof salt)) + "$" + b64(std::string((char*)out, sizeof out));
}

bool check_password(const std::string& pw, const std::string& stored) {
    auto p = split(stored, '$');
    if (p.size() != 5 || p[0] != "pbkdf2" || p[1] != "sha256") return false;
    int iter = atoi(p[2].c_str());
    std::string salt = unb64(p[3]), want = unb64(p[4]);
    if (iter < 1000 || salt.empty() || want.size() < 16 || want.size() > 64) return false;
    unsigned char out[64];
    PKCS5_PBKDF2_HMAC(pw.data(), (int)pw.size(), (const unsigned char*)salt.data(), (int)salt.size(),
                      iter, EVP_sha256(), (int)want.size(), out);
    return CRYPTO_memcmp(out, want.data(), want.size()) == 0;
}

// ---- JSON (just enough) ----------------------------------------------------
std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = utf8_next(s, i);  // invalid bytes become Latin-1: always valid UTF-8 out
        if (cp == '"') o += "\\\"";
        else if (cp == '\\') o += "\\\\";
        else if (cp < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", cp);
            o += b;
        } else utf8_put(o, cp);
    }
    return o + "\"";
}

// Flat object of string/number/bool values -> map. Enough for client messages.
std::map<std::string, std::string> json_flat(const std::string& s) {
    std::map<std::string, std::string> m;
    size_t i = 0;
    auto ws = [&]() { while (i < s.size() && isspace((unsigned char)s[i])) i++; };
    auto str = [&](std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        while (i < s.size() && s[i] != '"') {
            char ch = s[i++];
            if (ch != '\\') { out += ch; continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            if (e == 'n') out += '\n';
            else if (e == 't') out += '\t';
            else if (e == 'r') out += '\r';
            else if (e == 'b' || e == 'f') {}
            else if (e == 'u') {
                if (i + 4 > s.size()) return false;
                uint32_t cp = (uint32_t)strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                i += 4;
                if (cp >= 0xd800 && cp < 0xdc00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    uint32_t lo = (uint32_t)strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
                    if (lo >= 0xdc00 && lo < 0xe000) cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00), i += 6;
                }
                utf8_put(out, cp);
            } else out += e;
        }
        return i++ < s.size();
    };
    ws();
    if (i >= s.size() || s[i++] != '{') return m;
    for (;;) {
        ws();
        std::string k, v;
        if (!str(k)) return m;
        ws();
        if (i >= s.size() || s[i++] != ':') return m;
        ws();
        if (i < s.size() && s[i] == '"') { if (!str(v)) return m; }
        else while (i < s.size() && s[i] != ',' && s[i] != '}' && !isspace((unsigned char)s[i])) v += s[i++];
        m[k] = v;
        ws();
        if (i >= s.size() || s[i] != ',') break;
        i++;
    }
    return m;
}

// ---- WebSocket framing -----------------------------------------------------
std::string ws_frame(const std::string& p, int op) {
    std::string f(1, (char)(0x80 | op));
    size_t n = p.size();
    if (n < 126) f += (char)n;
    else if (n < 65536) f += (char)126, f += (char)(n >> 8), f += (char)(n & 255);
    else {
        f += (char)127;
        for (int k = 7; k >= 0; k--) f += (char)((uint64_t)n >> (k * 8) & 255);
    }
    return f + p;
}

long ws_parse(const std::string& b, int& op, std::string& payload, bool* fin) {
    if (b.size() < 2) return 0;
    unsigned char b0 = (unsigned char)b[0], b1 = (unsigned char)b[1];
    if (!(b1 & 0x80)) return -1;                     // clients must mask
    if (!(b0 & 0x80) && (!fin || (b0 & 0x08))) return -1;  // control frames are never split
    if (fin) *fin = b0 & 0x80;
    op = b0 & 0x0f;
    uint64_t len = b1 & 0x7f;
    size_t pos = 2;
    if (len == 126) {
        if (b.size() < 4) return 0;
        len = (uint64_t)(unsigned char)b[2] << 8 | (unsigned char)b[3];
        pos = 4;
    } else if (len == 127) {
        if (b.size() < 10) return 0;
        len = 0;
        for (int k = 0; k < 8; k++) len = len << 8 | (unsigned char)b[2 + k];
        pos = 10;
    }
    if (len > (1 << 20)) return -1;
    if (b.size() < pos + 4 + len) return 0;
    const char* mask = &b[pos];
    payload.assign(b, pos + 4, (size_t)len);
    for (size_t k = 0; k < payload.size(); k++) payload[k] ^= mask[k % 4];
    return (long)(pos + 4 + len);
}

// ---- self-signed certificate -----------------------------------------------
static bool gen_selfsigned(const std::string& crt, const std::string& key, std::string& err) {
    EVP_PKEY* pk = nullptr;
    EVP_PKEY_CTX* kc = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    bool ok = kc && EVP_PKEY_keygen_init(kc) > 0 &&
              EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kc, NID_X9_62_prime256v1) > 0 &&
              EVP_PKEY_keygen(kc, &pk) > 0;
    EVP_PKEY_CTX_free(kc);
    X509* x = ok ? X509_new() : nullptr;
    if (x) {
        X509_set_version(x, 2);
        unsigned char serial[8];
        RAND_bytes(serial, sizeof serial);
        serial[0] &= 0x7f;
        BIGNUM* bn = BN_bin2bn(serial, sizeof serial, nullptr);
        BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(x));
        BN_free(bn);
        X509_gmtime_adj(X509_getm_notBefore(x), 0);
        X509_gmtime_adj(X509_getm_notAfter(x), 3650L * 24 * 3600);
        X509_set_pubkey(x, pk);
        X509_NAME* name = X509_NAME_new();  // own copy: OpenSSL 4 made the getter const
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)"isekur", -1, -1, 0);
        X509_set_subject_name(x, name);
        X509_set_issuer_name(x, name);
        X509_NAME_free(name);
        ok = X509_sign(x, pk, EVP_sha256()) > 0;
    }
    if (ok) {
        mode_t old = umask(077);  // private key must not be world-readable
        FILE* kf = fopen(key.c_str(), "w");
        umask(old);
        FILE* cf = fopen(crt.c_str(), "w");
        ok = kf && cf && PEM_write_PrivateKey(kf, pk, nullptr, nullptr, 0, nullptr, nullptr) &&
             PEM_write_X509(cf, x);
        if (kf) fclose(kf);
        if (cf) fclose(cf);
    }
    if (!ok) err = "could not create self-signed certificate: " + ssl_errstr();
    X509_free(x);
    EVP_PKEY_free(pk);
    return ok;
}

// ---- server ----------------------------------------------------------------
Web::~Web() { stop(); }

// Drops every browser and the listener; logins stay valid (web.sessions).
void Web::stop() {
    clients.clear();
    if (ctx) SSL_CTX_free(ctx), ctx = nullptr;
    if (lfd >= 0) close(lfd), lfd = -1;
    secure = false;
    pending.clear(), known.clear(), last_order.clear();
    url.clear();
}

// [web] autostart (or the older "enabled") decides whether main() calls this at launch.
bool Web::autostart(const Ini& cfg) {
    std::string v = ini_get(cfg, "web", "autostart", ini_get(cfg, "web", "enabled", "off"));
    return v == "on" || v == "true" || v == "yes" || v == "1";
}

bool Web::start(std::string& err) {
    if (running()) return true;
    if (!start_(err)) { stop(); return false; }
    return true;
}

bool Web::start_(std::string& err) {
    const Ini& cfg = c.cfg;
    std::string bind = ini_get(cfg, "web", "bind", "127.0.0.1");
    int port = atoi(ini_get(cfg, "web", "port", "8443").c_str());
    user = ini_get(cfg, "web", "user");
    pass_hash = ini_get(cfg, "web", "password_hash");
    if (user.empty() || pass_hash.rfind("pbkdf2$", 0) != 0) {
        err = "[web] needs user and password_hash (generate one with: isekur --hash-password)";
        return false;
    }
    bool loopback = bind == "127.0.0.1" || bind == "::1" || bind == "localhost";
    std::string crt = ini_get(cfg, "web", "cert"), key = ini_get(cfg, "web", "key");
    if (crt.empty() && (!loopback || ini_get(cfg, "web", "tls", "auto") == "true")) {
        std::string dir = c.cfg_path.substr(0, c.cfg_path.rfind('/') + 1);
        crt = dir + "web.crt", key = dir + "web.key";
        if (access(crt.c_str(), R_OK) || access(key.c_str(), R_OK)) {
            if (!gen_selfsigned(crt, key, err)) return false;
            c.print(c.bufs.front().get(), LK::INFO, "Generated self-signed web certificate " + crt);
        }
    }
    if (!crt.empty()) {
        ctx = SSL_CTX_new(TLS_server_method());
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        if (SSL_CTX_use_certificate_chain_file(ctx, crt.c_str()) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx, (key.empty() ? crt : key).c_str(), SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(ctx) != 1) {
            err = "web TLS: " + ssl_errstr();
            return false;
        }
        secure = true;
    }
    sessions_path = c.cfg_path.substr(0, c.cfg_path.rfind('/') + 1) + "web.sessions";
    load_sessions();
    lfd = listen_tcp(bind, port, err);
    if (lfd < 0) { err = "web listen " + bind + ":" + std::to_string(port) + ": " + err; return false; }
    url = std::string(secure ? "https://" : "http://") + (bind.find(':') != std::string::npos ? "[" + bind + "]" : bind) +
          ":" + std::to_string(port) + "/";
    return true;
}

void Web::add_fds(std::vector<pollfd>& v) {
    fd_start = v.size();
    fd_count = 0;
    if (lfd < 0) return;
    v.push_back({lfd, POLLIN, 0});
    for (auto& cl : clients) v.push_back({cl->conn.fd, cl->conn.events(), 0});
    fd_count = 1 + clients.size();
}

void Web::process(const std::vector<pollfd>& v) {
    if (lfd < 0) return;
    time_t now = time(nullptr);
    std::vector<Client*> dead;
    for (size_t k = 1; k < fd_count; k++) {
        const pollfd& p = v[fd_start + k];
        Client& cl = *clients[k - 1];
        if (!p.revents) continue;
        bool ok = cl.conn.pump(p.revents, cl.in);
        if (ok && cl.conn.st == Conn::OPEN && !cl.conn.closing) {
            if (cl.ws) ws_frames(cl);
            else http(cl);
        }
        if (!ok) dead.push_back(&cl);
    }
    if (v[fd_start].revents & POLLIN) {
        for (;;) {
            auto cl = std::make_unique<Client>();
            if (!cl->conn.accept_from(lfd, ctx)) break;
            cl->since = now;
            if (clients.size() >= MAX_CLIENTS) continue;  // dropped: destructor closes it
            clients.push_back(std::move(cl));
        }
    }
    for (auto& cl : clients)  // idle HTTP (non-WebSocket) connections
        if (!cl->ws && now - cl->since > 30) dead.push_back(cl.get());
    clients.erase(std::remove_if(clients.begin(), clients.end(),
                                 [&](auto& cl) {
                                     return std::find(dead.begin(), dead.end(), cl.get()) != dead.end();
                                 }),
                  clients.end());
}

static void respond(Conn& conn, int code, const std::string& status, const std::string& ctype,
                    const std::string& body, const std::string& extra = "") {
    conn.out += "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\n" +
                "Content-Type: " + ctype + "\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                "Referrer-Policy: no-referrer\r\nX-Frame-Options: DENY\r\n"
                "Content-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline'; "
                "style-src 'self' 'unsafe-inline'; img-src 'self' data:; frame-ancestors 'none'\r\n" +
                extra + "Connection: close\r\n\r\n" + body;
    conn.closing = true;
}

void Web::http(Client& cl) {
    size_t he = cl.in.find("\r\n\r\n");
    if (he == std::string::npos) {
        if (cl.in.size() > 16384) cl.conn.closing = true, cl.conn.out.clear();
        return;
    }
    std::map<std::string, std::string> h;
    auto lines = split(cl.in.substr(0, he), '\n');
    if (lines.empty()) return;
    auto rl = split(lines[0], ' ');
    for (size_t i = 1; i < lines.size(); i++) {
        size_t col = lines[i].find(':');
        if (col == std::string::npos) continue;
        std::string k = lines[i].substr(0, col), val = lines[i].substr(col + 1);
        for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
        val.erase(0, val.find_first_not_of(' '));
        while (!val.empty() && (val.back() == '\r' || val.back() == ' ')) val.pop_back();
        h[k] = val;
    }
    size_t clen = (size_t)atol(h["content-length"].c_str());
    if (rl.size() < 2 || clen > 4096) { respond(cl.conn, 400, "Bad Request", "text/plain", "bad request"); return; }
    if (cl.in.size() < he + 4 + clen) return;
    std::string body = cl.in.substr(he + 4, clen);
    cl.in.erase(0, he + 4 + clen);
    std::string method = rl[0], path = rl[1].substr(0, rl[1].find('?'));

    time_t now = time(nullptr);
    std::string token;
    for (auto& ck : split(h["cookie"], ';')) {
        std::string t = ck.substr(ck.find_first_not_of(' '));
        if (t.rfind("isekur_sid=", 0) == 0) token = t.substr(11);
    }
    auto sit = sessions.find(sha256_hex(token));
    bool authed = !token.empty() && sit != sessions.end() && sit->second > now;
    // browsers send Origin on WebSocket and POST: it must be us (blocks CSRF / CSWSH)
    std::string origin = h["origin"];
    bool same_origin = origin.empty() || origin == (secure ? "https://" : "http://") + h["host"];

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        respond(cl.conn, 200, "OK", "text/html; charset=utf-8",
                std::string(index_html, sizeof index_html - 1));
    } else if (method == "GET" && path == "/session") {
        if (authed) respond(cl.conn, 200, "OK", "application/json", "{\"user\":" + json_str(user) + "}");
        else respond(cl.conn, 401, "Unauthorized", "application/json", "{}");
    } else if (method == "POST" && path == "/login") {
        if (!same_origin) { respond(cl.conn, 403, "Forbidden", "text/plain", "bad origin"); return; }
        auto& f = fails[cl.conn.host];
        if (f.second > now) {
            respond(cl.conn, 429, "Too Many Requests", "application/json",
                    "{\"wait\":" + std::to_string(f.second - now) + "}");
            return;
        }
        auto form = json_flat(body);
        std::string u = form["user"];
        // compare both fields every time so timing doesn't reveal which was wrong
        bool user_ok = u.size() == user.size() && CRYPTO_memcmp(u.data(), user.data(), u.size()) == 0;
        bool pass_ok = check_password(form["pass"], pass_hash);
        if (user_ok && pass_ok) {
            fails.erase(cl.conn.host);
            token = random_hex(32);
            sessions[sha256_hex(token)] = now + SESSION_TTL;
            save_sessions();
            c.print(c.bufs.front().get(), LK::INFO, "Webchat login from " + cl.conn.host);
            respond(cl.conn, 200, "OK", "application/json", "{}",
                    "Set-Cookie: isekur_sid=" + token + "; Path=/; HttpOnly; SameSite=Strict" +
                        (secure ? "; Secure" : "") + "; Max-Age=" + std::to_string(SESSION_TTL) + "\r\n");
        } else {
            if (++f.first >= 5) f.second = now + std::min<time_t>(3600, 60L << std::min(f.first - 5, 6));
            c.print(c.bufs.front().get(), LK::ERROR, "Failed webchat login from " + cl.conn.host);
            respond(cl.conn, 401, "Unauthorized", "application/json", "{}");
        }
        if (fails.size() > 1000)  // forget old unlocked offenders
            for (auto it = fails.begin(); it != fails.end();)
                it = it->second.second < now ? fails.erase(it) : std::next(it);
    } else if (method == "POST" && path == "/logout") {
        if (same_origin && sessions.erase(sha256_hex(token))) save_sessions();
        respond(cl.conn, 200, "OK", "application/json", "{}",
                "Set-Cookie: isekur_sid=; Path=/; Max-Age=0\r\n");
    } else if (method == "GET" && path == "/ws") {
        if (!authed || !same_origin) { respond(cl.conn, 403, "Forbidden", "text/plain", "forbidden"); return; }
        std::string key = h["sec-websocket-key"];
        if (key.empty()) { respond(cl.conn, 400, "Bad Request", "text/plain", "not a websocket"); return; }
        unsigned char sha[20];
        std::string k2 = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        SHA1((const unsigned char*)k2.data(), k2.size(), sha);
        cl.conn.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: " + b64(std::string((char*)sha, 20)) + "\r\n\r\n";
        cl.ws = true;
        snapshot(cl);
        ws_frames(cl);
    } else {
        respond(cl.conn, 404, "Not Found", "text/plain", "not found");
    }
}

static Buffer* by_id(Core& c, int id) {
    for (auto& b : c.bufs)
        if (b->id == id) return b.get();
    return nullptr;
}

void Web::ws_frames(Client& cl) {
    for (;;) {
        int op;
        bool fin;
        std::string p;
        long n = ws_parse(cl.in, op, p, &fin);
        auto bad = [&] { cl.conn.out += ws_frame("", 8); cl.conn.closing = true; };
        if (n == 0) return;
        if (n < 0) return bad();
        cl.in.erase(0, (size_t)n);
        if (op == 8) return bad();
        if (op == 9) { cl.conn.out += ws_frame(p, 10); continue; }
        if (op < 8) {  // data: a message may arrive as a frame plus continuations (op 0)
            if ((op == 0) != (cl.frag_op != 0)) return bad();
            if (op) cl.frag_op = op;
            if ((cl.frag += p).size() > (1 << 20)) return bad();
            if (!fin) continue;
            op = cl.frag_op, cl.frag_op = 0, p.swap(cl.frag), cl.frag.clear();
        }
        if (op != 1) continue;
        auto m = json_flat(p);
        Buffer* b = by_id(c, atoi(m["buf"].c_str()));
        if (!b) continue;
        if (m["t"] == "input") c.exec(b, m["text"]);
        else if (m["t"] == "read" && b->activity) { b->activity = 0; c.dirty = true; c.notify(Ev::BUFFER, b); }
        else if (m["t"] == "seen" && b->unread && b != c.active) { b->unread = 0; c.notify(Ev::BUFFER, b); }
        if (cl.conn.closing) return;
    }
}

// ---- events -> JSON ----------------------------------------------------------
static const char* KIND[] = {"msg", "action", "notice", "join", "part", "quit",
                             "nick", "mode", "topic", "info", "error"};

static std::string line_json(const Line& l) {
    return "{\"ts\":" + std::to_string((long long)l.ts) + ",\"k\":\"" + KIND[(int)l.kind] +
           "\",\"n\":" + json_str(l.nick) + ",\"x\":" + json_str(l.text) +
           ",\"s\":" + (l.self ? "1" : "0") + ",\"h\":" + (l.hilight ? "1" : "0") + "}";
}

std::string Web::buf_json(Buffer* b) {
    static const char* TYPE[] = {"status", "channel", "query"};
    static const char* STATE[] = {"disconnected", "connecting", "registering", "connected"};
    Server* s = b->srv;
    std::string srv = s ? s->cfg.name : "isekur";  // same label as the console tabs
    // ponytail: whole nicklist per update (coalesced in flush()); diff it if huge channels lag
    std::string nicks = "[";
    for (auto& kv : b->nicks) {
        if (nicks.size() > 1) nicks += ",";
        nicks += "[" + json_str(kv.second.prefix) + "," + json_str(kv.second.nick) + "," +
                 (kv.second.away ? "1" : "0") + "]";
    }
    nicks += "]";
    return "{\"t\":\"buf\",\"id\":" + std::to_string(b->id) + ",\"name\":" + json_str(b->name) +
           ",\"type\":\"" + TYPE[(int)b->type] + "\",\"srv\":" + json_str(srv) +
           ",\"topic\":" + json_str(b->topic) + ",\"modes\":" + json_str(b->modes) +
           ",\"act\":" + std::to_string(b->activity) + ",\"unread\":" + std::to_string(b->unread) + ",\"joined\":" + (b->joined ? "1" : "0") +
           ",\"me\":" + json_str(s ? s->nick : "") +
           ",\"state\":\"" + (s ? STATE[(int)s->state] : "connected") + "\"" +
           ",\"prefixes\":" + json_str(s ? s->prefix_chars : "") + ",\"nicks\":" + nicks + "}";
}

std::string Web::order_json() {
    std::string o = "{\"t\":\"order\",\"ids\":[";
    for (size_t i = 0; i < c.bufs.size(); i++) o += (i ? "," : "") + std::to_string(c.bufs[i]->id);
    return o + "]}";
}

std::string Web::theme_json() {
    std::string o = "{\"t\":\"theme\",\"name\":" + json_str(c.theme.name) + ",\"el\":{";
    bool first = true;
    for (auto& [k, st] : c.theme.el) {
        o += (first ? "" : ",") + json_str(k) + ":{\"fg\":" + json_str(rgb_hex(st.fg)) +
             ",\"bg\":" + json_str(rgb_hex(st.bg)) + ",\"a\":" + std::to_string(st.attr) + "}";
        first = false;
    }
    o += "},\"nicks\":[";
    for (size_t i = 0; i < c.theme.nicks.size(); i++) o += (i ? "," : "") + json_str(rgb_hex(c.theme.nicks[i]));
    return o + "]}";
}

std::string Web::lines_json(Buffer* b) {
    std::string ls = "{\"t\":\"lines\",\"buf\":" + std::to_string(b->id) + ",\"lines\":[";
    size_t from = b->lines.size() > BACKLOG ? b->lines.size() - BACKLOG : 0;
    for (size_t i = from; i < b->lines.size(); i++) ls += (i > from ? "," : "") + line_json(b->lines[i]);
    return ls + "]}";
}

void Web::snapshot(Client& cl) {
    auto send = [&](const std::string& j) { cl.conn.out += ws_frame(j); };
    send(theme_json());
    for (auto& b : c.bufs) {
        send(buf_json(b.get()));
        send(lines_json(b.get()));
        known.insert(b->id);
    }
    send(order_json());
    std::string cm = "{\"t\":\"cmds\",\"list\":[";  // for Tab completion
    for (auto& k : commands()) cm += std::string(cm.back() == '[' ? "" : ",") + json_str(k.name);
    send(cm + "]}");
    if (c.active) send("{\"t\":\"active\",\"buf\":" + std::to_string(c.active->id) + "}");
}

bool Web::attached() const {
    for (auto& cl : clients)
        if (cl->ws && !cl->conn.closing) return true;
    return false;
}

void Web::broadcast(const std::string& json) {
    std::string f = ws_frame(json);
    for (auto& cl : clients) {
        if (!cl->ws || cl->conn.closing) continue;
        if (cl->conn.out.size() > MAX_OUT) { cl->conn.closing = true; continue; }  // too slow
        cl->conn.out += f;
    }
}

void Web::flush_buf(Buffer* b) {
    pending.erase(b->id);
    broadcast(buf_json(b));
    if (known.insert(b->id).second) broadcast(lines_json(b));  // new tab: send its history
}

// Buffer updates (nicklists!) are coalesced to one per buffer per loop pass,
// so a netsplit is one message per channel instead of one per quit.
void Web::flush() {
    if (lfd < 0 || clients.empty()) { pending.clear(); return; }
    for (auto& b : c.bufs)
        if (pending.count(b->id)) flush_buf(b.get());
    pending.clear();
    std::string o = order_json();
    if (o != last_order) broadcast(o), last_order = o;
}

void Web::on_event(Ev e, Buffer* b, const Line* l) {
    if (lfd < 0 || clients.empty()) return;
    switch (e) {
    case Ev::LINE: {
        bool fresh = !known.count(b->id);  // its history (incl. this line) goes out now
        if (fresh || pending.count(b->id)) flush_buf(b);
        if (!fresh) broadcast("{\"t\":\"line\",\"buf\":" + std::to_string(b->id) + ",\"u\":" +
                             std::to_string(b->unread) + ",\"line\":" + line_json(*l) + "}");
        break;
    }
    case Ev::BUFFER: pending.insert(b->id); break;
    case Ev::CLOSE: {
        pending.erase(b->id), known.erase(b->id);
        broadcast("{\"t\":\"close\",\"buf\":" + std::to_string(b->id) + "}");
        std::string o = "{\"t\":\"order\",\"ids\":[";
        bool first = true;
        for (auto& x : c.bufs)
            if (x.get() != b) o += (first ? "" : ",") + std::to_string(x->id), first = false;
        broadcast(last_order = o + "]}");
        break;
    }
    case Ev::THEME: broadcast(theme_json()); break;
    }
}
