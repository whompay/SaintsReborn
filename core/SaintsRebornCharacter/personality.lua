-- Saints Reborn character (built in): personality, chosen on the Body page of
-- character creation and the Plastic Surgeon (CONCEPTS.md step 4), story mode.
--
--   Voice       every gang voice the game has, men's or women's by Body Type
--               (SR Tweak Menu 2.6.6, zip 05): your character's lines, grunts
--               and screams in that voice. Default = your race's own.
--   Expression  the face your character wears: SR1's six emotions and the 24
--               SR2 faces baked onto the SR1 head (zip 02; facial_expressions.lua,
--               banks in juiced_runtime/). Default = none.
--   Taunts      D-pad taunts the way multiplayer does them (zip 09): down, left
--               and right play a taunt with a voice line 3 times in 4, standing
--               still. Off = the D-pad switches weapons as usual.
--
-- The choices are Body page sliders (patch.lua, rows 10-12; body_rows.lua):
-- 0 = default / none / off, each slider step (1/30) the next choice, saved
-- with the character. The rows show the choice by name ("Voice: Hispanic
-- Male 2"): each row's shown name (morph info +4) points at a text of ours.
-- A on a row opens the list (movement_menu.lua, row_lists.lua); the cursor
-- previews each choice on your character: a sample line in that voice, the
-- face, the first taunt of the set.
local G = require("game")
local ROWS = require("body_rows")
local LISTS = require("row_lists")
local E = require("facial_expressions")
local R = require("juiced_runtime")
local r8, r32, w16, w32, rf = wml.read_u8, wml.read_u32, wml.write_u16, wml.write_u32, wml.read_f32
local MASK = 0xFFFFFFFF

local P = { frame = 0, own = false, screen = false }
local VOICE, FACE, TAUNTS = ROWS.FIRST_PERSONALITY, ROWS.FIRST_PERSONALITY + 1, ROWS.FIRST_PERSONALITY + 2
local debug = wml.setting("debug", false)
local logged = {}
local function log_once(key, text) if not logged[key] then logged[key] = true; wml.log(text) end end

local function cstring(a, max)
  if not G.pointer_ok(a) then return nil end
  local chars = {}
  for i = 0, (max or 48) - 1 do
    local c = r8(a + i)
    if c == 0 then break end
    if c < 32 or c > 126 then return nil end
    chars[#chars + 1] = string.char(c)
  end
  return table.concat(chars)
end

-- ------------------------------------------------------------ the lists
-- Voices (zip 05's list): by the creator's races, each race's voices numbered
-- across the gangs that have them (Vice Kings, Los Carnales, Rollerz, Saints),
-- three each. 30 for men, 30 for women - one slider each, picked by Body Type.
local RACES = { { "B", "African American" }, { "W", "Caucasian" }, { "A", "Asian" }, { "H", "Hispanic" } }
local GANG_RACES = { VK = "BW", LC = "HW", WR = "AW", ["3SS"] = "BWAH" }
local VOICES = { M = {}, F = {} }
for _, sex in ipairs({ "M", "F" }) do
  for _, race in ipairs(RACES) do
    local n = 0
    for _, gang in ipairs({ "VK", "LC", "WR", "3SS" }) do
      if GANG_RACES[gang]:find(race[1], 1, true) then
        for k = 1, 3 do
          n = n + 1
          local list = VOICES[sex]
          list[#list + 1] = { race[1] .. sex .. "_" .. gang .. k,
            string.format("%s %s %d", race[2], sex == "M" and "Male" or "Female", n) }
        end
      end
    end
  end
end
local function female(p) return ROWS.value(p, ROWS.BODY_TYPE) >= 0.5 end
local function voice_list(p) return VOICES[female(p) and "F" or "M"] end

-- Faces: SR1's six emotions, then the SR2 faces (juiced_faces.lua; Blank is
-- the same as none). A name both have gets a number ("Angry 2").
local FACES = {}
for _, name in ipairs({ "Angry", "Disgust", "Fear", "Joy", "Sad", "Surprise" }) do FACES[#FACES + 1] = { name, name } end
do
  local seen = {}
  for _, f in ipairs(FACES) do seen[f[2]:lower()] = 1 end
  for _, clip in ipairs(require("juiced_faces").clips) do
    if clip.id ~= "Juiced_blank" and #FACES < ROWS.STEP then
      local key = clip.label:lower()
      seen[key] = (seen[key] or 0) + 1
      FACES[#FACES + 1] = { clip.id, seen[key] > 1 and (clip.label .. " " .. seen[key]) or clip.label }
    end
  end
end

-- Taunt sets: D-pad down, left, right = { action, voice lines }.
local SETS = {
  { "Trash Talk", {
    [0x2] = { "Taunt 2", { "threat - knocked down NPC/PC" } },
    [0x4] = { "compliment", { "combat - congratulate self", "combat - congratulate player", "voice - cheer" } },
    [0x8] = { "interaction gang sign stand 2", { "voice - cheer", "observe - PC (flash gang sign rank 2)", "misc - walk by ambient" } } } },
  { "Gang Signs", {
    [0x2] = { "interaction gang sign stand 1", { "observe - PC (flash gang sign rank 2)" } },
    [0x4] = { "interaction gang sign stand 3", { "voice - cheer" } },
    [0x8] = { "interaction gang sign stand 4", { "observe - PC (flash gang sign rank 2)", "misc - walk by ambient" } } } },
  { "Multiplayer", {
    [0x2] = { "Taunt 1", { "threat - knocked down NPC/PC" } },
    [0x4] = { "Taunt 2", { "combat - congratulate self" } },
    [0x8] = { "Taunt 3", { "voice - cheer" } } } },
}

LISTS.providers[VOICE] = function()
  local names = { [0] = "Default (your own)" }
  for i, v in ipairs(voice_list(G.player())) do names[i] = v[2] end
  return { title = "Voice", names = names }
end
LISTS.providers[FACE] = function()
  local names = { [0] = "None" }
  for i, f in ipairs(FACES) do names[i] = f[2] end
  return { title = "Facial Expression", names = names }
end
LISTS.providers[TAUNTS] = function()
  local names = { [0] = "Off (D-pad switches weapons)" }
  for i, s in ipairs(SETS) do names[i] = s[1] end
  return { title = "D-Pad Taunts", names = names }
end

-- ------------------------------------------------------------ script calls
-- Run from the update with its context; the game's own script functions.
local function script(ctx, code)
  local ok = G.script(ctx, code)
  if debug then wml.log("Personality: " .. code .. (ok and "" or " (failed)")) end
  return ok
end
local action_numbers = {}
local function action_number(ctx, name)
  local n = action_numbers[name]
  if n == nil then
    n = G.call(ctx, G.ACTION_INDEX, { G.cstr(ctx, name, 0) })
    if n == MASK then log_once("action" .. name, "Personality: no action '" .. name .. "'") end
    action_numbers[name] = n ~= MASK and n or false
  end
  return n or nil
end
local function play_taunt(ctx, p, t, line_chance)
  local n = action_number(ctx, t[1])
  if not n then return false end
  local ok = G.call(ctx, G.ACTION_PLAY, { p, n, 0, 1, 0, 0, 0, MASK }, { 0.0, 1.0, -1.0 })
  if (ok & 0xFF) == 0 then return false end
  if math.random() < line_chance then
    script(ctx, string.format('audio_play_persona_line_do("#PLAYER#", "%s", -1)', t[2][math.random(1, #t[2])]))
  end
  return true
end

-- ------------------------------------------------------------ voice
-- The player's own voice (SP_BMPLAYER ...) is remembered whenever it's on, so
-- Default puts it back (also after the race changes in the creator).
local V = { own_ptr = nil, applied = nil, next_check = 0, sample = nil, sample_at = 0 }
local function persona_now(p) return cstring(r32(p + G.PERSONA_NOW), 24) or "" end
local function set_voice(ctx, p, name)
  if name then
    script(ctx, string.format('human_set_persona("#PLAYER#", "%s")', name))
  elseif V.own_ptr then
    w32(p + G.PERSONA_NOW, V.own_ptr)
  end
  V.applied = name
end
local SAMPLE_LINES = { "combat - congratulate self", "threat - alert (group attack)", "taunt - fleeing enemy" }
local sample_n = 0
local function voice_tick(ctx, p, preview)
  local now = persona_now(p)
  if now:find("^SP_%u%uPLAYER$") then V.own_ptr = r32(p + G.PERSONA_NOW) end
  local list = voice_list(p)
  if preview then
    -- the list is open: the voice under the cursor, with a line, once it rests there
    local want = preview > 0 and list[preview] and list[preview][1] or nil
    if V.sample ~= preview then V.sample, V.sample_at = preview, P.frame + 12 end
    if V.sample_at and P.frame >= V.sample_at then
      V.sample_at = nil
      set_voice(ctx, p, want)
      sample_n = sample_n % #SAMPLE_LINES + 1
      script(ctx, string.format('audio_play_persona_line_do("#PLAYER#", "%s", -1)', SAMPLE_LINES[sample_n]))
    end
    return
  end
  V.sample = nil
  local c = ROWS.choice(p, VOICE)
  local want = c > 0 and list[c] and list[c][1] or nil
  -- (checked every second: a respawn, a cutscene or the creator's race
  -- change gives the player their own voice back)
  if want ~= V.applied then
    V.next_check = P.frame + 50
    set_voice(ctx, p, want)
  elseif want and P.frame >= V.next_check then
    V.next_check = P.frame + 50
    if now ~= want then set_voice(ctx, p, want) end
  end
end

-- ------------------------------------------------------------ face
local F = { key = nil, retry = 0 }
local function face_name(c) return c > 0 and FACES[c] and FACES[c][1] or nil end
local function face_tick(ctx, p, preview)
  local name = preview and face_name(preview) or face_name(ROWS.choice(p, FACE))
  local key = (name or "None") .. (preview and "+" or "")
  -- an SR2 face needs its banks registered for this body (a respawn makes a new one)
  local stale = name and E.names[name] and require("juiced_player").names[name] and not R.owns(p)
  if (F.key ~= key or stale) and P.frame >= F.retry then
    local ok, why = E.request(ctx, name or "None", preview ~= nil)
    if ok then
      F.key = key
    else
      F.key, F.retry = nil, P.frame + 100
      log_once("face" .. tostring(why), "Expression: " .. tostring(why))
    end
  end
  E.tick(face_name(ROWS.choice(p, FACE)) or "None")
  if not preview then E.preview = nil end
end

-- ------------------------------------------------------------ taunts
-- From the controller (any thread): new presses of D-pad down / left / right
-- while a set is chosen and you're playing on foot; the game doesn't see them.
local T = { set = 0, prev = 0, pending = nil, pos = nil, playing = false, preview = nil, preview_at = nil }
G.hook(G.INPUT_GET_STATE, function(ctx)
  local user, state = ctx:r(3) & MASK, ctx:r(5) & MASK
  ctx:call_original()
  if user ~= 0 or state == 0 or (ctx:r(3) & MASK) ~= 0 then return end
  local buttons = wml.read_u16(state + 4)
  -- B (Q on keyboard) opens the weapon/food radial; its D-pad selects food.
  -- Remember held directions so closing it cannot turn a food press into a taunt.
  if (buttons & 0x2000) ~= 0 or T.set == 0 or not T.playing then
    T.prev, T.pending = buttons, nil
    return
  end
  -- Vehicles use these buttons for the radio. Check the current player here
  -- because input can be polled between the character update ticks.
  local p = G.player()
  if p == 0 or G.vehicle_object(p) then
    T.prev, T.pending = buttons, nil
    return
  end
  local hide = buttons & 0xE
  for bit = 2, 8, 2 do
    if bit ~= 6 and (buttons & bit) ~= 0 and (T.prev & bit) == 0 then T.pending = bit end
  end
  T.prev = buttons
  if hide ~= 0 then
    w16(state + 4, buttons & ~hide & 0xFFFF)
    w32(state, r32(state) + 1)
  end
end)
local function taunts_tick(ctx, p, preview)
  local set = ROWS.choice(p, TAUNTS)
  T.set = SETS[set] and set or 0
  if preview then
    -- the list is open: the set's first taunt (D-pad down), once the cursor rests
    if T.preview ~= preview then T.preview, T.preview_at = preview, P.frame + 15 end
    if T.preview_at and P.frame >= T.preview_at then
      T.preview_at = nil
      local s = SETS[preview]
      if s then play_taunt(ctx, p, s[2][0x2], 1) end
    end
    return
  end
  T.preview = nil
  -- standing still only: a taunt while running plays on the spot while you slide
  local x, _, z = G.vec(p + G.P.POS)
  local moving = T.pos ~= nil and ((x - T.pos[1]) ^ 2 + (z - T.pos[2]) ^ 2) > 0.0004
  T.pos = { x, z }
  local bit = T.pending
  T.pending = nil
  if not bit or T.set == 0 or not T.playing or moving then return end
  if r32(p + G.H_MELEE_MOVE) ~= MASK or G.vehicle_object(p) then return end
  play_taunt(ctx, p, SETS[T.set][2][bit], 0.75)
end

-- ------------------------------------------------------------ row names
local SHORT = { [VOICE] = "Voice", [FACE] = "Expression", [TAUNTS] = "Taunts" }
local LABEL_BYTES = 128
local labels = { block = nil, text = {} }
local function update_labels(ctx, p)
  if not labels.block then
    local block = G.call(ctx, 0x8271B570, { 0, LABEL_BYTES * 3, 0x3000, 4 })
    if not G.pointer_ok(block) then return end
    labels.block = block
  end
  for row = VOICE, TAUNTS do
    local info = ROWS.info(row)
    if info then
      local list = LISTS.get(row)
      local c = p ~= 0 and math.min(ROWS.choice(p, row), list and #list.names or 0) or 0
      local name = list and list.names[c] or ""
      if row == VOICE and c == 0 then name = "Default" end
      if row == TAUNTS and c == 0 then name = "Off" end
      local text = SHORT[row] .. ": " .. name
      local at = labels.block + (row - VOICE) * LABEL_BYTES
      if labels.text[row] ~= text then
        local n = math.min(#text, LABEL_BYTES // 2 - 1)
        for i = 1, n do w16(at + (i - 1) * 2, text:byte(i)) end
        w16(at + n * 2, 0)
        labels.text[row] = text
      end
      if r32(info + 4) ~= at then w32(info + 4, at) end
    end
  end
end

-- ------------------------------------------------------------ every update
-- own: the player is on their own character model (not a story character).
-- screen: the creator or the surgeon's Body page is up (previews work there).
-- preview_row / preview_choice: what an open list's cursor is on.
E.enabled = function() return P.own end
E.screen_ok = function() return P.screen end
R.screen_ok = function() return P.screen end
function P.tick(ctx, p, own, screen, preview_row, preview_choice, frame)
  P.frame, P.own, P.screen = frame, own, screen
  if ROWS.count() < ROWS.ROWS then return end
  update_labels(ctx, p)
  if p == 0 or not own then
    T.playing = false
    E.tick("None")
    return
  end
  T.playing = not G.in_front_end() and not G.screen_up() and not G.in_cutscene() and rf(p + G.P.HEALTH) > 0
  local function pv(row) return preview_row == row and preview_choice or nil end
  -- (worn facial pieces' banks load here, one per update, as the draws ask for them)
  pcall(require("juiced_player").update, ctx, p)
  local ok, err = pcall(voice_tick, ctx, p, pv(VOICE))
  if not ok then log_once("voice", "Voice: " .. tostring(err)) end
  ok, err = pcall(face_tick, ctx, p, pv(FACE))
  if not ok then log_once("face", "Expression: " .. tostring(err)) end
  ok, err = pcall(taunts_tick, ctx, p, pv(TAUNTS))
  if not ok then log_once("taunts", "Taunts: " .. tostring(err)) end
end

return P
