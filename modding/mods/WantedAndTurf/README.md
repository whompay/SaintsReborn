# Wanted & Turf

A native WML mod menu for heat, turf and playa allegiance. **F6** opens it; the
number keys (and P) act while it is open.

| Key | Action |
|---|---|
| 1 | Next faction (Police, West Side Rollerz, Los Carnales, Vice Kings) |
| 2 / 3 | Stars -1 / +1 (0-5) |
| 4 | Apply that many stars to the selected faction |
| 5 | Lock / unlock the selected faction at the chosen number of stars |
| 6 | Clear every wanted level and release all locks |
| 0 | Cycle playa allegiance (Civilian, Police, Saints, Vice Kings, Rollerz, Carnales) |
| P | Set the playa's team to the chosen allegiance |
| 7 | Cycle the new turf owner (Saints, Carnales, Vice Kings, Rollerz, Nobody) |
| 8 | Give the turf the player is standing in to that owner |
| 9 | Dump teams, notoriety and the hood list to `mods/wml.log` |

Locking at **0 stars** means that faction never wants you: the lock writes the
faction's min and max wanted-level clamps to the locked value, so the crime
pipeline cannot add heat and decay cannot remove it. Unlocking (or key 6)
restores the clamps the save shipped with.

Allegiance writes the same team id field `set_team` uses (`player+232`), so
friend/foe and who you can recruit from follow the new team. Opening the menu
starts the allegiance picker on whatever team the playa is already on.

Build with `./Build.ps1` (installs into `dist/mods/WantedAndTurf`), then add
`+ WantedAndTurf` to `dist/mods/modlist.ini`.

## How it works

- Notoriety lives on the player object, `*(0x8309ABEC) + 3868 + faction*24`
  (`+0` level, `+4` min clamp, `+8` max clamp, `+12` progress, `+16` decay
  timer). Levels are set through the game's own setter
  `sub_821D2F50(entries, faction, level_float)` - the one `notoriety_set` uses -
  so NPC alerts, chase music and the HUD meter all react normally.
- Playa allegiance is `player+232` (runtime team id). Same store as the
  `set_team` script thunk.
- Turf ownership lives in the hood table: `*(0x8370D098)`, with
  `*(int*)0x8370D094` records of 256 bytes (name inline at `+0`, owner team id
  at `+72`, zone volume at `+108`). `sub_821201A8(&position, 0)` returns the
  index of the hood a position falls in. The ambient spawn code reads the owner
  out of that table each time it spawns, so changing it changes which gang turns
  up; peds already in the world are left alone.

Key conflicts: Saintify uses F3, Whompay's Trainer uses F4, and both also read
the number keys while their own menus are open - close them before using this
one.
