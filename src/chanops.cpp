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
