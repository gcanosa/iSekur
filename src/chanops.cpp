// Channel operator shortcuts: mass modes, lockdown, kick+ban, temp bans,
// channel stats and topic rotation.
#include <algorithm>
#include <functional>
#include "core.h"

// pfx ranks at least as high as `need` ('@' op, '%' halfop; '%' means '@'
// on servers without halfops)
bool rank_ok(const Server* s, const std::string& pfx, char need) {
    size_t r = s->prefix_chars.find(need);
    if (r == std::string::npos) r = s->prefix_chars.find('@');
    return !pfx.empty() && r != std::string::npos && s->prefix_rank(pfx) <= (int)r;
}

static std::string trim(const std::string& t) {
    size_t a = t.find_first_not_of(' '), z = t.find_last_not_of(' ');
    return a == std::string::npos ? "" : t.substr(a, z - a + 1);
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
    auto is_me = [&](const NickEntry& e) { return irc_lower(e.nick) == irc_lower(s->nick); };
    auto mine = b->nicks.find(irc_lower(s->nick));
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
            if (irc_lower(t.chan) == irc_lower(b->name))
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
        auto it = b->nicks.find(irc_lower(argv[0]));
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
                            [&](const TempBan& t) { return t.mask == mask && irc_lower(t.chan) == irc_lower(kb.chan); }),
             tb.end());
    tb.push_back({kb.chan, mask, time(nullptr) + kb.minutes * 60});
    print(b, LK::INFO, "Banned " + mask + " for " + std::to_string(kb.minutes) +
                           " minutes. iSekur lifts the ban, so it has to be running then");
}

// once a second-ish from tick(), while s is connected
void Core::chan_timers(Server* s, time_t now) {
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
    add("Query", "/query " + nick);
    add("Whois", "/whois " + nick);
    add("Mention", nick + ": ");  // ends in a space: goes to the input line
    add("", "");
    add("CTCP Version", "/ctcp " + nick + " VERSION");
    add("CTCP Ping", "/ctcp " + nick + " PING");
    add("CTCP Time", "/ctcp " + nick + " TIME");
    auto masks = split(ini_get(cfg, "general", "ignore"), ' ');
    bool ign = std::find(masks.begin(), masks.end(), nick + "!*@*") != masks.end();
    add("", "");
    add(ign ? "Unignore" : "Ignore", (ign ? "/unignore " : "/ignore ") + nick);
    if (b->type == BT::CHANNEL && b->joined) {
        auto me = b->nicks.find(irc_lower(s->nick)), them = b->nicks.find(irc_lower(nick));
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
        auto them = b->nicks.find(irc_lower(nick));
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
    if (b->type != BT::CHANNEL || !s) return m;
    auto& aj = s->cfg.autojoin;
    bool in_aj = std::any_of(aj.begin(), aj.end(), [&](const std::string& e) { return irc_lower(split(e, ' ')[0]) == irc_lower(b->name); });
    m.push_back({b->quiet ? "Quiet mode off" : "Quiet mode on", b->quiet ? "/quiet off" : "/quiet on"});
    m.push_back({"Clear", "/clear"});
    m.push_back({e2e_on(b) ? "Turn off E2E" : "Turn on E2E...", e2e_on(b) ? "/e2e off" : "/e2e " + b->name});
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
        std::string an = irc_lower(a.chan), zn = irc_lower(z.chan);
        if (by_name) return rev ? an > zn : an < zn;
        return a.users != z.users ? (rev ? a.users < z.users : a.users > z.users) : an < zn;
    });
    auto len = [](const std::string& t) { size_t n = 0; for (unsigned char ch : t) n += (ch & 0xc0) != 0x80; return n; };
    auto pad = [&](const std::string& t, size_t w) { return t + std::string(w > len(t) ? w - len(t) : 0, ' '); };
    auto lpad = [&](const std::string& t, size_t w) { return std::string(w > len(t) ? w - len(t) : 0, ' ') + t; };
    long total = 0;
    size_t nw = 7;  // "Channel"
    for (auto& e : L) total += e.users, nw = std::max(nw, std::min<size_t>(len(e.chan), 30));
    const size_t shown = std::min<size_t>(L.size(), 1900), rw = std::to_string(std::max<size_t>(shown, 1)).size() + 1;
    out("\x02" "Channels on " + (s->network.empty() ? s->cfg.name : s->network) + "\x02  \x03" "14" + std::to_string(L.size()) +
        " channels, " + std::to_string(total) + " users");
    if (L.empty()) { out("\x03" "14Nothing matched. /list alone lists them all, /list *word* filters"); return; }
    const char* arrow = rev ? " \u25B2" : " \u25BC";  // ▼ the usual order: users high to low, names A-Z; ▲ reversed
    std::string head = " " + lpad("#", rw) + "  " + pad(std::string("Channel") + (by_name ? arrow : ""), nw) + "  " +
                       lpad(std::string("Users") + (by_name ? "" : arrow), 7) + "  Topic";
    out("\x03" "00,02\x02" + pad(head, std::max<size_t>(len(head) + 30, 72)) + "\x0f");
    for (size_t i = 0; i < shown; i++) {
        auto& e = L[i];
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
    out("\x03" "14" + (shown < L.size() ? "The " + std::to_string(shown) + " busiest of " + std::to_string(L.size()) + " · " : std::string()) +
        "double click a channel to join · click Channel or Users to sort · /list *word* filters");
    dirty = true;
}
