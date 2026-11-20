// /e2e: end-to-end encrypted queries between two iSekur users who share a
// password. key = scrypt(password, salt); messages are AES-256-GCM, so a wrong
// password fails the tag check instead of showing garbage.
//
//   A: /e2e B      -> password prompt, CTCP ISEKUR-E2E <salt>
//   B: /e2e A      -> password prompt, CTCP ISEKUR-E2E-OK <seal("ok")>
//   A: opens it    -> CTCP ISEKUR-E2E-ACK <seal("ack")>, or ISEKUR-E2E-BAD
//   messages: PRIVMSG B :+iSE2 <b64(nonce | ciphertext | tag)>
//
// The GCM AAD says who sent the line. A query has one key for both ways, so without it the
// server could send you your own line back as your peer's: the AAD is the sender's role
// ("i" invited, "r" answered). A channel's is the channel and the sender's nick, so a line
// copied to another channel, or moved under another nick by the server, doesn't open.
//
// /e2e #chan: no handshake, everyone who types the same password has the same key
// (salt "isekur-chan:" + channel). People without it see "+iSE2 ..." lines. Anyone with
// the password can still write as anyone: one shared key can't tell its holders apart.
//
// "+iSE " lines (before +iSE2) still open, with their old AAD: the channel name, or none.
// ponytail: drop that on receive in a release or two. An old iSekur can't read +iSE2 lines.
//
// ponytail: password only, no forward secrecy; an X25519 exchange
// authenticated by the password is the upgrade if that matters. No replay check in
// channels: chathistory replays old lines on purpose; a timestamp inside would be the fix.
#include <cstdlib>
#include <cstring>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <vector>
#include "core.h"

static const char* OK_TOKEN = "isekur-e2e-ok";
static const char* ACK_TOKEN = "isekur-e2e-ack";  // differs from OK: a reflected OK is no ACK
static const char* OFF_TOKEN = "isekur-e2e-off";  // sealed with the chat's key: only its holders can end it

// scrypt, RFC 7914. LibreSSL (OpenBSD) has PBKDF2 but no EVP_PBE_scrypt, so it uses this one; OpenSSL builds keep
// theirs. Same output either way, so a key made on one system opens on the other. The tests check it against the
// RFC's vectors, and against OpenSSL's own where there is one. ponytail: plain and unoptimised, N = 2^15 takes
// about a tenth of a second; N must be a power of two.
static void salsa8(uint32_t B[16]) {
    uint32_t x[16];
    memcpy(x, B, sizeof x);
    auto R = [](uint32_t a, int b) { return (a << b) | (a >> (32 - b)); };
    for (int i = 0; i < 8; i += 2) {
        x[4] ^= R(x[0] + x[12], 7);    x[8] ^= R(x[4] + x[0], 9);
        x[12] ^= R(x[8] + x[4], 13);   x[0] ^= R(x[12] + x[8], 18);
        x[9] ^= R(x[5] + x[1], 7);     x[13] ^= R(x[9] + x[5], 9);
        x[1] ^= R(x[13] + x[9], 13);   x[5] ^= R(x[1] + x[13], 18);
        x[14] ^= R(x[10] + x[6], 7);   x[2] ^= R(x[14] + x[10], 9);
        x[6] ^= R(x[2] + x[14], 13);   x[10] ^= R(x[6] + x[2], 18);
        x[3] ^= R(x[15] + x[11], 7);   x[7] ^= R(x[3] + x[15], 9);
        x[11] ^= R(x[7] + x[3], 13);   x[15] ^= R(x[11] + x[7], 18);
        x[1] ^= R(x[0] + x[3], 7);     x[2] ^= R(x[1] + x[0], 9);
        x[3] ^= R(x[2] + x[1], 13);    x[0] ^= R(x[3] + x[2], 18);
        x[6] ^= R(x[5] + x[4], 7);     x[7] ^= R(x[6] + x[5], 9);
        x[4] ^= R(x[7] + x[6], 13);    x[5] ^= R(x[4] + x[7], 18);
        x[11] ^= R(x[10] + x[9], 7);   x[8] ^= R(x[11] + x[10], 9);
        x[9] ^= R(x[8] + x[11], 13);   x[10] ^= R(x[9] + x[8], 18);
        x[12] ^= R(x[15] + x[14], 7);  x[13] ^= R(x[12] + x[15], 9);
        x[14] ^= R(x[13] + x[12], 13); x[15] ^= R(x[14] + x[13], 18);
    }
    for (int i = 0; i < 16; i++) B[i] += x[i];
}

// B: 2r blocks of 16 words, mixed in place; Y is scratch of the same size
static void scrypt_blockmix(uint32_t* B, uint32_t* Y, size_t r) {
    uint32_t X[16];
    memcpy(X, B + (2 * r - 1) * 16, sizeof X);
    for (size_t i = 0; i < 2 * r; i++) {
        for (size_t k = 0; k < 16; k++) X[k] ^= B[i * 16 + k];
        salsa8(X);
        memcpy(Y + i * 16, X, sizeof X);
    }
    for (size_t i = 0; i < r; i++) {  // even blocks first, then the odd ones
        memcpy(B + i * 16, Y + 2 * i * 16, 64);
        memcpy(B + (i + r) * 16, Y + (2 * i + 1) * 16, 64);
    }
}

bool scrypt_rfc7914(const std::string& pass, const std::string& salt, uint32_t N, uint32_t r, uint32_t p, unsigned char* out, size_t outlen) {
    if (N < 2 || (N & (N - 1)) || !r || !p || r > 1024 || p > 1024 || N > (1u << 20)) return false;  // the RFC's N is a power of two
    const size_t w = 32 * (size_t)r;  // words in a block of 2r*64 bytes
    std::vector<unsigned char> B(p * w * 4);
    if (PKCS5_PBKDF2_HMAC(pass.data(), (int)pass.size(), (const unsigned char*)salt.data(), (int)salt.size(), 1, EVP_sha256(), (int)B.size(), B.data()) != 1)
        return false;
    std::vector<uint32_t> X(w), Y(w), V((size_t)N * w);
    for (uint32_t i = 0; i < p; i++) {
        unsigned char* b = &B[i * w * 4];
        for (size_t k = 0; k < w; k++)  // little-endian words whatever the machine is
            X[k] = (uint32_t)b[4 * k] | (uint32_t)b[4 * k + 1] << 8 | (uint32_t)b[4 * k + 2] << 16 | (uint32_t)b[4 * k + 3] << 24;
        for (uint32_t n = 0; n < N; n++) {
            memcpy(&V[(size_t)n * w], X.data(), w * 4);
            scrypt_blockmix(X.data(), Y.data(), r);
        }
        for (uint32_t n = 0; n < N; n++) {
            size_t j = X[(2 * r - 1) * 16] & (N - 1);  // Integerify: the low bits of the last block's first word
            for (size_t k = 0; k < w; k++) X[k] ^= V[j * w + k];
            scrypt_blockmix(X.data(), Y.data(), r);
        }
        for (size_t k = 0; k < w; k++)
            for (int s = 0; s < 4; s++) b[4 * k + s] = (unsigned char)(X[k] >> (8 * s));
    }
    bool ok = PKCS5_PBKDF2_HMAC(pass.data(), (int)pass.size(), B.data(), (int)B.size(), 1, EVP_sha256(), (int)outlen, out) == 1;
    OPENSSL_cleanse(B.data(), B.size()), OPENSSL_cleanse(X.data(), X.size() * 4), OPENSSL_cleanse(Y.data(), Y.size() * 4), OPENSSL_cleanse(V.data(), V.size() * 4);
    return ok;
}

std::string e2e_key(const std::string& pass, const std::string& salt) {
    unsigned char k[32];
#ifdef LIBRESSL_VERSION_NUMBER
    if (!scrypt_rfc7914(pass, salt, 1 << 15, 8, 1, k, sizeof k)) return "";
#else
    if (EVP_PBE_scrypt(pass.data(), pass.size(), (const unsigned char*)salt.data(), salt.size(),
                       1 << 15, 8, 1, 64 << 20, k, sizeof k) != 1)
        return "";
#endif
    return std::string((char*)k, sizeof k);
}

// ponytail: a failed RNG is fatal: an unchecked nonce or salt would be whatever the stack held
static void rand_bytes(unsigned char* p, int n) {
    if (RAND_bytes(p, n) != 1) abort();
}

std::string e2e_seal(const std::string& key, const std::string& plain, const std::string& aad) {
    unsigned char nonce[12], tag[16];
    rand_bytes(nonce, sizeof nonce);
    std::string ct(plain.size(), '\0');
    int n = 0;
    EVP_CIPHER_CTX* x = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(x, EVP_aes_256_gcm(), nullptr, (const unsigned char*)key.data(), nonce);
    if (!aad.empty()) EVP_EncryptUpdate(x, nullptr, &n, (const unsigned char*)aad.data(), (int)aad.size());
    EVP_EncryptUpdate(x, (unsigned char*)&ct[0], &n, (const unsigned char*)plain.data(), (int)plain.size());
    EVP_EncryptFinal_ex(x, (unsigned char*)&ct[0] + n, &n);
    EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_GCM_GET_TAG, sizeof tag, tag);
    EVP_CIPHER_CTX_free(x);
    return b64(std::string((char*)nonce, 12) + ct + std::string((char*)tag, 16));
}

bool e2e_open(const std::string& key, const std::string& sealed, std::string& plain, const std::string& aad) {
    std::string raw = unb64(sealed);
    if (key.size() != 32 || raw.size() < 28) return false;
    std::string ct = raw.substr(12, raw.size() - 28), tag = raw.substr(raw.size() - 16);
    plain.assign(ct.size(), '\0');
    int n = 0;
    EVP_CIPHER_CTX* x = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(x, EVP_aes_256_gcm(), nullptr, (const unsigned char*)key.data(),
                       (const unsigned char*)raw.data());
    bool ok = (aad.empty() || EVP_DecryptUpdate(x, nullptr, &n, (const unsigned char*)aad.data(), (int)aad.size()) == 1) &&
              EVP_DecryptUpdate(x, (unsigned char*)&plain[0], &n, (const unsigned char*)ct.data(), (int)ct.size()) == 1 &&
              EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_GCM_SET_TAG, 16, &tag[0]) == 1 &&
              EVP_DecryptFinal_ex(x, (unsigned char*)&plain[0] + n, &n) == 1;
    EVP_CIPHER_CTX_free(x);
    if (!ok) plain.clear();
    return ok;
}

std::string e2e_aad_q(bool from_initiator) { return from_initiator ? "i" : "r"; }

std::string e2e_aad_c(const std::string& chan, const std::string& sender) { return irc_lower(chan) + " " + irc_lower(sender); }

bool e2e_line(const std::string& text) { return text.rfind("+iSE2 ", 0) == 0 || text.rfind("+iSE ", 0) == 0; }

std::string e2e_line_seal(const std::string& key, const std::string& plain, const std::string& aad) {
    return "+iSE2 " + e2e_seal(key, plain, aad);
}

bool e2e_line_open(const std::string& key, const std::string& text, std::string& plain, const std::string& aad,
                   const std::string& old_aad) {
    bool v2 = text.rfind("+iSE2 ", 0) == 0;
    return e2e_open(key, text.substr(v2 ? 6 : 5), plain, v2 ? aad : old_aad);
}

bool e2e_on(const Buffer* b) {
    Server* s = b->srv;
    if (!s) return false;
    if (b->type == BT::CHANNEL) return s->chan_e2e.count(irc_lower(b->name)) > 0;
    auto e = b->type == BT::QUERY ? s->e2e.find(irc_lower(b->name)) : s->e2e.end();
    return e != s->e2e.end() && e->second.ok;
}

static void ctcp(Server* s, const std::string& nick, const std::string& body) {
    s->send("PRIVMSG " + nick + " :\x01" + body + "\x01");
}

// a notice in the query tab if there is one, and in the server tab
void Core::e2e_note(Server* s, const std::string& nick, LK k, const std::string& text) {
    Buffer* q = find(s, nick);
    if (q) print(q, k, text), notify(Ev::BUFFER, q);  // the tab's 🔒
    if (!q || active != q) print(s->status, k, text);
}

// /e2e nick | /e2e #chan | /e2e off [nick|#chan] | /e2e
void Core::e2e_cmd(Buffer* b, const std::string& args) {
    Server* s = b->srv;
    auto argv = split(args, ' ');
    if (!s) { print(b, LK::ERROR, "No server here"); return; }
    if (argv.empty()) {
        if (s->e2e.empty() && s->chan_e2e.empty()) print(b, LK::INFO, "No encrypted chats. Start one with /e2e nick or /e2e #channel");
        for (auto& [k, e] : s->e2e)
            print(b, LK::INFO, e.nick + ": " + (e.ok ? "encrypted" : e.key.empty() ? "invited you (/e2e " + e.nick + ")" : "waiting for the other side") +
                                   (e.pend.empty() ? "" : "; a new invitation is waiting (/e2e " + e.nick + " takes it)"));
        for (auto& [k, key] : s->chan_e2e) print(b, LK::INFO, k + ": encrypted");
        return;
    }
    if (argv[0] == "off") {
        std::string nick = argv.size() > 1 ? argv[1] : b->type != BT::STATUS ? b->name : "";
        if (s->is_chan(nick)) {
            if (!s->chan_e2e.erase(irc_lower(nick))) { print(b, LK::ERROR, "No encryption in " + nick); return; }
            e2e_keep(s, true);
            Buffer* c = find(s, nick);
            print(c ? c : b, LK::INFO, "Encryption in " + nick + " turned off; you send plain text again");
            if (c) notify(Ev::BUFFER, c);
            dirty = true;
            return;
        }
        if (s->state != SS::CONNECTED) { print(b, LK::ERROR, "Not connected"); return; }
        auto eo = nick.empty() ? s->e2e.end() : s->e2e.find(irc_lower(nick));
        if (eo == s->e2e.end()) { print(b, LK::ERROR, "No encrypted chat with " + (nick.empty() ? "anyone here" : nick)); return; }
        ctcp(s, nick, "ISEKUR-E2E-OFF" + (eo->second.key.empty() ? "" : " " + e2e_seal(eo->second.key, OFF_TOKEN)));
        s->e2e.erase(eo);
        e2e_note(s, nick, LK::INFO, "Encryption with " + nick + " turned off");
        return;
    }
    std::string nick = argv[0];
    if (s->is_chan(nick)) {
        Buffer* c = find(s, nick);
        if (!c) { print(b, LK::ERROR, "Join " + nick + " first"); return; }
        select(c);
        std::string pass = after_words(args, 1);
        if (pass.empty()) ask_pass(c, "password for " + c->name, [this, c](const std::string& p) { e2e_chan(c, p); });
        else e2e_chan(c, pass);
        return;
    }
    if (s->state != SS::CONNECTED) { print(b, LK::ERROR, "Not connected"); return; }
    if (irc_lower(nick) == irc_lower(s->nick)) { print(b, LK::ERROR, "/e2e takes a nick or a channel"); return; }
    Buffer* q = open(s, BT::QUERY, nick);
    select(q);
    std::string pass = after_words(args, 1);
    if (pass.empty()) {
        ask_pass(q, "password for " + nick, [this, q](const std::string& p) { e2e_pass(q, p); });
        return;
    }
    e2e_pass(q, pass);
}

// the channel password arrived: nothing to check, a wrong one just won't open the others' lines
void Core::e2e_chan(Buffer* c, const std::string& pass) {
    std::string k = e2e_key(pass, "isekur-chan:" + irc_lower(c->name));
    if (k.empty()) { print(c, LK::ERROR, "Couldn't make the key"); return; }
    c->srv->chan_e2e[irc_lower(c->name)] = k;
    e2e_keep(c->srv, true);
    bool kept = ("," + ini_get(cfg, "server." + c->srv->cfg.name, "e2e_keys")).find("," + irc_lower(c->name) + ":") != std::string::npos;
    print(c, LK::INFO, "\xf0\x9f\x94\x92 Encryption in " + c->name + " is on: only people with the same password can read "
                       "what you send. " + (kept ? "Kept in the config for autojoin. " : "") + "/e2e off turns it off");
    notify(Ev::BUFFER, c);
    dirty = true;
}

// e2e_keys in [server.x]: the keys of the autojoin channels, so a restart doesn't ask again.
// The scrypt key is stored, not the password: it opens the channel but doesn't give the password
// back. now: written at once (/e2e), else with the next /save (/autojoin add|del).
// ponytail: protected only by the file's 0600, like sasl_pass; a master password or the OS
// keychain is the upgrade if that's not enough.
void Core::e2e_keep(Server* s, bool now) {
    std::string sec = "server." + s->cfg.name, v;
    for (auto& e : s->cfg.autojoin) {
        auto k = s->chan_e2e.find(irc_lower(split(e, ' ')[0]));
        if (k != s->chan_e2e.end()) v += (v.empty() ? "" : ",") + k->first + ":" + b64(k->second);
    }
    if (v == ini_get(cfg, sec, "e2e_keys")) return;
    cfg[sec]["e2e_keys"] = unsaved[sec]["e2e_keys"] = v;
    if (!now) return;
    Ini part{{sec, unsaved[sec]}};  // the whole section: a /server one isn't in the file yet
    std::string err;
    if (ini_save(cfg_path, part, err)) unsaved.erase(sec);
    else print(s->status, LK::ERROR, err);
}

// the password arrived: accept an invite, or send one
void Core::e2e_pass(Buffer* q, const std::string& pass) {
    Server* s = q->srv;
    std::string nick = q->name;
    E2E& e = s->e2e[irc_lower(nick)];
    e.nick = nick;
    if (!e.pend.empty()) e = E2E{nick, e.pend, "", false, false, ""};  // they asked again (a restart?) and now we answer it
    bool accept = !e.salt.empty() && e.key.empty() && !e.mine;
    if (!accept) {
        unsigned char salt[16];
        rand_bytes(salt, sizeof salt);
        e = E2E{nick, std::string((char*)salt, 16), "", true, false, ""};
    }
    e.key = e2e_key(pass, e.salt);
    if (accept) {
        ctcp(s, nick, "ISEKUR-E2E-OK " + e2e_seal(e.key, OK_TOKEN));
        print(q, LK::INFO, "Checking the password with " + nick + "...");
    } else {
        ctcp(s, nick, "ISEKUR-E2E " + b64(e.salt));
        print(q, LK::INFO, "Asked " + nick + " for an encrypted chat; waiting for them to type the password");
    }
}

// CTCP ISEKUR-E2E* from nick
void Core::e2e_ctcp(Server* s, const std::string& nick, const std::string& verb, const std::string& arg) {
    std::string k = irc_lower(nick), plain;
    auto it = s->e2e.find(k);
    auto mismatch = [&]() {
        s->e2e.erase(k);
        e2e_note(s, nick, LK::ERROR, "Password mismatch with " + nick + ": nothing is encrypted. Start again with /e2e " + nick);
    };
    if (verb == "ISEKUR-E2E") {
        std::string salt = unb64(arg);
        if (salt.size() != 16) return;
        if (it != s->e2e.end() && it->second.ok) {  // a chat that is on keeps its key: this line could come from anyone, and the server
            it->second.pend = salt;                 // sees every one. They take the new key up with /e2e nick
            e2e_note(s, nick, LK::INFO, nick + " sent a new invitation (a restart?). Your encrypted chat goes on as it is; /e2e " + nick +
                                            " takes the new one, /e2e off " + nick + " ends it");
            return;
        }
        s->e2e[k] = E2E{nick, salt, "", false, false, ""};
        e2e_note(s, nick, LK::INFO, nick + " wants an encrypted conversation. To accept: /e2e " + nick);
    } else if (verb == "ISEKUR-E2E-OK") {
        if (it == s->e2e.end() || it->second.key.empty() || it->second.ok) return;
        if (!e2e_open(it->second.key, arg, plain) || plain != OK_TOKEN) {
            ctcp(s, nick, "ISEKUR-E2E-BAD");
            mismatch();
            return;
        }
        it->second.ok = true;
        ctcp(s, nick, "ISEKUR-E2E-ACK " + e2e_seal(it->second.key, ACK_TOKEN));
        e2e_note(s, nick, LK::INFO, "\xf0\x9f\x94\x92 Encrypted chat with " + nick + " is on");
    } else if (verb == "ISEKUR-E2E-ACK") {
        if (it == s->e2e.end() || it->second.key.empty() || it->second.ok) return;
        if (!e2e_open(it->second.key, arg, plain) || plain != ACK_TOKEN) { mismatch(); return; }
        it->second.ok = true;
        e2e_note(s, nick, LK::INFO, "\xf0\x9f\x94\x92 Encrypted chat with " + nick + " is on");
    } else if (verb == "ISEKUR-E2E-BAD") {
        if (it != s->e2e.end() && !it->second.ok) mismatch();  // only a handshake can fail: a chat that is on isn't ended by a line
    } else if (verb == "ISEKUR-E2E-OFF") {
        if (it == s->e2e.end()) return;
        if (it->second.ok && !(e2e_open(it->second.key, arg, plain) && plain == OFF_TOKEN)) {  // ending it takes the key
            e2e_note(s, nick, LK::ERROR, nick + " asked to turn encryption off without proving they hold the key, so it stays on. /e2e off " + nick + " ends it here");
            return;
        }
        s->e2e.erase(it);
        e2e_note(s, nick, LK::INFO, nick + " turned encryption off; messages are plain text again");
    }
}
