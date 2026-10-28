-- Saints Reborn character (built in): Body Type, movement styles and
-- personality.
--
-- patch.lua adds Body Type, seven movement rows (Stance, Walk, Run, Sprint,
-- Melee Run, Combat, Melee Combat) and three personality rows (Voice,
-- Expression, Taunts; personality.lua) to the Body set of
-- player_creation.xtbl, so the game draws them on the creator's BODY tab and
-- the Plastic Surgeon's Body page, saves them with the character and exports
-- them to multiplayer. This script:
--   - shifts Hair and Skin in sliders saved before those rows existed;
--   - keeps those rows out of the Muscle + Fat budget of the Body page (the
--     game's code takes the excess from "the other slider", written for two);
--   - keeps Randomize (X) on the Body pages off them, Muscle + Fat within
--     their budget; stops a movement row at its last style;
--   - runs the movement styles (movement_style.lua) and the personality
--     (personality.lua: voice, facial expression, D-pad taunts);
--   - draws the female shape from the slider (SR2's body-gender shape on the
--     player and all stock clothing, with breast physics), live in the creator
--     and at the surgeon too.
--
-- The female shape and its data are SR Tweak Menu 2.6.6's (00xJS, itsMJQ),
-- see "SaintsReborn New Base Assets" zip 01. game.lua, body_gender_data.lua,
-- breast_physics.lua, facial_attachments.lua and the juiced_*.lua files are
-- that mod's files unchanged; body_gender.lua is adapted (see its header).

local G = require("game")
local B = require("body_gender")
local P = require("breast_physics")
local r32, rf, w32, wf = wml.read_u32, wml.read_f32, wml.write_u32, wml.write_f32

local SETS        = 0x83C593E8  -- 11 morph sets, 44 bytes each; Body is set 0
local BODY_MAX    = SETS + 40   -- the Body set's Max_Total_Value (float, 1): Muscle + Fat at most this
local INFO_COUNT  = 0x83117BB8  -- how many sliders (morph infos, 36 bytes each at 0x831169B8)
local PAIRS_APPLY = 0x82491388  -- (pairs, count, record): saved / received {index, value} pairs onto a record
-- The budget code itself (creator 0x82300210, surgeon 0x823204C0) can't be
-- hooked (MinHook refuses those two compiled functions), so the screens'
-- input handlers that call it are wrapped instead.
local CREATOR_INPUT = 0x822FDF40  -- creator input (page): slider steps, Randomize
local SURGEON_INPUT = 0x8231C3D0  -- the surgeon's
local SLIDER_SET  = 0x823227E8  -- (player, morph info, f1 = value): queues a slider change
local CREATOR_UPDATE = 0x822FDC88 -- the creator screen's per-frame update
-- Return addresses of SLIDER_SET in the Body Randomize loops: the creator's
-- (every row: value at r30 + 4, slider at r28) and the surgeon's (Muscle,
-- then Fat: value at r31 + 4, copied to the slider after the call).
local CREATOR_RANDOMIZE = 0x822FE8E0
local SURGEON_RANDOMIZE_MUSCLE, SURGEON_RANDOMIZE_FAT = 0x82320864, 0x82320880

local debug = wml.setting("debug", false)
local function log(...) wml.log(...) end
local logged = {}
local function log_once(key, ...) if not logged[key] then logged[key] = true; wml.log(...) end end

-- ------------------------------------------------------------ the rows
local ROWS = require("body_rows")
local function body_type_info() return ROWS.info(ROWS.BODY_TYPE) end
local function slider_value(p) return ROWS.value(p, ROWS.BODY_TYPE) end
local MOVE = nil   -- movement_style.lua, loaded below
local MENU = require("movement_menu")
local LISTS = require("row_lists")
-- Files from two versions (an update cut short, or a synced folder such as
-- OneDrive bringing an older file back): af8e127's main.lua with a
-- movement_menu.lua that has no SCREENS hooked the creator and then failed,
-- so every key in the creator ran into "attempt to index a nil value (field
-- 'SCREENS')" and the creator stopped working until a reinstall. Now nothing
-- is hooked: the creator works without this mod's extras, and the log says
-- what to do.
if type(MENU) ~= "table" or type(MENU.SCREENS) ~= "table" or not MENU.SCREENS.creator
    or not MENU.SCREENS.surgeon then
  wml.log("Saints Reborn character: movement_menu.lua is from another version than main.lua - mod off, "
          .. "the creator works without it. Run SaintsReborn-Updater to repair the files.")
  return
end
local PERS = nil  -- personality.lua, loaded below

-- ------------------------------------------------------------ older saves
-- Saves (and multiplayer appearance) keep {index, value} pairs, as many as
-- there were sliders. A shorter list was made before the new Body rows: its
-- last three sliders (Hair, Skin x2) move up by as many as were added (70
-- before Body Type, 71 before the movement rows, 78 before personality). The shifted copy goes in
-- stack space below the caller (the game's function is a leaf), so the save
-- in memory is never changed and can be applied again.
G.hook(PAIRS_APPLY, function(ctx)
  local pairs_at, n = ctx:r(3) & 0xFFFFFFFF, ctx:r(4) & 0xFFFFFFFF
  local total = r32(INFO_COUNT)
  if body_type_info() and G.pointer_ok(pairs_at) and (n == 70 or n == 71 or n == 78) and n < total and total <= 128 then
    local copy = (ctx:r(1) & 0xFFFFFFFF) - 0x1000
    local first, by = n - 3, total - n
    for i = 0, n - 1 do
      local k = r32(pairs_at + i * 8)
      w32(copy + i * 8, k >= first and k + by or k)
      w32(copy + i * 8 + 4, r32(pairs_at + i * 8 + 4))
    end
    ctx:set_r(3, copy)
    log_once("old_save" .. n, string.format("Body page: a character saved with %d sliders - Hair and Skin moved up by %d", n, by))
  end
  ctx:call_original()
end)

-- ------------------------------------------------------------ Body page budget
-- Raising Muscle or Fat past what's left of Max_Total_Value takes the excess
-- from the other one, found as row (1 - this row): the game's code handles
-- two rows. While a screen's input runs with one of our rows selected the
-- budget is lifted; with Muscle or Fat selected it's raised by our rows'
-- values, so only those two count. (The budget code itself, 0x82300210 /
-- 0x823204C0, can't be hooked: MinHook refuses those two functions.)
local function with_budget(ctx, page, row, values, count)
  local before = rf(BODY_MAX)
  if row >= 2 then
    wf(BODY_MAX, 1000)
  else
    local extra = 0
    for k = 2, count - 1 do extra = extra + rf(page + values + 8 * k) end
    wf(BODY_MAX, before + extra)
  end
  local ok, err = pcall(function() ctx:call_original() end)
  wf(BODY_MAX, before)
  if not ok then error(err, 0) end
end
-- The Body page being shown and its selected row, for the movement preview.
local shown = { page = 0, row = -1, at = 0 }
local frame = 0
-- Both screens with a Body page (movement_menu.lua, Menu.SCREENS): the
-- character creation / Plastic Surgeon screen (tab 4 = Body, rows at +128,
-- how many at +288, row at +124, page +20 = 0) and the older creator screen
-- (tab 3, rows at +2120, +2440, +188, page +16 = 0). Their input:
--   - a movement style list open: its input instead of the game's;
--   - A on a movement row: opens its list;
--   - otherwise the game's, with the Muscle + Fat budget as above.
local function body_input(ctx, S, max_rows)
  local page = ctx:r(3) & 0xFFFFFFFF
  local info = body_type_info()
  if info and G.pointer_ok(page) and r32(page + 8) == S.tab and r32(page + S.sub) == 0
      and r32(page + S.count) >= 3 and r32(page + S.slots + 16) == info then
    local count = math.min(r32(page + S.count), max_rows)
    if MENU.input(ctx, page, S) or (count >= ROWS.ROWS and MENU.maybe_start(ctx, page, S)) then
      shown.page, shown.row, shown.at = page, MENU.previewing() or -1, frame
      return
    end
    shown.page, shown.row, shown.at = page, r32(page + S.row), frame
    return with_budget(ctx, page, r32(page + S.row), S.slots + 4, count)
  end
  ctx:call_original()
end
G.hook(SURGEON_INPUT, function(ctx) body_input(ctx, MENU.SCREENS.surgeon, 20) end)
G.hook(CREATOR_INPUT, function(ctx) body_input(ctx, MENU.SCREENS.creator, 40) end)

-- The Body page builders: only Muscle, Fat and Body Type as slider rows, the
-- movement rows as plain menu rows after them (movement_menu.lua). A builder
-- that can't be hooked leaves that screen's movement rows as sliders.
local function body_build(S)
  return function(ctx)
    local page = ctx:r(3) & 0xFFFFFFFF
    local n = r32(SETS + 4)
    if n < ROWS.ROWS or not G.pointer_ok(page) or MENU.open then return ctx:call_original() end
    w32(SETS + 4, ROWS.FIRST_MOVE)
    local ok, err = pcall(function() ctx:call_original() end)
    w32(SETS + 4, n)
    if not ok then error(err, 0) end
    local ok2, err2 = pcall(MENU.add_rows, ctx, page, S)
    if not ok2 then log_once("add_rows", "Movement rows: " .. tostring(err2)) end
    log_once("built" .. S.tab, "Body page: movement rows shown as menu rows")
  end
end
for address, S in pairs({ [0x8231FCE0] = MENU.SCREENS.surgeon, [0x822FFCA8] = MENU.SCREENS.creator }) do
  if not G.hook(address, body_build(S)) then
    log(string.format("Body page: the builder %08X couldn't be hooked - movement rows stay sliders there", address))
  end
end

-- ------------------------------------------------------------ slider changes
-- Every slider change is queued with SLIDER_SET (player, morph info, f1).
--  - Body Randomize draws Muscle in [0, budget] and Fat in [0, budget -
--    Muscle], the budget being Max_Total_Value as raised above: those draws
--    are scaled back into the real budget (1). The creator's also does every
--    further row: our rows keep their values. The row shown gets the value.
--  - A movement row stops at its last style (the slider goes on to 1).
local drawn = {}
local CREATOR_STEP, SURGEON_STEP = 0x82300348, 0x82320630   -- return addresses of a slider step's call
G.hook(SLIDER_SET, function(ctx)
  local lr = ctx:lr() & 0xFFFFFFFF
  local target = ctx:r(4) & 0xFFFFFFFF
  local infos = r32(SETS + 8)
  if not body_type_info() or not G.pointer_ok(infos) then return ctx:call_original() end
  local ours = ROWS.row_of(target)
  -- a movement or personality row past its last choice
  if ours and ours >= ROWS.FIRST_MOVE and (lr == CREATOR_STEP or lr == SURGEON_STEP) then
    local most = LISTS.choices(ours) / ROWS.STEP
    if ctx:f(1) > most + 1e-4 then
      local page = ctx:r(31) & 0xFFFFFFFF
      if G.pointer_ok(page) then
        if lr == CREATOR_STEP then
          local row = r32(page + 188)
          wf(page + 2124 + 8 * row, most); wf(page + 2460 + 32 * row, most)
        else
          local row = r32(page + 124)
          wf(page + 132 + 8 * row, most); wf(page + 472 + 32 * row, most)
        end
      end
      ctx:set_f(1, most)
    end
    return ctx:call_original()
  end
  local creator = lr == CREATOR_RANDOMIZE
  if not creator and lr ~= SURGEON_RANDOMIZE_MUSCLE and lr ~= SURGEON_RANDOMIZE_FAT then
    return ctx:call_original()
  end
  local raw, v = ctx:f(1), nil
  local budget = rf(BODY_MAX)
  if ours then
    if not creator then return ctx:call_original() end
    v = ROWS.value(G.player(), ours)
  elseif target == infos then           -- Muscle
    -- (the creator draws Muscle from the budget, the surgeon from 0..1)
    drawn.muscle_raw = raw
    v = (creator and budget > 0) and math.min(1, raw / budget) or math.min(1, raw)
    drawn.muscle = v
  elseif target == infos + 36 then      -- Fat
    local room_raw = budget - (drawn.muscle_raw or 0)
    local room = math.max(0, 1 - (drawn.muscle or 0))
    v = room_raw > 0 and math.min(room, raw / room_raw * room) or math.min(room, raw)
  else
    return ctx:call_original()
  end
  if creator then
    local row, slider = ctx:r(30) & 0xFFFFFFFF, ctx:r(28) & 0xFFFFFFFF
    if G.pointer_ok(row) then wf(row + 4, v) end
    if G.pointer_ok(slider) then wf(slider, v) end
  else
    local row = ctx:r(31) & 0xFFFFFFFF
    if G.pointer_ok(row) then wf(row + 4, v) end
  end
  if ours then return end    -- Body Type and the movement rows keep their values
  ctx:set_f(1, v)
  ctx:call_original()
end)

-- ------------------------------------------------------------ the shape
-- Only on your own character model (StyleTest_PC): a story character or
-- other model from a mod keeps its own body.
local function own_model(p)
  local def = r32(p + G.HUMAN_DEF)
  if not G.pointer_ok(def) then return false end
  local name = r32(def + G.D.NAME)
  if not G.pointer_ok(name) then return false end
  local s = {}
  for i = 0, 15 do
    local c = wml.read_u8(name + i)
    if c == 0 then break end
    s[#s + 1] = string.char(c)
  end
  return table.concat(s):lower() == "styletest_pc"
end
B.enabled = function()
  local p = G.player()
  return p ~= 0 and own_model(p)
end
B.inputs = function(p)
  -- Muscle and Fat as baked into the body (+8), the corners the female
  -- shape is blended over.
  local record = r32(p + G.CUSTOM)
  local sliders = G.pointer_ok(record) and r32(record + 4) or 0
  local infos = r32(SETS + 8)
  if not G.pointer_ok(sliders) or not G.pointer_ok(infos) then return 0, 0 end
  local function baked(row)
    local v = rf(sliders + r32(infos + row * 36 + 24) * 12 + 8)
    return v == v and v or 0
  end
  return baked(1), baked(0)   -- fat, muscle
end

-- The draw hook that hands the female shape and the facial expression to the
-- player's meshes is installed by facial_expressions.lua (through
-- personality.lua).
MOVE = require("movement_style")
PERS = require("personality")

local last_tick, last_value = -1, nil
wml.on_frame(function() frame = frame + 1 end)
local function tick(ctx)
  -- once per frame, from whichever update comes first
  if frame == last_tick then return end
  last_tick = frame
  local p = G.player()
  local value = -50 * slider_value(p)
  B.set_value(value)
  B.update(ctx, p)
  P.update(p, B.value, p ~= 0 and G.vehicle_object(p) == nil)
  MOVE.override_row, MOVE.override_choice = MENU.previewing()
  MOVE.tick(ctx, p, p ~= 0 and own_model(p), shown, B.in_creator(), frame)
  -- (previews on the creator, and on the surgeon's Body page while it's up)
  local screen = B.in_creator() or (shown.page ~= 0 and frame - shown.at <= 30)
  local prow, pchoice = MENU.previewing()
  PERS.tick(ctx, p, p ~= 0 and own_model(p), screen, prow, pchoice, frame)
  if debug and value ~= last_value then
    last_value = value
    log(string.format("Body Type %.0f%% female (%s)%s", -value * 2, B.note(), B.in_creator() and ", in the creator" or ""))
  end
end

G.hook(G.UPDATE, function(ctx)
  ctx:call_original()
  local ok, err = pcall(tick, ctx)
  if not ok then log_once("tick", "Body Type: " .. tostring(err)) end
end)
-- The creator screen updates every frame itself (if the game's update pauses
-- behind it, the shape still follows the slider there).
G.hook(CREATOR_UPDATE, function(ctx)
  ctx:call_original()
  local ok, err = pcall(tick, ctx)
  if not ok then log_once("tick_creator", "Body Type (creator): " .. tostring(err)) end
end)

log("Saints Reborn character: Body Type, movement styles and personality ready")
