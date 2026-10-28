// /e2e: end-to-end encrypted queries between two iSekur users who share a
// password. key = scrypt(password, salt); messages are AES-256-GCM, so a wrong
// password fails the tag check instead of showing garbage.
//
//   A: /e2e B      -> password prompt, CTCP ISEKUR-E2E <salt>
//   B: /e2e A      -> password prompt, CTCP ISEKUR-E2E-OK <seal("ok")>
//   A: opens it    -> CTCP ISEKUR-E2E-ACK <seal("ack")>, or ISEKUR-E2E-BAD
//   messages: PRIVMSG B :+iSE <b64(nonce | ciphertext | tag)>
//
// /e2e #chan: no handshake, everyone who types the same password has the same key
// (salt "isekur-chan:" + channel); the channel name is the GCM AAD, so a line copied
// to another channel doesn't open. People without it see "+iSE ..." lines.
//
// ponytail: password only, no forward secrecy; an X25519 exchange
// authenticated by the password is the upgrade if that matters. No replay check in
// channels: chathistory replays old lines on purpose; a timestamp inside would be the fix.
#include <openssl/evp.h>
#include <openssl/rand.h>
#include "core.h"

static const char* OK_TOKEN = "isekur-e2e-ok";
static const char* ACK_TOKEN = "isekur-e2e-ack";  // differs from OK: a reflected OK is no ACK

std::string e2e_key(const std::string& pass, const std::string& salt) {
    unsigned char k[32];
    if (EVP_PBE_scrypt(pass.data(), pass.size(), (const unsigned char*)salt.data(), salt.size(),
                       1 << 15, 8, 1, 64 << 20, k, sizeof k) != 1)
        return "";
    return std::string((char*)k, sizeof k);
}

std::string e2e_seal(const std::string& key, const std::string& plain, const std::string& aad) {
    unsigned char nonce[12], tag[16];
    RAND_bytes(nonce, sizeof nonce);
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
            print(b, LK::INFO, e.nick + ": " + (e.ok ? "encrypted" : e.key.empty() ? "invited you (/e2e " + e.nick + ")" : "waiting for the other side"));
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
        if (nick.empty() || !s->e2e.erase(irc_lower(nick))) { print(b, LK::ERROR, "No encrypted chat with " + (nick.empty() ? "anyone here" : nick)); return; }
        ctcp(s, nick, "ISEKUR-E2E-OFF");
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
    bool accept = !e.salt.empty() && e.key.empty() && !e.mine;
    if (!accept) {
        unsigned char salt[16];
        RAND_bytes(salt, sizeof salt);
        e = E2E{nick, std::string((char*)salt, 16), "", true, false};
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
        s->e2e[k] = E2E{nick, salt, "", false, false};
        e2e_note(s, nick, LK::INFO, nick + " wants an encrypted conversation. To accept: /e2e " + nick);
    } else if (verb == "ISEKUR-E2E-OK") {
        if (it == s->e2e.end() || it->second.key.empty()) return;
        if (!e2e_open(it->second.key, arg, plain) || plain != OK_TOKEN) {
            ctcp(s, nick, "ISEKUR-E2E-BAD");
            mismatch();
            return;
        }
        it->second.ok = true;
        ctcp(s, nick, "ISEKUR-E2E-ACK " + e2e_seal(it->second.key, ACK_TOKEN));
        e2e_note(s, nick, LK::INFO, "\xf0\x9f\x94\x92 Encrypted chat with " + nick + " is on");
    } else if (verb == "ISEKUR-E2E-ACK") {
        if (it == s->e2e.end() || it->second.key.empty()) return;
        if (!e2e_open(it->second.key, arg, plain) || plain != ACK_TOKEN) { mismatch(); return; }
        it->second.ok = true;
        e2e_note(s, nick, LK::INFO, "\xf0\x9f\x94\x92 Encrypted chat with " + nick + " is on");
    } else if (verb == "ISEKUR-E2E-BAD") {
        if (it != s->e2e.end()) mismatch();
    } else if (verb == "ISEKUR-E2E-OFF") {
        if (it == s->e2e.end()) return;
        s->e2e.erase(it);
        e2e_note(s, nick, LK::INFO, nick + " turned encryption off; messages are plain text again");
    }
}
