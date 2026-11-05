-- version: 1.0
-- awaylog: while you're away (or nothing is attached), it keeps your highlights and private messages.
-- When you're back, a summary in the tab you're on: "While you were away (2h13m): 3 mentions in #isekur,
-- 2 PMs from bob", then the lines. /awaylog shows the last one again, /awaylog clear forgets it.
isekur.setting("show", 10, "g")         -- lines in the summary (the rest: /awaylog)
isekur.setting("keep", 200, "g")        -- lines kept at most
isekur.setting("detached", true, "g")   -- also while nothing is attached (--daemon, browser closed), not only while away

-- ponytail: in memory only, a restart of iSekur forgets it; the store if that ever matters
local log, gone_at = {}, nil   -- collecting now; gone_at: when you went away (seen by the tick)
local last, last_from, last_to = {}, 0, 0   -- the last summary's lines, gone and back, for /awaylog

local function present()
  if isekur.opt("detached") and not isekur.attached() then return false end
  for _, srv in ipairs(isekur.servers()) do
    if isekur.away(srv) then return false end
  end
  return true
end

local function dur(s)
  if s < 60 then return s .. "s" end
  if s < 3600 then return math.floor(s / 60) .. "m" end
  local h, m = math.floor(s / 3600), math.floor(s % 3600 / 60)
  if h < 48 then return h .. "h" .. (m > 0 and m .. "m" or "") end
  return math.floor(h / 24) .. "d"
end

local function plural(n, word) return n .. " " .. word .. (n == 1 and "" or "s") end

-- the head line: who and where, in the order they first came
local function head(lines, from, to)
  local count, order, nets = {}, {}, {}
  for _, l in ipairs(lines) do nets[l.srv] = true end
  local multi = next(nets) and next(nets, next(nets)) ~= nil
  for _, l in ipairs(lines) do
    local k = l.srv .. "\t" .. (l.chan and l.target or l.nick)
    if not count[k] then count[k] = 0; order[#order + 1] = {k, l} end
    count[k] = count[k] + 1
  end
  local parts = {}
  for _, o in ipairs(order) do
    local k, l = o[1], o[2]
    local where = (l.chan and plural(count[k], "mention") .. " in " .. l.target or plural(count[k], "PM") .. " from " .. l.nick)
    parts[#parts + 1] = where .. (multi and " on " .. l.srv or "")
  end
  return "While you were away (" .. dur(to - from) .. "): " .. table.concat(parts, ", ")
end

local function line(l)
  local who = l.action and ("* " .. l.nick .. " ") or ("<" .. l.nick .. "> ")
  return "  " .. isekur.date(l.t) .. "  " .. (l.chan and l.target .. " " or "") .. who .. l.text
end

local function show(lines, from, to, max, srv, name)
  isekur.print(srv, name, head(lines, from, to), true)
  local first = math.max(1, #lines - max + 1)
  if first > 1 then isekur.print(srv, name, "  … " .. (first - 1) .. " earlier: /awaylog") end
  for i = first, #lines do isekur.print(srv, name, line(lines[i])) end
end

isekur.on("message", function(e)
  if e.self or not e.hilight or present() then return end
  log[#log + 1] = {t = isekur.time(), srv = e.srv, target = e.target, nick = e.nick, text = e.text,
                   chan = isekur.is_chan(e.srv, e.target), action = e.action}
  local keep = isekur.opt("keep")
  while #log > keep do table.remove(log, 1) end
end)

-- back: the summary, once, in the tab you're looking at
isekur.on("tick", function()
  if not present() then gone_at = gone_at or isekur.time(); return end
  local from = gone_at
  gone_at = nil
  if #log == 0 then return end
  last, last_from, last_to, log = log, from or log[1].t, isekur.time(), {}
  local srv, name = isekur.active()
  show(last, last_from, last_to, isekur.opt("show"), srv, name)
end)

isekur.command("awaylog", "[clear]", function(args, srv, target)
  if args == "clear" then last = {}; isekur.print(srv, target, "awaylog: forgotten"); return end
  if args ~= "" then return "Usage: /awaylog [clear]" end
  if #log > 0 then show(log, gone_at or log[1].t, isekur.time(), #log, srv, target); return end  -- still away: what came so far
  if #last == 0 then isekur.print(srv, target, "awaylog: nothing came while you were away"); return end
  show(last, last_from, last_to, #last, srv, target)
end)
