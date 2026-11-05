-- version: 1.1
-- seen: /seen nick says when and where a nick was last seen: talking, joining, leaving, quitting, renaming.
-- It remembers what happens in the channels where it's on. With reply on in a channel
-- (/plugin set seen@network/#channel reply on), !seen nick there gets an answer too.
isekur.setting("reply", false, "c", "answer !seen nick in this channel (once every 10 seconds at most)")
isekur.setting("max", 5000, "g", "nicks remembered, all networks together; the oldest go first")

-- the store: "<network>\t<nick lowercased>" -> "<time>\t<nick>\t<channel or empty>\t<what they did>"
local function key(srv, nick) return srv .. "\t" .. nick:lower() end
local count = #isekur.store_keys()
local answered = {}   -- network\tchannel -> when !seen was last answered there

local function prune()
  local max = isekur.opt("max")
  if count <= max * 1.1 then return end   -- a little slack: not a full sort on every new nick
  local all = {}
  for _, k in ipairs(isekur.store_keys()) do
    all[#all + 1] = {k, tonumber(isekur.store_get(k):match("^(%d+)")) or 0}
  end
  table.sort(all, function(a, b) return a[2] < b[2] end)
  for i = 1, #all - max do isekur.store_set(all[i][1], nil) end
  count = math.min(#all, max)
end

local function put(srv, nick, chan, what)
  local k = key(srv, nick)
  if not isekur.store_get(k) then count = count + 1 end
  isekur.store_set(k, isekur.time() .. "\t" .. nick .. "\t" .. (chan or "") .. "\t" .. what)
  prune()
end

local function get(k)
  local v = k and isekur.store_get(k)
  if not v then return nil end
  local t, nick, chan, what = v:match("^(%d+)\t([^\t]*)\t([^\t]*)\t(.*)$")
  return t and {t = tonumber(t), nick = nick, chan = chan, what = what, srv = k:match("^([^\t]*)")}
end

local function ago(s)
  if s < 60 then return s .. "s" end
  if s < 3600 then return math.floor(s / 60) .. "m" end
  local h, m = math.floor(s / 3600), math.floor(s % 3600 / 60)
  if h < 48 then return h .. "h" .. (m > 0 and m .. "m" or "") end
  return math.floor(h / 24) .. "d"
end

local function short(text) return #text > 120 and text:sub(1, 117) .. "..." or text end

-- the answer. public (!seen): only what happened in the asking channel is told in full
local function answer(srv, nick, from_chan)
  if srv and isekur.me(srv) and isekur.me(srv):lower() == nick:lower() then return "That's you" end
  local here = {}
  for _, ch in ipairs(srv and isekur.channels(srv) or {}) do
    if isekur.rank(srv, ch, nick) and (not from_chan or ch:lower() == from_chan:lower()) then here[#here + 1] = ch end
  end
  if #here > 0 then return nick .. " is here now, in " .. table.concat(here, ", ") end
  local r = get(srv and key(srv, nick))
  if not r and not from_chan then  -- the tab's network doesn't know them: the newest on any other
    local want = "\t" .. nick:lower()
    for _, k in ipairs(isekur.store_keys()) do
      if k:sub(-#want) == want then
        local o = get(k)
        if o and (not r or o.t > r.t) then r = o end
      end
    end
  end
  if not r then return "I haven't seen " .. nick end
  local age = isekur.time() - r.t
  local s = r.nick .. " was last seen " .. ago(age) .. " ago (" .. isekur.date(r.t, age < 6 * 86400 and "%a %H:%M" or "%b %d %Y") .. ")"
  if r.srv ~= srv then s = s .. " on " .. r.srv end
  if from_chan and r.chan:lower() ~= from_chan:lower() then return s end  -- elsewhere: when, not where or what
  return s .. ", " .. r.what
end

isekur.on("message", function(e)
  if e.self or e.notice or not isekur.is_chan(e.srv, e.target) then return end
  local q = e.text:match("^!seen%s+(%S+)")
  if q and isekur.opt("reply", e.srv, e.target) then
    local k = e.srv .. "\t" .. e.target:lower()  -- one answer per 10s per channel: no flooding by asking
    if (answered[k] or 0) + 10 <= isekur.time() then
      answered[k] = isekur.time()
      isekur.say(e.srv, e.target, answer(e.srv, q, e.target))
    end
  end
  put(e.srv, e.nick, e.target, (e.action and "doing: * " .. e.nick .. " " or "saying: ") .. short(e.text) .. " in " .. e.target)
end)

isekur.on("join", function(e) if not e.self then put(e.srv, e.nick, e.target, "joining " .. e.target) end end)
isekur.on("part", function(e)
  if e.self then return end
  local why = e.reason ~= "" and " (" .. short(e.reason) .. ")" or ""
  put(e.srv, e.nick, e.target, (e.kick and "being kicked from " .. e.target .. " by " .. e.by or "leaving " .. e.target) .. why)
end)
isekur.on("quit", function(e) if not e.self then put(e.srv, e.nick, nil, "quitting" .. (e.reason ~= "" and " (" .. short(e.reason) .. ")" or "")) end end)
isekur.on("nick", function(e)
  if e.self then return end
  put(e.srv, e.nick, nil, "changing nick to " .. e.new)
  put(e.srv, e.new, nil, "changing nick from " .. e.nick)
end)

isekur.command("seen", "nick", function(args, srv, target)
  local nick = args:match("^%s*(%S+)")
  if not nick then return "Usage: /seen nick" end
  isekur.print(srv, target, answer(srv, nick))
end)
