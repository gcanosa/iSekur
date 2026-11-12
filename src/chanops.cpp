// Channel operator shortcuts: mass modes, lockdown, kick+ban, temp bans,
// channel stats and topic rotation.
#include <algorithm>
#include <cstring>
#include <functional>
#include "core.h"

// pfx ranks at least as high as `need` ('@' op, '%' halfop; '%' means '@'
// on servers without halfops)
bool rank_ok(const Server* s, const std::string& pfx, char need) {
    size_t r = s->prefix_chars.find(need);
    if (r == std::string::npos) r = s->prefix_chars.find('@');
    return !pfx.empty() && r != std::string::npos && s->prefix_rank(pfx) <= (int)r;
}

// how long the flood queue takes to send n more lines
static std::string queued_for(const Server* s, size_t n) {
    long secs = n > (size_t)s->flood_burst && s->flood_ms > 0
                    ? (long)(n - (size_t)s->flood_burst) * s->flood_ms / 1000 : 0;
    return secs < 5 ? "" : " (flood control spreads them over ~" + std::to_string(secs) + "s)";
}

bool Core::chanop(Buffer* b, const std::string& cmd, const std::string& args) {
    static const std::set<std::string> names = {"massop", "massdeop", "massvoice", "massdevoice", "masskick",
                                                "lockdown", "unlock", "kickban", "kb", "tempban",
                                                "chanstats", "topicrotate"};
    if (!names.count(cmd)) return false;
    Server* s = b->srv;
    if (b->type != BT::CHANNEL || !b->joined) {
        print(b, LK::ERROR, "/" + cmd + " works in the tab of a channel you are in");
        return true;
    }
    auto argv = split(args, ' ');
    auto is_me = [&](const NickEntry& e) { return s->lower(e.nick) == s->lower(s->nick); };
    auto mine = b->nicks.find(s->lower(s->nick));
    std::string my = mine == b->nicks.end() ? "" : mine->second.prefix;
    auto usage = [&]() {
        const CmdInfo* k = find_command(cmd);
        print(b, LK::ERROR, "Usage: /" + std::string(k->name) + " " + k->usage + "  (/help " + k->name + ")");
        return true;
    };

    if (cmd == "chanstats") {
        int ops = 0, half = 0, voice = 0, reg = 0, away = 0;
        for (auto& kv : b->nicks) {
            const NickEntry& e = kv.second;
            if (rank_ok(s, e.prefix, '@')) ops++;
            else if (e.prefix.find('%') != std::string::npos) half++;
            else if (e.prefix.find('+') != std::string::npos) voice++;
            else reg++;
            away += e.away;
        }
        auto out = [&](const std::string& k, const std::string& v) {
            Line l{time(nullptr), LK::INFO, "", "\x03" "11" + k + "\x0f " + v};
            l.nolog = true;
            add(b, l);
        };
        out("\x02" + b->name, "on " + (s->network.empty() ? s->cfg.name : s->network));
        out("Users:", std::to_string(b->nicks.size()) + "  (" + std::to_string(ops) + " ops, " + std::to_string(half) +
                          " halfops, " + std::to_string(voice) + " voiced, " + std::to_string(reg) + " regular; " +
                          std::to_string(away) + " away)");
        out("Modes:", b->modes.empty() ? "none" : "+" + b->modes);
        out("Topic:", b->topic.empty() ? "none" : b->topic + "\x0f");
        if (!b->rot.empty())
            out("Topic rotation:", std::to_string(b->rot.size()) + " topics every " + std::to_string(b->rot_min) + " min");
        for (auto& t : s->tempbans)
            if (s->lower(t.chan) == s->lower(b->name))
                out("Temp ban:", t.mask + ", lifted in " + std::to_string((t.until - time(nullptr) + 59) / 60) + " min");
        return true;
    }

    if (cmd == "topicrotate") {
        if (argv.empty()) {
            if (b->rot.empty()) { print(b, LK::INFO, "No topic rotation in " + b->name); return true; }
            print(b, LK::INFO, "Rotating " + std::to_string(b->rot.size()) + " topics every " + std::to_string(b->rot_min) +
                                   " min in " + b->name + "; /topicrotate off stops it");
            for (size_t i = 0; i < b->rot.size(); i++)
                print(b, LK::INFO, std::string(i == b->rot_i ? "  next: " : "        ") + b->rot[i] + "\x0f");
            return true;
        }
        if (irc_lower(argv[0]) == "off") {
            print(b, LK::INFO, b->rot.empty() ? "No topic rotation in " + b->name : "Topic rotation in " + b->name + " stopped");
            b->rot.clear();
            return true;
        }
        int mins = atoi(argv[0].c_str());
        std::vector<std::string> topics;
        for (auto& t : split(after_words(args, 1), '|'))
            if (!trim(t).empty()) topics.push_back(trim(t));
        if (mins < 1 || mins > 1440 || topics.size() < 2) return usage();
        if (b->modes.find('t') != std::string::npos && !rank_ok(s, my, '%'))
            print(b, LK::ERROR, b->name + " is +t: the server will refuse the topics unless you get ops");
        b->rot = topics, b->rot_min = mins, b->rot_i = 0, b->rot_at = 0;  // the first one goes out now
        print(b, LK::INFO, "Rotating " + std::to_string(topics.size()) + " topics in " + b->name + " every " +
                               std::to_string(mins) + " min, starting now (/topicrotate off stops it)");
        return true;
    }

    bool full_op = cmd == "massop" || cmd == "massdeop" || cmd == "lockdown" || cmd == "unlock";
    if (!rank_ok(s, my, full_op ? '@' : '%')) {
        print(b, LK::ERROR, "You need to be a channel " + std::string(full_op ? "operator" : "operator or halfop") +
                                " in " + b->name + " for /" + cmd);
        return true;
    }
    if (cmd == "lockdown") {
        s->send("MODE " + b->name + " +mntsi");
        print(b, LK::INFO, b->name + " locked down: moderated, no outside messages, topic lock, secret, invite only. /unlock lifts it");
        return true;
    }
    if (cmd == "unlock") {
        s->send("MODE " + b->name + " -msi");
        print(b, LK::INFO, b->name + " unlocked: -m -s -i (+n and +t stay)");
        return true;
    }

    if (cmd == "kickban" || cmd == "kb" || cmd == "tempban") {
        bool temp = cmd == "tempban";
        if (argv.empty() || (temp && argv.size() < 2)) return usage();
        KickBan kb{b->name, "", after_words(args, temp ? 2 : 1), temp ? atoi(argv[1].c_str()) : 0};
        if (temp && (kb.minutes < 1 || kb.minutes > 10080)) {
            print(b, LK::ERROR, "Minutes go from 1 to 10080 (a week)");
            return true;
        }
        auto it = b->nicks.find(s->lower(argv[0]));
        if (it == b->nicks.end()) { print(b, LK::ERROR, argv[0] + " is not in " + b->name); return true; }
        if (is_me(it->second)) { print(b, LK::ERROR, "That's you"); return true; }
        kb.nick = it->second.nick;
        if (!it->second.host.empty()) kickban(s, b, kb, it->second.host);
        else s->kb_wait.push_back(kb), s->send("USERHOST " + kb.nick);  // the 302 reply finishes it
        return true;
    }

    // mass commands: everyone but you that the filter picks, in nicklist order
    std::function<bool(const NickEntry&)> pick;
    char sign = '+', mode = 'o';
    auto has = [](const NickEntry& e, char p) { return e.prefix.find(p) != std::string::npos; };
    if (cmd == "massop") pick = [&](const NickEntry& e) { return !rank_ok(s, e.prefix, '@'); };
    else if (cmd == "massdeop") sign = '-', pick = [&](const NickEntry& e) { return has(e, '@'); };
    else if (cmd == "massvoice") mode = 'v', pick = [&](const NickEntry& e) { return !has(e, '+') && !rank_ok(s, e.prefix, '%'); };
    else if (cmd == "massdevoice") sign = '-', mode = 'v', pick = [&](const NickEntry& e) { return has(e, '+'); };
    else pick = [&](const NickEntry& e) { return !rank_ok(s, e.prefix, '%'); };  // masskick: all but ops/halfops
    std::vector<std::string> who;
    for (auto& kv : b->nicks)
        if (!is_me(kv.second) && pick(kv.second)) who.push_back(kv.second.nick);
    if (who.empty()) { print(b, LK::INFO, "Nobody to change in " + b->name); return true; }

    if (cmd == "masskick") {
        bool yes = !argv.empty() && argv[0] == "-yes";
        std::string reason = yes ? after_words(args, 1) : args;
        if (!yes) {
            print(b, LK::ERROR, "This kicks " + std::to_string(who.size()) + " users (everyone but ops and halfops) from " +
                                    b->name + ". To go ahead: /masskick -yes" + (reason.empty() ? "" : " " + reason));
            return true;
        }
        for (auto& n : who) s->send("KICK " + b->name + " " + n + " :" + (reason.empty() ? "Clearing the channel" : reason));
        print(b, LK::INFO, "Kicking " + std::to_string(who.size()) + " users from " + b->name + queued_for(s, who.size()));
        return true;
    }
    size_t lines = 0;
    for (size_t i = 0; i < who.size(); i += (size_t)s->max_modes, lines++) {
        size_t n = std::min(who.size() - i, (size_t)s->max_modes);
        std::string l = "MODE " + b->name + " " + sign + std::string(n, mode);
        for (size_t j = 0; j < n; j++) l += " " + who[i + j];
        s->send(l);
    }
    print(b, LK::INFO, std::string(sign == '+' ? "Giving +" : "Taking -") + mode + (sign == '+' ? " to " : " from ") +
                           std::to_string(who.size()) + " users in " + b->name + queued_for(s, lines));
    return true;
}

void Core::kickban(Server* s, Buffer* b, const KickBan& kb, const std::string& userhost) {
    std::string mask = "*!*@" + userhost.substr(userhost.find('@') + 1);  // no '@': npos + 1 = 0, all of it
    s->send("MODE " + kb.chan + " +b " + mask);
    std::string why = !kb.reason.empty() ? kb.reason
                      : kb.minutes      ? "Banned for " + std::to_string(kb.minutes) + " minutes"
                                        : "Banned";
    s->send("KICK " + kb.chan + " " + kb.nick + " :" + why);
    if (!kb.minutes) return;
    auto& tb = s->tempbans;
    tb.erase(std::remove_if(tb.begin(), tb.end(),
                            [&](const TempBan& t) { return t.mask == mask && s->lower(t.chan) == s->lower(kb.chan); }),
             tb.end());
    tb.push_back({kb.chan, mask, time(nullptr) + kb.minutes * 60});
    print(b, LK::INFO, "Banned " + mask + " for " + std::to_string(kb.minutes) +
                           " minutes. iSekur lifts the ban, so it has to be running then");
}

// ---- /bans: the lists of a channel (bans b, exceptions e, invite exceptions I, quiets q) -------------------
std::string ban_kinds(const Server* s) {
    std::string a = s->chanmodes.substr(0, s->chanmodes.find(',')), k;
    for (char m : std::string("beIq"))
        if (a.find(m) != std::string::npos) k += m;
    return k;
}

// halfop or more may change them (an IRC operator too: some servers let them override)
bool Core::ban_rank(const Buffer* b) const {
    const Server* s = b->srv;
    if (!s) return false;
    auto me = b->nicks.find(s->lower(s->nick));
    return (me != b->nicks.end() && rank_ok(s, me->second.prefix, '%')) || s->umodes.find('o') != std::string::npos;
}

void Core::ban_list(Buffer* b, char mode, int ui) {
    Server* s = b->srv;
    if (!s || b->type != BT::CHANNEL || !b->joined || s->state != SS::CONNECTED) {
        print(b, LK::ERROR, "The lists are those of a channel you are in: use it in its tab");
        return;
    }
    std::string kinds = ban_kinds(s);
    if (kinds.find(mode) == std::string::npos) {
        print(b, LK::ERROR, std::string("This server has no +") + mode + " list" + (kinds.empty() ? "" : " (it has: " + kinds + ")"));
        return;
    }
    s->bl = BanList{};
    s->bl.chan = b->name, s->bl.mode = mode, s->bl.loading = true, s->bl.win = ui != 0, s->bl.at = time(nullptr);
    s->send(std::string("MODE ") + b->name + " +" + mode);
    if (ui == 1) bans_win = true, bans_buf = b->id;
    dirty = true;
    notify(Ev::BANS, b);
}

void Core::ban_edit(Buffer* b, char mode, bool add, const std::string& mask) {
    Server* s = b->srv;
    if (!s || b->type != BT::CHANNEL || !b->joined) return;
    if (!ban_rank(b)) { print(b, LK::ERROR, "You need to be a channel operator or halfop to change this list"); return; }
    std::string m = mask.find_first_of("!@*:") == std::string::npos ? mask + "!*@*" : mask;  // a bare nick: nick!*@*
    if (!strchr("beIq", mode) || m.empty() || m.find_first_of(" \r\n") != std::string::npos) return;
    s->send(std::string("MODE ") + b->name + (add ? " +" : " -") + mode + " " + m);
    s->send(std::string("MODE ") + b->name + " +" + mode);  // the list again, with the change in
    s->bl.err.clear(), s->bl.v.clear(), s->bl.loading = true, s->bl.at = time(nullptr);
    notify(Ev::BANS, b);
}

// the replies to a list we asked for belong to its window (or are printed as a table when they are in);
// a /mode +b typed by hand has nobody waiting: false, the generic numeric shows it
bool Core::ban_reply(Server* s, const Msg& m) {
    static const std::map<std::string, std::pair<char, bool>> num = {  // reply -> list mode, is it the end
        {"367", {'b', false}}, {"368", {'b', true}}, {"348", {'e', false}}, {"349", {'e', true}},
        {"346", {'I', false}}, {"347", {'I', true}}, {"728", {'q', false}}, {"729", {'q', true}}};
    BanList& bl = s->bl;
    if (!bl.loading) return false;
    Buffer* b = find(s, bl.chan);
    if (!b) return false;
    if (m.cmd == "482") {  // not allowed to see it: it ends the wait too
        if (s->lower(m.p(1)) != s->lower(bl.chan)) return false;
        bl.loading = false, bl.err = m.p(2);
        notify(Ev::BANS, b);
        return bl.win;  // with a window it shows the error; else the generic line does
    }
    auto it = num.find(m.cmd);
    if (it == num.end() || it->second.first != bl.mode || s->lower(m.p(1)) != s->lower(bl.chan)) return false;
    size_t o = bl.mode == 'q' ? 3 : 2;  // 728/729 name the mode: "me #chan q mask setter time"
    if (!it->second.second) {
        bl.v.push_back({m.p(o), m.p(o + 1), (time_t)atoll(m.p(o + 2).c_str())});
        return true;
    }
    bl.loading = false;
    if (!bl.win) {  // nobody has a window: the lines, newest first
        std::vector<BanEntry> v = bl.v;
        std::stable_sort(v.begin(), v.end(), [](const BanEntry& a, const BanEntry& z) { return a.ts > z.ts; });
        static const std::map<char, std::string> what = {{'b', "bans"}, {'e', "exceptions"}, {'I', "invite exceptions"}, {'q', "quiets"}};
        print(b, LK::INFO, std::to_string(v.size()) + " " + what.at(bl.mode) + " on " + bl.chan);
        time_t now = time(nullptr);
        for (auto& e : v)
            print(b, LK::INFO, "  " + e.mask + "  by " + e.setter.substr(0, e.setter.find('!')) +
                                   (e.ts > 0 ? ", " + fmt_dur((long)(now - e.ts)) + " ago" : ""));
    }
    notify(Ev::BANS, b);
    return true;
}

// once a second-ish from tick(), while s is connected
void Core::chan_timers(Server* s, time_t now) {
    if (s->bl.loading && now - s->bl.at > 15) {  // no end to the list: an error shown somewhere else, or a server that doesn't say
        s->bl.loading = false, s->bl.err = "The server did not send the list";
        if (Buffer* b = find(s, s->bl.chan)) notify(Ev::BANS, b);
    }
    for (size_t i = 0; i < s->tempbans.size();) {
        TempBan& t = s->tempbans[i];
        Buffer* b = find(s, t.chan);
        if (now < t.until || (b && !b->joined)) { i++; continue; }  // not in the channel: try once back in
        if (b) {
            s->send("MODE " + t.chan + " -b " + t.mask);
            print(b, LK::INFO, "Temp ban on " + t.mask + " is over, unbanning");
        }
        s->tempbans.erase(s->tempbans.begin() + (long)i);  // tab closed: dropped
    }
    for (auto& bp : bufs) {
        Buffer* b = bp.get();
        if (b->srv != s || b->rot.empty() || !b->joined || now < b->rot_at) continue;
        s->send("TOPIC " + b->name + " :" + b->rot[b->rot_i]);
        b->rot_i = (b->rot_i + 1) % b->rot.size();
        b->rot_at = now + b->rot_min * 60;
    }
}

// What you can do to a nick: the usual for anyone, channel ops by your own rank
// (halfops kick and voice, ops the rest).
// (the terminal's right click and the webchat's both use it; "" label = separator, a command
// ending in ' ' or ':' goes to the input line to be finished)
std::vector<std::pair<std::string, std::string>> Core::nick_menu(Buffer* b, const std::string& nick) const {
    std::vector<std::pair<std::string, std::string>> m;
    Server* s = b->srv;
    if (!s) return m;
    auto add = [&](const std::string& l, const std::string& cmd) { m.push_back({l, cmd}); };
    if (nick.find(' ') != std::string::npos) {  // "a b c": the nicklist's selection, all at once
        auto who = split(nick, ' ');
        std::string list, mention, kicks, kbs;
        for (auto& n : who) {
            list += " " + n, mention += (mention.empty() ? "" : ", ") + n;
            kicks += "\n/kick " + b->name + " " + n, kbs += "\n/kb " + n;
        }
        std::string many = std::to_string(who.size()) + " users";
        add("Mention", mention + ": ");
        if (b->type != BT::CHANNEL || !b->joined) return m;
        auto me = b->nicks.find(s->lower(s->nick));
        std::string my = me == b->nicks.end() ? "" : me->second.prefix;
        bool op = rank_ok(s, my, '@'), half = rank_ok(s, my, '%');
        if (!half) return m;
        add("", "");
        if (op) add("Op", "/op" + list), add("Deop", "/deop" + list);
        add("Voice", "/voice" + list), add("Devoice", "/devoice" + list);
        add("Kick...", "?Kick " + many + " from " + b->name + "?" + kicks);
        if (op) add("Kickban...", "?Kick and ban " + many + " from " + b->name + "?" + kbs);
        return m;
    }
    add("Query", "/query " + nick);
    add("Whois", "/whois " + nick);
    add("Mention", nick + ": ");  // ends in a space: goes to the input line
    add("", "");
    add("CTCP Version", "/ctcp " + nick + " VERSION");
    add("CTCP Ping", "/ctcp " + nick + " PING");
    add("CTCP Time", "/ctcp " + nick + " TIME");
    bool ign = false;
    time_t until, now = time(nullptr);
    for (auto& e : split(ini_get(cfg, "server." + s->cfg.name, "ignore"), ' '))
        if (ign_mask(e, &until) == nick + "!*@*" && (!until || until > now)) ign = true;
    add("", "");
    add(ign ? "Unignore" : "Ignore", (ign ? "/unignore " : "/ignore ") + nick);
    std::vector<std::pair<std::string, std::string>> pm;  // the plugins on there add theirs (pmguard: Allow PMs)
    const_cast<Core*>(this)->plugin_emit("nickmenu", s, b->name, {{"nick", nick}}, nullptr, &pm);  // ponytail: the menu is const, the Lua calls aren't
    if (!pm.empty()) m.push_back({"", ""}), m.insert(m.end(), pm.begin(), pm.end());
    if (b->type == BT::CHANNEL && b->joined) {
        auto me = b->nicks.find(s->lower(s->nick)), them = b->nicks.find(s->lower(nick));
        std::string my = me == b->nicks.end() ? "" : me->second.prefix;
        std::string their = them == b->nicks.end() ? "" : them->second.prefix;
        bool op = rank_ok(s, my, '@'), half = rank_ok(s, my, '%');
        if (half) {
            add("", "");
            if (op) add(their.find('@') != std::string::npos ? "Deop" : "Op",
                        "/mode " + b->name + (their.find('@') != std::string::npos ? " -o " : " +o ") + nick);
            add(their.find('+') != std::string::npos ? "Devoice" : "Voice",
                "/mode " + b->name + (their.find('+') != std::string::npos ? " -v " : " +v ") + nick);
            add("Kick", "/kick " + b->name + " " + nick);
            add("Kick with reason...", "/kick " + b->name + " " + nick + " :");
        }
        if (op) {
            add("Kickban", "/kb " + nick);
            add("Ban 10 minutes", "/tempban " + nick + " 10");
        }
    }
    if (s->umodes.find('o') != std::string::npos) {  // IRC operator
        add("", "");
        add("Kill...", "/kill " + nick + " :");
        auto them = b->nicks.find(s->lower(nick));
        std::string host = them == b->nicks.end() ? "" : them->second.host;
        if (host.find('@') != std::string::npos)
            add("G-line 1 day...", "/gline *@" + host.substr(host.find('@') + 1) + " 1d :");
    }
    return m;
}

// right click on a channel tab. A command starting with "?" asks first: "?question\n/command"
std::vector<std::pair<std::string, std::string>> Core::tab_menu(Buffer* b) const {
    std::vector<std::pair<std::string, std::string>> m;
    Server* s = b->srv;
    if (b->type == BT::STATUS && s) {  // a server tab
        bool up = s->state != SS::DISCONNECTED;
        if (up) m.push_back({"Reconnect", "/reconnect"}), m.push_back({"Disconnect", "/disconnect"});
        else m.push_back({"Connect", "/connect"});
        m.push_back({b->collapsed ? "Show its channels" : "Hide its channels", "/fold"});
        m.push_back({s->cfg.autoconnect ? "Don't connect at startup" : "Connect at startup", s->cfg.autoconnect ? "/autoconnect off" : "/autoconnect on"});
        m.push_back({"Edit network...", "/servers " + s->cfg.name});
        m.push_back({"", ""});
        m.push_back({"Close", up ? "?Disconnect from " + s->cfg.name + " and close all its tabs?\n/close" : "/close"});
        m.push_back({"Rename server...", "/server rename " + s->cfg.name});
        m.push_back({"Delete server...", "?Delete " + s->cfg.name + ": disconnect, close its tabs and remove it from the config?\n/server del " + s->cfg.name});
        return m;
    }
    if (b->type == BT::SHELL) {  // /shell or /attach: keys go to the tab, so its menu is the way out
        bool remote = b->term && !b->term->host.empty();
        if (remote) m.push_back({"Disconnect", "?Disconnect from " + b->term->host + "? Only this link closes: the iSekur there keeps running "
                                               "and stays on its networks. /attach brings it back.\n/close"});
        else m.push_back({"Close", "/close"});
        return m;
    }
    if (b->type == BT::QUERY && s && (b->name == "*channels" || b->name == "*snotices" || b->name == "*raw")) {  // iSekur's own tabs: no nick behind them
        if (b->name == "*channels") {
            m.push_back({"Refresh list", "/list"});
            if (!s->list_filter.empty()) m.push_back({"Clear filter", "/list filter"});
        } else m.push_back({"Clear", "/clear"});
        m.push_back({"", ""});
        m.push_back({"Close", "/close"});
        return m;
    }
    if (b->type == BT::QUERY && s) {  // a private chat: the nick's menu (bar Query, Mention), then the tab's own
        for (auto& it : nick_menu(b, b->name))
            if (it.first != "Query" && it.first != "Mention" && !(m.empty() && it.first.empty())) m.push_back(it);
        m.push_back({"", ""});
        m.push_back({e2e_on(b) ? "Turn off E2E" : "Turn on E2E", e2e_on(b) ? "/e2e off" : "/e2e " + b->name});
        m.push_back({"Clear", "/clear"});
        m.push_back({"", ""});
        m.push_back({"Close", "/close"});
        return m;
    }
    if (b->type != BT::CHANNEL || !s) return m;
    auto& aj = s->cfg.autojoin;
    bool in_aj = std::any_of(aj.begin(), aj.end(), [&](const std::string& e) { return s->lower(split(e, ' ')[0]) == s->lower(b->name); });
    m.push_back({b->quiet ? "Quiet mode off" : "Quiet mode on", b->quiet ? "/quiet off" : "/quiet on"});
    m.push_back({muted(b) ? "Unmute channel" : "Mute channel", muted(b) ? "/mute off" : "/mute on"});
    m.push_back({"Clear", "/clear"});
    m.push_back({e2e_on(b) ? "Turn off E2E" : "Turn on E2E...", e2e_on(b) ? "/e2e off" : "/e2e " + b->name});
    if (b->joined) m.push_back({"Ban list...", "/bans"});
    m.push_back({in_aj ? "Remove from autojoin" : "Add to autojoin", in_aj ? "/autojoin del" : "/autojoin add"});
    m.push_back({"", ""});
    m.push_back({"Close", b->joined ? "?Leave " + b->name + " and close its tab?\n/close" : "/close"});
    return m;
}

// /list sort name|users, or a click on the header: a second time on the same column reverses it
void Core::list_sort(Buffer* b, const std::string& by) {
    Server* s = b->srv;
    if (!s || s->list.empty()) { print(b, LK::ERROR, "Nothing to sort: /list first"); return; }
    if (by != "" && by != "name" && by != "users") { print(b, LK::ERROR, "Usage: /list sort name|users"); return; }
    char k = by.empty() ? s->list_sort : by[0] == 'n' ? 'n' : 'u';
    s->list_rev = k == s->list_sort ? !s->list_rev : false;
    s->list_sort = k;
    list_table(s);
}

// /list filter text, or text typed in *channels: the rows whose channel or topic has it (any case); "" all of them
void Core::list_filter(Buffer* b, const std::string& text) {
    Server* s = b->srv;
    if (s && text == s->list_filter) return;  // Esc with no filter on: nothing to redraw
    if (!s || s->list.empty()) { print(b, LK::ERROR, "Nothing to filter: /list first"); return; }
    s->list_filter = text;
    list_table(s);
}

// /list, busiest first (or by name, /list sort):
//    #   Channel          Users  Topic
//    1.  #linux            1834  [+nt] Welcome to ...
// colors are mIRC codes, so the webchat shows the same table (names white: 12 is too dark on dark themes)
void Core::list_table(Server* s) {
    Buffer* b = open(s, BT::QUERY, "*channels");
    b->lines.clear(), b->scroll = b->unread = 0;
    notify(Ev::CLEAR, b);
    auto out = [&](const std::string& t) { add(b, Line{time(nullptr), LK::INFO, "", t, false, false, true}); };  // never logged
    auto& L = s->list;
    bool by_name = s->list_sort == 'n', rev = s->list_rev;
    std::sort(L.begin(), L.end(), [&](auto& a, auto& z) {
        std::string an = s->lower(a.chan), zn = s->lower(z.chan);
        if (by_name) return rev ? an > zn : an < zn;
        return a.users != z.users ? (rev ? a.users < z.users : a.users > z.users) : an < zn;
    });
    std::vector<const Server::ListEntry*> V;  // the rows shown: all, or those the filter matches
    std::string f = s->list_filter;
    to_lower(f);  // ponytail: ASCII case folding, UTF-8 matches as typed
    for (auto& e : L) {
        std::string hay = e.chan + " " + mirc_strip(e.topic);
        to_lower(hay);
        if (hay.find(f) != std::string::npos) V.push_back(&e);
    }
    auto len = [](const std::string& t) { size_t n = 0; for (unsigned char ch : t) n += (ch & 0xc0) != 0x80; return n; };
    auto pad = [&](const std::string& t, size_t w) { return t + std::string(w > len(t) ? w - len(t) : 0, ' '); };
    auto lpad = [&](const std::string& t, size_t w) { return std::string(w > len(t) ? w - len(t) : 0, ' ') + t; };
    long total = 0;
    size_t nw = 7;  // "Channel"
    for (auto* e : V) total += e->users, nw = std::max(nw, std::min<size_t>(len(e->chan), 30));
    const size_t shown = std::min<size_t>(V.size(), 1900), rw = std::to_string(std::max<size_t>(shown, 1)).size() + 1;
    out("\x02" "Channels on " + (s->network.empty() ? s->cfg.name : s->network) + "\x02  \x03" "14" + std::to_string(V.size()) +
        (V.size() == 1 ? " channel, " : " channels, ") + std::to_string(total) + " users" +
        (f.empty() ? std::string() : " matching \x0f\x02" + s->list_filter + "\x0f\x03" "14 (of " + std::to_string(L.size()) + ", Esc shows all)"));
    if (V.empty()) {
        out("\x03" "14" + (f.empty() ? std::string("Nothing matched. /list alone lists them all, /list *word* filters")
                                     : "No channel or topic has that. Type another word, or Esc shows all"));
        dirty = true;
        return;
    }
    const char* arrow = rev ? " \u25B2" : " \u25BC";  // ▼ the usual order: users high to low, names A-Z; ▲ reversed
    std::string head = " " + lpad("#", rw) + "  " + pad(std::string("Channel") + (by_name ? arrow : ""), nw) + "  " +
                       lpad(std::string("Users") + (by_name ? "" : arrow), 7) + "  Topic";
    out("\x03" "00,02\x02" + pad(head, std::max<size_t>(len(head) + 30, 72)) + "\x0f");
    for (size_t i = 0; i < shown; i++) {
        auto& e = *V[i];
        // busy channels stand out: 1000+ pink, 100+ yellow, 10+ green, the rest gray
        const char* uc = e.users >= 1000 ? "13" : e.users >= 100 ? "08" : e.users >= 10 ? "09" : "14";
        std::string topic = e.topic;
        if (topic.rfind("[+", 0) == 0 && topic.find(']') != std::string::npos) {  // the server's "[+nt] " modes, dimmed
            size_t close = topic.find(']');
            topic = "\x03" "14" + topic.substr(0, close + 1) + "\x0f" + topic.substr(close + 1);
        }
        out(" \x03" "14" + lpad(std::to_string(i + 1) + ".", rw) + "\x0f  \x02\x03" "00" + pad(e.chan, nw) + "\x0f  \x03" + uc +
            lpad(std::to_string(e.users), 7) + "\x0f  " + topic);
    }
    out("\x03" "14" + (shown < V.size() ? "The " + std::to_string(shown) + " busiest of " + std::to_string(V.size()) + " · " : std::string()) +
        "double click a channel to join · click Channel or Users to sort · type a word here to filter by name or topic, Esc clears it");
    dirty = true;
}
