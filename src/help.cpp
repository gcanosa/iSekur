// Command reference: /help, /help <command>, and the list Tab completes from.
#include "core.h"

// mIRC colors, so the terminal and the webchat both show them
#define BOLD "\x02"
#define RESET "\x0f"
#define C_GROUP "\x02\x03" "08"  // bold yellow
#define C_CMD "\x03" "11"        // light cyan
#define C_ARGS "\x03" "15"       // light grey
#define C_EX "\x03" "09"         // light green
#define C_DIM "\x03" "14"        // grey

static const std::vector<CmdInfo> CMDS = {
    // ---- Connection
    {"server", "Connection", "host [port] [-tls] [-noverify]", "Connect to a new server",
     "Opens a server tab and connects. With -tls the port defaults to 6697, otherwise 6667. "
     "-noverify skips the certificate check (self-signed servers). The server is remembered "
     "like /set does, so /save keeps it for the next start. Naming a server you already have "
     "reconnects it.",
     {"/server irc.libera.chat 6697 -tls", "/server irc.example.org", "/server 10.0.0.5 6697 -tls -noverify"}},
    {"connect", "Connection", "[name]", "Reconnect a configured server",
     "Connects the server of the current tab, or the [server.<name>] you give.",
     {"/connect", "/connect libera"}},
    {"disconnect", "Connection", "[message]", "Quit this server, keep the tabs",
     "Sends QUIT to the server of the current tab and does not reconnect. Its tabs stay open; "
     "/connect brings it back and rejoins the channels.",
     {"/disconnect", "/disconnect brb, rebooting"}},
    {"quit", "Connection", "[message]", "Quit every server and exit iSekur",
     "Sends QUIT everywhere and exits. To keep iSekur running in the background instead, use "
     "/detach or Alt+D (Option+D on a Mac).",
     {"/quit", "/quit see you tomorrow"}},
    {"nick", "Connection", "newnick", "Change your nickname",
     "Changes your nick on the server of the current tab. When disconnected, it is used on the next connect.",
     {"/nick ethernet", "/nick ethernet_away"}},

    // ---- Chat
    {"join", "Chat", "#channel [key]", "Join a channel",
     "The # is added when you leave it out. Give the key for channels with +k.",
     {"/join #sekurnet", "/j linux", "/join #private s3cret"}, "j"},
    {"autojoin", "Chat", "[add|del] [#channel [key]] | snap [here]", "Channels joined when the server connects",
     "Without arguments, lists this server's autojoin channels. add / del change the list, the "
     "current channel when you name none; add keeps the key you gave /join. /save keeps it. "
     "snap makes every server's autojoin the channels open right now (here: only this server's) "
     "and writes it to the config at once.",
     {"/autojoin", "/autojoin add", "/autojoin add #private s3cret", "/autojoin del #old", "/autojoin snap"}},
    {"part", "Chat", "[#channel] [message]", "Leave a channel",
     "Leaves the current channel, or the one you name. The tab stays open with the history; "
     "/close (or Ctrl+W) closes it.",
     {"/part", "/part #linux off to bed"}},
    {"msg", "Chat", "target text", "Send a private message",
     "Sends text to a nick or channel without opening a tab. If a tab for the target is "
     "already open the message shows there.",
     {"/msg NickServ IDENTIFY password", "/msg alice are you around?"}},
    {"query", "Chat", "nick [text]", "Open a private chat tab",
     "Opens (or switches to) a query tab with nick, and sends text if you give it.",
     {"/query alice", "/query bob hey, got a minute?"}},
    {"e2e", "Chat", "nick | #channel | off [nick|#channel]", "Encrypted chat with other iSekur users",
     "Asks for a password (typed hidden; an empty line cancels) and invites nick. They get a notice and "
     "accept with /e2e yournick and the same password. If the passwords match, both sides see "
     "\"Encrypted chat is on\" and the query is AES-256-GCM encrypted from then on; if not, "
     "both are told and nothing is encrypted. Keys live in memory only: after a restart, start "
     "again. /e2e off ends it, /e2e alone lists sessions. Other clients see +iSE lines. "
     "/e2e #channel asks for a password and encrypts what you say there; whoever typed the same "
     "password reads it (lines with 🔒), the rest see +iSE noise. No check: a wrong password just "
     "can't open the others' lines. Anyone with the password reads it all; to drop someone, change it. "
     "Also in the channel tab's right click menu. For an autojoin channel the key (not the password) "
     "is saved in the config as e2e_keys, so a restart doesn't ask; anyone who reads the file can read the channel.",
     {"/e2e alice", "/e2e #secret", "/e2e off", "/e2e"}, "sq"},
    {"me", "Chat", "action", "Send an action (/me waves)",
     "Shows as \"* yournick action\" in the channel or query.",
     {"/me waves", "/me is grabbing coffee"}},
    {"notice", "Chat", "target text", "Send a notice",
     "Notices never trigger automatic replies, which is why bots use them.",
     {"/notice alice the build is green"}},
    {"topic", "Chat", "[text]", "Show or set the channel topic",
     "Without text, shows the topic. With text, sets it (channels with +t need ops).",
     {"/topic", "/topic Welcome! Rules: be nice | https://example.org"}},
    {"quiet", "Chat", "[on|off]", "Hide joins, parts, quits and nick changes in this tab",
     "Toggles for the current tab only. Hidden lines are still logged and come back with /quiet off. "
     "Your own joins and parts stay visible.",
     {"/quiet", "/quiet off"}},
    {"ctcp", "Chat", "target VERSION|TIME|PING", "Send a CTCP request",
     "PING shows the round trip time when the reply arrives.",
     {"/ctcp alice VERSION", "/ctcp alice PING"}},
    {"raw", "Chat", "line", "Send a raw IRC line",
     "Sends the line to the server exactly as typed. Unknown commands do this too.",
     {"/raw PRIVMSG #chan :hello", "/quote WHO #sekurnet"}, "quote"},

    // ---- Channel ops
    {"massop", "Channel ops", "", "Op everyone in the channel",
     "Gives +o to everyone who isn't an op yet, several per MODE line (as many as the server's "
     "MODES allows). Needs ops.",
     {"/massop"}},
    {"massdeop", "Channel ops", "", "Deop everyone except yourself",
     "Takes +o from every op but you. Needs ops.",
     {"/massdeop"}},
    {"massvoice", "Channel ops", "", "Voice everyone",
     "Gives +v to everyone below halfop who doesn't have it. Handy with /lockdown: the regulars can "
     "keep talking while the channel is moderated.",
     {"/massvoice"}},
    {"massdevoice", "Channel ops", "", "Remove voice from everyone",
     "Takes +v from everyone but you.",
     {"/massdevoice"}},
    {"masskick", "Channel ops", "[-yes] [reason]", "Kick everyone who isn't an op or halfop",
     "Asks first: without -yes it only says how many users it would kick. Ops, halfops and you stay. "
     "Kicks go through flood control, so a big channel takes a while.",
     {"/masskick spam wave", "/masskick -yes spam wave"}},
    {"lockdown", "Channel ops", "", "Lock the channel: +mntsi",
     "Moderated (only voiced and ops talk), no outside messages, topic lock, secret, invite only. "
     "Use it during a flood or raid; /unlock lifts it.",
     {"/lockdown", "/lockdown then /massvoice to let the regulars talk"}},
    {"unlock", "Channel ops", "", "Lift a lockdown: -msi",
     "Removes moderated, secret and invite only. +n and +t stay.",
     {"/unlock"}},
    {"kickban", "Channel ops", "nick [reason]", "Ban *!*@host and kick, in one go",
     "Bans the user's host (so a new nick doesn't get them back in) and kicks them. If the host isn't "
     "known yet iSekur asks the server for it first.",
     {"/kickban troll", "/kb troll flooding the channel"}, "kb"},
    {"tempban", "Channel ops", "nick minutes [reason]", "Kick + ban, unbanned after some minutes",
     "Like /kickban, then lifts the ban after the given minutes (1 to 10080). The unban is sent by "
     "iSekur, so it has to be running then. /chanstats lists the pending ones.",
     {"/tempban troll 30 spam", "/tempban flooder 1440 come back tomorrow"}},
    {"chanstats", "Channel ops", "", "Users, ops, voices, topic and modes",
     "Counts the nicklist (ops, halfops, voiced, regular, away) and shows the modes, the topic, "
     "the topic rotation and pending temp bans. Nothing is sent to the server.",
     {"/chanstats"}},
    {"topicrotate", "Channel ops", "minutes topic | topic ... | off", "Change the topic on a timer",
     "Sets the topics in turn, one every N minutes (1 to 1440), starting right away. Separate them "
     "with |. Without arguments shows what's rotating; off stops it. It runs while iSekur runs and "
     "is not saved.",
     {"/topicrotate 60 Welcome! | Rules: be nice | Meeting Friday 18:00 UTC", "/topicrotate", "/topicrotate off"}},

    // ---- Windows
    {"win", "Windows", "N", "Go to tab N",
     "Same as Alt+1..9 / Alt+0, but works for tabs past 10.",
     {"/win 3", "/win 12"}, "window"},
    {"close", "Windows", "", "Close the current tab",
     "Parts the channel first. On a server tab it disconnects and closes all of the server's tabs.",
     {"/close"}, "wc"},
    {"clear", "Windows", "", "Clear the current tab's scrollback",
     "Only clears what is on screen; logs are untouched.",
     {"/clear"}},
    {"grep", "Windows", "[-all] text", "Search the scrollback",
     "Shows the lines of the current tab that contain text (nick or message, case insensitive), "
     "the last 50 at most. -all searches every tab.",
     {"/grep http", "/grep -all alice"}},
    {"theme", "Windows", "[name]", "Show or switch the color theme",
     "Themes are files in themes/*.theme. The switch is live; /set theme name + /save keeps it.",
     {"/theme", "/theme neon", "/theme solarized"}},

    // ---- Settings
    {"set", "Settings", "[section.]key [value]", "Show or change a setting",
     "Without arguments lists everything; with a key shows it; with a value changes it right away. "
     "The section defaults to [general]. While typing the key, matching keys are listed and Tab completes them.",
     {"/set", "/set theme neon", "/set server.libera.autojoin #a,#b", "/set log on"}},
    {"reload", "Settings", "", "Re-read the config file",
     "Applies an edited config without restarting: theme, keys, mouse, colors, bell, logging and "
     "the rest right away; server settings on the next /connect; [web] after /web stop and /web "
     "start. Servers added to the file are opened (and connected when autoconnect is on); removed "
     "ones stay until you close their tab. /set changes not yet saved stay on top.",
     {"/reload"}},
    {"save", "Settings", "", "Write changed settings to the config file",
     "Saves what /set, /server, /autojoin and /ignore changed. Comments in the file are kept.",
     {"/save"}},
    {"hilight", "Settings", "[add|del] word", "Words highlighted besides your nick",
     "Without arguments, lists them. Matching ignores case and takes whole words; * matches anything "
     "(isekur*). They're hilight_words in the config, per server too. /save keeps the list.",
     {"/hilight", "/hilight add isekur", "/hilight add ger*", "/hilight del isekur"}},
    {"ignore", "Settings", "[mask]", "Ignore a nick or mask, or list ignores",
     "A bare nick becomes nick!*@*. Ignored users' messages, notices, invites and CTCPs are dropped. "
     "/save keeps the list.",
     {"/ignore", "/ignore spammer", "/ignore *!*@*.evil.host"}},
    {"unignore", "Settings", "mask", "Stop ignoring a mask",
     "Use the mask exactly as /ignore lists it.",
     {"/unignore spammer!*@*"}},

    // ---- Session
    {"exec", "Session", "[-o] [-y] command", "Run a shell command",
     "The output shows up in the cmd tab as it comes, without blocking the chat. With -o it is "
     "said in the tab you typed it in once the command ends, unless it is longer than "
     "paste_confirm lines (-y sends it anyway). Commands are killed after exec_timeout "
     "seconds (default 30) or 1000 lines. /exec alone lists the running ones. Off in the "
     "webchat unless /set exec_web on, since it runs commands on this machine.",
     {"/exec ls -la", "/exec -o uptime", "/sh git log --oneline -5"}, "sh"},
    {"web", "Session", "[start|stop|status]", "Control the built-in webchat",
     "The webchat mirrors this session in a browser. It needs [web] user and password_hash "
     "(isekur --hash-password prints one).",
     {"/web", "/web start", "/web stop"}},
    {"detach", "Session", "", "Leave iSekur running in the background",
     "Gives the terminal back; iSekur stays connected, in the channels, logging. isekur --attach "
     "(or just isekur) brings it back, from any terminal or SSH session. Alt+D does the same "
     "(Option+D on a Mac). Closing the terminal or losing SSH detaches too, instead of quitting.",
     {"/detach", "isekur --attach  (from your shell, later)"}},
    {"help", "Session", "[command]", "This help",
     "Without arguments lists every command. With a command name shows details and examples. "
     "Tab completes command names here and after \"/\".",
     {"/help", "/help join", "/help set"}},

    // ---- Server (sent as typed)
    {"whois", "Server", "nick", "Who is this user",
     "Unknown commands are sent to the server as typed, so everything your server supports works.",
     {"/whois alice"}},
    {"away", "Server", "[message]", "Set or clear away",
     "With a message marks you away, without one clears it. A manual /away turns off auto-away until you come back.",
     {"/away lunch", "/away"}},
    {"mode", "Server", "target modes [args]", "Change channel or user modes",
     "Sent as typed.", {"/mode #chan +o alice", "/mode #chan +b *!*@bad.host"}},
    {"kick", "Server", "#channel nick [reason]", "Kick a user",
     "Sent as typed. /kickban kicks and bans in one go.", {"/kick #chan troll bye"}},
    {"invite", "Server", "nick #channel", "Invite a user to a channel",
     "Sent as typed.", {"/invite alice #private"}},
    {"list", "Server", "[pattern]", "List channels",
     "Sent as typed.", {"/list", "/list *linux*"}},

    // ---- IRC operator (sent to the server; the reason/message needs no ':')
    {"oper", "IRC operator", "user [password]", "Log in as an IRC operator",
     "Without the password you are asked for it, hidden and kept out of the input history. "
     "While you are +o or +s, server notices (\"*** ...\") and WALLOPS go to a *snotices tab, "
     "and the nick menu gets Kill and G-line.",
     {"/oper gera", "/oper gera s3cret"}},
    {"kill", "IRC operator", "nick reason", "Disconnect a user from the network",
     "The user sees \"Killed (yournick (reason))\" and may reconnect; /gline keeps them out.", {"/kill troll flooding #help"}},
    {"wallops", "IRC operator", "message", "Message every user with +w (and the opers)",
     "/globops and /locops (opers only, network or this server) work the same way.",
     {"/wallops services restart in 5 minutes", "/globops checking the spam wave"}},
    {"gline", "IRC operator", "mask [duration] reason", "Ban a user@host from the whole network",
     "/kline (this server), /zline (an IP, before the connection registers) and /shun (connected "
     "but silenced) take the same arguments; /ungline, /unkline, /unzline and /unshun lift them. "
     "Durations: 0 (permanent), 3600, 30m, 1d, 1w. Without arguments lists the active lines.",
     {"/gline *@1.2.3.4 1d spam bots", "/kline *@bad.isp 0 abuse", "/ungline *@1.2.3.4", "/gline"}},
    {"sajoin", "IRC operator", "nick #channel", "Force a user into (or /sapart out of) a channel",
     "/sanick nick newnick and /samode #channel modes work the same way, ignoring channel rules.",
     {"/sajoin alice #help", "/sapart troll #help", "/samode #chan +o alice"}},
    {"chghost", "IRC operator", "nick host", "Change a user's host (/chgident: the ident)",
     "/sethost host changes your own.", {"/chghost alice staff.sekurnet.org"}},
    {"rehash", "IRC operator", "", "Reload the server configuration",
     "/die and /restart stop or restart the server. Server linking: /squit server reason, and "
     "/quote CONNECT server (iSekur's own /connect reconnects you).",
     {"/rehash", "/squit hub.example.org maintenance"}},
};

const std::vector<CmdInfo>& commands() { return CMDS; }

const CmdInfo* find_command(const std::string& name) {
    std::string n = irc_lower(name);
    for (auto& c : CMDS) {
        if (n == c.name) return &c;
        for (auto& a : split(c.aliases, ' '))
            if (n == a) return &c;
    }
    return nullptr;
}

static std::string usage(const CmdInfo& c) {
    return C_CMD BOLD "/" + std::string(c.name) + RESET + (*c.usage ? " " C_ARGS + std::string(c.usage) + RESET : "");
}

void Core::help(Buffer* b, const std::string& topic) {
    auto out = [&](const std::string& t) {
        Line l{time(nullptr), LK::INFO, "", t};
        l.nolog = true;
        add(b, l);
    };
    std::string q = topic;
    if (!q.empty() && q[0] == '/') q.erase(0, 1);
    if (q.empty()) {
        out(BOLD "iSekur commands" RESET C_DIM "  -  /help <command> for details and examples, Tab completes" RESET);
        const char* group = "";
        for (auto& c : CMDS) {
            if (std::string(group) != c.group) {
                group = c.group;
                out(C_GROUP + std::string(group) + (std::string(group) == "Server" ? RESET C_DIM " (sent to the server as typed)" : "") + RESET);
            }
            std::string plain = "/" + std::string(c.name) + (*c.usage ? " " + std::string(c.usage) : "");
            std::string pad(plain.size() < 34 ? 34 - plain.size() : 2, ' ');
            out("  " + usage(c) + pad + c.summary);
        }
        out(C_DIM "Anything else starting with / goes to the server unchanged. Type // to send a line that starts with /." RESET);
        return;
    }
    const CmdInfo* c = find_command(q);
    if (!c) {
        std::string near;
        for (auto& k : CMDS)
            if (std::string(k.name).find(irc_lower(q)) != std::string::npos) near += " /" + std::string(k.name);
        out("\x03" "04No such command: /" + q + RESET +
            (near.empty() ? " - /help lists them all. Unknown commands go to the server as typed." : ". Did you mean:" C_CMD + near + RESET "?"));
        return;
    }
    out(usage(*c) + C_DIM "  -  " RESET + c->summary);
    if (*c->aliases) {
        std::string al;
        for (auto& a : split(c->aliases, ' ')) al += (al.empty() ? "/" : ", /") + a;
        out("  " C_DIM "Also: " RESET C_CMD + al + RESET);
    }
    out("  " + std::string(c->details));
    out("  " C_GROUP "Examples" RESET);
    for (auto& e : c->examples) out("    " C_EX + std::string(e) + RESET);
}
