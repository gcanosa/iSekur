// Built-in bouncer: other IRC clients (irssi, weechat, hexchat, mIRC...) log in with
// user/network:password and share the live session with the terminal and the webchat.
#pragma once
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <poll.h>
#include "core.h"

struct Bnc {
    Core& c;
    explicit Bnc(Core& core) : c(core) {}
    ~Bnc();
    static bool autostart(const Ini& cfg);  // [bnc] autostart
    bool start(std::string& err);  // listen now (no-op if already running)
    void stop();                   // drops every client
    bool running() const { return lfd >= 0; }
    bool attached() const;         // a logged-in client is connected
    std::string url;               // "ircs://127.0.0.1:6698", "" when stopped
    std::vector<std::string> status();  // /bnc status: where it listens, who is on
    void add_fds(std::vector<pollfd>& v);
    void process(const std::vector<pollfd>& v);  // after poll()
    // Server::tap: a line from the server (before iSekur handles it) and one to it
    void from_server(Server* s, const std::string& line);
    void to_server(Server* s, const std::string& line);

private:
    struct Client {
        Conn conn;
        std::string in, ip, nick, user, pass, net;
        std::set<std::string> caps;  // what it asked for, of OFFER (bnc.cpp)
        std::map<std::string, std::string> chans;  // lowered name -> name: the channels it was told it's in: PARTed when the network drops
        bool reg = false, capneg = false;
        bool sasl_wait = false, sasl_ok = false;  // SASL EXTERNAL: sent "+" and waits for our answer; the certificate matched
        time_t since = 0, rx = 0, pinged = 0;
        SS seen = SS::DISCONNECTED;  // the network's state it last heard about
    };
    int lfd = -1;
    SSL_CTX* ctx = nullptr;
    std::string user, pass_hash;
    std::set<std::string> certfps;  // [bnc] certfp: SHA-256 of the client certificates that log in with SASL EXTERNAL
    bool sasl_on() const { return !certfps.empty(); }  // needs TLS, which certfp requires
    std::vector<std::unique_ptr<Client>> clients;
    size_t fd_start = 0, fd_count = 0;
    std::map<std::string, std::pair<int, time_t>> fails;  // ip -> (count, locked until)
    Client* origin = nullptr;  // the client whose line is going out now: it doesn't get its own echo
    // with echo-message upstream: what clients sent, waiting for the server's echo, so a client
    // that didn't ask for echoes doesn't get its own line back
    struct Pend { Client* cl; std::string net, cmd, tgt, text; time_t at; };
    std::vector<Pend> pend;
    Client* take(Server* s, const std::string& cmd, const std::string& tgt, const std::string& text);
    int batch_n = 0, lab_n = 0;
    // labeled-response: our label on a client's command -> that client and its own label; and the
    // labeled BATCHes the answers came in, by reference
    struct Lab { Client* cl; std::string label; };
    std::map<std::string, Lab> labs, lab_batches;
    void history(Client& cl, Server* s, const Msg& m, const std::string& label);  // CHATHISTORY, from iSekur's own tabs

    bool start_(std::string& err);
    Server* server(const std::string& net) const;
    // tags ("k=v;k2", escaped): to message-tags clients, account also to account-tag ones, label to labeled-response
    // ones; batch: to batch clients
    void send(Client& cl, const std::string& line, time_t ts = 0, const std::string& tags = "", const std::string& batch = "");
    void line(Client& cl, const std::string& raw);
    void cap(Client& cl, const Msg& m);
    void authenticate(Client& cl, const std::string& arg);  // AUTHENTICATE: SASL EXTERNAL, by the client's certificate
    void login(Client& cl);
    void welcome(Client& cl, Server* s, const std::string& why = "");  // s null: on no network yet, why says so
    void attach(Client& cl, Server* s, const std::string& why);
    void control(Client& cl, const std::string& text);  // /msg *isekur networks|use|server|save
    void tell(Client& cl, const std::string& text);
    bool motd(Client& cl, const std::string& extra);  // bnc.motd in the config's folder, if there     // a line from *isekur
    void gone(Client& cl, const std::string& why);
    void sync(Client& cl, Server* s);  // tell it the network connected or dropped, once
};
