# Co-op v2: design

Status: approved by whompay as a revamp; first build (co-op 2.0, milestone 1) written, **not tested yet**. Built on
branch `coop-v2` of the user's fork; nothing goes to whompay/SaintsReborn until it is tested and confirmed on two
machines (Windows PC and Mac). What 2.0 does is in section 10.

Goal: **up to 12 players in the story city who all see the same world**: the same traffic and parked cars, the
same people, the same police, the same explosions and wrecks, the same knockdowns, and every weapon. One mod
(WhompaysCoop); no companion mods.

Research behind every decision (private research repo, reports with H/M/L confidence marks):
- **P** `P_coop_inventory.md`: what co-op 1.71 syncs, how, and its two-player assumptions.
- **Q** `Q_game_multiplayer.md`: how the game's own 12-player multiplayer replicates the world.
- **R** `R_weapons_sync.md`: all 51 weapons, their engine paths, aim, and per-category sync recipes.
- **S** `S_world_sim_sync.md`: traffic, people, police, explosions, pickups, randomness, streaming.

## 1. Why v1 can't get there

v1 (co-op 0.57-1.71) is a two-game mirror: each player runs a full single-player game, and the mod copies the
host's ambient world into one joiner's game while the two are within 120-200 m, suppressing the joiner's own
simulation with about 25 separate hooks. It assumes exactly one remote game everywhere (one peer, one avatar, one
proxy car, a lobby of 2). Its packets carry no sender or version, events are sent three times and hoped for,
and the host sends about 190 KB/s to one joiner. Police, rockets, grenades, prop explosions, pickups, knockout
state and car hits are not synced at all (P sections 3-4).

## 2. The model: the game's own multiplayer as the guide

The game's multiplayer cannot run on the city (its memory overlaps the city's streaming memory, and story
missions and MP modes can't run together; Q section 7). But it is the blueprint, and parts of it are a library:

| From the game's multiplayer | v2 |
|---|---|
| Star topology: the server talks to every client | Same: the host (slot 0) relays; clients 1..11 |
| 8 object types, each with create / update / apply / destroy callbacks | Same object kinds; call the game's per-type functions where they work in story mode (NPC create 823A7AB0/823A7DB0, vehicle create 823BA888, player creation as 823AF420, projectile copy as 823ACB08), own code where they need MP state |
| Every object the server creates is registered at its constructor (82352D38 call sites) | Hook those constructors (8247A6D0 humans, 8253BF08 vehicles, 8241FCB0 projectiles, 82408190 items) instead of setting the MP flags |
| Snapshot scheduler: priority = -10 x distance, +500 in view or within 10 m, sorted, byte budget per client per packet | Same algorithm, per peer, 20 Hz |
| Only the server applies damage, creates explosions and spawns projectiles; clients detect their own hits and the server replays them through the normal hit functions | Same rules, with "server" replaced by "owner" (section 4) |
| Message bundles with a reliable part (acked) and an unreliable part | Same: reliable channel for creates, destroys and events; unreliable for snapshots |

## 3. Network layer

- **Transport**: co-op's own Epic link (`eos_link.h`), made multi-peer: lobby of up to 12, stand-in address per
  peer (10.255.0.N, already there), outgoing queue capped (1.71). Direct UDP stays for LAN and debugging and
  speaks the same protocol. The runtime's mod channel (`SrModSend`, used by Zombies) is the fallback option.
- **Packet**: `{magic 'SRC2', u8 version, u8 sender slot, u16 seq, u16 ack, u32 ack bits}` then messages
  `{u8 type, u16 len, payload}`; at most 1100 bytes (Epic's limit is 1170).
- **Channels**: reliable-ordered per peer (resent until acked, at most 1024 messages in flight, a peer more than
  16384 behind is dropped) for joins/leaves, creates, destroys and events; unreliable newest-wins for snapshots.
- **Session**: join with a version check (mismatched versions are refused with a clear message), slot
  assignment by the host, leave and timeout per slot, host relays client messages to the other clients. Each join
  attempt carries an epoch, so a client that lost the host (or left and came back) from the same address is taken
  as a new join at once instead of being stuck behind its old link.
- **Budget**: per peer and per tick, filled by the scheduler in priority order; far objects update less often.

## 4. Identity and ownership

- **NetId** = owner slot (8 bits) + owner's serial (24 bits). Every game keeps NetId <-> local handle maps.
- **Owners**:
  - a player character: that player's game;
  - a car being driven: the driver's game (ownership moves on enter/exit, arbitrated by the host);
  - the ambient world (traffic, parked cars, people, police, props, pickups): the **area owner**. M1: the host
    owns it within its world radius. M3: each game owns the chunks it has loaded (host first, then lowest slot),
    with handover (S section 7);
  - a projectile: its shooter's game; an explosion: the owner of its source;
  - global state (clock, weather, missions, cutscenes, HUD): the host;
  - notoriety: per player (2.1). Each player's game owns its own player's stars, decay and HUD; every other game
    mirrors them onto that player's character, so its police and gangs go after the wanted player.
- **Puppets**: every object a game does not own is a puppet. One predicate, `IsPuppet(obj)`, drives every
  suppression hook (no own AI actions, no own damage, no own knockdowns/ejections/get-ups, no own despawn, no own
  explosions), replacing v1's separate avatar/copy checks.

## 5. What is synced, and how

| Area | State (snapshot) | Events (reliable) | Suppression on puppets / non-owners |
|---|---|---|---|
| Players | position, yaw, velocity, movement state (on foot / car + seat / ragdoll or knocked out / swimming / climbing / dead), anim set, action, crouch, health, weapon, aiming bit, camera eye/rows (+4112/+4124) | actions, knockdown, get-up, ejection, enter/exit, door, fire, trigger press/release, throw, reload, death, respawn | v1 hooks, unified; ragdoll/KO shown from state with no move orders (the "KO'd vs running into a wall" fix) |
| Cars | model, position, rows, linear/angular velocity, hit points, damage bits (smoke/fire/wreck), lights/siren, doors; paint once | impacts, ejection, wreck | car damage 825397E0, hit points 8252D2A8, explosion push 82536B00, ejection 82540780, own physics steered to the owner's state |
| Traffic, parked cars | as cars + occupants | create/destroy | traffic byte 0x8309A35D = 0, parked spawner hook 8241E378 (S 1c) |
| People | model, look (once), position, yaw, velocity, movement state, anim set, action, dead, ragdoll, car + seat | actions, hit reactions, knockdown, get-up, death, ignite | people bytes 0x8309A364..68 + 82413398, gang manager 824189E0, despawn 8244BC40/824789A8, MakePassive |
| Police | cops and police cars are ordinary people/cars from the area owner | - | response manager 8242ABA0, roadblocks *0x8370F808, cop/gang spawn bytes 0x827AD06C/6D |
| Notoriety | per player (2.1): 5 x level, min, max, points of the owner's +3868 block, written onto that player's character everywhere (+16 = -1) | a player's crimes seen in another game go to their game (`821D3148` there); an arrest of their character goes to them (`82483440` there) | decay `821D24F0` skipped on other players' characters, their hit credit `82481CD8` dropped, `82483440` on them intercepted |
| Bullets (single, auto, shotgun, sniper) | - | fire with the shooter's hit point (r5 aim point), trigger held/released for automatics | replay via 82470A50 with spread off; aiming stance raised first (82449708) so no deferred shot escapes the guard |
| Rockets, grenades, pipe bombs, Molotovs | projectile copy position/velocity | spawn (pos, rows, velocity), detonate | one live projectile (the owner's); copies never detonate (825BF418 skipped); the owner's explosion played once everywhere (82138AE8 with a replay flag) |
| Melee | - | attack/punch (as v1) | v1 hooks |
| Damage | - | hit requests from the attacker's game to the victim's owner, applied there through the game's hit functions (MP's bullet_hit / melee_hit model) | 824470D0 refuses on non-owners |
| Pickups, money and weapon drops | position, value | create/collect/destroy | pickup creation/collection routed to the owner |
| Clock, weather | 0x82B2EF30, weather entries | time jumps | 82157598 / 82156DB0 |
| Missions, cutscenes, HUD, pause | v1's host-driven system, generalized to N; other players as arrows on the minimap and the pause map, a colour per slot (2.1.4) | v1 | v1 |

Randomness is never synced as a seed: the owner decides, everyone else is told the outcome (S section 6).

## 6. Engine limits (per game)

32 vehicles, 75 humans, 12 people-model slots, 11 remote player objects. The scheduler sends the nearest first
and the receiver never exceeds these; models that can't load get same-sex lookalikes (v1's model manager).

## 7. Everything from v1 and Co-op Car Guard is built in

No companion mods. Kept from v1: player-pool characters with own body models and clothing (one component set
per slot, 11 slots), dressing and colours, the movement smoothing (Follower), car copies and steering, seats and
enter/exit, the gun handover as the game's multiplayer does it, shot replay rules, missions/cutscenes/HUD/pause,
chat, diagnostics (Perf lines, crash/freeze reports, F9 trace, debug_cmd.txt).

Fixes carried over: car-removal freeze guard (1.64), car-list checks (debug_cars), object-table scans that read
the type first and the 4096-page memory-check cache (1.65), shooting (1.66-1.69), no crash after a death and
no thrown-weapon handover (1.70), capped Epic queue and link diagnostics (1.71), and from Car Guard 0.4-0.5: under
Wine, skip QueryWorkingSetEx once it has never answered and cache VirtualQuery by region.

Fixed on the way: v1 passes car smoke and fire the wrong way round (S).

## 8. Milestones

1. **M1, first playable v2** (the first build to test, 2+ players): network layer and sessions for up to 12;
   players complete (state-based puppets, weapons with aim, projectiles and explosions, damage rules); cars driven
   by players; the host's ambient world (traffic, parked cars, people) through the new scheduler within its world
   radius, other games' spawners off there; missions/cutscenes/HUD for N; all fixes in section 7.
2. **M2**: pickups and money, car hits/ejection/fire, NPC gunfire, weather, props (notoriety per player and the host's police response for a wanted client near it are in since 2.1).
3. **M3**: area ownership for players spread across the city, 12-player load and bandwidth tests, delta
   compression, more of the game's own per-type functions.

## 9. Code layout (for upstream)

`coop.cpp` keeps the mod entry, settings and the menu exports (`WhompaysCoop*`, so coop_menu.cpp keeps working).
New files next to it: `net.*` (transport, sessions, reliability, bundling), `protocol.h` (messages), `registry.*`
(NetIds, ownership, puppets), `players.*`, `vehicles.*`, `people.*`, `weapons.*`, `world.*` (spawner control,
clock, weather, notoriety), `missions.*`, `engine.h` (addresses and game-call helpers). The network layer has a
stand-alone test program that runs several peers over loopback without the game.

## 10. Co-op 2.0 and 2.1 (milestone 1): what is built

Every player needs the same version: 2.0 brought a new protocol (a 1.71 game knocking is told so on screen and
in the log), 2.1 bumped it to 3 (a 2.0 game can't join), 2.1.1 to 4, 2.1.2 to 5 (2.1.3 and 2.1.4 keep it), 2.2 to 6
(section 11).

**Session** (`net.h`, tested by `tests/net_test.cpp`: 12 players through 20% loss, refusals, leave/timeout,
rejoin, a lost join reply, a one-way outage, a leave lost before a rejoin, 3000 reliable messages through a 5 s
outage, an oversized name): host = slot 0, up to 11 clients, the host relays. Events (actions, calls, car
enter/exit, hits, fire, throws) and projectiles are reliable and in order (v1 sent each event three times);
poses, traffic and people snapshots are unreliable, newest wins. Epic lobby of 12. Packets up to Epic's 1170 bytes.

**One record per other player** (`RemoteIn` from the network, `Remote` on the game thread): character (player-pool
object with its own body model and clothing set per pool slot), clothes, colours, body sliders, gun, movement
follower, car bookkeeping, shot queue, knockdown state. Hooks ask "is this any other player's character"
(`AvatarSlotOf`) instead of comparing with one avatar.

**Cars** are named by owner slot + handle in the owner's game (`CarId`, `FindCar`, `car_copies`), so any player can
drive or ride in any other player's car or the host's traffic; copies go when unused, far, or their owner leaves.
The host shares its traffic and people around every client (nearest client counts).

**Car damage** (2.1, round 1): the game driving a car decides its damage. It sends the hit points (out of 255:
`TrafficCar.hp`, pose bits 24-31) and the damage bits of +53976: fire 0x800, smoke 0x04000000, wreck 0x80000000,
heavy smoke 0x02000000. 2.0 sent 0x40000000 as the wreck, but that is the fade-out before a car is removed (H), so
intact cars fading out blew up on the other games, and real wrecks only lost their smoke there. On a car another
game drives, a game writes the hit points it is sent (never 0 without the wreck bit), sets smoke and fire with
`8253CD70` (a refusal waits 2 s; fire also gets the burn rate `8252D2A8` gives with it, +54408 = 0.03 of the most
a second, which `8253CD70` leaves at 0 (H), so the car burns down once this game decides again), loses no hit
points of its own while that game sends its damage (`8252D2A8`, which also covers the burn tick, pending damage and
`82540370`/`82540458`; another player's car nobody sends for after they got out burns down here as in 2.0) and
isn't pushed by explosions (`82536B00` runs with the game's no-fling bit set for that call only; the owner's car is
pushed and the copy follows it). A copy is wrecked with the game's MP wreck replay `82542650` (no cash, no script
callbacks), without the explosion when it wasn't shown intact just before (made of a wrecked car, or without
physics until long after the wreck), and keeping its motion then (`82542650` lifts the car whatever the explosion
did, H). A copy without its physics set up (byte +50300, far off or parked: `82542650` returns at once then, and
the game's own MP wreck client `8238BDE0` doesn't set it up either, H) is tried 4 times (1 s), then waits quietly
and is wrecked without the explosion the moment its physics is set up (2.1.4; before, it was tried 4 times a
second for as long as it lasted). One of this game's own cars driven from another game is blown up with `825397E0` as before (3 tries 1 s
apart when refused, then every 30 s; each try damages every part); only that car passes the hooks (2.0 let the
whole explosion's area pass damage every copy: chain wrecks). Protocol 3: 2.0 sent 0 hit points and the fade-out
bit, so it can't join a 2.1 game. Dents and broken parts: sent since 2.2.10.

**Weapons** (research report R): bullets go from the character's muzzle to the shooter's own hit point (first
pellet, `8246FD18`), spread off, gun raised first, the game's own deferred shot swallowed, refused shots retried for
750 ms; automatic guns get the trigger release; rockets, grenades, pipe bombs, Molotovs and bottles: the owner's
projectile is the only real one, other games make a copy (`8241FCB0`) that never explodes by itself and explodes
once at the owner's spot when the owner's does; explosion damage to the host's people is decided by the host,
each game applies explosions to its own player. The fire and throw animations are no longer sent twice.

**Knockdowns** (research S 2d): each player's game decides their ragdoll, knockdown and crash ejection. The state
(+780) goes with the pose; other games knock the character down / get it up to match, never seat or move it while
it lies, and skip crash ejections for other games' cars and characters (the "knocked out in the car on one screen,
running into a wall on the other" case). 2.1: the pose's ragdoll bit means lying (+780 in state 8/15), not getting
up; other games knock on its rising edge and get the character up with its own get-up (`82449FE0`) when it clears.
The owner's knockdown, state 8 (8/15-17) and get-up animations (0xE0/0xE2/0x232) are never replayed, nor anything
else while the character lies or gets up there. The replayed 8/16 put a character that never got up itself at the
world origin, ~700 m off: its get-up spots (+792/+808) were still the constructor's zero vectors (H); a state set
mid-ragdoll could leave it ragdolled in state 0 for good (H), which is now caught and knocked again so it gets up.

**Built in from Co-op Car Guard 0.5**: the car-removal guard and car-list checks (already in 1.64), and the memory
check speed-up under Wine (`memory_speedup`). Car Guard is no longer needed (co-op says so if it is installed).

**Fixed on the way**: car smoke and fire were applied the wrong way round; the "game took the other player's
character for ours" check (1.34) never matched.

**Spawners** on a client while the host's world is shown: traffic spawner byte off (no spawn/despawn churn),
parked-car spawns refused, gang manager skipped (plus v1's density 0 and removals as a backstop). 2.1: its own
police response `8242ABA0` ends the way its multiplayer exit does (the response group the spawners serve next,
0x8309A540, one of the 12 group pointers at 0x8309A510, cleared; `824129C8` / `82411310` skip it when 0) and its
roadblocks `82424708` are skipped (the world update ignores both results), so no police only that client sees.
Only while the client's character stands in the host's game, which is what the host's police come after: it is
made within 60 m of the host and removed past 130 m (`UpdateAvatar`, alike both ways), so the host's character
shown on the client (slot 0) stands for it, held 2 s after it goes; beyond, the client's own police come back for
its own stars (2.0 left no police at all from 130 m to the shared world's 200 m). Not with mod.ini `police = 0`:
the client's police then run as in 2.0.
With `8242ABA0` skipped nothing updates or calls off its groups, so what they had out when the host's world came
(the client wanted first) is removed once then: each group's 8 squads (44 bytes from +92: active byte, car +4,
8 units +12, read from `82429E38`) (H), never a player, copy, follower or mission character, nor a car a player
uses. `allow_cop_spawning` (0x827AD06C) and the roadblock counter `82425150` are left alone: mission scripts own them.

**Copies of the host's people and traffic crews** (2.1, on clients): they wear the host's team. The minimap draws a
person's dot from their team (+232) after its own skips (`822E8D00`: +120 flags, dead, players, followers): 0 the
purple "revive" dot, 6 none, 4 red only for the definition at 0x8370D020, any other red; a car by its driver's
(`822E8E80`) (H). Copies took theirs from their model (definition +108), stand-ins and lookalikes from whatever
loaded model they use (L that this was the purple police). The host sends each person's team (`PedState.team`)
and each car seat's (`TrafficCar.teams`, 4 bits a seat), and the client writes it the way set_team `824DB708`
does on whatever shows that person: copy, lookalike, stand-in, seat stand-in (seated or not) or own character
taken over (never one of the player's homies). The host's police shown here are logged once each. Copies play only
the host's actions: their own actions (crews' car moves excepted), shots (`82470A50`, deferred shot cleared,
drive-bys too) and hits on the client's player (not explosions, fire or a car hitting them, `82451C98`) are refused
outside co-op's replays, which include co-op's own seating and exit orders. Hits from the host's people on foot
come across as before; since the notoriety round, hits from people it doesn't share as people (its crews, its
police response's units) come across too (their own hits: not a car hitting the character, explosions or fire),
whatever mod.ini `police` says.
Car crews are passive like the people (`MakePassive`); a crew member getting out on its own is counted and its asker logged (2.1.1 refused it; 2.1.3 lets it through again: the first asker seen was the game's own character removal 824789A8, which takes a human out of its car before it goes)
(below; 2.1 logged and counted it).

**Notoriety and arrests, per player** (2.1, round 1; mod.ini `police = 0` turns it all off: no stars sync, crime
credit, arrests across games or response steering, and a client's own police stay on near the host; the host's
stars still rise for the crimes seen in its game, as in 2.0, and hits from the host's people come across either
way). Each game owns its
player's notoriety: the +3868 block (5 entries of 24 bytes, one per gang - 0 Los Carnales, 1 Vice Kings, 2 Rollerz,
3 Playas, 4 police: +0 level, +4 min, +8 max, +12 points, +16 decay timer, -1 off, +20 timer), its decay
(`8247E850` -> `821D24F0`), HUD (`822F5300`) and stars stay native. Level, min, max and points go to everyone
(`kMsgNotoriety`: reliable at a change of a level, min or max, else once a second unreliable; clamped on receipt),
and every game writes them onto that player's character when it is a pool player (+224 == 1), always with +16 = -1:
the police there go after a player-type target with its own entry 4 >= 1 (`820D28E0` / `823FEBE8`), the gangs
through `82481AB8` (H). Never through the game's setters (`821D2F50`, `821D3070`, `821D2D38`, `821D2950`): they
end in `821D1A20`, which acts on the local player (H), as does each decay step, so `821D24F0` is skipped on other
players' characters, and a character's levels are cleared (level 0, points 0, +16 = -1) before it goes back to the
pool and before a pool object is respawned (`8247E248` -> `8247BA28` -> `821D2D38` would run `821D1A20(0, gang)` on
the local player for a level left in it, H; `821D2D38` also empties the local crime queue, count 0x82B2BA6C, so the
queued records at 0x838F9348 are saved around that respawn and put back). A mirrored level dropping to 0 is forgiven the way `821D1A20` does
it: every person of that gang's team (0x8203A29C) on the list *0x8309ABB8 takes the character's handle off its
enemy lists (`820D8398` / `820D8538`).

Crimes are credited to the culprit's own game. `821D3148(activity, team, flag, weapon info)` queues a crime for
the local player; it is reached from `82482098` (kills), `82481CD8` (hits, shots near people, cars hit) and
`82482568` (government cars, helicopters, trains destroyed), all with the culprit in r3, so those are hooked and
the culprit noted around them: another player's character's crime is sent to that player (`kEventNotoriety`:
activity, team, flag, rocket launcher) behind the game's own gates (*0x8370D1A0 lock, multiplayer byte) and the
original skipped; the receiver validates (activity < 24, team -1..6) and queues it with `821D3148` on its own
player, so the game's lock, delays and queue apply. The record's flag byte (+12) means witnessed: when it is 0 and
the activity's entry has Check_Detection (byte +16 of the 20-byte sub-record at 0x82B2BAC8 + activity*100 +
entry*20, with +0 Points, +4 Delay, +8 Min_Level, +12 Max_Level; the entry is the gang's own for a gang team, 4 for
everything else), `821D2840` runs the detection test `821D1F98(local player, entry)` as the record is applied, 500 ms
after the crime: a living member of the entry's team (5 for the police) on the people list sees the player within
8 m unless it is behind it, within 40 m in its front 120-degree cone, or within 40 m of its car, through walls
(ai_rank_attribs.xtbl, no line of sight). An unseen crime gives a quarter of its points (at least 1), a seen one at
least what its Min_Level takes (H). Since 2.1.2 the host judges this for a client: it runs the test on the
client's character when it forwards a crime and sends the flag set (`WitnessedFlag`), and tells each client
whether a cop sees its character (`kEventSeen`), during which the client queues its own crimes as witnessed (below). `824828C0` (carjack) and `82482290` (a cop sees a gun fired)
put their record in the queue themselves (H), so their record is taken back out after the original (with the
queue full, 30, they would apply it to the local player at once: another player's is lost and counted instead).
No double credit: a player's own game counts its melee hits and run-overs on copies (`82481D50`, from `82457900`
/ `82451C98` / `8256A138`) and its shots near them (`82481EE8`, from the bullet's `824581C8` before its damage
call, so a copy's refused damage doesn't skip it; H), so hit credit with another player's character as the
attacker is dropped, and so are their carjacks (the jacker's game sees the pulled-out driver's team on the copy;
its own queued record is logged, "Crime queued here by this player", since `824828C0` never reaches `821D3148`);
kills, government-car crimes, cop sightings and cops' alerts (`820DB9D0`, 2.1.2) are forwarded; a police car
taken (`82482400`, 2.1.2) is dropped like a carjack (the thief's own game counts the act: see 2.1.2 below); on a client, kills of copies and sightings by copied cops are dropped (the host's game sees
them). Every caller of `821D3148` and of `82481CD8` is logged once with
its return address and name (`82481D50` hits, LR 82481EDC; `82481EE8` shots near people, LR 82482084; `82482760`
cars hit, LR 824828B4); the second tells whether a client's bullets at copies reach `82481EE8` there at all (M:
proven for melee only) - if they never do, hit credit from that caller has to be forwarded instead of dropped.

Arrests: `82483440(target)` (from the cops' arrest `820B1A28`; the bust check `820B1F90` / `820B1EB8` never asks
whether the target is the local player, H) sets the target's busted bit (+3592 0x20) but then acts on the local
player and screen (mission callbacks, `82215360`). With another player's character as the target it is
intercepted and `kEventBusted` sent to that player (at most every 3 s); their game arrests its own player behind
the bust check's own gates there (busting on, byte 0x827D95C5; no multiplayer; lying down, +780, and not getting
up, 8/16), with a police level left and not busted already, and the native busted screen, respawn and notoriety
reset follow. A client showing the host's world refuses an arrest of its own player by its copies, and never
forwards one its copies attempt on another player's character: only the host's cops arrest there.
A "Police:" line every 10 s shows the levels here and on other players' characters, notoriety packets sent /
taken / old, credits sent / received / dropped / refused / lost (queue full), since 2.1.2 the crimes seen by this
game's cops and this player's own queued witnessed, arrests sent / applied / refused, forgivings and skipped decay
steps.

**The host's police response for a wanted client** (2.1, round 1). The response manager `8242ABA0` (every frame
from `824131F0`; a client in the host's world skips its own) works for `*0x8309ABEC` alone: the level a group
runs at (`823FCE40`; the spawn timers and squad pick `823FBFE0` read the entry themselves), the vehicle budgets
(`823FCC80`, the gangs' `823FD008` / `823FCEB0`), the spawn rings of 200 / 150 / 100 / 50 m (`8242A540`) and the
pursuit orders (`8242A228` -> `825A91A0(car+50376, &order)` / `8259DCC8(helicopter, handle)`) all read the local
player (H); so do the pursuit's own level reads while a car chases (`825A8828`, the timers; `825A5C00`, a
manoeuvre gate), through `82481AB8` alone. A client sends `kPoseHostWorld` in its pose while its own police are
off (its character stands in the host's game: within 130 m of the host once made; see Spawners); the host picks a response
target every update: the highest police level (gang levels among equals) among itself and those clients'
characters within 200 m, kept 10 s at least and until it is over 230 m away, no longer wanted or gone, or a
strictly higher level takes over (the character made again keeps it; the cars after its old handle are called
off). While the target is a client's character, each of the six readers gets `*0x8309ABEC` swapped to that
character around the original alone (`PlayerSwap`: back on every path, never across a frame; nothing under them
reaches `821D1A20`, `822F5300`, the crime queue, camera, HUD, save or input: their callees are tables, timers,
model lookups and geometry, H), the two pursuit orders get the character's handle at their call sites in
`8242A228` while `8242A228` runs for the police, helicopter or a gang group (slots 0-4 of 0x8309A510, a flag set
around it; the activity spawners, 5-11, keep the player), the spawn point at `8242ABA0`'s call (LR 8242AC94)
while the pending group `*0x8309A540` is one of those, and `825A8828` / `825A5C00` get the swap for a car whose
pursuit target (+50468, modes 4 / 10 / 11) is the character, so the chase runs at the client's level, not the
host's. The gang groups work at the character's gang levels (the swap, not a max with the host's). Not one swap
around `8242ABA0`: it would run the car and crew creation and the activity spawners under it, and its read of
the host's +21252 (the host's nearest road node, -1 indoors, swimming or off the roads): `8242ABA0` starts no
new group while that is -1, so a client's response comes only while the host stands near a road (counted in
the "Police:" line; the value is the game's own and is never written). The game's own level-0 clean-up
`821D1A20` only calls off cars aimed at the local player (`820C6A58`), so when a mirrored level drops to 0, or
the target changes or goes (`DestroyAvatar` too), the host does that pass for the character's handle
(`CallOffPursuit`, on the game's vehicle list `*0x83289904` / +50308 with `821D1A20`'s skips: `825B9B10(car+50376,
0)`, `825419F0` / `825255E0` for the team's cars after it in modes 4 / 10 / 11 (3), and the helicopter leaves
with `825A3000`, its own end of a pursuit); `Forgive` also tells the team's people on foot whose target is the
character to stop (event 61, as `821D1A20` does for the player). Roadblocks (`82424708`) stay the host's. The
host sends the response to its clients with its people and traffic: the 12 groups' squad units on foot
(`CapturePeds`; seated ones with their car) and the squad cars and cars a unit drives, with their crews
(`CaptureTraffic`; `82411118` makes them off the traffic list (M), and the table scan is off during a mission).
The "Police:" line adds, on the host, the target, spawn points, pursuits steered, cars and helicopters after
it now, cars and helicopters called off, target changes, updates with the host off the roads, and the units
and cars of the response sent; "units told to stop" goes with the forgivings. The client's blocked response
runs stay in the "Own spawners" line.

**2.1.1** (after the first 2.1 test, 2026-10-16). A car a script or cheat makes for a client (`82569700` with source 7
or 3; not co-op's own copies, `making_car`) is that player's own (`personal_cars`, at most 8, the oldest forgotten,
forgotten when the game removes it) and the client's removals of its own parked and traffic cars while it shows the
host's world leave it alone (the mod menu's car was deleted before it showed); the other players see it as any
player's car once it is driven. A client's hits on the crews of the host's traffic (drivers, passengers, seat
stand-ins) are zeroed there like hits on its people and sent to the host as the car's handle and seat
(`kEventCrewDamage`; the traffic record names their models, not handles), where the real crew member takes the hit
from the client's character (`824470D0` under the host's own `DamageHook`), so it can die there and the kill is
credited to the client through `82482098` (2.1 let the copy take the bullets and die on the client alone: the host's
cop drove on and the client's game asked to remove the body every frame). A crew member's own exit request on a
client was refused in 2.1.1 and 2.1.2 and is let through again since 2.1.3 (the first asker seen in a test was the game's own character removal 824789A8, which takes a human out of its car before it goes; refused, a deleted character stayed in the seat) - the refusal was safe as far as the return value goes (r3 = 0: `8257B8E0` returns no value of its own, r3 is whatever its last call left, 0 from
`82448010` when the character isn't in a car, and no caller reads it; the generic exit `8244D5F8` passes it through to
nine callers that read none), except the driver this player pulls out and co-op's own replays; the asker behind
`8244D5F8` is logged from its 128-byte frame (the request at r1+80, the saved LR at r1+120). The get-up spots
(+792/+808) are set where the owner stands, before each knockdown and right before each get-up, when the character
lies over 3 m from them (the client's car ploughed the host's ragdolled character 42 m along one screen, and it got
up there): the game's own sub-state-16 pass (`82443808` -> `8243C780`) lerps it to those spots. The "Police:" line
shows the police points (entry 4's +12) with each game's levels, so credit short of a star is visible (the first
test's crimes were all Check_Detection ones and none was detected on the client). Seating another player's character
is retried every 500 ms; from the second refusal in a row the character is reset first (out of any car with `8257C400`
mode 19, its current action ended with `82445778`, placed at the car - what the game's own seat path does) and after
five refusals it is made again; and a client moving its own player over to a passenger seat (the car's own game keeps
its driver) takes it off the wheel with `8257C400` mode 19 first, while the car is slow, because `8257B2C0` refuses
the local player a direct seat while the driver's seat holds anyone, itself included (`82576D20` at `825771E8`; a
fast car keeps it at the wheel). Known: why the host refused the seat in the first test is not pinned yet; the
refusal line names the state (`seat N refused: +508, state, +780, action, melee, weapon, car/seat, +2569; car: list,
+120, +53976, ai, the seat's occupant and reservation`).

**2.1.2** (2026-10-17, protocol 5). Detection by the host's cops. A client's crimes gave no stars in the first 2.1
test: the game credits an unseen crime a quarter of its points, and its test runs in the game applying the record -
the client's, whose cops near the host are passive copies (whether those pass the test's own skips is open; the
"crime detection" lines show it). Now the host, which has the real cops, runs `821D1F98` on the culprit's character
when it forwards a crime whose flag is 0 and whose entry has Check_Detection, from the hook's own context
(`GuestScratch scratch(c)`), and sends the record's witnessed flag set when it holds (`WitnessedFlag`, in `CrimeHook`
and `RunQueuingCrime`); the client's `821D2840` then skips its own test. For the crimes a client credits itself (its
hits and carjacks with a civilian victim, flag 0) the host tells each client whether a cop sees its character
(`821D1F98(character, 4)` every 250 ms in `UpdateNotoriety`; `kEventSeen` on change and once a second), and for
1.5 s after a positive the client queues those witnessed (`CrimeHook` sets r5 = 1 before `821D3148`;
`RunQueuingCrime` sets +12 of the record just added), only in the host's world (`OwnPoliceOff`): far from the host
its own cops judge. Two more queuers are hooked: `82482400` (a police car, +53992 == 5, taken by a player-type
human, from the seat completion `825786A0`: government car theft queued for the local player whoever sat down) is
dropped for another player's character like a carjack: the thief's own game already counts the act (a pull-out
there is a carjack, after which the car's 0x200 bit blocks its own `82482400`; a copy whose driver copy sat at the
wheel carries team 5 in +53992, written by `825786A0`), so forwarding credited one act twice (found in review); a
never-crewed police-car copy counts nowhere, accepted. And `820DB9D0` (a cop's police alert or a gang member's gang alert at
a player-type subject, the handle at event+36, flag 1, queued for the local player; never raised by copies, whose
+3692 bit 0x08 is set) is forwarded to the subject's player. `821D1F98` is hooked for a diagnostic: "<who>: crime
detection: seen / not seen (entry N)" with the nearest member of the entry's team here (handle, distance, on foot
or in a car, +120, +2996), 20 lines. The Police line counts other players' crimes seen by this game's cops and this
player's own crimes queued witnessed (with the tellings sent and taken).

**2.1.4** (2026-10-17, protocol 5 kept). How other players' characters move. The 2.1 tests' "floats" lines (a
character placed on its path every update and not moving by itself for up to 14 s while its player walked with
the gun up, sliding with its legs still) came from the way it was moved: a move order (`82441670`, `OrderMove`)
along the path. Its state `+516 == 2` is not an aiming or strafing state but "walking a move order": `82441670`
ends with `82441308(h, 2, 0)` and the state-2 handler `82464DF8` is the path follower (H); the walk, strafe and
aim-walk animation under it comes from the same table the player's own locomotion uses (`8244E018` -> armed
`824342B0` / unarmed `82434D48`), but the layer never got going on the character (which step stops it isn't
settled; `notes/coop-2.1/aiming_movement_investigation.md`). The game never moves a player that way: the local
player (`82168D60`) and multiplayer's remote players (`8237B7B8`, from the snapshot apply `823AFA88`) are moved by
a stick input every frame - the point to walk to 5 m ahead in `+464` (`8216C9E0`), state 1 and
`8245FC08(h, f1 side, f2 forward, r6 sprint, r7 standing)`, which picks the gait from the stick's magnitude (walk
up to 0.81 squared, run above, sprint by flag), plays the aim-walk and strafe animations with the body facing the
aim (the camera rows `+4148`, which `WriteRemoteCamera` sets from the pose) and the run turn-around on a reversal
of the stick (H). `DriveAsPlayer` (from `Follower::Drive`, for other players' characters only, after `MoveHuman`,
which stays the position authority; its teleport `8243E6F8` writes `+464` too) does the same every update: it
cancels a running move order with `82441308(h, 0, 0)` (the game's own cancel, which frees the path), writes `+464`
= the path position + 5 m along the path velocity (the position itself when standing), the stick in the owner's
camera frame (its right and forward rows flat: `+3028` / `+4424` forward, `+3032` / `+4420` sideways, as
`82168D60` stores them; 0.6 for a walk, 1.0 for a run or sprint, from the path speed with the old gait hysteresis
and the 150 ms stop debounce), sets state 1 when it is 0 and calls `8245FC08`, all under `replicated_depth`.
States 3-8 (car walk, jump, climb, landing, knocked down) are left to their own handling, as are the gap
placement, the knockdown, get-up and car paths, and the host's people copies (move orders as before). `mod.ini`
`walk = 0` goes back to move orders. Diagnostic "<who> walks here (update N): ..." at the first, third and tenth
update after each start (10 lines per character): state, mode `+508`, gait `+512`, the layer-0 action `+576` at
its weight `+580`, layer 1 `+764` (the turn-around 335/336), `+756`, the gun, the stick and its band angle and
`8245FC08`'s r3. Expected: `+516` 1, layer 0 an aim-walk (147-156) or walk action at 1.00 while moving, and no
more "floats" lines (that line keeps watching). Also in 2.1.4: only the sync knocks another player's character
down - `KnockdownHook` dropped an avatar's knockdown only when `!replicated_depth`, so a replay (the host replaying
that player's own rocket's explosion) knocked it down while its player stood and fired there (55 of their shots
refused in the second test); now only `SyncKnockdown`'s two `824497F0` calls pass (`knocking_avatar`). The Shots
line counts the refused shots by reason (lying, not set up, other gun, no ammo, fire refused, gun raising, in a
car, other: `Remote::Shot::held`, the last reason a shot waited for before the 750 ms ran out), and a wreck
replayed on a copy without its physics set up waits quietly for it (the car damage paragraph above). Three more
2.1.4 fixes from the second test. (1) A client's hits on the host's traffic crews land now: the host resolved the
seat with the car's own seat slots (`+52320`, `OccupantHandle`), which are empty for most AI crews - the reason
`CaptureTraffic` names the crews by every person's own `+2496` / `+2500` - so all 352 of a test's crew hits were
dropped without a counter; `SeatOccupant` tries the slot, then that scan, and the "Copies" line counts the hits that
found no one in the seat, no such car, or a player there (the first 20 empty-seat hits are logged). The same slot
test picked the car a response unit drives for `CaptureTraffic`; its own `+2500` comes first now. (2) The host's
mission-end message is forwarded only while a mission runs (`MissionActive()`, `0x82B3106C`: `82215360`'s own
first test); the player's death calls it "failed" in free roam too, and every death of the host showed "Host's
mission failed" on the clients (a capped log says when it isn't forwarded). (3) The Police line's "helicopters
after it" and the helicopter call-off test the helicopter AI block (`+54840`) for mode `+684` 1 as well as 2, with
`+160` the handle: 2 is the pursuit (`8259DCC8` / `825A0560`), 1 is over the target (`825A0F18`, reached from the
pursuit update `8259DD80`, which keeps `+160`), and `825A3000`'s mode 4 (target gone, or called off) leaves `+160`
as it was (H); the test counted mode 2 alone and showed 0 while four helicopters attacked. The cars' test
(`+50376` 4 / 10 / 11 with `+50468` the handle) matches `825A91A0`'s stores (mode 4, sub-mode 17, `+92` = the
order's handle).

The people-model references on a client (a leak since `6c88c8a`, the first `CreateActor`). Every character co-op
makes - a copy of one of the host's people, a stand-in, a traffic driver or passenger - took one streaming
reference of kind 5 on its model (`8250C750(group, id, 5, 0, 1)`, as the game's own network NPC creator `823A7DB0`
does) and nothing gave it back: no human destroy path in the image releases a people-model reference (the three
`8250CA48` callers are the population sets' `82419C08` and the vehicles' `8247BA28` / `825324D0`; H), so a model's
count grew by one per character made with it (BF 1 -> 139, 120 1 -> 221 in 12 min of the second 2.1.2 test) and only
`ClearUnusedModelRefs` (nobody wearing it) ever brought it down. The harm, in two sentences: a request with a count
never dies, and a kind-5 request paired with a slot in the group's pass (`8250D3E0`) evicts that slot at once, so a
dead model's leftover request could take the slot of a model thirty copies wore and park them invisible
(`8247AA70`). A leaked, worn model also kept its request, slot and queue place for the whole session. 2.1.4 keeps
character handle -> model id and reference (`actor_models`: `NoteActorModel` after each `CreateActor` and after the
stuck copy's re-request, which starts over since the first reference went with the dropped request) and releases
exactly one `8250CA48(group, id, 5, 0, 0)` per reference in `DestroyObject` and `DestroyActor` (`ReleaseActorModel`),
with a 1 s sweep (`SweepActorModels`) for the characters the game removed itself; `ClearUnusedModelRefs` forgets the
entries of a model it clears. A guard every 10 s (`ModelRefGuard`, client) compares each model's kind-5 count with
its wearers plus its pin and lets any excess go (capped "Model X: N references of kind 5 more than its wearers and
pin here, let go"); the People models line counts the references given back and the excess, and the slots dump
shows each slot's level (`L`) and the per-level slot quota table `0x827ACFFC` (14 words) - the other half of the
"waiting for room" picture: the pass pairs a level's requests only with that level's slots, co-op asks at the top
level (`PinModel`, level 13) and level 13 owns only the slots `8241BD40` last moved there (8, later 4-6 in the PC
logs) while the rest sit idle at levels nobody asks at (H for the mechanism, M for the numbers over time). Not
changed in this build, the next step once the counts are flat: ask at level 0 like the game (and give level 0 all
12 slots in the table) or call the game's own quota recompute `8241BD40(0,0,0)` each second. The car copies' model
references have the same shape (`CreateVehicle`: a direct `8250C750` like `823BA888`; the game's own cars go through
`82532360(v, kind)`, which notes the kind at `v+50304` for the destructor `825324D0` to release): measured only, in a
10 s "Car models here" line (references taken by copies against the vehicle group's kind-5 count) and a capped line
with `+50304` of the first copies (-1 expected). Review fixes on the 2.1.4 batch: `DriveAsPlayer` runs only on
characters from the player pool (`IsPlayerObject`: the stick block at `+4420..+4448` lies past the end of a
4252-byte NPC human; an NPC-class character shown for a player keeps the move orders); the owner's replayed
`82441308(0/1, 0)` no longer holds such a character for 120 ms (it is in state 1 by itself); the stop debounce keeps
the last direction of travel instead of stepping toward the camera forward; the Car damage line's "cars waiting for
it now" is counted at print time (copies with 4 tries without physics and no wreck bit); each character made gets
its 10 "walks here" lines; and the crew-hit seat lookup is remembered per car and seat for 250 ms (one 4096-slot
scan per burst of hits, not one per hit).

Other players on the maps (2.1.4). The other player showed on the minimap as a purple dot, like a homie, and not at
all on the pause map: the minimap's people pass `822E8D00` draws every human on the people list by its team `+232` -
a player-pool character has team 0, which gets icon [4] `map_other_blip_revive` through `822E8C78(pos = obj+20,
params)` - and the people, car and follower passes are called from the HUD minimap draw `822ECDB0` only, so the
pause map (`822B3F70`, the render of the MAP tab's view `0x8206169C`) shows the icon lists, the route, the waypoint
and the player's own arrow and nobody else (H). Now every other player in the session is an arrow in their slot's
colour on both maps, drawn the way the minimap draws homies (`822E8FE8`: `map_blip`, HUD item `0x83023660[9]`,
turned by the heading - `atan2` of the forward row's x and z, as `8262EB58(obj+32)` - plus pi, `8227E720(bitmap,
x, y, angle, scale, params)` with the colour from `8263E7D8(r, g, b, a)`) at the player's own size (homies are 0.8 of
it): a hook after the followers pass projects each position with `822E8A78(&xy, &aux, pos, 0)` (a point past the
minimap's radius is put on its edge, so a far player sits at the rim), adding the map's turn `*0x8370E438` while the
map doesn't turn with the player (byte `0x8370E407`), and a hook after `822B3F70` uses the pause map's own transform
(block `0x8370DD60`, as its icon pass `822EDC50` and its arrow: `px = +1072 + +1080*zoom + (+1104 - x)*zoom`, `py =
+1076 + +1084*zoom + (z - +1108)*zoom`, zoom `*0x827AF7B4`; the page's x runs against the world's) with its arrow
bitmap `0x8300FC78[0]` at `HUD scale * (clamped zoom *0x827AF7A8 * 0.333 + 0.667)`, kept on the page by hand (the
game sets its scissor back to the whole screen before it returns). The position is their character's `+20` while it
exists here, else their newest pose - so a player beyond 130 m, not yet made, hidden in a cutscene or dead is on the
map where they are or were last seen - with half alpha while they lie or are dead (`MapMarker`, filled by
`UpdateAvatar` from every pose, cleared when the slot leaves); the character's purple dot is skipped (`822E8C78`
returns without drawing when the position is an avatar's). Nothing is added to the icon lists, so no mission clears
it and no icon slot is used. The hook on `822B3F70` fires although nothing calls it directly (it is reached through
the view descriptor's pointer): the loader detours the host function (MinHook), as it does for `824789A8` and
`825324D0`, which have no direct caller either. Log: "Map markers:" once per map (the bitmap handles, HUD scale,
the rotate byte, MP blip 108's bitmap, the zoom and page), "Map marker: Player N from their character here / their
last pose" on each change of source, and a 10 s "Markers:" line (arrows drawn per map, how many at the edge,
purple dots skipped, failed draw calls).

Not in 2.1 yet (milestones 2-3): roadblocks for a wanted client, pickups, car hit forwarding, NPC gunfire and NPC
projectiles, weather, area ownership for players spread across the city, bandwidth scheduling for 12 players.

## 11. Co-op 2.2: the shared area mirrored (in progress)

Goal (the user's): players who share an area see the same cars, people, cops, actions and effects, the way the game's
multiplayer mirrors its world, instead of 2.1's streams with their 90 / 200 / 250 m gates and fixed-size packets
(which dropped and re-made about 8 cars and 20 people every 10 s on a two-player test). The model is the MP's (research
Q 2.1): every object the authority simulates is created once on each member (reliable), updated by priority within a
byte budget per member (unreliable, newest wins) and removed once (reliable); the member decides what it can hold and
says so. In 2.2 the host is the owner of the one area; a client is a member of it or runs its own world (solo), as
in 2.1. Package A of the round is built in steps; steps 1 to 4 are in, Package B/1 (content around every member) and
Package C (the area's actions and effects: gunfire, driver inputs, burning, explosions; with A/9).
The 2.1 streams are kept for players with mod.ini `world = 0` (switchable for the tests) and go to nobody else.

**Step 1: `world.h`** (no game code; `tests/world_test.cpp`). The records: `WWS1` updates (25-byte header with the
owner slot, the OwnerMap epoch, the owner's clock and a reference point; `CarRec` 29 bytes (30 with Package C's drive byte): id, vehicle-table index,
position in 2 cm from the reference point, smallest-three quaternion, velocity 1/128 m/s, angular velocity, the
damage / siren bits of 2.1 and the hit points; `PedRec` 20 bytes on foot: position, heading, flags (orientation sign,
mission, dead, crouching, ragdoll, burning, hidden or cutscene, seated), state `+516 << 4 | +520`, animation set,
action, velocity, team, gun in hand; `SeatedRec` 12 bytes: id, car id, seat, team, gun, flags), `WWC1` creates
(kind, authority, model, team, name, the look block `8238B0B8` writes, the car paint, a car's crew ids and the first
record with the reference point its position is from), `WWR1` removes (fade time, reason: gone or the area left),
`WWA1` member reports (cars and people held and the caps, the people models resident, the loaded chunks, the ids
declined, the copies shown within 50 m of its camera), `WWD1` set digests (the sorted ids a
member was offered, every 10 s) and `WOM1` (the OwnerMap: per slot its area and owner). The `Registry` (keyed owner
slot + handle, idempotent adds, removals queued for the members the id was created on) and the `Scheduler`: per
member and 50 ms tick the MP's priority (-10 a metre, +500 in the view cone or within 10 m, `82353DB0`'s constants,
plus the age), bands by distance to the member's camera (within 50 m 20 Hz, 150 m 10 Hz, beyond 2 Hz; parked and
resting cars and seated people 2 Hz unless something changed), shares of the budget per band in proportion when it is
short, a fair queue inside a band so nothing starves, at most 4 creates a tick (a car before its crew), no records for
a car a player drives (`kEntryPlayerCar`, or another player's authority), declined ids offered again after 3 s when
the member has room, and the digest. The budget per member is `world_upload_kbps` shared by the members, 6-24 KB/s
each, halved for 2 s on backpressure. Tested: 12 members x 200 objects keep 20 / 10 / 2 Hz within 50 / 150 m / beyond
at 1200 bytes a tick and slow every band alike at 570, never over the budget, deterministic.

**Step 2: the owner side** (`coop.cpp`, "The shared area mirrored" next to the network thread). Every game keeps a
registry of the cars and people its own game makes, fed where the game makes and removes them (the MP registers its
objects from the constructors through `82352D38` when both MP bytes are set; co-op hooks the same points and never
sets those bytes, H):
- cars: `82569700(source, ...)` after the call (`VehicleCreateHook`). The source at its 17 call sites (H): 2 traffic
  (the traffic creator `82411310` passes 4 for its police cars), 4 the response (`82411118`) and the gangs' cars
  (`825611D8`), 5 parked (`8241E378`), 6 special (`824224F0`), 7 scripts and cheats, 3 co-op's own copies
  (`making_car`; the MP's creators don't run in story mode) and 9 the El Train (`8242D248` under the train manager
  `8242EFB0`), which is left out and counted (trains as records are later). Cars a script makes during a mission are
  left out too (each game's mission makes its own; 2.4's experimental `mission_cars = 1` streams them, section 2.4).
- people: `8247A6D0(human, -, park)` after the call. It is the activation (H): the human goes on the active list
  `0x8309ABB8` (+3592/+3596) when `+200` bits 0 and 1 are set, else (r5 = 1) on the inactive list `0x8309ABB0`. It is
  called by the constructor `82477B78` (only when `+216` 0x4000 says the model is set up, else `8247A8E0` parks the
  human inactive), by `8247A9F8` - the people stream group's model-loaded callback (`+140` of its config in
  `82185498`), which activates every inactive human of that model - and by `8240B228` for queued mission
  characters once their model is in. So the same human comes again whenever its model is unloaded (`8247AA70`) and
  loaded again: the add is idempotent and counted as a re-activation. The kind is `+4216`, written before (report
  L: 1 ambient, 2 traffic crews, 4 police response, 6 special, 8 hood, 0/3/7 system and script characters). Co-op's
  own copies (`making_actor` around its `82479AB0` call, and the copies by handle), the players' characters
  (player-pool objects) and the test characters are left out.
- removal: `824789A8` (`HumanDestroyHook`) and `825324D0` (`VehicleRemoveHook`), when the game really removes the
  object. A fade-out the game begins is a removal at once, with the time it has left: a car's `82537D40` sets `+53976`
  0x40000000 with the clock `+54436` and the length `+54444` and fades every occupant with it; a person's `824517A0`
  sets `+212` 0x80 with `+2488` / `+2492`; both clocks are `*0x827AA6E0`, less 1800000000 past that (H).
- the registry is filled from the object table when the session starts (cars named by the traffic list, the parked
  records at `0x83AD346C`, the response squads, an El Train car by its vehicles.xtbl row 65; people only once
  active) and checked against it every 10 s ("audit": what the hooks missed both ways; it should stay 0).

On the host, 20 times a second (`WorldOwnerTick`): the members are the clients whose pose says they show the host's
world (`kPoseHostWorld`, 2.1's rule for the host's police: the client shows the host's traffic or people - from 120 m,
kept to 200 m - and the host's character, made within 60 m and removed past 130 m, held 2 s; with mod.ini police = 0
the bit is never set and nobody is a member yet); their camera from the pose (`eye`, the forward row of `cam`). The
registry's state is read from the game: positions; the car's chunk bit (`+120` 0x04000000, what
`vehicle_in_loaded_chunk` `824DAB38` reads: a car whose chunk unloaded is parked without physics and doesn't stream);
resting (slower than 0.3 m/s or no physics: 2 Hz) and a change of hit points, damage, siren or driver; a person's
activity (`+200`), seat (`+2496`/`+2500`: a seated person is a crew record of its car), and the rules 2.1 kept: mission
and script characters stream only while a mission runs, the story characters' models (Gat, Dex, Julius, Troy) never
on the street. Authority (F2): a car a player sits in or is getting into (this player's `+2496`, another player's
character's car or the car it is getting into) is theirs: its pose carries it, so no records go for it, and the
area's members are told with `kEventAuthority` (owner, id, the player's slot, 1); it comes back to the host's game
1 s after it rests with nobody in it (`kEventAuthority` ..., 0). It is never removed for that. Then `Scheduler::Plan`
with each member's budget (for the time since the last tick) and the stream point (the camera `0x827D9778` +44, where
the game's own stream point `82485DB0` starts and what its despawn radii are measured from), and the messages:
creates with their tail (a car's paint read again for each, `ReadCarLook`; a person's name and look block, `ReadLook`),
removes, the records packed into `WWS1` messages of at most 1146 bytes (each object described once a tick, a record
past 655 m skipped and counted), the digest every 10 s and the OwnerMap (reliable when someone joins or leaves the
area, else once a second). The network thread sends them in order, before the events. Backpressure: a member's link
whose unreliable backlog stays past 16 KB after a flush, that drops snapshots (`Session::LinkStats` per link, new in
`net.h`), or Epic's outgoing queue past 16 KB halves that member's budget for 2 s. A member's `WWA1` is taken (what it
holds and declined). The host still sends the 2.1 streams.

After the step 2 review (notes/coop-2.2/review_A2.md), with step 3:
- an entry that stops streaming after its create (its chunk or model unloaded, a mission person after the mission)
  stays in the digest and gets no records; after 5 s it is removed (reason 0) where it was created and created again
  when it streams (a model reload, under 5 s, costs nothing) (A2-2, world.h, world_test);
- the owner tick runs on a 50 ms grid that catches up (a 30 fps host ticks after 66 and 33 ms: 20 a second) with the
  plain budget per tick; the owner line's rates are sends per object per second over the ticks that ran ("t ticks")
  and the budget is what the member had over the window (A2-1);
- backpressure also when a link's reliable bytes not acked pass 16 KB or its RTT passes twice its lowest of the last
  10 s (at least 250 ms); Epic's queue and the link counters start from 0 each session (A2-3, A2-6);
- a Stop or a lost host turns the registry off; a client's own registry no longer keeps removals nobody takes (A2-4,
  A2-5);
- the audit and the activation hook skip objects queued for deletion (`+120` 0xC0000000, set by `823FE4A8`; the object
  stays in the table until `823FDBE8`) (A2-7); `kEventAuthority` goes only to members the car was created on (A2-8); a
  passenger or a player getting in takes a car's authority only while nobody but a player is at the wheel (A2-9); a
  handle still here 30 s after its fade began is counted when the audit adds it back ("back after a fade", A2-10); a
  car a script made during a mission is streamed once the mission is over, and only with `missions = host` (A2-11);
  the activation hook counts a re-activation only for a person active after it and logs which callers reach it
  (A2-15); "the first member in the area" once per session (A2-18).

**Step 3: the members' side** (`coop.cpp`, "The shared area mirrored (co-op 2.2, step 3: the members' side)"). A
client shows the host's world stream while it is in the host's area (mod.ini `world = 1`, the default; with `0`, or
while the host streams nothing - no OwnerMap from it for 3 s, the host's `world = 0` - 2.1's streams show the host's
world as before):
- membership: the client asks for the area within 90 m of the host's character and leaves it past 140 m, after 2 s /
  5 s (`kPoseHostWorld` in its pose: the spec's Package A numbers; step 4 moves the rule to the host); the host's
  OwnerMap says who is in. While in it, and until the last copy here is gone, this game's own traffic, people (and
  their models' own references), parked cars and police stay off as 2.1 kept them off (UpdateSharedTraffic /
  UpdateSharedPeds / OwnPoliceOff, now keyed to the area), and none of 2.1's copies are made (those there go, two cars
  / three people an update);
- one table of copies keyed (owner slot, id) with a reverse index by this game's handle, next to 2.1's maps; the
  predicates the hooks use (`IsSharedCarHandle`, `SharedCarHostHandle`, `SharedPedHostHandle`, `CrewSeat`,
  `OurCharacter`, `FindCar`, `CarId`...) know its copies, so a copy is protected, named and driven as 2.1's were: a
  player who gets into one gets into the host's car there (its pose names `(0, id)`);
- admission is the member's (spec 3.6): cars up to 32 less this player's own, one per other player and 4 spare (26
  with two players), people up to 56, made only within the live counts (vehicles `0x83710238` of 32 less 2, humans
  `0x8309ABAC` of 75 less 8; fading copies keep their place until they go, and a create waits for room, declined
  after 3 s); a person whose model isn't loaded here is declined while the models the people copies here want fill
  the people group's slots (12, one kept for the players); when full, a new copy nearer the camera than the farthest
  held one of its kind by 20 m (people 10 m) takes its place (that one fades, its crew with it), else it is declined;
  a crew member goes with its car (declined when its car isn't held, made after it); a car a player drives (the
  create's authority bit) is always taken, and a car copy a player's pose made before it came is taken into the table
  instead of a second car. Declined, evicted, lost (removed here by the game) and missing ids go back in `WWA1`; the
  host offers them again after 3 s while there is room;
- creates, 4 an update at most, with co-op's own sequences: `CreateVehicle` (and its model at the top priority) with
  the paint, `CreateActor` with the look block once it is set up (hidden until then, 3 s at most) and the host's team,
  crews seated with `SeatInVehicle`; a driver whose model hasn't loaded after 2 s in a moving car gets a lookalike
  (`LoadedLookalike`); a person not set up in 20 s is let go (offered again);
- updates: per id only a newer owner time; records before their create are held 1 s, after a remove dropped (5 s),
  for declined ids dropped, from before the OwnerMap's last change (its epoch) dropped. Cars move the way the game's
  MP frame moves a remote car (`823BB068`, H): the body (`+244`) is woken with `826CD960` - not a velocity setter: it
  takes only the body, returns at once when the body's island (`+92`, byte `+40`) is active or the body is fixed
  (`+153`), and otherwise puts the island on the world's active list (`826DDE40`; with no island it would write
  through 0, so co-op calls it only for an island that sleeps) - and the velocities are set through the body's motion
  (`+88`): vtable `+88` the linear, `+92` the angular velocity, 16-byte vectors, as `823BB068` does after each of its
  two `826CD960` calls. The velocities steer the car towards its path (2.1's error terms); it is placed (`82535C68`)
  only when off by more than 8 m + 0.35 s of its speed or turned by about 60 degrees, and left asleep when it rests
  where it should. A car without physics (`+50300 != 1`, which `823BB068` leaves alone) is placed with `82535C68(v,
  pos, rows, 1)`, which for such a car writes `+20`, `+32..+64`, bit 0x80 of `v+53984` and places its trailer
  (`+54628`) with it (H; review A3-9: the direct write left the trailer behind), at most twice a second unless the
  record moved.
  Hit points, smoke, fire, wreck and siren through `ApplyCarDamage` / `ApplyCarSiren`; a car a player drives (here, a
  character here, or named by `kEventAuthority`) gets no records. People follow 2.1's path follower (the same
  per-frame smoothing), with the host's flags (crouching; hidden or a cutscene actor there: hidden here, never
  removed; hidden too while a cutscene plays here), dead from the owner's hit points (killed as 2.1's copies), team,
  animation set, the gun in hand (`MatchWeapon`), seated or out of a car as the owner says (not while a player pulls
  them out here);
- removes fade the copy (cars `82537D40(v, ms, 1)`, its crew with it; people `824517A0(h, ms, 1)`, H) and co-op
  destroys it at the end of the fade, 4 an update (the game's own deleters never reach a copy: the traffic
  housekeeping `82410B80` walks the traffic list only; `HumanDestroyHook` lets the corpse clean-up's ask through for a
  fading copy, spec C2); a copy not made or not shown goes at once; a car a player sits in or gets into is let go (it
  stays as that player's car, named as the host's). Leaving the area (the OwnerMap, no OwnerMap for 3 s, the session
  over, the host lost) fades everything;
- the host's person events (actions, knockdowns, hit reactions, effects) and this player's hits on the copies work on
  the world stream's copies as on 2.1's (the host counts the people of its world stream as sent); a hit on a crew
  member goes as `kEventPedDamage` with its own id (not by seat);
- `kEventAuthority`: a car a player drives gets no records here until the host says it is back with its game;
- `WWA1` every second while in the area, and 200 ms after a decline; the digest is diffed against the table: missing
  ids go back as declined (the host creates them again), extra ones are removed.

Logs (every 10 s on a member):
- `World (member of slot 0's area, epoch e): held cars a of o offered (declined d: reasons), people ...; now cars,
  people (queued, fading, driven by players); copies made / faded / destroyed (at once, past deadline), let go (their
  crews), gone here; stand-ins, waiting for a model, models loaded l of the people group's 12 slots (11 for copies;
  player bodies, waiting); creates applied (held for a car,
  replaced), updates applied (old, before the create: used / expired, after a remove, declined, old epoch), removes
  (gone, area, digest); placements p per car per second (placed, steered, woken, written without physics); crews
  seated (refused), out of a car; authority taken / back; bytes in; malformed`;
- `World set match: held h of n (declined d, model-wait m, chunk c) = x%; missing k [ids]; extra k [ids]` (each digest);
- `World update age: p50 a ms, p95 b ms` (apply time less the owner time, less the window's least receive delay);
- `World match: within 50 m of me the host lists cars c / people p, I show c' / p' (fading, waiting, declined)`;
- `World models: per id refs (pins, copies) ...; n with more refs than pins + copies + 1`.
One-shot: the first create and remove applied, `full: cars 26/26, declined id` (20), area joined / left (20), the
chunk list (`World chunks here`: `8211E380(id)` says a chunk is loaded when its record's `+20` bit 0x80 is clear, H:
the mask's bit is the list index), `Copy never finished loading` (25), authority changes (30). The owner adds a
`World (owner) match slot S` line from each member's `WWA1` (two clocks). Pass numbers: spec section 6.

Measured offline (world.h with a two-player street: 12 traffic cars with drivers, 6 parked, 26 people, churn of a car
and two people every 2 s; budget 1200 bytes a tick): about 11 KB/s per member with every moving object at its band's
rate (20 / 10 / 2 Hz), resting ones at 2 Hz, 3 creates a second, never over the budget; 11 members at 568 bytes a tick:
10 KB/s each, 19 / 10 / 2 Hz. In step 2 that comes on top of the 2.1 streams (about 41 KB/s to each client near the
host) and the poses.

Log (every 10 s): `World (owner): area: ...; set: cars (traffic, parked, response, special, script; trains and mission
cars left out, inactive, players' cars), people (ambient, traffic crews, response, hood, special, mission/script;
seated; story characters left out, mission/script outside a mission, inactive); audit +a/-b; made cars / people,
removed cars / people (of them as their fade began), re-activated people` (the host's own churn: the baseline the
members' numbers are judged against); per member `World (owner) slot S: offered (cars, people), held (from WWA1),
declined (re-offered), created (KB; catch-up left, deferred for the budget), removed (gone, area), near / mid / far Hz
of the moving objects (resting apart), records in messages, bytes of the budget (and the plan's own count),
backpressure halvings, digest (ids), records skipped (past 655 m), WWA1`; `World (owner) authority: n cars driven by
players now (slots), taken, returned`. One-shot: `World registry on` (the count at the start and the MP net time
`*0x83061944`, a probe), `World (owner): the first member in the area` (what it catches up), `slot S joined / left
the area` (40), the authority changes (30). A client: `World (this game's own, a client): registry; ...` and
`World (member, 2.2 step 2: counted, not applied yet): from the host n update messages, creates, removes, digests,
owner maps, KB/s`.

Protocol 6 (2.1 can't join; 7 since the review fixes below): message types `kMsgWorld` (WWS1), `kMsgWorldCreate` (WWC1), `kMsgWorldRemove` (WWR1),
`kMsgWorldAdmit` (WWA1), `kMsgWorldDigest` (WWD1), `kMsgOwnerMap` (WOM1) after `kMsgNotoriety`; events
`kEventCarDamage` (25, the members' side) and `kEventAuthority` (26). The session's relay needed nothing new (the host
sends to one slot; a client's message for the host stays there). mod.ini: `world_upload_kbps = 1000` (each member
gets at least 6 KB/s, more than the setting in all when many share it, and at most 24 KB/s) and `world = 1` (the world
stream shown on a member; 0: 2.1's streams, and on the host no world stream; set it the same everywhere). `WWC1`
carries its reference point (39 bytes), `WWA1` the near counts (26 bytes).

**Step 4: the cut-over, kept switchable** ("The shared area mirrored (co-op 2.2, step 4: the cut-over)").
- Membership is the host's (spec 3.1/3): a client whose pose says it shows the world stream (`kPoseWorldStream`, pose
  bit 23: mod.ini `world = 1` there and the host's world messages within 3 s) joins the area within `member_join_m` of
  the host's character (2D, `PosePacket.pos`) after 2 s in a row and leaves past `member_leave_m` after 5 s; at once
  when its pose is 3 s old, it stops showing the stream or it left. mod.ini on the host, 40-400 m, leave at least join
  + 20; 150 / 200 m with Package B (90 / 140 m for a test of A alone). The OwnerMap's epoch changes with each join and
  leave as before.
- 2.1's traffic and people streams (`CaptureTraffic` / `CapturePeds`, `kMsgTraffic`, `kMsgTrafficNames`, `kMsgPeds`,
  `kMsgPedLook`) go only to the clients that don't show the world stream (their `world = 0`, or no OwnerMap seen yet;
  sent to those slots alone, or to everyone when that is everyone), and nothing is captured or named as sent when no
  client needs them. The 2.1 code stays for that case; the owner line says per client whether they still go.
- `kPoseHostWorld` has its 2.1 meaning again ("this client's own police are off": `OwnPoliceOff`), the input of the
  host's police response. On a client in world mode `OwnPoliceOff` = a member by the OwnerMap and the host's character
  shown here (held 2 s), one membership rule. Another player's character is kept to `member_leave_m + 30` m (the host:
  a member's; a member: the host's) instead of 130 m, still made within 60 m, so the host's police can come for a
  wanted member anywhere in the area.
- A member's levers of spec 3.8/36 that were missing: `824290D0` (the spawn-region manager: hood gang members and cops
  `82427548`, the special spawns; one caller, `824131F0`; the MP client's skip) returns while the area is shown (its
  enable bytes `0x83AE3A90/91` are left to the scripts), and `spawn_trains` `0x827ACF87` (read by the train manager
  `8242EFB0` as `+10407` off `0x827AA6E0` right before it starts a train, `8242E3D0`) is 0 on a member and on a host
  with members (3.8/37: trains are not mirrored; running ones finish their route); put back as it was afterwards. The
  one-time removal of a member's own parked / response / special cars is `RemoveOwnParked`'s sweep (every unoccupied
  car of this game's own within 160 m, every 500 ms) and the response removal at `OwnPoliceOff`'s edge, as in step 3.
- The games compare their vehicle tables when a player joins (`kEventVehicleTable` 27: the row count `*0x8371023C` and
  a hash of the rows' name hashes `+120`): a difference is logged (`Vehicle table:`) and shown (review A2-12). A car's
  paint is read once a tick whoever its create goes to (A2-16); `Session::LinkStats`' reliable bytes in flight are kept
  as they change instead of walked (A2-17).
- The step 3 review (notes/coop-2.2/review_A3.md): the stream counts as there while the host is paused
  (`HostPauseHold`) and for 10 s while its state packets still come (its game thread stalled or loading), and from any
  world message, not only the OwnerMap; a member that leaves on its own says it doesn't show the stream until the
  host's OwnerMap has it out, so the host starts its offers over and the two never disagree silently (A3-1); the
  member's area, epoch, declines, tombs, early records and digest start over at each session's start and end, and an
  OwnerMap more than 1000 epochs behind is a new host's (A3-2); world.h re-offers a declined id only into the room the
  member reported, counting the tick's own creates as held until the next WWA1, doubles the hold on each repeat decline
  (3, 6, 12 ... 30 s; forgotten once a create has held 30 s) and re-offers a person only while the member can load its
  model (it has it, or fewer than 11 people models there) (A3-3, world_test 6b); a car named driven gets its owner's
  records again 1 s on (a lost "back" event, A3-4); a copy a create replaces is destroyed within the 4-an-update budget
  (A3-6); the velocity vectors handed to the motion's setters are 16-byte aligned (`lvx`, A3-8).

Logs: the owner's per-member line starts `World (owner) slot S (in the area at d m; 2.1's traffic and people streams to
it: off | still sent)`; one-shot `World (owner): slot S joined the area at d m ... / left the area at d m (why)` (40),
`World (owner): 2.1's traffic and people streams to slot S on | off (why)` (40), `World: new trains off here ...` /
`trains back`, `Vehicle table: slot S has n rows (hash), this game n (hash): the same | DIFFERENT`; the member line ends
`this game's hood / special spawn passes skipped n, trains off | as the game has them`; `World (member): the host's
OwnerMap has this game out too: it can join again` (20).

**Package B/1: content around every member** ("The shared area: content around every member", notes/coop-2.2/plan_B.md).
The engine fills the world only around the viewpoint its spawners read (parked cars 80 m in 2D from the player
`*0x8309ABEC`, traffic 130-140 m and people 90 m from the camera `0x827D9778`, research S 0.2), so on the host each
spawner also runs for one member's viewpoint at a time, its radii unchanged and its caps shared by the area (one street
of 12 cars, 26 people, 6 parked). The viewpoints: this game's camera and player, and each member's pose (eye, the
camera's forward row, position, forward speed from its car's velocity or its camera's moves), refreshed by the owner
tick. A member's viewpoint stays with its last pose while it is in the area (pose under 3 s, as the membership): every
protection below uses it, so a short gap of lost poses doesn't empty its street; only the spawn passes need a fresh one
(under 500 ms) (review AB-2). Nothing runs without members, in a cutscene, or with the host's `world = 0`; the
traffic and people passes also need the host's `traffic = shared` / `peds = shared` (H for every address below, read
in the asm):
- **Parked cars**: `8241D930` (one caller, `824131F0`) only spawns records already queued (state 2, list `0x8309A480`;
  record `+100` the state) and re-states those beyond 80 m of `*0x8309ABEC` (`r26`, read once: `+20`/`+28`) through
  `8241D210(rec, 0|1)` at `8241DA10` / `8241DA24`; `8241DC78(0)` queues new ones from the 6 nearest candidates
  `8241DAE0` picks (state 1, `0x8309A47C`; `+20/+24/+28` of the player). After the game's own pass the hook runs
  `8241D930` and `8241DC78(0)` once more for one member (each at most every 200 ms) with `*0x8309ABEC` on a stand-in
  that holds only the member's position (a human object's 4252 bytes, zero elsewhere: handle 0, type 0, so a reader
  outside the checked closure - a `bctrl`, another thread - sees zeros, not old stack; review AB-4); `8241E378` (the
  spawn: the vehicle constructor, seats, model references) and `8241D210` (model references, the car's destroy) run
  with the real player. `8241D930`'s re-state of a queued record to the candidates (state 1, LR `8241DA14`) is vetoed
  so one pass doesn't undo another's queue: in a member's pass for a record within 80 m of this game's player (its own
  street), and in any pass for one within 80 m of another member only while its chunks are resident here (`8241DA30`'s
  own test: `8250F3C8` on the chunk ids of the table `0x83E8ABD8` at the record's `+88` / `+104`; else it can't spawn
  and would hold one of the queue's 6 for good) and for 3 s at most per queueing; never with `member_spawns` lacking 1
  and never its state 0 (LR `8241DA28`: no chunk table entry). `8241D6B0`'s array (`0x8309A444`, {record, distance
  squared} x 6, count `0x8309A474`; the only input of `8241D5C0`, which makes room) loses only the spawned records
  (`+100` = 3) near a member, so a queued one can still be made room from (review AB-1: queued records near a member
  could fill the area's 6 for good, the host's street with them).
- **Traffic**: `824129C8` asks `82412298(out, &centre, &dir, f1 speed, r7 ahead)` (one call, LR `82412F30`) for spots
  around the camera; `82412298` reads no camera and the player only for its highway flag (`+21260`). On a member's turn
  the hook gives it the member's eye (written to the caller's free `r1+80`), its forward (into the caller's copy at
  `r1+96`, which `r5` points to), its speed and "ahead" (over 2.2352 m/s, or - the game's own sparse-street rule at
  `82412EB0` - fewer cars near it than a quarter of the wanted; review AB-11); the turn goes to the member furthest
  below its share of the wanted count (the cars within 140 m of each eye), not a full one (its cap spent on copies
  while it shows its share near its camera by its `WWA1`; a member full of the host's far copies still gets cars:
  admission swaps a far one for a nearer, review AB-7), the host's own at least every other tick. One car per tick
  (200-300 ms) for the whole area, as the game spawns. `82412878` (the despawn test: r4 the car, returns 1 to remove)
  keeps a car within 140 m (3D) of a member's eye.
- **People**: `8240FBB0` spawns on the updates where its timer `0x82832744` hasn't run out against `*0x827AA6E0` and
  `*0x8370FEBC` is set; on those, after it, a member below its share of the wanted people (90 m of its eye; full by the
  same rule as traffic; 10 passes a second in all, each member at most every 250 ms) gets `8240F148(1, !moving)` with
  the camera's eye (`+44`) and
  forward row (`+104`) the member's: its picker `8240EC88` and the spawn `8240DD40` read only those of the camera (and
  the view's FOV). The constructor `82479AB0` (LR `8240E178`) and the set-up `820E9550` (LR `8240E1AC`), whose callees
  reach the camera code, run with the real pose; never the make-room path (`8240E448`) for a member: the pass runs only
  below the wanted count and `8240E338` says "not removable" inside it. `8240FA90` (returns 1 beyond 90 m of the camera)
  keeps a person within 90 m (2D) of a member's eye; `8240E338` (make room's "removable", one caller) says no for one.
  A person made in a chunk the host hasn't loaded is queued for deletion inside the constructor (the region check
  `8243EB60`): counted.
- **The slot allocator**: `82400968` calls `823FFF78(sys, kind, level)` when a creation finds its pool full; it tries
  the categories of a lower priority through their evict functions `823FFA48` (people, list `+0`) / `823FFC48` (cars,
  `+4`), which take the valid record (`+12` handle, `+20` state, not a visible one in pass 1) with the largest `+24`,
  the distance squared from the host's player: a member's first. The hooks give the records near a member the lowest
  score for the call, so an unprotected one goes first; with nothing else there the creation still takes one when its
  kind's priority (`0x82038858 + 28 x kind`) is below 3: priority 0 (a mission's, a script's, co-op's copies), police
  (kind 4, priority 1) and special / gang (6, 2) (review AB-6). Only the street's own kinds (priority 3: ambient,
  traffic, parked) would be refused, and those evict nothing (plan_B 6), so in practice nothing is refused.
- **Density**: `824194A8` (traffic wanted, <= 12) and `82419208` (people, <= 26) return `max(x, min(cap, x (1 +
  member_density x members)))`: never past the game's limits, never below what the game wants (a script's density
  can take it past them), and a base of 1 or less is left alone: the game clamps to at least 1 after multiplying by
  the density (`82419304`), so a mission's density 0 (an empty street) comes out as 1 and stays 1. The member traffic /
  people passes don't run while `set_traffic_density` (`0x827AD060`) / `set_ped_density` (`0x827AD064`) is 0
  (review AB-3).
- **The host's cars another player drives** (review A2-14, the B/2 half): `82412878` (traffic far from the host's
  camera) and the parked removals keep a car of the host's another player's character sits in or gets into, in any
  session (members or not, `world` 0 or 1): removed, it came back on the host as a car copy outside the registry and
  never streamed again. The allocator's car score `823FF730` already leaves out cars with people other than ambient
  ones in them. A car the game removes anyway (a script, an explosion) is still re-made outside the registry (open).
- mod.ini (host): `member_spawns` 1 parked, 2 traffic, 4 people (7; 0: only the keep-alive and the allocator's cover),
  `member_density` (1.0; 0 off), `member_join_m` / `member_leave_m` 150 / 200.
- Scaling (review AB-10): the area is one street (12 cars, 26 people, 6 parked), so with the host and 3 members each
  has about 3 cars, 6-7 people and 1-2 parked cars near it, and at 12 players about 1 car and 2 people; the B pass
  numbers below are for 2-3 players.
- Visibility (review AB-13): the people pass's frustum test (`8218DE20` in `8240DD40`) and `823FE380` use the render
  view, not the swapped camera, so people made for a member are kept out of the host's view only and can pop in
  inside the member's (it fades its copies in anyway). Threads: `*0x8309ABEC` and the camera's eye / forward are
  swapped for 20-150 us inside the world update; a reader on another thread (a render-side reader, another mod's
  on_frame) could see the member's pose for that window.

Logs (every 10 s with the World lines): `World (owner) B: viewpoints n (members m: slots, "(stale)" after one whose
pose is 0.5-3 s old; went stale k); wanted cars w (boosted from w0) / people p (from p0); parked: member passes,
candidates near members (of the state-1 records), made for members, re-states vetoed (not vetoed: chunk not here,
held 3 s), removals kept; traffic: turns t of T ticks (0 spots z), made for members (in their view); people: member
passes (full), made (deleted by the region check at birth); member passes skipped for density 0; kept alive people /
cars; make room spared; evictions refused (protected taken for a creation of priority 0-2: per kind; covered); cars
another player sits in kept` (without members, on any host: `World (owner): cars another player's character sits in
kept from this game's traffic / parked removals n`); per member `World (owner) B slot S: d m from
this player (in since t s); near its eye (people 50 m / cars 100 m): this game has a / b (registry; streaming); made for
it parked / traffic / people`; on the member `World (member) B: near me (people 50 m / cars 100 m of my eye) by distance
to the host: <40 m: people a0, cars b0 (n0 s); 100-150 m: people a1, cars b1 (n1 s); session ratio a1/a0 = x%, b1/b0 =
y%` (the pass: x >= 60 %, y >= 80 % with n0, n1 >= 60 s, A's numbers unchanged). One-shots: `World (owner) B: hooks on
(parked/traffic/people, member_density; the traffic ring *0x82832724 and the removal radii *0x83AD2640/44)`, the first
pass of each kind, `parked re-state vetoed for record R (spot S) near slot S` (10), `eviction refused for kind K` (10),
`people made for slot S deleted at birth` (10), and any call a member's people pass makes to `82479AB0` / `820E9550`
from elsewhere than the spawn's own call (plan_B's check). Perf parts `B parked`, `B people`, `B keep` and `B keep in a
pass` (an eviction or keep test inside a member's pass: its ms are part of `B parked` / `B people` too; review AB-12).
The start-up line names `member_join_m` / `member_leave_m` instead of a literal 150 / 200 m (AB-12).

**Package C: the area's actions and effects** ("The shared area: actions and effects", notes/coop-2.2/plan_C.md). What
the host's people and cars do shows on every member's screen the way the game's own MP mirrors its NPCs (`823A8240`
applies a fire byte, a burn byte and the aim from the NPC's state, not an event per shot). mod.ini `world_actions` (1;
0 turns all of it off on that game, sent and applied alike; each game tells the others its setting with
`kEventVehicleTable` a[4] when a player joins, and Package C runs between two games only when both have it on).
Protocol 6 was kept for Package C itself (no protocol-6 build went to testers); the review fixes below make it 7, so a
2.2 build from an earlier step is refused at the join with the version message instead of silently showing nothing
(reviews A3-16, AB-8).
H for every address below (read in the asm):
- **Wire**: `PedRec` / `SeatedRec` keep their sizes; the weapon byte is the gun's weapon-table index + 1 in bits 0-5
  (an index past 62 goes as 0, counted), bit 6 the gun raised (`+216` 0x80000000, the 2.5 s hold `82449708` sets) and
  bit 7 "a `FireExt` follows" (7 bytes: the person's running round count mod 16, trigger held, blank, aim valid,
  drive-by mode, and the last round's first-pellet hit point in 2 cm from the header's reference). A person who fired in
  the last second carries one in every record (so the last count and the release always arrive), never in a create;
  the scheduler charges it (`Entry.extra`, 27 / 19 bytes a record). `CarRec` is 30 bytes: damage bits 6-7 brake lights
  and reversing, and a drive byte (the steering `E+412 + E+416` in 6 bits, the horn, the engine bit `+53976`
  0x08000000). `kEventExplosion` (28) and WPJ1's `shooter` are new; `kEventVehicleTable` also carries the explosion
  defs (`*0x8370D114`) and weapon counts, compared at the join.
- **Gunfire**: on the host `FireHook` runs a world person's `82470A50` call with the rounds counted (a round re-arms
  the refire timer, ammo `+24`, or uses one, `+16`: 1-4 per call), its blank flag (`r6`), its gun's Trigger_Type
  (`+96`) and the first pellet's hit point (`BulletRayHook` by the shooter `r5` of `8246FD18`); a seated person's
  records go at the band rate while it fires. The member takes the count difference per record (exact while records
  come: a record without a fire part means no round for a second, so the first round after quiet plays too; from the
  first fire part or after a gap of 1.5 s with no records only onwards); single-trigger guns queue up to 4 rounds,
  each played as other players' shots are (`TryRemoteShots`: the gun matched, never empty, raised on foot, the "run
  180" turn stopped, the trigger latch `ammo+12` 0xA0 cleared, `+24` = the clock, `+40` = 0) with `82470A50(copy, 0,
  &hit point, blank)`; automatic guns hold the trigger (`+3592` 0x40, a call each update when the refire timer is due,
  so the gun's own rate) and release it (`82474738` + `82476B70`) when the hold bit goes or its fire part is 300 ms
  old. All under `fire_replay_depth` (no damage, no blood or shove) and `copy_fire_replay_depth` (`DetonateHook`: no
  car takes the round's hit: the car this player drives gets the host's round by `kEventCarHitByPed` below); within
  150 m of the camera, 8 calls an update in all, a round older than 750 ms dropped with its last wait. A seated copy fires blanks (flash and sound; drive-by mode
  on copies is later). Its gun settles like the other players' (400 ms to another, 1.5 s to none, kept while rounds
  wait); `MatchWeapon` logs copies as "World copy gun" (their own cap) and stops after 3 tries of a gun a copy's
  model doesn't take, until it should hold another (review A3-11). The copy's own replayed rocket or throw is refused
  (`ProjectileCreateHook`); the host's person's projectile comes as WPJ1 with its id (unicast to each member of the
  area; the member's copy is credited to the copy of that person), its explosion too after the person is gone.
- **Driver inputs**: the member answers the brake getter `82533DA8` (r3 handle, f1 = `E+408` with `E = *(v+224)`
  while byte `v+50300` is 1) and the reversing test `82535C00` (r3 the vehicle: physics up, `info+284` != 6, `(int8)
  E+180`) for a car copy with the host's bits from the last second (a lock-free slot per handle index, since any thread
  may ask): the tail lights `8255A730` read the 10-frame brake average `v+53500` that `82527ED8` fills from the getter
  (> 0.2 lights them), the reverse lights `8255ACA0` need `82535C00` and the engine bit, which is written to the
  host's while it brakes or reverses. The horn is held with `825241F0(copy)` each update while the host's sounded in
  the last 150 ms (`825231A0` stops it on a frame without a call); the host notes its own horns with a post-hook on
  `825241F0`. `E+412` (only `825872E0` writes it; the physics' packer `82583810` steers by `E+412 + E+416`) gets the
  host's sum less this copy's own `E+416` once per record, for a copy with physics.
- **Burning**: PedRec flag 5 (`+216` 0x00400000) lights the copy with `8245E3A0(copy, 0, 0, 0, f1 = 0.0)` as
  `823A8240` does, its refusal bits `+120` 0xD0000000 masked for the call (counted), once a second while it differs;
  `8245E0B8(handle, 0)` puts it out. `IgniteHook` refuses the member's own ignites of copies (explosions `82458A50`,
  fire patches `82142E18`, spreading `8245ED40`, burning cars `82529838`, by their return addresses). When the 4 burn
  slots (`0x8309A9E8`, 24 bytes, 12 with the MP byte) are full the ignite puts out a dead or gone one, else the one
  furthest from the camera; a copy is not lit when that would be this player or another player's character.
- **Explosions**: on the host `ExplosionHook` sends a prop's death (`82138AE8` from `823FAA38`, LR `823FAC68`: the
  def `*(*(state+136)+140)`, the mover's uid `*(*(mover+216)+40)` and sub `+224`) and a car's second explosion
  (`8252B870`, LR `8252B9B0`: def `info+872`, the call's `+92` word `info+24`) as `kEventExplosion` to the members
  within 200 m (def index `(def - 0x829ACEB8) / 140` below `*0x8370D114`, position, the forward row, the attacker:
  the host's player, a player's character or a person of its world); wrecks (`8252CBAC`: the member's wreck replay
  explodes), projectiles (WPJ1), the deferred queue and replays are counted, not sent. The member plays it with
  `82138AE8(def, source, attacker, rows, pos, 0, 0, 0)` under `world_boom_depth` (the source: its mover by
  `824995B0(uid, sub)`, or its car copy, whose own `info+24` goes in the `+92` word; never this player), skips one
  this player caused (its own game exploded it) and a prop that exploded here in the last 120 s; a local explosion of
  a prop the host's exploded here, or of one a copy's replayed round broke (the host's round decides), is held
  (`823FAA38` then shows only its own visual fallback), and a copy car's own explosion (not the wreck replay, LR
  `82542BFC`) is held: the host's comes. The host no longer forwards its people's explosion and fire hits on a member's
  character (`kEventPlayerHitByPed` under `explosion_depth` / `fire_update_depth`) to a member that plays them itself:
  each game plays explosions and fire on its own player (R 3.0).
- **A/9, a member's bullets on the host's cars**: the member's `DetonateHook` sends `825397E0`'s arguments from the
  bullet path (`82536270`, LR `82536470`, the caller's `+84` = 2) for this player's round on a car copy of the
  host's world (`kEventCarDamage` 25: the car's id, the weapon's table index, the damage `r10`, the hit point `r7`,
  the normal `r8`, the push `r9`) and the copy takes nothing; the host applies it with `825397E0(v, the member's
  character, 0, weapon info, ...)` and the call's constants (`f1` `*0x820380CC`, `f2` `*0x8208A354`, `+84` = 2,
  `+92` the push) under `replicated_depth`, and its replay of that member's round leaves a car of its world alone (the
  member's screen decides the hit). A car a player drives is left to the replays of the round, as in 2.1.
- **The other direction: the host's people's rounds on the car a member drives** (`kEventCarHitByPed` 29, built with
  the review fixes; the mapping is A/9's): on the host a real round (not a replay) of a person of its registry on a
  car another player's character drives (seat 0) - that player's own car's copy, or a car of the host's they took -
  reaches `DetonateHook` from the same bullet path (LR `82536470`, `+84` = 2); for a member of the area with Package C
  on it goes to that member with the car as its pose names it (owner slot and id: `CarCopyRef`, else the host's own
  handle), the weapon's table index, the damage (16 bits), the hit point, the normal, the shooter's id and the push
  (direction and length), and nothing happens on the host (2.1's block refused it there anyway: the car is another
  game's). The member finds the car (`FindCar`), applies it only to the car this player is in, with the copy of that
  person as the attacker (skipped when the copy isn't here: `825397E0`'s attacker paths want a person), through the
  same `825397E0` call as A/9 under `replicated_depth`. The copy's replay of that round here still hits no car
  (`copy_fire_replay_depth`), so the car takes the round once. Before, a member's car took no NPC bullets at all.

Logs (every 10 s): the owner's `World (owner) gunfire: rounds r by p people (bursts, blanks, hit points, aim past 655 m,
guns past index 62), fire parts in n records (KB)`, `World (owner) driving: car records with brake / reverse / horn /
steering (of n with physics, n in all); lights forced on / off`, `World (owner) explosions: sent (props, cars' second),
skipped as wrecks / projectiles / queue / replays / far / other, bad def; NPC projectiles sent, exploded`, `World
(owner) members' bullets on its cars: applied (car gone, a player drives it, bad); their replayed rounds' car hits left
to their events; its people's rounds on cars members drive sent to them (weapon not named)`; the member's `World
(member) gunfire: rounds played (single, held frames, releases, blanks) of n counted from the host; waited/dropped:
not set up, lying, other gun, no ammo, raising, refused, too old, past 150 m, frame cap; guns given (carried, failed;
given up after 3 tries), raises; cost ms/s (us a call)`, `World (member) driving: brake shown (getter answers),
reverse shown (answers), horns frames, steer written (no physics, mean |E+416|), engine bit written`, `World
(member) burning: lit, put out, relit; refused (in a car, slots full kept own, +120 masked, +3694, dead, water, other);
own ignites of copies refused (explosions, fire patches, spreading, cars, other)`, `World (member) explosions: played
(props with the prop here / without; cars' second, copy missing), skipped here already / own act, held again here,
held from copies' rounds, chain props recorded, local copy-car explosions held, bad def; NPC projectile copies made (exploded), copies' own
launches refused; this player's bullets on the host's cars sent (weapon not named), copies' replayed rounds on cars
skipped; the host's people's rounds on this player's car applied (car not here, not this player's, shooter not here,
bad)`. One-shots: `World copy ID (model): round of GUN not played: why` (20, the first of each reason), `World copy
gun: ...` (20), `World copy ID not set alight: why (+120, +216, +3694, +2496)` (10), `World explosion from the host: def
n (kind k) at x y z, source found|none, attacker who` (10), `World (owner): slot S's bullet hit car ID here (weapon,
damage)` (10), `World (owner): a round of person P hit car C (owner slot o) that slot S drives: sent to it (weapon w)`
(10), `Explosion defs / weapons: slot S has a / b, this game c / d: the same | DIFFERENT`. Perf part `world
fire` (in `world apply`).

**People models and car copies: where the references go** (notes/coop-2.1/model_refcount_investigation.md 7.2a and
9.4; 2.1.4 balanced the per-character reference and measured the rest). H for the calls below (read in the asm):
- **Pins at level 0.** `PinModel` asks a people model (group 0) with `8250C750(group, id, 5, level 0, now)` instead of
  the top level (13): the pass `8250D3E0(group, level)` pairs a level's requests only with that level's slots, and
  the top level had only the slots `8241BD40` last moved there (the "8 loaded" plateau of every client log, then 4-6,
  with slots idle at levels 1, 2, 4, 5 that nobody asks at). `ReleasePins` releases at the pin's own level when a
  reference was taken (`ModelPin.held`). Other groups (car models) keep the top level (`pin_top`). With mod.ini
  `people_level0 = 0` or `world = 0` people are pinned at the top level again and the table is left alone (2.1.4).
- **The quota table** (as corrected by the final review, F1/F3). The game's own people do *not* all ask at level 0:
  `82419A28(desc, kind, level)` picks a group-0 request's level from the character's category (`8241CBF0`: 2-11 by
  category, 12 with `desc+600` 0x800; 0 only in MP or without a descriptor) - homies, script-prioritised mission
  characters, this game's own police and gangs. The live table `0x827ACFFC` (14 words) is rebuilt by `8241BE28`
  (zeroed, then 12 at level 0 in MP, else from its defaults), copied into the group's `+196` by `8241CA80`, and
  raised by `8241BD40(0, flag, n)`: for levels 1-13 each level is raised to its count `8250E208(group, level, 5)` (the
  level's queued requests - bound or waiting, the pass pushes them all back - whose highest kind is exactly 5) out of
  level 0's spare (`table[0] - n`), never shrinking one, then `8250F780(0, table, 14)`; `82413398` runs all three
  (co-op's own `SetOwnPedSpawning(false)` and missions call it). On a client, once a second and only while it shows
  the host's people (`peds_shared`, or in the host's world area), level 0 gets the slots of the levels nobody asks
  at: a level with anything in its request queue (`*(group+56) + level * 24`, `+0` the count) keeps the game's own
  slots and at least its kind-5 count (`8250E208`, as `8241BD40` would give it), and keeps them 10 s after its last
  request (a set that comes and goes doesn't move slots to and fro); the rest go to level 0 (the group's
  slot count `+0` less the kept ones, `people_models` included). Applied with `8250F780(0, 0x827ACFFC, 14)` (it copies
  each changed word to the group's array and sets its dirty bit `+200` 0x80, so the next pass's balancer `8250D0A8`
  moves the surplus slots down); skipped while the group's pass runs (`+200` 0x20). A table that isn't co-op's last
  was rebuilt by the game: it is the game's (the levels it raised kept for the put-back) and co-op's is applied again
  10 s after co-op's last write at the soonest, logged. When the client stops showing the host's people, and when the
  session ends, the game's table goes back, then `8241BD40(0, 0, 0)` as `82413398` does after `8241BE28` / `8241CA80`,
  so levels with kind-5 requests get their slots at once (single player keeps the game's 8 1 1 0 1 1 0 ...).
- The 2.1.4 guard (`ModelRefGuard`) stays log-only; it now counts a pin by its reference, at any level. The 10 s
  `slots here` dump keeps each slot's level and the quota line (`slot quota per level 827ACFFC`).
- **Car copies.** `CreateVehicle` takes the model reference through the game's own `82532360(vehicle, 5)`
  (`8250C750(group, *(*(v+50324)+0), kind, 0, 1)`, then `v+50304` = kind) instead of `8250C750` directly (the
  network creator `823BA888`'s way, which leaves the constructor's -1 at `+50304`): the vehicle destructor `825324D0`
  releases the kind noted at `+50304` when it isn't -1, so a copy's reference goes with the copy.

Logs: `People-model slot quota set: 827ACFFC was q0 .. q13, now w0 .. w13 (level 0: the slots of the levels nobody
asks at here; kept for this game's own requests at level: L (a asking, k kind 5) ...)`, `People-model slot quota
rebuilt by the game: q0 .. q13 (kept; co-op's applied again 10 s after the last time | co-op's applied again | the
game's own from now on)`, `People-model slot quota put back: 827ACFFC was ..., the game's ..., after its recompute
8241BD40 ...` (30 in all); `Model priority: n models pinned (people p at level 0, the rest at the top level), r
released, h held; slot quota written w times, rebuilt by the game g times (co-op's applied now: yes|no)`; `Car copy ID made: model M, reference kind noted at +50304 = 5` (3; `-1`
would mean the old way), `Car copy ID made with a model reference noted already` (5), and every 10 s `Car models: per
model of a copy here its kind-5 references (copies, this game's own; copies noted at +50304): ID k (c, o; noted n) ...;
m with more references than its cars + 1` next to the 2.1.4 `Car models here` line.

**Review fixes** (notes/coop-2.2/review_A3.md minors, review_A4_B.md, review_A2.md A2-14, impl_C_notes open point 2;
the list with what was applied is in impl_C_notes.md, "Review fixes"). Besides the B and C changes above:
- **Let-go cars** (A3-5, A3-7): `WorldFade` and `WorldDestroy` share one rule, `WorldLetGoCar`: a car copy this
  player sits in becomes its taken car (named as the host's), one another player's character sits in or is getting
  into becomes a car copy that player's pose drives; the crew copies sitting in it leave the table with it as 2.1's
  pulled-out drivers (removed 4 s on, once out of the car; `ReleaseOrphanDrivers` now runs every update on a client,
  world mode too) and go back to the host as "gone here", so its offer starts over. Before, they stayed copies of a car
  no longer in the table and were faded and destroyed next to this player.
- **2.1's copies dropped with a player in them** (A3-12, the `world = 1` drop and the out-of-range drop): this
  player's becomes its taken car with the crew as pulled-out drivers, another player's character's a car copy
  (`AdoptSharedCar`); before, the car became an anonymous car of this game and its crew this game's own people.
- **A seated create whose seat a player has here** waits (`WorldMake` returns made / wait / failed), declined "pool"
  after 3 s like a full pool, instead of three failed tries (A3-10).
- **The host's person events** (actions, calls, hit reactions, effects) go to every game only while 2.1's people stream
  has that person (players with `world = 0`), else only to the members of the area (`QueuePersonEvent`, A3-13).
- **Diagnostics** (A3-15): `WWA1`'s dropped "model" / "chunk" counts are the ones since the last `WWA1`; the per-type
  counts of the host's world messages are kept only for the `world = 0` line; a stand-in copy's original model no longer
  counts against the 11 people-model slots (the stand-in replaced it for good).
- **The chunk mask** (A3-17) is sent and not used yet: world.h skips an entry whose chunk a member hasn't loaded, and
  coop.cpp names no entry's chunk. The bit is the index in the list `0x829A97E8`, the same chunk on both games only if
  that list keeps the image's order; the two games' `World chunks here` lines decide that first. Then the owner can
  set `Entry.chunk` from the object's chunk and hold offers a member can't load.
- **Another player's character kept past 130 m** (AB-5): out there its clothing isn't put on again (the game unloads
  it at that distance); once it is back within 60 m its slot is checked at once and dressed again when anything went
  (`R back within 60 m after d m (kept past 130 m: a member of the host's area): clothing lost, dressing again | all
  there (...)`, 20).
- **Trains** (AB-9): `spawn_trains` is a console variable in the image's data: its one writer is the console handler
  `8242C128` and its one reader the train manager `8242EFB0` (H), so no script sets it and the save can't keep the 0
  (M: no other reference to `0x827ACF87`, direct or as `+10407`). A 0 written while it is held stays unknown to the put-back.
- **Protocol 7** (A3-16, AB-8): a step build of 2.2 is refused at the join; the same build on every game.

**Final review fixes** (notes/coop-2.2/review_final.md; dispositions in impl_C_notes.md section 8):
- **People-model quota** (F1, F3): as described under "The quota table" above - only while the host's people are
  shown, only the idle levels' slots, never a level with requests, the game's rebuilds left alone for 10 s, the game's
  table and its own recompute `8241BD40(0,0,0)` back at the end. mod.ini `people_level0 = 0` turns the level-0 pins
  and the table writes off (2.1.4's top-level pins).
- **What `world = 0` undoes** (F2). With `world = 0` on a game: no world stream shown or sent (2.1's streams), no B
  passes or keep-alives, no C replays; and, since the final review, people pinned at the top level and the people
  quota never written (as `people_level0 = 0`). What stays with `world = 0` (fixes, or harmless): the registry, the
  vehicle-table compare and the train lever (steps 2-4, accepted in review A2-A4); the car copies' model reference
  through `82532360` (a leak fix: the destructor `825324D0` gives back exactly the one taken, and co-op releases no
  vehicle-group reference anywhere else, review_final "verified clean"; 2.1.4 kept every car copy's model count up for
  good); the host keeping cars another player's character sits in from its traffic and parked removals (A2-14, a fix);
  `QueuePersonEvent` (no effect: with `world = 0` the person is in 2.1's stream); the pulled-out drivers removed after
  a session (F5, below). So a `world = 0` fallback on both games is 2.1.4 plus those fixes.
- **Far car copies placed with flag 0** (F4): `WorldSteerCar` calls `82535C68(v, pos, matrix, 0)` for a copy without
  physics, as the MP car code (`823BB068`, `823BA888`) and `CreateVehicle` do. Flag 1 set `v+53984` bit 0x80, which the
  physics set-up `8253BF08` reads once to lift the car by its lowest wheel (scripts' "on the ground" teleports): the
  host's body position is lifted already, so the copy popped up and dropped when its physics came.
- **Pulled-out drivers after a session** (F5): `ReleaseOrphanDrivers` runs after the session too while any are left
  (they count in `leftovers`), and the session's end skips their 4 s wait (one still seated: 10 s more, as before).
- **A let-go crew member offered again on foot** (F6): `WorldMake` takes over its orphan (within 5 m, out of the car,
  same model) instead of making a second copy next to it, 2.1's pulled-out driver rule (`let go n (their crews m;
  offered again on foot and carried on k)`).
- **Nothing put on past 130 m** (F7): the clothing check's dressing (record or count changed, colours) waits while
  another player's character is out past 130 m; the back-within-60 m check runs first.
- **Two 10 s lines split** (F8): `World (member of slot 0's area ...)` (held, made, models) and `World (member)
  applied: ...`; `World (owner) B: viewpoints ... parked ...` and `World (owner) B: traffic ... kept ...`.
- The start-up line's history names Packages B and C protocol 7 (F9).

**2.2.1: the car-look freeze** (the first 2.2 test; notes/coop-2.2/impl_C_notes.md "2.2.1"). The PC client froze for
good after 6 minutes: "inside game function 8254DCC0, last game function called 82535C68" (the paint apply of a car
copy `WorldMake` had just made; `82535C68` is `CreateVehicle`'s placement). The minidump's game thread was not in the
blob's walk but in its last step `8254DEB8`, the rebuild that takes the car's physics body out of the world and back:
`825422D8` -> `8252FD88(*(v+224))` -> `826C86A8(*0x8370DC88, *(*(P+60)+16))` -> `826DC780`, which calls each of the
body's `+104` entries at `+100`; there `+100` was 0xC0 and `+104` 0x43CB13xx (a float): no live body, 1.1 billion
turns (H). The look went on in the update the car was made in; the game never does that - the garage (`821C12D8`)
waits for the car's physics (`8253A378`) and the MP code (`8238B258`) applies a later message's (H). Why the body
wasn't there yet is not proven (M: the car's set-up not done in its first update; 2.2's balanced car-model references
let models come and go, 8 loaded of 13-14 asked).
- **The fix**: `WorldMake` no longer applies the paint; `WorldApplyCar` does from a later update. `ApplyCarLook` (every
  caller: 2.1's `kMsgCarLook` copies and traffic too) puts a look off (`CarLookWait`) while the car was made by
  `CreateVehicle` under 250 ms ago, its look state isn't its own (`+4` its handle, `+8` its info + 208, as `8254C650`
  makes it), its model isn't loaded (stream entry `+0`), or, with physics (`+50300`), the body `826DC780` would walk
  isn't one (at most 256 entries, each 0 or an object with a vtable) or `*(*(v+244)+88)` isn't; tried every 250 ms,
  given up after 20 tries (a model: every 1 s, 2 minutes). It returns put on / refused / put off; a refused or given-up
  look isn't tried again.
- **The guard**: the look is walked first as `8254DCC0` walks it (`coopworld::PaintWalk` in `world.h`: magic
  0x0A55B00B, a variant block of at most 64 bytes, the parts and their colours by their own counts, ending exactly at
  its end, 320 bytes at most) and checked against the car's tables the way `8254DAF8` wrote it (`CarLookMismatch`:
  slot below the info's slot count, part below the slot's with `part+44` = the slot, colours no more than its colour
  slots, palette below `*0x83710108`, colour below the palette's count with `colour+0` = the palette); the 320-byte
  buffer is cleared before the copy. A refused look is logged ("Car look refused: car, model (n bytes, why; where)",
  20 a session) and so is a given-up one ("Car look given up: ..."). The owner never reads a malformed look into a
  `WWC1` or `kMsgCarLook` (`ReadCarLook` drops it). No wire change: protocol 7.
- **Log**: every 10 s while any happened, "Car looks: n put on, n refused (not this car's), n put off (just made, look
  state, model not loaded, physics body), n given up after 20 tries; n read here malformed (not sent)".
- `world_test`: a full 320-byte look round-trips in a `WWC1` and walks clean; 321 bytes aren't written; a cut tail, a
  cut look, a stray byte, another magic and a variant block over 64 are refused.

**2.2.2: physics bodies** (the 2.2.1 test; research tools/coop_test/TEST-2.2.1-findings.md). The PC client froze for
good after 111 s, mid police chase, in the same loop: `826DC780+0x132`, loop index 0x43CA0B61 (a float, ~404), `+100`
0. This time a person's update reached it: `82479638` (the active list) -> `82479130` -> `82441B00` -> `8227B6A8`
(the character controller `*(h+476)` changing its shape) -> `8227CE30`, which takes the controller's body
`*(C+16)` out of the Havok world (`826C86A8`) and back (H). Both freezes: an object's physics body pointer leads to
memory that isn't a body, with ~405.0f at `+104` (H); why is not known (M: a body freed and its memory used again).
After the walk `826C86A8` also calls the body's vtable `+44`, and the destructors' teardowns (`82447CB0` ->
`8227DB20` for a person, `825330C8` -> `82550600` for a car) walk it too, so no game call can take such a body out (H).
- **The check** (`BodyCheckTick`, from the update hook right after co-op's update, so before the game's next one;
  while connected): every person and car of the object table, as the game uses it - a person's controller readable,
  its `+8` readable, its body `+16`; a car with physics (`+50300 == 1`) its body `*(*(*(v+224)+60)+16)` and
  `*(*(v+244)+88)`. A body: readable, a vtable whose `+44` is code, `+104` at most 256, `+100` readable for that many,
  each entry 0 or an object whose vtable's `+8` is code (what `826DC780` calls).
- **A bad one** is logged ("Bad physics body: person/car, model, what co-op knows of it (world copy, 2.1's copy,
  this game's own, a player's), why (the body's vtable, +100, +104), dead / alive; what was done", 30 a session) and,
  unless a player is it or sits in it, taken out: a person off the active list (`+3592` next, `+3596` prev, head
  `0x8309ABB8`; self-linked, so the game's own unlink later is harmless), never destroyed (`HumanDestroyHook` refuses
  its removals, `DestroyObject` skips it); a car's `+50300` set to 0, the game's no-physics state. Either goes out of
  co-op's tables (`BodyForget`); a world copy is told to the host as gone here (it offers a new one).
- **Dead copies**: `HumanDestroyHook` lets the corpse clean-up `824793C0`'s ask through for a world copy (the 2.2.1
  test refused it 300 times in 10 s for a dead copy kept on the active list): it goes here, told to the host as gone
  (offered again only after its back-off); the host's own clean-up takes the original. 2.1's copies stay kept (its
  people stream would make the corpse again at once).
- **Reports**: a freeze report stops the game thread and writes its registers, guest memory at each register that
  points into it, the guest register blocks (`PPCContext`) on its stack with memory at their r3 / r30 / r31, and every
  person and car whose body is bad now; a crash report does the same for the crashing thread.
- **Log**: every 10 s while connected, "Bodies: people / cars checked per update (us), bad: people (taken out), cars
  (taken out), a player's (logged only); removals of taken-out people refused; dead copies let go to the corpse
  clean-up; taken out this session" and "Car copies: n here (physics on, off); n follow another game's damage, off for
  over 1.5 s: hit points (more than 8 of 255), smoke/fire, wreck: examples". No wire change: protocol 7.

**2.2.3: the body guard** (the 2.2.2 test; research tools/coop_test/TEST-2.2.2-findings.md). The PC froze a third
time, ~4 minutes in, again 3 s after a car copy was wrecked to match the host, in the same walk through the same person
update (`82479130` -> `82441B00` -> `8227B6A8` -> `8227CE30` -> `826C86A8` -> `826DC780`; index 0x43CB90DB, a float;
`+100` 0xC0). 2.2.2's scan had found no bad body all session, and its report found none on any person or car of the
object table at the freeze (H): the body is reached through something the table doesn't list (M: a person removed but
still on the active list, or a body freed and used again within the game's update).
- **The guard**: `8227CE30(C)` (a controller's shape change, its body `*(C+16)`), `826C86A8(world, body)` (a body's
  removal) and `826DC780(body)` (the walk) are hooked; a body that fails `BodyProblem` is skipped there (no freeze; the
  game may still trip over it later) and logged ("Body guard: where skipped (from LR): why (the body's vtable, +100,
  +104), controller; the person with that controller or body: in the object table or not, on the active list or not,
  +120, +200, hit points, what co-op knows of it", 30 a session). A body checked by the caller isn't checked again
  down the chain (`body_checked`).
- **The scan** runs once a second (every update cost a Mac under Wine about 20 ms a second: its page checks) and reads
  only the fields it uses; it also walks the active person list and logs once each person on it that the object table
  doesn't hold ("Active list: ..."). The car copies' damage comparison runs every 250 ms (as `ApplyCarDamage`).
- **Reports**: the live guest register block is found through the host registers (in the 2.2.2 freeze `rdi`: host
  memory the minidump leaves out), with its guest stack's frames (the back chain, each frame's LR and the r20-r31 its
  `__savegprlr` saved); `BodyReport` adds the active list's people that the object table doesn't hold or whose body is
  bad.
- **Log**: "Bodies: people / cars checked a second (us), bad ..., active list n people, n times one not in the object
  table; skipped by the body guard: shape changes, removals, walks; ...". No wire change: protocol 7.

**2.2.4: the seat request** (the 2.2.3 test; research tools/coop_test/TEST-2.2.3-findings.md). The PC hosted and froze
getting in where the joiner drove, 150 ms after the joiner's character was seated again: `SeatRequestHook` asked for the
passenger seat, the player wasn't in the car right after the call ("Passenger seat refused"), and it made the request
again "the usual way" in the same call. That second request (`8257B2C0` -> `82556E50` -> `82440200`) ran the get-in
action on `0x8309AD84`, no person: the animation layer update `82433308` took its `+572` as a layer count and wrote -1
into every 28th byte's word over memory for 8 s (the report's memory showed it everywhere, code included) (H for the
path and the writes; M for where the bad pointer came from).
- **The fix**: the passenger request is made once; if the player isn't in the car right after it, the passenger watch
  seats it from co-op's update (at once when the game started nothing, after 3.5 s while a get-in is under way: `+2556`
  set), and the request the other games are told of is the passenger one.
- **The guard**: `82433308(person)` skips a layer count over 8 (only the person's set-up `8243DAE8` writes it: 3) and
  logs it ("Person guard: 82433308 skipped (from LR): X isn't a person (n animation layers ...)").

**2.2.5: freed people and wrecks that change hands** (the 2.2.4 test; research tools/coop_test/TEST-2.2.4-findings.md).
The Mac joined and froze: the body guard skipped a body's removal from `8227CE30`, which then hung putting the same body
back (`826C8580`). The report's active list held `person 00000000 at C3B09268` (not in the object table; handle, `+120`,
`+200` and hit points 0): freed memory the active list walk still updated. Its controller `+476` is 0, and `8227CE30`
with a controller of 0 reads its "body" at guest `0x10` - junk (H). The same explains 2.2-2.2.2 (the floats at `+104`)
and why no scan saw a bad body: a controller of 0 was taken as "none". What frees a person still on the list isn't
known (M: the list's link was overwritten - the walk ended at it, 9 people of 28).
- `82479130` (one person's update) is hooked: in a session, a person the object table doesn't hold is skipped and its
  next link pointed at the head (the walk ends there this update); each update `BodyCheckTick` walks the list and, if
  it holds such a node, rebuilds it from the object table (the list's order up to the bad node, then every person the
  table counts active: `+200 & 3 == 3`); `8227CE30` refuses a controller of 0; `826C8580` (a body's add) is guarded as
  the removal; every guard's calls and skips are counted in the Bodies line; a hook that fails to install is logged.
- **Wrecks**: the host's car blew up there with the joiner inside; its authority went to the joiner (the only player
  in it), whose game decided the car from then on and drove it on intact. Now a wrecked car's authority goes back to its
  owner at once (its records carry the wreck), and `ApplyCarDamage` puts the owner's wreck on a copy this player drives
  (`WorldApplyCar` asks for it while the copy is driven).

**2.2.6: player-class characters off the people's list** (the 2.2.5 test; research tools/coop_test/TEST-2.2.5-findings.md).
The Mac joined and froze again; the 2.2.5 guards fired on `person 00000000 at 83B8D9A8`: the player pool's free slot 10
(`0x83B8D1A8`, the host's character co-op had given back to the pool 65 s before) with bit 0x800 set. 2.2.4's node
`0xC3B09268` was `0x83B09268` with bit 0x40000000 set. Both are valid addresses with one bit flipped in a list link. For
the player class `+3592`-`+3595` are flag bytes, not that link (H: `party_allow_max_followers` 824D81F0 sets 0x80 at
player+3592; 8247DBB0 clears 0x08 there; 8247E248, 82480660, 82481EE8, 82482AA0, the damage / knockdown / punch / seat
code write `+3593`-`+3595`). co-op's characters of the other players are player-class (the pool `0x83B57148`) and reach
the people's active list (`8247A6D0` from the creation 82477EEC and the model pass 8247AA5C): their flag writes flipped
the link of the node before them and the walk went to the flipped address (M for the whole chain; H for each part).
2.2.5's rebuild also put every person with `+200 & 3 == 3` back on the list, player-class ones included.
- `HumanActivateHook` refuses `8247A6D0` for a player-class character in a session ("Active list: player-class character
  ... not put on the people's list (8247A6D0 from LR)"); they are updated as players through the player list
  `0x8309AC54`.
- `BodyCheckTick`'s per-update walk counts a player-class node as damage, and `RepairActiveList` never takes one (the
  list's own order up to the first bad node, then the table's active people, player-class left out).
- `ReleasePlayerObject` takes the character off the people's list before `8247DBB0` (which only unlinks the player
  lists).
- `PersonUpdateHook` writes the head into a stale node's next only outside the player pool (that memory is a live
  player-class character's), and marks the list for the rebuild after the update.

**2.2.7: repairing the people's list from its own nodes** (the 2.2.6 test; research tools/coop_test/TEST-2.2.6-findings.md).
The Mac lasted 11 minutes (2.2.5: 110 s), then hung in the game's people-list walker `82448710` (no node cap) on a list
gone round in a loop (the report's walk hit its cap of 256 with ~35 people). 2.2.6's `RepairActiveList` had run 90 times
in the last 2.3 minutes, each time for a player-class node on the list (never a freed one; `8247A6D0` never refused one:
they come on another way), and it rebuilt the list from the object table's active people - which include people on the
game's other lists sharing `+3592` / `+3596` (the pending list 0x8290CA80, the inactive one): cross-linked lists, a loop
(M). For the player class `+3592` is flag bytes (8247BA28 sets it 0, then `+3593` bit 0x80) (H).
- `RepairActiveList` keeps only the list's own valid nodes (in the object table, not player-class): forward from the
  head to the first bad one, then backward from the head along the prev links to the first bad one (one broken link
  loses no one), relinked in a circle; a bad head starts it from its neighbour. A player-class node taken off gets
  `+3592` back as 8247BA28 sets it. Each repair is logged with the nodes taken off (what each is: this player, another
  player's character, a free pool slot, not in the table; its links and `+200`).
- The per-update walk also counts a loop (a node twice before the head again) as damage, so it is repaired before the
  game's walkers meet it.
- `PersonUpdateHook` no longer writes into a stale node (it may be another list's node or a live character's memory).
- The game's other `+3592` linkers (82477B78, 82478E68, 8247A8E0, 8222A0A0, 8222A278, 8222A8D8, 8222AA98, 82211F18,
  824E1AD0) are hooked to log a player-class character they are handed, with the caller ("Active list: X called with
  player-class character Y (as rN, from LR)"): what puts them on the list.

**2.2.8: the cause, deaths, wrecks and seats** (the 2.2.7 test; research tools/coop_test/TEST-2.2.7-findings.md and
notes/coop-2.2/investigation_*.md). Protocol 8.
- **The people's-list damage (H)**: `WorldCopyFire`'s held automatic trigger wrote the trigger-held bit `+3592 | 0x40`
  (what 82480528 / 823B24D0 set after a shot on a player-class character) on copies of the host's people, where
  `+3592` is the top byte of the people's active list link: the copy's next `0x83Bxxxxx` read `0xC3Bxxxxx` while the
  trigger was held (2.2.4's `C3B09268`, 2.2.7's ten repairs of `C3B51E28` / `C3B0E588` / `C3B2B708`), and the walk
  82479638 went there; the people behind it weren't updated (frozen bodies) and the games froze (2.2 - 2.2.6). The bit
  is no longer written on copies; `TryRemoteShots` clears it only on a player-class character. 2.2.5 - 2.2.7's guards
  and repairs stay.
- **Deaths**: `KillCopy` runs under `applying_ped_call`: the death 8244A708's fall `824497F0(h, 0, 0)` (its ragdoll) was
  refused by `KnockdownHook` on every copy not played for the host, so a copy killed by its record died standing unless
  the host's knockdown event came first; a refused death is called directly (`+1912 = 0`, `8244A708`). A dead copy not
  lying (`+780` 0, not in a car) gets the fall again once a second, 10 times. `ApplyPedAction` plays nothing on a dead
  copy.
- **Wrecks told**: `kEventCarWrecked` (30; a[0] the car's `CarId`, a[1] its owner): `CarDestroyedHook` sends it for a
  car a player sat in that the game deciding it wrecked (the driver is thrown out at once, so its pose no longer carried
  the damage); receivers wreck it through `ApplyCarDamage(..., told)` (the MP replay on a copy, the full destroy on their
  own car), tried each update for 10 s.
- **Wrecks held**: a copy's wreck (the MP replay 82542650) also started a fade that drew it invisible 10 s later while
  the owner (the single-player destroy 8252BF38 and its clean-up 8253F698: 35 s - 2 min) still showed it burning.
  `HoldCopyWreck` keeps a world-stream / 2.1 traffic copy's wreck in the replay's first state, renewed every 250 ms
  (`+53976 | 0x00400000`, `+54436` clock + 8 s, `+54444` 2 s, clean-up timers -1); the owner's removal ends it. Other
  players' own cars (`car_copies`) are left as they were: no removal message reaches those yet.
- **Seats**: another player's get-in (their `kEventEnterCar`, 8 s, the seat asked) claims its seat (`SeatClaimed`,
  `RemoteEnteringWheel`): this player's request for that car goes to a free passenger seat, and none at all when every
  seat is taken or being taken. The passenger watch drops a stuck get-in (`8257C400(player, 19)`, the action ended)
  before its direct seat. Both pressing within the link's delay is still decided by the existing rule.
- **Inventory items** (research notes/coop-2.2/investigation_item_use.md): the HUD's use 8229DD58 -> `8245CB28(person,
  item)` sets `+2380` and plays the use's action (299-316, 323) through 82444F88; co-op sent only that action, so the
  other games played a bare animation without the use (`+2380` 0 there: no stand/move switch, smoke, bottle or end)
  that the walk cut off. `ItemUseHook` sends `kEventItemUse` (31: the drugs table entry `0x83082D60` + 68 n, the
  pour-out byte) and the action no longer goes on its own; the receiver ends a running use (`8245DDF8`), makes the item
  (`8245C6E0`) and uses it on the player's character (`8245CB28`), freeing it when refused (`8245C7B0`, `823F6240`); a
  stand variant (flags `0x4080`) holds the character's moves 3.5 s. The effect `8245C488` is skipped on another
  player's character (health and damage reduction stay the owner's). `ReleasePlayerObject` ends a use and frees the
  carried items before the character goes back to the pool.
- **Log**: "Deaths and wrecks: copies killed ... (falls tried again ...); wrecks of cars a player sat in told ...,
  told here ..., put on here ...; copies' wrecks held for their owner ..." and "Items: n of this player's uses sent;
  other players' uses played here n, refused n" every 10 s; "<player> uses an item (entry n): played / refused here".

**2.2.9: out of a wreck** (the 2.2.8 test; research tools/coop_test/TEST-2.2.8-findings.md). 2.2.8 held: no list damage,
deaths matched, the wreck told, the passenger seat, an item played. The host's car blew up with the joiner's character
inside; the joiner's copy got the MP wreck replay 82542650, which leaves its people seated (the single-player destroy
8252BF38 throws them out): the joiner sat 85 s in the wreck on both screens. `ApplyCarDamage` now orders this game's
player out of a copy it wrecked for its owner (`OrderExitVehicle`; the exit goes to the other games as usual). The
"copies' wrecks held" count counts each wreck once (it counted only copies gone invisible).
- **A mod-menu car's colours** (the user: a car spawned from the host's mod menu showed one colour there, another on
  the joiner's screen): a car a script or a mod menu makes (82569700 source 7 / 3, `kKindScriptCar`) gets its paint
  after it is made, and the world stream reads a car's paint once, at its create. The owner now shares those cars'
  looks as a player's car's (`WorldShareLooks`: `ShareCarLook` once a second while members are in the area, each car
  read every 2 s, `kMsgCarLook`; kept by the 2.1 prune through `WorldSharesLook`), and `WorldApplyCar` repaints a copy
  whose owner's newest look differs from its own (checked once a second). A joiner's own spawned car still shows on
  the host only while the joiner drives it (2.1's player cars; members own no world objects in 2.2).
No wire change.

**2.2.10: poses and corpses** (the 2.2.9 test, 18 minutes with no freeze on either machine; research
notes/coop-2.2/investigation_tweak_poses.md and investigation_corpse_positions.md).
- **Poses** (the user: "one player would go into a pose or action using the tweak menu and the other player would
  not be able to see it"): a pose is an animation state (`+952`: set_animation_state 824DA2A0; the SR Tweak Menu
  writes it and applies it with `82440200(h, -1, -1, 0)`), a custom animation a file played by `826F5C20` on the
  character's animation player `*(*(+568)+3592)` (`+756` 297). Neither reached the other games (the 2.2.9 test: the
  host's blunt pose, the Mac's "Player 1 floats ... 77114 ms"); an action (82444F88) was held there 600 ms and walked
  over. `WatchLocalAnim` sends `kEventAnim` (32: `+952`, the custom animation's name hash from `AnimFileHook`
  (826F5C20: slot -> `0x828E5290 + id x 28 + 4`), its flags and f1-f4, the action still playing) on a change held
  250 ms and every 5 s; `UpdateAvatar` puts the state on the player's character (when its animation set has it,
  `82226DA8`), plays the custom animation as play_custom_action_do 824D8FB8 does (`AnimSlotOf`, `826F5B88` /
  `826F5BC0` / `826F5C20`, `+760` / `+576` / `+756` / `+592`) and stops it (`StopMoves`) when the sender's goes; an
  action or custom animation of theirs holds the character's moves while their path stands (`Follower::Still`); the
  action replays pass the game's r10 (`*(0x820144B4)`). Old games ignore kind 32.
- **Corpses** (the user: "some dead bodies were not always in the same location"): the record of a dead person
  carries the host's ragdoll root (82443808 copies body 1 to `+20` each frame), and the member never read it after
  the death; its fall started from the copy's own spot, and 2.2.8's retry dropped corpses again after their ragdoll
  slot went to a newer fall (82267200 takes a dead person's first: 341 falls again for 173 kills). Now no fall again
  once a copy lay (`sp.lay`); `PlaceCorpse` puts a standing copy at the host's person before it falls; `FollowCorpse`
  follows the host's corpse as the game's MP mirror follows a ragdolling NPC (823A8240): over 4 m the ragdoll moved
  there (`822678F0`) and stopped (`82267848`), 0.3-4 m while the host's moves an impulse (`82267AF0`, offset x 3),
  once both are still (1.5 s, `8226BDC0`) an offset over 0.5 m moved there. 2.1's copies keep the old behaviour.
- **Car parts** (the user: "a few vehicles whose damage would show a door off on one vehicle but not the other";
  research notes/coop-2.2/investigation_car_parts.md): parts break only in the game that runs 825397E0 for the car
  (825447D0 per part, 82550378 the dent), DetonateHook skips it for every car another game decides, and only hit
  points, smoke, fire and the wreck were sent. Now the deciding game reads what broke on a car 825397E0 ran on (4 times
  a second at most: parts from `+252`, 688 bytes each, count `+248`, flags `+108`: 0x20000 off, 0x10000 on its hinge
  (sent as hanging only below half its hit points: an opened door is on its hinge too), 0x80000 glass, 0x100000 flat;
  the dents' ring from `*(+54456)` with the MP host's 0.01 share) and sends a change as `kMsgCarParts` (WCP1: the car
  as CarId names it, the part bytes with their hit points out of 15, the new dents; reliable, everyone). The others put
  it on the cars they show with the calls of the game's damage restore 821BF290 (82547C40 off, 82547B30 hanging,
  82547050 glass - its shards only within 50 m -, the flat bit, 82550718 a dent not there yet), checked again every
  2 s (a copy made again). Breaks only add up (the union of what was sent, as SR1's MP: set-only), so a late message
  from the game that decided before undoes nothing, for any number of players; a repair (82536F30 on a broken car)
  is a new generation and repairs the copies. A player who joins gets every broken car in full. Older games ignore
  the message.
- **Log**: "Poses: n of this player's sent, n of other players' taken; put on their characters here: states n,
  custom animations n, refused n"; "Deaths and wrecks: ... corpses placed n, moved n, pushed n toward the owner's";
  "Car parts: n changes of cars this game decides sent (bytes, repairs), n taken; put on here: off, hanging, glass,
  flat tyres, part hit points, dents, repairs; cars whose part count differs here".
Protocol 8 (kEventAnim and kMsgCarParts are new kinds older games ignore).

**2.2.11: doors, the seat race, guns, poses and corpses again** (the 2.2.10 test, 20 minutes with no freeze on either
machine, both games with the renderer fix; research notes/coop-2.2/investigation_world_doors.md,
investigation_seat_race.md, investigation_npc_gunfire_sync.md).
- **World doors** (the user: "I opened or kicked a door open holding Y on one screen and it did not open on the
  other"): the kick (action 538) reached the other games, but the person update 82441B00 opens the door the action
  button found (+2836 / +2840, from 82480768) only in the game that kicked; SR1's MP never synced doors.
  `WorldDoorHook` (823F57E0 from 0x82442520, this player's person in r31) sends `kEventWorldDoor` (33: the door's
  name hash, its position, the direction); the others find the same door in their loaded chunks (0x829A9900 by slot,
  doors from +420, count +416, 60 bytes; the name from the mover *(+0)) within 1 m and open it with 823F57E0. A door
  locked there stays shut; each game's door manager closes its own.
- **Poses**: the SR Tweak Menu ends a pose by stopping and resetting the animation player, then putting the old
  state back; only the state was put back here, and the pose's loop kept playing. Now a state that ends a pose put on
  here goes with `StopMoves`.
- **Corpses**: once the host's corpse lies still, the copy is moved there when over 0.3 m (3 times at most), and at
  once over 2 m (was 4 m and the rest test 8226BDC0, which held most back: 180 moved for 350 deaths).
- **The seat race** (the user: both jumped into a car at once; one drove, the other was knocked down until he
  died): both games agreed (the car's owner keeps the wheel), but the Mac's move-over to the passenger seat left the
  driver's seat (+52316) naming him: the host's character was refused the wheel 82 times there, and once out the
  car's impact handler 82278E18 (82279174) knocked him down 13 times. `WatchStaleSeat` clears a seat of the car this
  player sits in or last sat in that names them while they don't sit there (4 times a second); `RefuseImpactFall`
  refuses the third car-impact fall of this player within 12 s and any for 6 s after.
- **The host's people's gunfire** (the user: their animations "not exactly in sync"): their fire actions (126-157)
  went as reliable events (5,916, up to 21 a second) and each held the copy 8 s, so it never turned with the host's
  person through a fight, while the round's replay played its own fire action too. Now those aren't sent while
  Package C sends the rounds, a weapon action holds a copy 1.2 s, and a copy's own AI can't change its gun
  (`EquipHook` on 82446280: only co-op's MatchWeapon, and holstering). The stance (+576) isn't sent yet (a PedRec
  change: next).
- **Shots refused "gun raising"** (73 in 40 s on the Mac after the host left a car, until its character was made
  again): after 3 deferrals in a row its state is applied again (82440200), once a second, logged with +772 / +576.
- **Log**: "World doors: ..."; "Seats: car seats still naming this player cleared n; car-impact falls of this player
  refused n"; "Guns: copies' own gun changes refused n; other players' characters stuck raising the gun put right n".
Protocol 8 (kEventWorldDoor is a new kind older games ignore).

**2.2.12: the host's pause, the pose's end** (the 2.2.11 test, ~6 minutes, no freeze; the user: bodies and doors
fixed).
- **The host's pause menu** (the user: "when the host pressed start, all npcs, cars and the world stopped moving ...
  could we make sure they keep going"): since 1.61 the host's menu paused its game (8220C778 from 822BFD80) and every
  client with it. Now in a session the host skips that one pause, as clients always did (its prompts and mission
  screens that pause the world still do); its menu gets the multiplayer answer (823494D0 -> 2) whenever it runs over
  a running world, so it still closes; clients pause only when the host's world really is paused under its menu.
- **The pose's end** (the user: "still did not stop ... same when I tried it the other way"): the SR Tweak Menu puts
  back the state from before the pose, and a player's own is none (-1); the receiver only took states 0-226, so the
  end was skipped and the pose stayed (both logs: one state put on, the start). Now a sender back to no state ends the
  pose put on here (the state, then `StopMoves`). Each other player's pose change is logged ("<player> pose: state n,
  custom animation h, action n").

**2.2.13: people spawned on the host** (the 2.2.12 test, ~4 minutes, no freeze; the user: doors, the simultaneous
get-in, poses and their stop all worked - the PC's log: the Mac's pose 178 put on, then its -1 end).
- The user: "I spawned people into the world on the host's game, I just want to make sure they sync with everyone if
  they are added in or removed". The world stream's registry names a person's kind from its creation kind (+4216):
  0 / 3 / 7 are script people, `kKindMissionPerson`, which go only while a mission runs (a mission's own characters
  stay standing after it); the SR Tweak Menu makes its spawns with 82479AB0 kind 3 and its scenes with kind 0, so
  outside a mission they were held back ("mission/script outside a mission" up to 6 in the PC's log) and never
  shown on the Mac. Now `HumanCreateHook` notes a script-kind person made on the host while no mission runs
  (`spawned_people`): it streams as the street's people do (created on, and removed from, every member with the
  registry, the game's own removal reaching them as for anyone), and goes without `kPedMission`, so it is a
  targetable copy there. A mission's characters keep the old rule. A member's own spawns stay on its own game (members
  own no world objects in 2.2).

**2.2.14: states of their own** (the 2.2.13 test, ~6 minutes, no freeze; the Mac's `World match` within 50 m had the
same people count as the host in every window, spawned people included).
- The user: "some would be walking and on another screen they are holding their arm or reading a book ... it also
  affected the other player animation too". A person's animation state (+952: street idles and walks) goes in PedRec
  (`anim`, 0xFF for none), and a copy got the host's only when there was one: a host person in none (-1) left the copy
  in whatever its own game gave it when it was made. Now a copy in a state of its own while the host's is in none is
  taken out of it (-1, then `StopMoves`: animation player stopped and reset, the state applied), once the host's has
  held 1.5 s and no action holds it. Counted in "People's states: copies out of a state of their own".
- The same for another player's character: since 2.2.12 only a state co-op put on was ended; now any state on their
  character here while they are in none, on foot and seated (the user: "when the player got into my car, it showed
  them standing up"; logged "<player> seated here in state n (none where they are): out of it").

**2.2.15: walk styles** (the 2.2.14 test, ~2 minutes: no copy needed 2.2.14's state clear - the difference wasn't
+952; research notes/coop-2.2/investigation_walk_styles.md).
- The user: "older peds walking with canes, a walker or even with different walking styles ... not always matching
  on both screens". A person's walk style is its base states: +1064 stand, +1068 walk, +1080 run, +1076 / +1072
  cower stand / run, +1084 ready (+1088 = +1084 + 64), indices into the state table 0x827D1FA8 (the same on every
  game). 82432DA0(&+1064, &+1068, &+1080, human) draws them at random (the global pool and CRT rand) when the person
  is set up (8244B268, before +216 0x4000) and again after it drops its props (8243CC20, +212 0x1000; from 8243CE88 on
  a state change and 8244DBC0 from ~250 AI callers and the knockdown). The cane (POL1 walk variant 1 `olcane`), the
  walker (variant 2), a magazine, a forty or a blunt is the prop of that state's animation (824367C0 shows it each
  frame), not an item. A member's copy drew its own; SR1's MP never sent the style, and its clients never drop or draw
  (the gate 8243CBB0 returns 0 on an MP client).
- Now the host sends `kEventPedStyle` (34: the six states as bytes, the dropped bit) to a member right after a
  person's WWC1 (a set-up person), and to its members after a new draw or a drop of a person it streams
  (`StyleDrawHook` on 82432DA0, `PropDropHook` on 8243CC20). The member keeps the newest per host handle
  (`world_styles`) and puts it on the copy in WorldApplyPerson, after its look (`PutPedStyle`: each state its anim
  set has, the dropped bit without throwing items, then 82440200 when no override state plays); `DropGateHook` on
  8243CBB0 returns 0 for the copies, as on an MP client. Old games ignore kind 34. Log: "Walk styles: n of the host's
  people's sent; put on copies here n; copies' own drops and draws refused n".

**2.2.16: smoother people** (the 2.2.15 test, ~5 minutes, no freeze; walk styles put on 300 copies; research
notes/coop-2.2/investigation_people_motion.md, investigation_people_visibility.md, review_coop_2.2.15.md).
- The user: "slight moments of delay or lag when I would see a ped jumping or lagging to keep up or reposition".
  The copies are placed on their path every update (no snap threshold); the lag is in their animations and events:
  - **Events ahead of positions**: the host's people's actions, calls and reactions (kEventPedAction / PedCall /
    PedReact) played on arrival, ~130 ms before the position they belong to. Now each waits for its copy's follow
    delay (`HoldPedEvent` / `PlayHeldPedEvents`, in order).
  - **Action holds**: a non-weapon action held the copy 8 s with no facing and no position, and it jumped when the
    host's person moved off. Now 2.5 s, extended while the action still plays here (+756), with the host's facing
    written each update and a 20 % nudge toward its spot (not while it lies or gets up).
  - **Walking on the spot**: the standing early return skipped Drive's stop order, so the last walk order kept the
    legs going under a pinned body. Now it returns only once the copy isn't told to move.
  - **Velocity**: the gait, heading and stop used the newest records' velocity, ~180 ms ahead of the body; now the
    path's velocity at the time shown (`Follower::PathAt`, t +/- 50 ms) inside the buffer.
  - The "floats" lines of other players' characters counted standing too (every one was a walk's start): reset
    while the path stands.
- The user: "peds out of sync where I would see one on the joiner screen but not on the host ... clothes could look
  different from far away". No orphan copy in the logs (digests extra 0, removes all applied, the host's audit
  clean); the people counts that differed were copies still loading their model. The likely case: a person the
  host keeps alive near the member (PeopleDespawnHook) far from the host's camera, small or not drawn there. Clothes
  at a distance are the game's own level of detail (15 m), the same on both; but a create read before the person's
  look existed left the copy in its own random clothes for good: now a person's create waits up to 1.5 s for its
  look ("creates waiting for their look").
- Log: "People motion: host people's events played on the copy's timeline n; held copies nudged toward the host's
  spot n, released more than 0.3 m off n; creates waiting for their look n".

**2.2.17: stand-ins** (the 2.2.16 test; the user: "the cop looked like someone else instead of a cop").
- The Mac's log: "Host's police shown here: world stand-in ..., host's LW_X_M_Officer_01b team 5, here PD_W_M_Bum
  team 6". A driver whose model hasn't loaded in 2 s while its car moves gets a lookalike from the models loaded here
  (`LoadedLookalike`), scored by sex, build and name prefix, plus a bonus for a model the host also has; nothing kept
  a uniform's family, so a bum drove the police car. Every other copy is made with the host's own model.
- Now a uniform or a gang only by its own family (LW_: police, FBI, EMT, meter maids; TS_: gangs), its role first
  (the name's fourth part: Officer / FBI / EMT); none loaded: no stand-in (the real one shows when its model loads).
- A stand-in was kept for the copy's whole life, and its real model was left out of the wanted models: now still
  wanted, and once its car is slow (under 1 m/s) the stand-in goes and the copy is made again with the real model
  (queued as a create). Counted in the People motion line ("driver stand-ins made again as the real person").

**2.2.18: stances, protocol 9** (the 2.2.17 test; no police showed as a civilian on the Mac).
- The user: "npcs/police were crouched on one screen but not the other while they were shooting at me" and the
  enemies' movements and firing lagging. A person's stance is the state on layer 0 (+576: stand / crouch / walk
  ready, strafes, the fire stances), picked by 8244E018(move mode +508, sub +512, human) from 82440200 with the
  person's own AI target and facing; a member's copy picked its own (research notes/coop-2.2/
  investigation_npc_gunfire_sync.md 5.3). Now PedRec's spare byte (`state`, +516 / +520, never read) carries
  `stance` (+576, 0xFF none): `StancePickHook` (post-hook on 8244E018) returns the host's for a copy on foot whose
  record is under 500 ms old and whose anim set has it (checked once per stance with 82226DA8), and WorldApplyPerson
  re-picks (82440200(obj, -1, -1, 1)) a copy whose stance differs for 150 ms, at most every 200 ms. Log: "Stances:
  the host's people's stance put on copies here n; not in a copy's anim set n".
- Copies' move orders calmer (the motion note's P6): a re-aim needs a 0.5 rad turn (was 0.25) and 200 ms since the
  last order; players' characters keep theirs.
- **Protocol 9** (net.h kVersion): the byte's meaning changed, so an older build is refused at join (2.2.8-2.2.17 were
  all 8 and joined each other quietly missing features).

**2.2.19: weather, waypoints, dropped items** (the user: "start with the quick wins").
- **Weather**: the stages are *0x8370D1BC (212-byte records, weather.xtbl's order, count *0x8370D1B8), the current one
  at *(*0x8370D1D8). Each game drew its own. The host sends its stage (`kEventWeather` 35, on a change and every
  10 s); members lock theirs to it as the weather cheat does (821F57D0: 0x8370D204 = the stage, 0x8370D200 = 1) and
  let go when the session ends.
- **Waypoints**: a map waypoint is the GPS route of type 0 (0x8370E0EC, route *0x82FFE744, target 0x839E125C), set by
  822A95D8(from, to, type, flag) and cleared by 822A9830(type). Each player's own goes to every game (`kEventWaypoint`
  36), which sets it the same way, never over that game's own waypoint or a mission's route (the user: players with
  Teleport to Waypoint could otherwise be moved by another player's clicks), at most one per player per 3 s; a clear
  clears only that player's. mod.ini `waypoints = 0` turns it off.
- **Dropped items** (research notes/coop-2.2/investigation_dropped_items.md): a dead person's gun (824739D8), cash
  (8243CFC8 -> 82408B78 coins / 82408C58 big_money) and food (8245A7D8) are loose items (82408190, type 2; given only
  through 824090E8(human, item)). Each game made its own (a member's copies died with its own random drops), the host's
  lay on the host only, and the other players' characters picked items up unseen on each game (the pickup walks every
  player). Now the host owns its loose items (`ItemOwnerTick`: told 1 s after they land as `kEventItemDrop` 37, again
  every 5 s, kept from the game's 50 m despawn with +208 0x20, removed when no player is within 50 m); a member makes a
  copy (82408190 with the host's ammo and cash) that its player takes by asking the host (`kEventItemTake` 38): the
  first ask wins, the host removes it and tells everyone who took it (`kEventItemGone` 39), and the taker's game gives
  it through 824090E8. Members make no drops for their copies (hooks on 82408B78, 82408C58, 824739D8, 8245A7D8,
  8247A0B8); other players' characters never pick items up (`ItemTouchHook`). A member's own player's drops (a weapon
  swap) stay on its game.
- The `Stances` count is only real changes (the picker runs most frames). Log: "Weather and waypoints: ...", "Dropped
  items: told n, copies made here n; takes asked n, granted n; gone n; copies' own drops not made n; other players'
  characters' pickups refused n".

**2.2.20: level props** (the user's "flying debris and knocked-over objects"; research notes/coop-2.2/
investigation_world_movers.md).
- Every breakable or movable level prop is a mover (type 3) of its level chunk, named the same on every game by its
  uid (MoverId: *(*(m+216)+40), sub +224, found by 824995B0). It comes loose only in 82499988(mover, 1): from a contact
  (LR 0x82259C10, the other object's handle at the caller's r24+4), its damage 824982E8 (LR 0x82498350 / 0x82498394)
  or a person's push (0x82274088). Each game decided its own, so posts stood on one screen and lay on the other.
- Now the game that owns the cause decides: a car, the game whose player drives it; a person, that player's game;
  anything else, the host; its own explosion, the game it went off on (a replayed explosion only previews it).
  `MoverLooseHook` refuses the others' knock-overs and `MoverDamageHook` their damage; the decider sends
  `kEventMover` (40: uid, step, position, axes, velocity) loose at once, then 5 times a second while it moves (over
  0.5 m/s), and at rest (500 ms slow, or 8 s); the others make it loose the game's way (unanchored, 82499988) and put
  its body there (82253D70) with its velocity (82253F10). A claim per uid lasts 10 s (the lower slot wins within
  300 ms). Not yet: props destroyed outright (glass, a death without an explosion), damage short of a break, a
  catch-up for a late join. Log: "Level props: this game's knock-overs sent n (events), others' refused here n; other
  games' put on here n (moved into place n), not loaded here n".

**2.2.21: the Mac's freeze in the 2.2.20 test** (research tools/coop_test/TEST-2.2.20-findings.md).
- The Mac (joiner) froze 44 s in: its game thread hung inside the people despawn and spawn pass 8240FBB0 (under co-op's
  PeopleUpdateHook, which only passes through on a member), on the first update after it left the host's area and its
  own spawners came back on with 29 copies fading; HumanDestroyHook had refused 153 of its removals (copies are kept
  up to 60 s). The host had dropped the Mac from its area because the Mac's poses stopped arriving (3 s old), and the
  Mac's log showed a NaN in the host character's follow gap from the window where it put 23 knocked-over props'
  bodies in place with 82253D70(slot, pos, rows) and 82253F10 (2.2.20). 82253D70 turns the rows into a quaternion
  (8225FE38) from a layout other than the 36 bytes written (M): a NaN body, carried into characters by physics (M), a
  NaN pose dropped by the host, and the despawn's distances NaN (M).
- Now: props another game knocked over come loose here (82499988) but their bodies are not moved (their own physics
  from there; placing them waits for the transform's layout); a copy removal asked 4 times within 30 ms is let
  through (a game loop waiting for it; "Copy removal asked 4 times within 30 ms ... let through"); a NaN gap in a
  follower is skipped and logged ("a NaN position ...: not moved this update"); a prop event with a NaN position is
  ignored.

**2.2.22: the 2.2.21 freeze** (research tools/coop_test/TEST-2.2.21-findings.md).
- The Mac froze again at the same instruction (8240FBB0+10CD). Its log: the host's character and every person copy
  near it with a NaN position in the same millisecond, their paths from the host fine, 270 ms after a copy of the
  host's grenade (weapon 39); no prop was moved this time; 3 dropped-item copies were in its world (also in 2.2.20),
  and the user saw "a CD collectable registered" on the Mac. The NaN is in the member's own physics after an
  explosion (H); the first suspect is the item copies, made with rows built from int8 axes (neither unit nor at
  right angles) for a physics body an explosion then pushes (M).
- Now: item copies get orthonormal rows (identity if the axes are unusable); only fight drops are shared, by the
  item's touch handler (a gun 82405F98, money 824067E8 / 82406670, health 82406948 / 82406A40, drugs 82406330), never a
  collectible (824064A8: a CD is one of each player's 60) or a carried object (82405F00); a follower whose character
  has a NaN position puts it back on its path (MoveHuman, velocity 0); an item copy whose position goes NaN is removed;
  the host tells its knock-overs only when another player is within 150 m (165 events in 10 s before). Log: "NaN guard:
  characters put back on their path n; item copies removed n".

**2.2.23: the CD prompt, props off** (the 2.2.22 test, research tools/coop_test/TEST-2.2.22-findings.md).
- The Mac froze a third time: every character's position NaN at once with no item copy and no explosion, then a hang
  re-making the host's character. The user: a CD was collected on the Mac each time, "they had to accept the CD
  progress, then they were teleported ... or went to a dark screen", still seeing items on the ground. The Mac's log:
  the NaN at the pickup, then "Pause skipped ... from 822231F8": the game's prompt sequence (82223610, opened by the
  collectible handler 824064A8 through 82223930) pauses for the progress screen, fades out, and its unpause fades back;
  the joiner skipped every pause, so it played on behind a black screen. Now the joiner's own prompt pauses (822231F8)
  go through while the host isn't in a mission (the mission intro stays co-op's: UpdateMissionIntro). Co-op never sent
  CD progress; since 2.2.22 CDs aren't shared items either: each player collects their own.
- The level props sync (2.2.20) is off unless mod.ini `props = 1`: all three Mac freezes (2.2.20-2.2.22) happened with
  it and none before; it makes props loose from the update hook outside the game's physics callbacks and refuses the
  game's own knock-overs in its contact handler (M: the shared physics change).

**2.2.24: the gun drop** (the 2.2.23 test: the Mac froze ~33 s in with props off; the user: no CD was picked up).
- The Mac's log: the host's RPG and car explosions (10 skipped here as the host's cars), a copy of the host's rocket,
  then every co-op character NaN at 231834148, the window's "copies' own drops not made 20", and the CD prompt 1.06 s
  after the NaN. So the CD is a result (M: a NaN position passes a collectible's distance test and the prompt is queued
  1000 ms after, 82223930(34, 1000)), not the cause. All four Mac freezes began after explosions or knockdowns, which
  drop a person's gun through 824739D8 (from the knockdown 824497F0 and the ignite 8245E3A0); since 2.2.19 a copy's
  went to GunDropHook, which took the gun with 82446038(h, *(h+3548), 0) in place of the game's drop (M: the suspect).
  Now 824739D8 is not hooked: a copy's gun drops as the game drops it (a member may see a local gun next to the host's
  item copy). The cash and food suppression (82408B78, 82408C58, 8245A7D8, 8247A0B8: no item made) stays.

**2.2.25: a diagnostic build** (the 2.2.24 test: the Mac froze the same way ~55 s in with the gun-drop hook gone).
- Five joiner freezes (2.2.20-2.2.24), each with every co-op character's position NaN at once and then the hang in
  8240FBB0; none in the long fights of 2.2.10-2.2.17. Props (2.2.20) and the gun-drop hook are already off; no one
  event comes before all five (TEST-2.2.24-mac.md's table: car explosions skipped here, rocket copies, knockdowns,
  traffic crews let out, corpse pushes). Left from 2.2.18-2.2.19: the people's stances (StancePickHook on 8244E018,
  the re-pick) and the dropped items (item copies, ItemTouchHook, the members' drop suppression).
- mod.ini `stances` and `items` switch them (both 0 in this build's mod.ini); the next tests turn them back on one at
  a time.

**2.3.1: a joiner stuck on a black screen after a mission's opening cutscene** (the first mission test on 2.3:
Vice Kings 1; research tools/coop_test/TEST-2.2.25-mac-mission.md).
- Both players saw VK01's opening cutscene; the host went on, the joiner stayed on a black screen and its game
  stopped updating. When the host's cutscene ends, co-op ends the joiner's copy with 821F9700 (the game's own instant
  cutscene end), which runs the mission's next step: it loads the mission's people and recomputes the people-model
  slots (8241BD40 -> 8250E208, spinning in 8250BA90 in the joiner's stack). Co-op's level-0 slot quota (2.2) was in
  place: the game had rebuilt the table during the cutscene and co-op put its own back 10 s later.
- Now co-op's quota is never in place during a cutscene or for 20 s after one, and before co-op ends a cutscene the
  game's own table goes back first (PeopleQuota off).

**2.3.2: the real cause** (two more Vice Kings 1 tests: the same hang with people_level0 = 0, co-op's quota never
written; research tools/coop_test/TEST-2.3.1-findings.md).
- In all three tests co-op removed the joiner's own cutscene actors at the mission's start ("Own cutscene character
  left standing removed: VK_X_M_Killa1-03 / PD_X_F_GenericYoung-03 ..."), 2-3 s before the opening cutscene played:
  the rule (1.44) removes this game's own actors flagged 0x20000000 unless a cutscene played in the last 2 s, and none
  had played yet. The joiner's own mission had made them for its cutscene; at the cutscene's end its next step
  (824BFD78 -> 821F9700) hung in the people-model slot walk (8241BD40 -> 8250E208 -> 8250BA90) loading for them.
- Now that rule runs only 20 s after a cutscene this game showed, never before a mission's first one (the cutscene
  time is reset when the host's mission changes); the mission-over clean-up stays. 2.3.1's quota pause stays too.
- 2.3.3: the reset also each time the host's mission flag comes on (the 2.3.2 test passed VK01's opening and second
  cutscenes; with the mission started again - the same hash - and its cutscene skipped on the host, the earlier run's
  cutscene time let the removal run and the joiner hung the old way).
- 2.3.4: the hang is the cutscene end's wait (821F9700 -> 821F9230 -> 824C4850): a loop with no frame and no
  timeout until every character of the cutscene and the mission group has its model resident (8241BD40, 8250F098,
  825124C0). It hung with world = 0 too (no copies of the host's people), so the copies are not the cause. Three
  changes:
  - The second removal site for this game's own cutscene actors follows the 2.3.2 rule too (with world = 0 it still
    removed TS_X_M_Killa-A before the cutscene).
  - The quota put-back keeps a table the game has just rebuilt (2.3.3 wrote back co-op's session maximum, leaving
    level 0 two slots).
  - On the joiner, a hook on 821F9700 puts the game's own quota back and gives back co-op's people pins before any
    cutscene end, and pins no people models for 15 s after. A diagnostic on 8241BD40 logs, once, the models the wait
    is on when it lasts over 5 s.
- 2.3.5: the 2.3.4 test still hung, with co-op's pins given back and the game's quota in place. Its slot dump at VK01's
  end showed 9 of the 12 people-model slots worn (users 2-4), mostly by copies of the host's people. The wait raises
  the levels the mission's next characters ask at, and gives each level a slot. A slot with users is only marked for
  the culls to empty, and no cull runs inside the wait. So the copies of other games' people now go at once before a
  cutscene end on the joiner: they're hidden during the cutscene anyway, the host is told they are gone here (offered
  again), and nothing is made for 5 s. 2.1's copies and the NPC replica go too. The diagnostic now reads the waited
  records' flag byte and the list's end correctly.
- 2.3.6: 2.3.5 passed both of VK01's cutscene ends (24 and 29 copies removed) and the joiner got into the mission.
  Then its screen stayed black while it still played: right after the host's next mission group, the joiner's own
  mission started a cutscene that co-op held back (the host played none there), and its mission waited for it with
  the screen faded out. Co-op also removed its four VK_X_M_Killa1-04, the gang members that cutscene was for.
  - A held-back cutscene is now started and ended a second later, as a skip.
  - The removal of this game's own cutscene actors (both sites) never runs while the host's mission runs. The cost:
    leftovers from an earlier mission (1.37: Johnny Gat, Dex on the stairs) can stand around in the next mission
    until it ends.
  - A safety net for cutscenes not tested yet: past 3 s in the cutscene end's wait (824C4850), each waited model
    that isn't resident gets a kind-5 request at level 0 (8250C750). The slot walk then gives it a slot at once
    instead of waiting for culls that never run. The request is given back 20 s later.
- 2.3.7: 2.3.6 played Vice Kings 1 start to finish on the joiner: every cutscene end passed, the held-back cutscene
  ended as a skip, and the success screen showed on both. After the pass, the host's mission flag kept coming back
  during its end sequence, and co-op started VK01 on the joiner again each time ("didn't start here (trying again)",
  busy 1). The joiner was left in a half-started mission of its own, its title cut off at the top. A mission the host
  passed is now over on the joiner until the host's mission hash changes (2.4: or the session ends).
- 2.3.8: the 2.3.7 test hung at VK01's opening cutscene end again, in 824C4850's second wait: the mission group's
  vehicle list *0x8370F8D4 (+84 a vehicle handle, its model **(car+50324)). Its record had handle 0: the mission's car
  didn't exist on the joiner, so nothing could ever load for it. 2.3.6 got through because its car was there. On the
  joiner, a hook on 824C4850 now clears bit 0x80 (at +68) on each record of the group whose object doesn't exist here,
  person or car, so the wait skips it. Why the car was missing here is still open.
- 2.3.9 (smoothing, from the 2.3.8 test, which played VK01 start to finish with no freeze):
  - "Host was moved by the mission" now needs a jump faster than 100 m/s by the host's own clock, with the joiner
    over 40 m away. In the chase, late updates of a running or driving host moved the joiner 5 times (56-167 m).
  - The same mission coming back within 20 s of its end (the host's flag in a mission's end sequence) no longer
    brings the joiner over again.
  - The host's shots that the fire gate refused 5 times in a row get the shooter's state applied again, once a second
    (268 were refused in VK01's last 30 s), with the state logged.

**2.4: people, players, world, missions, network and performance** (the 2.3.9 tests, 2-6 players; research
notes/coop-2.3/stances_fix.md, items_freeze.md, npc_copy_flicker.md, flinch_audit.md, shells_gangs_time.md,
shared_world_gaps.md, mission_objective_sync.md, mission_cars.md). The protocol stays 9: every new message kind below
is ignored by older builds. The mission loading-screen and cutscene-end fixes are 2.3.1-2.3.9 above.

*People (the copies of the host's people on a member)*
- **Crouching** (always on). A copy's own combat AI (820CC008) crouched and stood it by its own target (820CADC0
  "crouch here?", then crouch_start / crouch_stop), and MatchCrouch put the host's back 250 ms later: the copy bobbed
  up and down.
  - 820CADC0 now answers with the host's crouch for a copy, shown once it has held 200 ms.
  - The AI's crouch_stop (82462108 from 820CBF8C) is refused while the host's person crouches, and so is the stance
    restart its caller 820CBEE0 makes right after it whatever the stop did (82440200(h, -1, -1, 1), LR 820CBFA0), for
    that copy only (each crouch_stop clears the refused copy first, so only the restart straight after it is skipped).
  - MatchCrouch re-picks only when the bit really changed: a crouch_start the game refused (82461E60: +508 2, +516 4,
    burning) was re-picked every 250 ms. A refusal waits 1 s and is logged.
- **Stances** (`stances = 1`, off by default). The stance picker hook (8244E018, 2.2.18) forced every stance on a copy,
  the walk, run and strafe loops too, while co-op moved the body: the legs looped on the spot. Now only still
  postures are shown (stand 0-3, crouch 4, ready 62-67, 97, 102, 125, 141), only while the copy stands still and its
  crouch matches, once per change that held 150 ms, at most every 500 ms. Still is below 0.3 m/s (moving again above
  0.6 m/s) for 300 ms, from the shown path and the copy's gap to it: the record's velocity was never read for people,
  so every copy counted as still. The probe from 820CADC0 is skipped, and the copy's AI "ready" re-pick (820D7A78
  from 820CC008, which restarts the stance every tick while the forced posture has no ready flag: crouched copies
  flickered) is skipped while the host's posture is forced.
- **Legs** (always on): a copy co-op holds in place (an action of the host's, or standing on its spot) shows a posture
  instead of the walk, strafe or back loop its own AI picks; a copy within 1 m of its path stops where it stands
  instead of stepping back to a path point behind it.
- **Facing**: a standing copy turns as the host's person turns (it kept the facing it stopped with), past 20 degrees,
  at 270 degrees a second by the update's time, until within 3 degrees. That turn is the only writer of a standing
  copy's facing (F6 below).
- **Flinching** (flinch_audit.md, F1-F6). Every writer of a shown character was listed; the flinches came from the
  follow path going backwards, two writers in one frame, and whole-gap jumps:
  - F1, the path never steps back. Past 300 ms beyond the newest record a copy stands at the extrapolated point
    (150 ms on, only while the last two records moved) instead of jumping back to the record. The jitter buffer also
    covers the sender's send interval (the 80th percentile of the last 16 gaps plus half the latency spread,
    `Follower::Add`), up to a cap: people 600 ms, cars 400, players' characters 300 (people past 50 m came at 2-10 Hz
    against a 130 ms buffer). It shrinks by a quarter of the excess (2-8 ms a record), so a person that comes close
    isn't 600 ms late for ~12 s. The held people's events wait up to the same cap. (The host-side 10 Hz floor for
    moving people was left out: the buffer covers the slow bands without a host change.)
  - F2: a fading person is stopped where it stands when the fade starts (its last move order aimed 0.8 s on), and the
    per-frame snap keeps it on its path's last point while it fades.
  - F3: 0.3-6 m off its path a character moves at most 0.35 m an update toward it (about 20 m/s), not the whole gap at
    once (hold releases, car exits, stop overshoots, get-ups); past 6 m it is placed as before. The per-frame snaps
    (people and players' characters) only snap under 0.3 m across, so the two agree.
  - F4: another player's character is left alone by the per-frame snap while getting in or out of a car
    (`car_anim_until`, as `Drive` already did).
  - F5: a copy lying or getting up (+780, state 8) is left to its ragdoll and get-up, held 300 ms past it (at most
    8 s): after the 3 s knockdown hold it was yanked upright onto the path.
  - F6: the per-frame snap puts a standing copy's position only (5 cm a frame when 0.3-2 m off).
  - Weapon actions (126-158 and the reloads 164-171) keep the body pinned to the path; every other hold (idle
    replays, falls, hit reactions, lying) lets it go, and a hold that runs out clears it.

- **The host's people that never loaded here** (the shared model budget; research notes/coop-2.3/
  shared_model_budget.md). In the 2.4 test 10 of 95 `World match` windows showed fewer people than the host listed (22
  "Copy never finished loading" in 16 min): a member asks for every copy's model at level 0 of the people group's 12
  slots, and its level 0 had 6-8 while 4-6 slots stayed free at levels its own game reserves and never used. Co-op's
  quota (2.2, `PeopleQuota`) never held: 8241BE28 rebuilds the table 0x827ACFFC on *every* world update (824131F0,
  called by 821FFAF0 with no gate, runs 8241BE28 then 8241CA80), so co-op's write lasted one frame (1424 "applied now:
  no" against 19 "yes" across every log).
  - Member: co-op's table now goes on in the existing hook on 8241BE28 (`QuotaHook` -> `QuotaAfterRebuild`) straight
    after each rebuild, outside cutscenes and the 20 s after one (memory only: the 14 words, which 8241CA80 then copies
    into the group as the game's own). A level with requests in its queue keeps the game's slots, at least its kind-5
    count (`PeopleQuota`, once a second) and 1; a level idle for 10 s gives its slots to level 0. Level 0 goes from 6-8
    to 10-12. `PeopleQuota` no longer writes the table; it turns the grant on and off and puts the game's own table
    back (with 8241BD40(0,0,0)) when the member stops showing the host's people. While the people group's pass runs
    the table goes back at once (memory only) and the two game calls follow on the next second's try.
  - Cutscenes (member and host): while 0x8370D991 is set 8241BE28 returns early and keeps the last frame's table,
    co-op's when it changed it. On the first such frame `QuotaHook` writes the game's own table from that rebuild back
    (memory only, when co-op's last write is still in place; 8241CA80 copies it next as the game's own), so a
    cutscene and its end's wait (821F9700 -> 824C4850) run with the game's slots, the host's parked ones included.
    The `People-model slot quota` and `Model budget (host)` lines count these put-backs.
  - Host (`model_budget = 1`): each member in its area reports its room for the host's people every 2 s
    (kEventModelRoom, 44: its level-0 quota less the level-0 slots bound to models neither the host has nor a copy
    waits for, its waiting and loaded counts). While a member waited for a model in the last 10 s and the host holds
    more people models than that member's room, the host parks R = slots - the least room (at most 3) at level 13,
    which nothing asks at on the host, so its game loads at most 12 - R people models. Up at once (at most once per
    10 s), down a step after 30 s, off at once when no member in its area has a report under 6 s old. Never while the
    host's own game waits at a level (more queued than its quota), never below 6 slots at level 0, nothing in a
    cutscene and 20 s after one (and off at the host's cutscene start), and only while the host's budget tick ran in
    the last 3 s (it stops while the host's player isn't one). A member counts as waiting only for models the host
    has or a copy wants (not its own mission characters or player bodies), and for each person it declined for its
    model since its last report (those never ask, so before, a member short of slots reported waiting 0 and the host
    never parked: the Mac-hosted test of 65830004); the host counts its people models without
    the players' bodies. An older host drops kind 44; an older member sends nothing and counts as unknown (nothing
    parked for it).
  - The member's admission (`WorldModelRoom`) uses that live level-0 quota less its level-0 slots held by player bodies
    and its own people (as the room report counts them; 4-11) instead of a fixed 11, so a person
    whose model can't fit is declined (the host holds the id back 3-30 s) instead of made, waited for 8 s and
    destroyed after 20. Every copy that doesn't finish loading is counted on the `World (member ...)` line ("never
    loaded", not only the first 25 lines).

*Players*
- **Name tags** (`name_tags`, `name_tag_m`): each other player's name over their character, drawn the way multiplayer
  draws its overhead info (8237DF20; MP's own pass 8237F4A0 runs only through the net-mode slot *0x8370F230 from the
  2D HUD pass 82200700 with the MP byte set). The frustum test 82632A00 and world-to-screen 82632178 at 2.15 m over
  the feet (in a car 1.4 m over it plus 0.35 m a seat, one line per seat); the text measure 8264EF48, then 8264E988
  twice (px_thinoutline *0x827D6D20, then px_thin *0x827D6D1C) at HUD scale x 0.87; the slot's map-arrow colour,
  faded over the last 30 m and at half alpha while the player is down. Drawn from a hook on the story HUD 822E1508,
  so they go with the HUD and its fade (822E3C10). Names are folded to printable ASCII (the font's coverage beyond it
  is unknown).
- **The host's freeze in the first 4-player session**: co-op played a player's exit from the host's car (8257B8E0,
  which reserves the seat: car+52360+seat*108); 630 ms into it their knockdown took them out at once with
  8257C400(h, 19). That leaves the reservation and the door seat +2508 that the exit's end (82576A60) would clear.
  Once they got up, the car's per-seat update (82531138 -> 82528338 -> 824482F8) looked up the seat pose of that
  reservation through their car, now 0, and 82542FC8 walked guest memory from 0x3F8 until it crashed. Now
  `LeaveCarNow` does event 19 plus those two clears at all four event-19 places, and a hook on 824482F8 skips the
  lookup for a person with no car (its no-car path, the byte *0x8370F832 set, always runs). Both are counted on the
  `Seats:` line.
- **Clothing that doesn't stay on**: the host's other freezes in the 5-player sessions were the game's own render bug
  in 822335E8, the sibling of 822366B8 (a player-type character's per-piece render instance returns 0 still holding
  its lock at +364 when the "dupe anim shadows" pool allocation fails); the renderer fix (render_fixes.cpp, #42)
  guards both. On the co-op side a re-dress of another player's clothing that doesn't stick (pieces unloaded again
  every ~3 s all session for 3 of 4 players) backs off: from the second within 10 s the next waits 10 s, doubling to
  60 s, counted from the later of the last re-dress and the end of the last wait. A real record or count change is
  still dressed at once.

*World*
- **Props** (`props = 1`, now on by default, also when the line is missing): no freeze or NaN with it in the 2.4
  runs; its 2.2.20 freeze was the item copies (below).
- **Dropped items** (`items = 1`, now on by default). Copies were made with -1.0 at +60 and 0 at +64; the game's own
  drops (824739D8, 82408B78) put the item collision filter *0x827E79EC at +60 and -1.0 at +64 (the mass from the
  items_3d entry). The copies were bodies of mass 0 with filter BF800000, the likely source of the NaN that froze the
  joiner in 2.2.20-2.2.24 (the hang itself was 8240FBB0's angle loop, "while a >= pi", on a NaN camera forward). Now:
  - copies use the game's two values, and are checked for a NaN in every row and in their height;
  - the people pass (8240FBB0) is skipped while the camera's forward is NaN (logged);
  - the gun drop 824739D8 is hooked again with the game's own drop always run (the 2.2.19 way, taking the gun with
    82446038, was the 2.2.24 freeze suspect). For a copy of the host's people on a member the new gun item (new in the
    item ring, the gun touch handler 82405F98) is removed on the next update, so only the host's item copy shows.
    Counted on the `Dropped items:` line.
- **Corpses** (`corpses = 1`): a hook on the corpse clean-up 824793C0 skips it for a dead copy shown here (made, not
  fading) past its gate, so the body stays until the host's remove fades it; before, the member's clean-up took
  bodies while the host's still lay there. Nothing of the copy is touched. A game loop waiting for it (4 asks for the
  same copy within one game update, counted per copy so two copies asked in turn are caught too; the normal call is
  once per update) is let through. A copy kept 10 minutes is let through too, so it can't stay on when no remove
  comes (a body hidden on the host): past the gate the clean-up hides it at once (+120 0x08000000, +3664 set 10 or
  15 minutes on for a later step) or, with neither +3619 bit, destroys it (then reported as gone, as with
  `corpses = 0`).
- **Weather blend**: the clock was already the host's, but the joiner held the host's stage pure while the host's sky
  was a blend of two (82157598 blends forecast entry i with i-1 in the first half of its duration and with i+1 in the
  second). A new event, kEventWeatherState (41), every 2 s and on a change (the stages, or the half), carries the
  host's previous / current / next stages, their durations and the time into the current entry. The joiner releases
  its hold and writes the two entries being blended with the host's durations and starts (only 2, the count the
  game's own forced path writes: the forecast array's size couldn't be confirmed from the code), so its own updater
  makes the host's blend. While the host holds its weather (a cheat or a menu) kind 41 sends the held stage
  (*0x8370D204) and the joiner holds the same. Kind 35 still goes for older builds and counts only without a kind 41
  in the last 30 s; both kinds' state is reset when the session ends. Counted on the `Weather and waypoints:` line.
- **The gang response for a wanted member**: the joiner had 5 stars with the Vice Kings and only police came. The
  host's people model quota (8241BE28, table 0x827ACFFC) is built from the host's player alone, so the member's gang
  got no model slots and its response group (823FD1B8: all its models loaded) was never eligible. A hook on 8241BE28
  runs it for the response's target (`PlayerSwap`) while that member's gang level is 2 or more and above every one of
  the host's. The host's `Police:` line counts the rebuilds and the steered orders per group (police, helicopter, the
  three gangs); the first 30 orders are logged (was 10).
- **Brass**: co-op never touches the game's casings (pool 0x837599C4, 64 x 224 B, each freed by 82132FA0 5 s after it
  is made), yet a tester saw some stay in place on one screen. `BrassCheck` walks the active list once a second and
  lets one older than 6 s go (its timer set to now: the game frees it the next frame).
- **Blood** (`hit_effects = 1`; research notes/coop-2.3/hit_effects.md): the other games skip the person hit
  824581C8 for a replayed shot, so only the shooter saw blood. Its blood is one cosmetic call, 82132BA8(pos, dir,
  type), from 824581C8 only (0x824586A4 headshot, 0x82458728 body). The game whose 824581C8 really ran (the
  shooter's; the host for its people's) sends what it played (event 45: the victim, the position from its centre, the
  direction, the type; at most 30 a second), and each other game plays 82132BA8 alone on its copy, held with the
  shot's follow delay, and not when its copy stands over 8 m from the shooter's spot. Line `Blood:` every 10 s.
- **The sync checker** (always on; research notes/coop-2.3/sync_checker.md): the host's truth near each member,
  compared there, so sync gaps show in the logs instead of by eye.
  - Host: every 3 s per member in its area (one member a call, at least 250 ms apart), a sample of the 24 registry
    entries nearest that member's character within 60 m, streaming or not, as its game has them now: position,
    velocity, heading, stance, crouch, gun, car and seat, dead, how long the person's visible state has held
    (`WorldObj::t_since`, kept by `WorldRefresh`; not steady when it wasn't looked at in the last 500 ms), what it knows of the offer to that member (created, declined, not
    offered yet, not streamed and why, the chunk off there, a player's car), and its loose items there (up to 8).
    A new session message type, kMsgWorldTruth (41, WWT1: `TruthHeader` / `TruthRec` / `TruthItem` in world.h, at
    most 822 bytes), unreliable and outside the stream's budget (~270 B/s per member). Older members drop the type.
    Props are in the format but not sampled (each would need a game call).
  - Member: a sample is held 600 ms, then compared once after an update's applies. A record whose host side doesn't
    stream it is "host-only" (not a gap). Then, in order: not created (lost create, create pending, declined and why,
    the host waits on a decline, removed here, chunk off), invisible (queued, fading, not resolvable, its model not
    loaded, set up pending, hidden, faded), the wrong model (stand-in, other), position at the sample's own time (the
    copy's shown owner time from its follower, the host's velocity times the difference; 1 m + 10% of the
    extrapolation, 2 m for a car above 15 m/s, twice without follower data; "stream" when the newest record is off as
    well, "apply" when only the copy is), facing (30 degrees), the stance by class (still, crouched, crouched moving,
    upright moving, other: a copy plays its own walk and run variants), crouch, gun, in a car and the seat, dead. The
    state is judged only once the host's person has held it 1 s ("in transition" otherwise), seated people get no
    position or facing check, a player's car no facing check, the first 10 s in the area count as "joining" (items
    told before the member joined come again with the host's 5 s resend). When the host listed everything within
    its radius a copy within 55 m of the center that isn't in the sample is an "extra" (a ghost): where its records put
    it at the sample's time, and never a copy created after the sample. Items: missing, gone, more than 1 m off.
  - Logged: one line per new id and category (again after 30 s; at most 8 a window and 100 a session), the 10 s
    `World truth:` line, and once at each session's end `World truth (session ...)` with the repeat offenders, and on
    the host `World truth (owner, session)`. The host's `World (owner) match slot` line counts the samples it sent.
  - Not built: the two-instance test harness of the research (section 9).
- Not done: blood and hit effects for replayed shots (824581C8 still skipped under `fire_replay_depth`). Its effects
  run only after the damage call 82454A30 returns true, which with the replay's damage zeroed isn't known, and the
  rest of it holds the knockdown 824497F0, the ragdoll impulses (82267DB0 / 82267F28 / 82268500) and unidentified
  calls (82455CF8, 8237A6F8, 82480348) that may be reactions: letting it run risks double knockdowns. It needs the
  effect call found first (Option B of the design).

*Missions*
- **The host's mission markers** (`mission_icons = 1`). In the 2.3.9 test the joiner's own copy of the mission (Los
  Carnales' Trojan truck, most likely lc04) marked its own truck with the green square; getting in it did nothing for
  the host's mission, and the 150 m catch-up kept pulling the joiner away from it.
  - The host sends its mission's map icons (list 4, head *0x83027018: icons 52 `map_act_kill`, 59
    `map_act_location`, 66 `map_act_protectacquire`, with their positions from 822EE5E8) as a new HUD kind,
    kHudMapIcons (6): every 250 ms when they change, every 2 s anyway, and once empty at the mission's end. The icon
    name table (0x82038DF8) is checked once: with other names at those indices nothing is sent.
  - The joiner shows them as the game's own icon nodes (822EC550, kind 6 with a fake handle 0x7EC00000 | slot); a hook
    on 822EE5E8 answers their positions, so the game draws them on both maps with its variants, glow and edge. A
    person's or a vehicle's marker follows the copy of the host's person or car here every update, so their moves
    don't count as a change on the host (a fleeing target sent the reliable snapshot at 4 Hz) - only while that
    person or car streams to every other player (each in the host's area with the copy made). Without a copy on
    every joiner (a mission vehicle with `mission_cars = 0`, a named story character, a joiner out of the area) the
    moves count as before, so the marker still moves at 4 Hz there.
  - While it runs the host's mission and the host sends them, the joiner's own `ingame_effect_add_vehicle` (824CF230,
    the green 3D car marker) is skipped. Its own `minimap_icon_add_*` (824D1EC0, 824D1FA0, 824D2088, 824D2168,
    824D2228, 824D2308) run, and the next update removes its own icons of the three kinds the host sends (a trigger's
    with its +372 / +364), as it does once at the start for those added before. Any other icon a mission step adds
    stays: the host doesn't send it, and skipping every add left that step with no marker at all.
  - Vehicle and location steps still count only for the host's player: the joiner follows the host's markers and
    helps (kills already count). The 3D location markers, the GPS route and the pause menu's objective list are still
    the joiner's own.
- **Following the host** (`mission_follow_m`, `mission_cutscene_follow_m`): the catch-up by distance is now
  `mission_follow_m` (0 = never, the default; 100-2000; 2.3.9 brought a joiner over 150 m away every 10 s, away from
  what it was doing). After the host's cutscenes (which move the host with teleport("#PLAYER#")) the joiner is moved
  only past `mission_cutscene_follow_m` (40 m). The pull at the mission's start and on the host's scripted jumps
  stays. The mark of a mission the host passed (2.3.7) is cleared when the session ends, so a next session can play
  it again.
- **The host's mission vehicles** (`mission_cars = 1`, experimental, off by default). Each game runs its own copy of
  the host's mission, so each makes its own mission vehicles, and the host's were left out of the world stream: in
  lc04 the joiner had its own truck, and the host's (the one its mission counts) wasn't shown. With it on both sides:
  - Host: the cars a script makes during its mission (source 7, `world_mission_cars`) stream like any script car (not
    while its script has one hidden: lc04's truck until its step), but only with `traffic = shared` on the host and
    while every member in its area has said within 10 s that it hides its own (kEventMissionCarsOn, 42, member ->
    host every 2 s). A member with 0 or an older build never says it, and the cars stay left out for everyone as
    before (one a member already has is removed there after kInactiveMs). Once they stream in a mission they go on
    streaming while a player sits in one of them, whoever comes into the area or acks late (left out under a member
    at the wheel of the host's truck, that member would get two trucks). Once a second the host lists them
    (kEventMissionCars, 43: up to 7 ids an event, a flag for "streaming").
  - Member: its own mission's vehicles (source 7 made while its mission runs, and the vehicle records of its mission
    groups, *0x8370F8D4 +84) are paired with the host's listed ones of the same model, nearest first, and hidden the
    way `vehicle_hide` does (82538A90(handle, 1)). They are not destroyed and the records aren't rebound: the joiner's
    script and the group waits (824C4850, GroupWaitHook) keep a live car under each record, and the joiner's script
    never drives, teleports, locks or removes the host's copy. One its script shows again is hidden again; one a
    player sits in, or with a shown driver, is shown and let go (an NPC-driven mission car stays the joiner's own;
    a driver shown during a cutscene and 1 s after doesn't count, as the mirror shows the joiner's hidden mission
    characters for every cutscene); one its script had hidden is left alone.
  - The host's copy not shown here for 3 s while the host still lists it (out of the area, declined): the own one is
    shown again. No longer listed (wrecked and removed there, or hidden there by its script): it stays hidden while
    it isn't listed, and the pair goes on as before when it is listed again (a late list doesn't end it). Mirror
    over, no "streaming" list for 4 s, or the session over (the link lost, the host gone): everything hidden is shown
    again (`vehicle_show`: 82538A90(handle, 0), 825321F8) and the mission-car lists start over.
  - Getting in the host's truck works as for any world car; whether the host's mission counts the joiner at its
    wheel is the host script's own check (it asks for its own player).

*Network*
- **Epic, a player who is gone**: in the first 4-player session (online, one player through Epic's relays) a player
  whose game stopped answering filled the shared send queue (capped at 64 KB, kOutgoingQueueBytes); the others'
  packets were refused (1190 in 10 s) and one of them dropped too. When the host drops a player (timed out, too far
  behind, left), co-op now clears what is queued for them and closes their Epic connection (EOS_P2P_ClearPacketQueue,
  EOS_P2P_CloseConnection; net.h `Session::OnDrop`). Not for "joined again", whose new join uses the same connection.

*Performance with more players* (the 2.3.9 group tests: co-op's game-thread time was 21-30 ms/s at 4 players and
83-113 ms/s at 6, with single updates of 492-738 ms; the parts that grew were `avatar`, 3.6-4.4 ms/s to 59-75, and
on a member `world apply`, 2.5-3.5 to 35-45)
- The re-dress back-off above: its streak was counted from the last re-dress, so the first 10 s wait always ended it
  (3 re-dresses every 16 s for good). Each cycle cost ~35 ms; at one every 3 s that was ~12 ms/s per such player.
- One heavy character step per update for all the other players together: a character made, its first dressing,
  each of the three steps of dressing again, and a colour rebuild. The 300 ms gate covered only the first two, and a
  first dressing ran in the update its character was made in (the 492 ms update: made in 400 ms, dressed in 86). The
  first dressing now comes in the next update, still without the 300 ms wait. A step the game refuses at once
  doesn't take the turn.
- World copies far from this player (cars past 100 m, people past 60 m) are steered every other update, half of them
  each time: a car's velocities (`WorldSteerCar`) and a person's `Drive` (the teleport 8243E6F8 and the move orders).
  A car keeps the velocity it was given meanwhile (its impact check and velocity hold go at that half rate too) and
  the per-frame snap still keeps people on their paths; everything else about a copy runs every update. In the 6-player test `world apply` followed the cars steered, and
  most copies were past 50 m. The `World (member) applied:` line counts the updates skipped.
- The cars players sit in are taken once per pass (each car copy looked up every player's character and car); the
  other players' look blocks are compared in place and their colours copied only when they go on.
- The Perf line names the worst update's cause: its biggest part, the biggest part inside that while it holds over
  half, and the heaviest character step, e.g. `(worst 492.3 ms: avatar 491.0 > avatar: made 400.2 (Player 4:
  character made 400.1))`. New parts: `avatar: made`, `avatar: clothing`, and `world apply: messages`, `: creates`,
  `: removes`, `: cars`, `: people`. `Perf parts` lists 12.
- Left: making a character in a pool slot released moments before took 400 ms inside the game's respawn (8247E248),
  and a first dressing waits for the clothing to load (8248CFC8: 86 ms of that 492 ms update). Each is now alone in
  its update; a first dressing without the wait (the re-dress path's) would remove the second.

*Settings* (mod.ini; with its line missing a setting takes the default)
- `name_tags = 1`, `name_tag_m = 150` (10-1000).
- `props = 1`, `items = 1`, `corpses = 1` (on); `stances = 0` (off: crouching and the legs fix run either way).
- `mission_icons = 1`, `mission_follow_m = 0` (never by distance), `mission_cutscene_follow_m = 40` (6-2000),
  `mission_cars = 0` (experimental; needed on the host and every member in its area, and `traffic = shared` on the
  host).
- `model_budget = 1` (the host: 0 = it never parks slots for the members' room).
- `hit_effects = 1` (0: no blood sent or played for other games' hits).
- Always logged, whatever `diagnostics` says: `World truth:` (members, every 10 s while samples come) and its session
  lines, `People-model slot quota (after the game's rebuild):` (members) and `Model budget (host):`.
- `diagnostics = 0`. With 1, the extra 10 s status lines for tracking sync problems are logged - `Legs (member):`,
  `Flinch:`, `Crouch (member):`, `Corpses (member):`, `Brass:`, `Mission icons:`, `Mission cars (host):` / `(member):`
  and `Name tags:` - with the first detail lines behind them (ready re-picks skipped, casings let go, the weather
  blend written, the icon snapshots, own mission markers removed after their add, own mission vehicles hidden). The
  counters run either way; one-off lines (settings checks, the first tag, refusals, errors) are always logged.

Not built yet: Package B/2-6 (personal cars as records, occupant events, the pose relay filter); Package C's later
parts (wreck looks without physics, props and movers as records, pickups; drive-by mode and the aim pose on copies).

## 12. Co-op 3: one shared world, no world host

Goal (the user's): every player is always in the same world, wherever they are, as in GTA Online: each game
simulates the people and cars around its own player and shows everyone else's, and objects change hands in place as
players move. In 2.2-2.4 the host owns one area around its player and a client runs its own world outside it, which
nobody else sees. Co-op 3 gives every game a world of its own to stream, keeps a session host for the lobby, the clock
and the weather, and hands objects over in place. It is built in phases, each with its switch (`world_mode = area`
keeps 2.4's world): 1a the multi-owner wire and names, 1b 2.1's streams removed, 2 every game owns (no areas), 3 the
hand-over in place, 4 host migration (built), 5 direct links. Research: notes/coop-3 (DESIGN.md, its section 12 overriding,
host_assumptions_audit.md, review_design.md).

**Phase 1a: owners and names on the wire, protocol 10** (no change in play: the host is still the only world owner).
- *Protocol 10.* `net.h` `kVersion` 10: 2.4 (9) and older builds are refused at join, as every version change.
- *NetId.* An object's name for its whole life, whoever owns it: (origin slot, origin handle, origin join nonce), in
  `world.h` packed origin << 56 | handle << 16 | nonce (map order is the origin's handle order, so `Plan` runs in the
  same order as in 2.x). It is not a handle: each game holds the object by its own (`Entry.local`, the copy's
  `local`). The join nonce (`world_nonce`) is a new one each time a game's registry turns on (a session start, a new
  slot; never the last one, never 0), so a slot that joins again, or a handle a later session uses again, names
  another object. Nothing names an object before its create, so the creates carry it and the join itself is as in
  2.x.
- *Registry.* Keyed by NetId and found by this game's handle too (`Find(local)`, `FindNet`, `FindOrigin` by origin
  and handle alone); `Entry` has `net`, `local` (was `id`), `owner` and an ownership `epoch` (0 in 1a; +1 a hand-over in
  phase 3) as fields instead of the key's owner / id. The scheduler's plans and removals still name records (owner,
  local): every record (`WWS1`, `WWR1`, `WWD1`, `WWT1`, crews, seats, `kEventAuthority`) names an object by its owner's
  handle in the owner's namespace.
- *Wire.* `WWC1` 49 bytes: + the owner, the NetId's origin, nonce and origin handle, and the ownership epoch (`id` is
  the owner's handle, the records' name; 1a: origin == owner, handle == id). `WWR1` 18 bytes: + the owner and the
  epoch it went at (a remove from before a copy's last hand-over is dropped). `WWA1` 27 bytes: + the owner it is for
  (its target; its declined ids are that owner's handles).
- *Members keyed by owner.* The member takes a world message only from the owner it names: `WWC1` / `WWR1` / `WWS1` /
  `WWD1` / `WWT1` whose owner is not the slot that sent it are malformed (`WorldIn` keeps the origin), and the
  OwnerMap only from the session host. Copies are keyed (owner, its handle) from the record and carry the create's
  NetId and epoch; the digest is per owner (`world_digest[owner]`); `WorldOnMap` is in another game's area when its row
  names an owner that isn't this game (was: owner 0), and keeps that owner (`world_area.owner`, `WorldAreaOwner()`):
  `WWA1` and the model room go to it, the sync checker takes its truth only, the members' lines name it. The lookups
  `WorldPersonLocal` / `WorldCarLocal` / `SharedPedLocalHandle` / `SharedCarLocal` / `FindCar` take the owner (2.1's
  copies and taken cars are the session host's).
- *Persons in events by NetId.* A person goes by its NetId's handle in the word that held the host's handle, and a tag:
  the full one (origin << 28 | epoch & 0xFFF << 16 | nonce) where a word is free, else the short one (origin << 12 |
  epoch, the full tag's top half, matched without the nonce) in the top half of a word that had 16 bits left. The
  person an event is about is taken from its owner (the event's origin) at its epoch; one named as an argument (an
  attacker, a call's pointer) by its origin and handle. "The host's player" is the sender's. The layouts:
  `kEventPedAction` a[4] handle, a[5] full tag; `kEventPedCall` a[1] handle, a[7] short tag << 16, a kind-4 argument's
  origin in a[6] bits 12 + 4i; `kEventPedReact` a[0] handle, a[1] kind | the kind-4 attacker's origin << 4 | short tag
  << 16; `kEventPedEffect` a[0] handle, a[5] full tag; `kEventPedStyle` a[0] handle, a[3] full tag; `kEventHitFx` kind
  2: a[0] handle, a[1] short tag << 16; `kEventPedDamage` and `kEventPlayerHitByPed` a[0] handle, a[7] short tag << 16.
  A hit on a copy goes to its owner (`kEventPedDamage`, was slot 0) and a member's bullet on a car copy to the car's
  owner (`kEventCarDamage`); the owner takes a hit at any epoch. `kEventAuthority` only from the car's owner,
  `kEventExplosion` only with the sender's own slot as its owner, and its people and cars by that owner's ids.
  `kEventCrewDamage` (2.1's traffic) goes to the session host; it goes with 2.1's streams in 1b. `kEventHostModels`
  stays.
- *Roles.* `SessionHost()` (the slot: 0 until host migration) and `IsSessionHost()` for the clock, the weather, the
  pause, the OwnerMap, the 10 Hz mission state and "lost the host"; `OwnsWorld()` / `WorldOwnerSlot(slot)` for the
  world stream (the owner tick, `WorldSends`, the person events, hits, the members' gates, Package B's spawns). In
  `world_mode = area` both are the session host. Mission, HUD, cutscene and police tests stay `hosting` (the mission
  host and the police owner come in phase 2).
- *Tests.* `world_test`: the new fields round trip, NetId packing and order, the tags and the epoch's 12-bit wrap,
  the registry by NetId / handle / origin, and an owner in slot 2 planning (2, its handle). `net_test`: protocol 9 is
  refused.

**Puppet copies** (the switch `puppets`, built before 1b: the hand-over needs a copy's brain to go off and on; no wire
change, member side only). Research: notes/coop-2.3/puppet_copies.md (phases 0-1), notes/coop-3/puppets_notes.md.
- *Brain off.* `PuppetBrainHook` on the people AI update `820D5B48` returns at once for a puppet, the way SR1's MP
  client skips it for every NPC (`0x8370E9F6 == 1 && 0x8370E9DF == 0`). No AI mode update runs for it (the combat AI's
  own fire, crouch, ready re-picks and stand-ups, gun changes, move orders); its animation, locomotion, physics, damage,
  ragdoll and get-up run as before (outside it in the human update `82441B00`). A puppet is a world copy on foot, made
  and set up, alive, not lying or getting up, no cutscene, with a stance from its owner in the last second
  (`PuppetBar`, each update in `WorldApplyPerson`); otherwise it is 2.4's copy for that update. Other players'
  characters and 2.1's copies are never puppets.
- *Legs.* The owner's layer 0 state (`PedRec.stance`, +576) is kept with its record's time (`WorldCopy.ps`), and the
  one at the copy's shown time plays (`PuppetShown`: it comes with its body, not 50-130 ms ahead). `StancePickHook`
  answers it whatever `held` / `still` say, and `WorldApplyPerson` re-picks a copy whose +576 differs, at most every
  100 ms (`82440200(h, -1, -1, 1)`: a blend, nothing on the same slot). 2.4's still-posture block is off for puppets;
  the crouch (`MatchCrouch`), the gun in hand, `sync_idle` and the animation state (+952) are as before. A state the
  copy's anim set lacks: its own pick, on 2.4's move orders (counted, the first 10 logged).
- *Body.* Placed only (`Follower::Place`: `Drive`'s sampling, NaN guard and capped catch-up, `MoveHuman` with the
  path's velocity; no move orders, holds or standing turns); `SmoothWorldPeople` puts it and its facing on the path
  every frame, through the owner's actions too. While an action plays with the path still it is stepped 5 cm a
  frame and not placed (placing cut actions short). A move order still running is stopped (`82441308(h, 0, 0)`).
- *Hooks that go quiet for puppets* (they live in the AI update): `CrouchDecideHook` (820CADC0), `CopyReadyHook`
  (820D7A78), the stand-up refusals of `CrouchStopHook` and `StanceRestartHook`. They stay for 2.4's copies.
  `MakePassive` stays (the combat bits matter again whenever a copy is not a puppet).
- *Never "may despawn"* (whatever `puppets` is). `CopyDespawnHook` on `8244BC40` answers no for a copy (not fading).
  It is asked only by `8244BCE8`, the deactivation behind the region check, the off-screen cull, the model unload and
  the area clear: yes there destroyed the person (and a driver's car) before deactivating it. A copy is now only
  deactivated, as a script person (then `WorldApplyPerson`'s not-set-up case as before). `HumanDestroyHook`'s 60 s
  refusal stays for the paths that don't ask.
- *Log* (with `puppets = 1`, or `diagnostics = 1`; every 10 s): `Puppets (member): N of M person copies on foot with
  the brain off; turned on, off by reason; AI updates skipped; picks answered (own pick kept); re-picks; updates placed
  / held in an action / on move orders; move orders stopped; shown state = copy's %; +3716 changes; despawn asks
  answered no`.
- *Later* (puppet_copies.md phases 2-3, need a `WWP1` message): the aim target (`820D8168`, +3220 / +3224), the ready
  bit and layers 1-2 from the owner; actions held to the copy's shown time.

**Phase 1b: 2.1's streams removed** (no change in play with `world = 1`, the default since 2.3: the world stream is
the only way another game's world shows; `kEventHostModels` stays). Research: notes/coop-3/phase1b_notes.md.
- *Removed (the host's side).* `CaptureTraffic` / `CapturePeds` and their packets (`WTR1` / `WTN1` / `WPD1` / `WPL1`,
  `kMsgTraffic`, `kMsgTrafficNames`, `kMsgPeds`, `kMsgPedLook`: the four type numbers stay unused, so the later types
  keep theirs), `WorldLegacyStreams` and `legacy_streams` (which clients still got 2.1's streams), `ClientPositions`,
  `traffic_out` / `peds_out` / `traffic_names` / `ped_looks_out`, `IsSentCar`, the police response's units and cars
  sent in them (the response streams as any person and car of the world).
- *Removed (the client's side).* `shared_cars` / `shared_peds` and everything that made, moved, seated, stood in for,
  dropped or adopted 2.1's copies (`SharedCar`, `RemoveSharedCar`, `AdoptSharedCar`, `LetGoSharedCar`,
  `SteerAlongPath`, `StandInSeated`, `DropCopy`, `AdoptOwnCharacter`, `SmoothSharedPeds`, `quick_gone`,
  `unloadable_models`), `traffic_in` / `peds_in` / `ped_looks_in`, and the 3 s wait before the first OwnerMap
  (`world_wait_until`: a new session showed 2.1's streams then). `kEventCrewDamage` (23, a hit on a crew of 2.1's
  traffic by its seat) is gone: a crew is a person of the world stream and is hit with `kEventPedDamage`.
- *Removed (the setting).* mod.ini `world` (`world = 0`: 2.1's streams, no world stream; `world_stream` in the code)
  and its log lines. `traffic = own` / `peds = own` keep their meaning for the world stream: on the owner its cars /
  people are left out of it (`kEntryExcluded`), on a member its own stay on.
- *Kept as they were, now on the world stream alone.* The levers (`UpdateSharedTraffic` / `UpdateSharedPeds`: this
  game's own traffic, parked cars, people and gang manager off while it shows another game's area and until its last
  copy is gone, its own police by `OwnPoliceOff`), the people-model memory (pins, `EvictNonHostModels`,
  `LoadedLookalike`, `DropOwnModelRefs`, the quota), `kEventHostModels` and its uses (`SendModelRoom`,
  `LoadedLookalike`, the eviction scores, the drop of own mission characters).
- *Ported to the world stream.* The predicates name world copies only: `OurCharacter` (`WorldPersonId(h, true)`),
  `CrewCharacter` (`WorldCrewSeat`), `IsSharedCarHandle` (car copies and world car copies); `SharedPedHostHandle`,
  `SharedPedLocalHandle`, `SharedCarHostHandle`, `SharedCarLocal`, `IsSentPed`, `NetPerson`, `FindCarToUse` gave way to
  `WorldPersonId(h, false)`, `WorldPersonLocal`, `WorldCarId`, `WorldCarLocal`, `WorldSends(h, false)`, `WorldPerson`,
  `FindCar`. `taken_cars` (a world car copy this player took: `WorldLetGoCar`) holds `CarKey(owner, its handle)`, so
  `CarId`, `FindCar` and `WorldAdoptCar` name it by its owner (it was a bare id of the session host's); `CarId` names a
  world car copy by `WorldCopyOwner`. `QueuePersonEvent` goes to the area's members only (its "every game" branch was
  2.1's). The mission icons on the host's people and cars find their copies through the world stream.
- *Fixes from the 1a review.* `WorldOnRemove` looks the copy up first and drops a remove of an older ownership epoch
  before it tombstones the key; `WorldOnCreate` drops a create older than the copy it would replace (M1). "No person"
  is origin 15 (`kNoPersonTag`, `PersonNet`'s default): the person-event senders skip a person no other game knows
  (`kEventPedAction` / `PedCall` / `PedReact` / `PedEffect` / `PedStyle` / `PedDamage` / `HitFx`; a call's or a
  reaction's person argument goes as none), and `kEventPlayerHitByPed` still goes with an attacker of origin 15, "not
  here" there (M2). `NetPersonLocal` takes this game's own person by its bare handle only when the registry has none
  under that handle (L1); a new join nonce is never the last one, also when the sum wraps to 0 (L2); the default
  create charges are 10 bytes higher (`kCreateBytesCar` 130, `kCreateBytesPed` 170, WWC1's 49 bytes: L3); the tag
  packing is named in `world.h` (`ShortBits`, `TagOfShortWord`, `OriginTag`; `ShortTag` is `TagFromShort`) and tested
  (L4, L5). Style: version tags read "(3.0: ...)", the comments say what a line does (not what it was), `cw::` after
  its alias, `world_mode_shared`.
- *Log lines.* Gone: `World (owner): 2.1's traffic and people streams to slot S ...`, `World (member, mod.ini world =
  0 ...)`, `Shared traffic: N cars (host sent N) ...`, `Traffic people: ...`, `Cars made with a different model than
  asked`, `Pedestrians not sent ...`, `Mission characters seen ...`, `Cutscene: the host's people hidden here ...` /
  `Cutscene over: N of the host's people shown again ...`, `Car copy (type T) was removed by this game ...`, `Copy
  never finished loading: ... (host ...)` with its `its model's streaming` / `stream entry` lines (2.1's copies; the
  world copies' `(world person ...)` one stays), `Remote vehicle: a player took a car of the host's shown here`,
  `Pulled-out driver copy carries on as the host's person`, `... hit the crew in seat ...`. Shorter: `World (owner) slot S (where): offered ...` (no "2.1's traffic and people streams to it"),
  `Shared pedestrians: looks applied, animation sets matched, own removed, actions`, `Copies: hits sent, hits taken
  from people, own actions blocked`, `Models not loaded here in time: model requests asked again; people refused by a
  full pool`, `Copies removed: made as kind ...` (no gone / not sent / far), the crews line without its hits on 2.1's
  crews, the police line without the units and cars sent.
- *Risk.* With no OwnerMap from the host for 3-10 s (a stall, a load) a member shows its own world until the map comes
  back; 2.4 showed 2.1's streams then if the host still sent them. The fallback for a broken world stream is the
  previous build, not `world = 0`.

**Phase 2a: every game owns its world, per-object interest** (`world_mode = shared` only; `area` plays exactly as
before). Research: notes/coop-3/phase2a_notes.md.
- *Roles.* In shared mode every game is an owner and a viewer at once: `OwnsWorld()` (this game's registry streams)
  and `WorldViews()` (this game shows copies of others) are both true, `WorldOwnerSlot(s)` is any other slot, and
  `WorldTakesFrom(s)` (`from_owner` in the dispatcher and the network thread) takes the world messages and events of
  every other game. A hook that has to tell a copy from this game's own asks about the object (`WorldPersonId`,
  `WorldCarId`, `OurCharacter`: a copy; `WorldSends`, `world_reg.Find`: its own), not about the game. In area mode
  the four reduce to 2.4's tests.
- *Owner side.* `WorldGameTick` runs `WorldOwnerTick` and `WorldMemberTick` on every game. A viewer is every game
  with a pose under 3 s whose pose says it plays shared (`kPoseShared`, pose flags bit 6; a game of the other mode is
  logged once, review_phase1a L9); no join or leave rule, no epoch, no OwnerMap (WOM1 is not sent and is ignored in
  shared mode). What a viewer gets is decided per object in `world.h`'s scheduler (`InInterest`): people within
  100 m and cars within 150 m of its camera eye, removed there (`WWR1` reason 1, out of interest, faded) past that
  plus 20 m; a crew goes by its car, so they come and go together. The chunk-mask filter stays as it was (entries
  carry no chunk yet, review A3-17). While this game is inactive (its player dead, a cutscene: `WorldSelfActive`, pose
  bit 7 `kPoseActive`) it has no viewers: its copies elsewhere get reason 3 removes and it offers everything afresh
  when it is back. Package B (spawning for members' viewpoints, `BoostWanted`, the keep-alive) is off in shared mode;
  each game's own spawners stay on, so two players together see about two games' density (2b). New trains stay off
  while a viewer is within 300 m. `QueuePersonEvent` sends a person's events only to the viewers it was created on.
- *Viewer side.* Every other game's stream at once in the one copy table (keyed by owner). Each owner gets its own
  WWA1 (to every owner with copies here, declines to report, or a world message in the last 3 s), with what this
  game holds of that owner's, its own declined ids, its own near counts, and an even share of the caps among the
  owners with copies here (at least 4 cars / 8 people; 2c sizes the shares). An owner whose pose is 2 s old has its
  copies faded and told to it as gone; one whose pose says inactive has them faded (it sends their removes itself);
  either way its creates are declined until it is back, and a game that leaves has its copies faded. The sync checker
  keeps a sample and a sequence per owner, and judges an owner's samples from 10 s after its first copy here.
- *Per object, any owner (design 7, review blocker 7).* Hits on a copy go to its owner (`kEventPedDamage`,
  `kEventCarDamage`, as in 1a); a person's hit on another player's character is sent to that player
  (`kEventPlayerHitByPed`) by the game that owns the person, never for a copy; copies don't hurt this player, take
  no damage, take no own actions, calls, reactions, equips, prop drops, shots or launches (`EquipHook`,
  `PlayActionHook`, `SharedPersonCall`, `AttackHook`, `FireHook`, `ProjectileCreateHook`, `DropGateHook`, the stance
  and crouch hooks, `CorpseCleanHook`, `IgniteHook`) on whichever game shows them; every owner sends its people's
  actions, calls, reactions, styles, effects (the attack arrows, now once a second from every owner), rounds
  (`WorldFireCapture`), projectiles, explosions and blood, and plays the other owners' on their copies (the blood
  from this game's own people's shots only: `BloodSprayHook`). Explosions: a game holds a copy car's own explosion and
  a prop a replayed one broke, and sends its own (a prop two games break both send it; the first played wins
  everywhere, `world_prop_boom`). Bullets on a car copy go to its owner (`WorldCarHit`), the others' on this game's
  own cars are applied here. Crimes against copies are dropped (their owner counts them). `TrafficDespawnHook` and
  `ParkedRemovalsHook` keep a car a player sits in on every owner.
- *Players.* Another player's character is kept to 170 m (the cars' interest; made within 60 m as before), whatever
  the areas; the maps' markers cover further.
- *Not in 2a* (2b / 2c). The spawn veto and density share, parked cars, hood and special spawns on the nearest game,
  keep-alive and the evacuate, the active bit's other states (paused, loading, interiors); the police (each game's own
  police and response stay on, `kEventSeen` and arrests by copies as in 2.4), missions and the mission lock, dropped
  items (no item copies in shared mode: each game keeps its own drops), the owner models and the model budget per
  viewer (the session host's model events are ignored in shared mode, its budget gets no room reports), viewer budget shares by
  copies and bytes, the pools; the props (`props = 1` still decides by slot 0).
- *Log lines (shared mode).* `World (owner, shared): slot S is a viewer now (D m from this player) ...` / `... no
  longer a viewer (why)`; `World (owner): slot S plays world_mode = M and this game M2 ...`; the owner line `World
  (owner): shared: N viewers (slots ...; per object, people 100 m / cars 150 m)`; per viewer `World (owner) slot S (a
  viewer at D m): offered ... ; out of its interest N entry-ticks, removed for interest N (sent N)`; the viewer's
  `World (viewer, shared): held cars ...` and `World (shared): owners shown here N: slot S: cars / people (queued,
  fading)[, out]; removes out of interest N; owners gone out N (silent N), their copies faded N, creates declined while
  out N; WWA1 sent N`; `World (viewer, shared): slot S is out (its pose 2 s old | inactive: dead, arrested, paused,
  teleported or in a cutscene): its N copies here fading`.

**Phase 2b: one street per spot** (`world_mode = shared` only; `area` plays exactly as before). Design 12 (1), (2),
(3), (4), (10), (14), (21); research: notes/coop-3/phase2b_notes.md. Every game's own spawners run; what changes is
which spots each one fills and what it keeps.
- *Spawn veto.* A spawn spot goes to one game: the nearest player by position (2D), every player within 10 m of the
  nearest one tying with it and the lowest slot of those spawning (`world.h` `SpawnWinner`, unit-tested: exactly one
  game fills each spot). Only another player whose pose is under 500 ms and says active counts, and only within its
  own spawners' reach of the spot (people 100 m, traffic 150 m). People: the ambient spawn at a point `8240DD40`
  (`PeopleSpawnHook`; its callers are the ahead, side and group pickers and the area fill) returns 0 for a point
  another game fills, so the picker tries its next one. Traffic: the spot query `82412298`'s list loses those spots
  (`TrafficSpotsVeto`). The chunk mask is not part of the test: the spots carry no chunk and the mask's index isn't
  known to match across games yet (review A3-17); the reach stands in for "it has the spot loaded".
- *Density share.* The wanted counts (`824194A8` traffic, `82419208` people) are lowered by the copies of other
  games' traffic and ambient people that are in this game's own veto region and its rings (people 90 m 2D, traffic
  140 m), never below 1 (response, mission, crew, hood, special, parked and players' copies don't count).
  `BoostWanted` is not used. On a meet-up (copies in the region), while this game's ambient count is over its
  lowered wanted, one of its own people goes every 500 ms through the game's make-room path `8240E448`; its test
  `8240E338` (not protected, beyond 10 m, not visible here) also refuses, in shared mode, a person another active
  player's camera may see (within 30 m of its eye, or within 120 m and 60 degrees of its forward row), for the
  game's own make room too.
- *Keep-alive.* Package B's hooks hold this game's objects for the viewers instead of the members' viewpoints: a
  person (`8240FA90`), a traffic car (`82412878`), a disturbed parked car (`8241D6B0`) or an allocator victim
  (`823FFA48` / `823FFC48`) that streams and that an active viewer has (its copy there, or within its interest: a
  candidate, design 12/(10)) is held from this game's own despawn for 20 s at most, or until its chunk unloads
  here (a car's entry inactive; people go with the chunk clear). The 20 s start again once the game kept it itself.
- *Parked cars.* An undisturbed parked car is each game's own and never streams (`kWhyParked`). Once someone is in
  it (or a player gets in), its hit points drop or it burns, or it has moved over 1 m, it streams as this game's
  for good. A game that shows another's disturbed parked car within 2.5 m of its own undisturbed one removes its own
  through the record (`8241D210(rec, 4)`, the game's "creation refused" state: it isn't made again until this
  player is past 80 m). By position, because the record ids aren't known to match across games.
- *Active owner and the evacuate.* The pose's active bit (`kPoseActive`) is also off while the pause menu has been
  open 1 s, while arrested and for 2 s after a teleport (the player's position jumping over 60 m between two owner
  ticks); a loading screen stops the game, so its pose goes stale. Interiors aren't told apart yet. On a death, an
  arrest, a teleport or a cutscene's start the game evacuates (`EvacuateNow`, pose bit 23 `kPoseEvacuate`, area
  mode's `kPoseWorldStream` never set in shared mode): for 3 s its viewers stay in (what still lives goes on
  streaming), the viewers don't fade its copies for being inactive, and the copies of what it removes meanwhile stay
  where they are until the 3 s are over, then fade. Phase 3 hands the objects over at that point instead.
- *Other spawners.* The hood and special spawns (`824290D0`) are skipped while another active player within 150 m
  has the lower slot (by the players: the manager's regions aren't read). New trains stay off while another active
  player is within 300 m. Ambient boats: if they come through the traffic spot query they are vetoed with it
  (not checked); helicopters are the police's (2c) or the special spawns' (the hood rule above).
- *Not in 2b.* The hand-over (phase 3: a split still fades the leaving player's share near the others, held up to
  20 s by the keep-alive), the police response owner, missions, items, owner models and budgets, pools (2c).
- *Log lines (shared mode, every 10 s).* `World (shared) 2b: spawn veto: people points N (left to others N), traffic
  spots N (left N; slot S xN ...); this game's region: its own people / traffic, copies of the others' there (the
  share's), wanted people N (the game's N) / cars ...; culled N (tries N); make room refused ...; kept for viewers:
  people, cars, parked (their 20 s over N, chunk unloaded N); evictions ...` and `World (shared) 2b: parked: own
  undisturbed N, disturbed and streaming N (newly N), own hidden under another's disturbed car N; hood / special
  spawn passes skipped N; new trains ...; this game inactive: dead / arrested / a cutscene / paused / teleported /
  no player (seconds), position jumps N; evacuations N (...), evacuating N s; copies held for an evacuating owner N
  (faded after N)`; with `diagnostics = 1` one line per other player's region. One-shots: `World (shared): the first
  people | traffic spawn spot left to slot S ...`, `World (owner, shared): parked car H disturbed (entered | damaged |
  moved) ...`, `World (shared): this game's parked car H (record R) removed: slot S's disturbed car ...`, `World
  (owner, shared): evacuating (why): N objects of this game's are shown elsewhere ...`.

**Phase 2c: police, missions, items, models, budgets** (`world_mode = shared` only; `area` plays exactly as before).
Design 8 and 12 (5), (6), (11), (12), (13), (16), (20); research: notes/coop-3/phase2c_notes.md. Also the phase 2a
review's fixes (review_phase2a.md: H1, H2, M1-M6 and some lows).
- *Police (the response owner).* Wanted players within 150 m of each other (chained) share one response owner: the
  highest police level, then the lower slot (`world.h` `ResponseGroup`, unit-tested: every game of a group computes
  the same owner). Its game's response chases the most wanted of the group (`UpdateResponseTarget`, now on every
  game that owns its group's response, with the group as the candidates instead of `kPoseHostWorld`); the other
  games of the group start no response of their own meanwhile (`8242ABA0` and the roadblocks skipped; units already
  out go on, streamed as any person and car). A player alone runs its own. Every game's cops tell a culprit's game
  whether they see its character (`kEventSeen` from any game, kept per game there). An arrest by any game's cop goes
  to the culprit's game; one by a copy of another game's cop is refused (the arrester is `r31` at `82483440`: its
  only caller is the cop's update `820B1A28`). Response copies don't count in the density share (2b) and fade when
  their owner leaves, as every copy does.
- *Missions (the mission host and the lock).* Any player may start a mission; one runs at a time. The session host
  keeps the lock (`kEventMissionLock`: request, grant or deny with the mission's hash, release, and its state to
  everyone each second). The holder is the mission host (`MissionHost()`; area mode: the session host): every
  `missions = host` path keys on it instead of slot 0 (its HUD, cutscenes, map icons, mission cars, the follow and
  the mission state packets; the clock and the pause stay the session host's). A start without the lock is held
  back, asked for, and started again once granted; a start while another holds it is refused with a notice. A
  mission or activity that runs without a start co-op saw (an activity's own launcher) asks for the lock once it
  runs; denied, it runs on its own, its objects local. The holder lets go 3 s after its mission is over. When the
  mission host leaves, every other game ends the mission it plays for it, with a notice. Only the mission host's
  mission characters stream: a game running another's mission (the mirror) keeps its own mission characters out of
  its registry, and its script cars are never streamed, even after the mission.
- *Items.* An item belongs to the game whose person (or player) dropped it: every game owns its own loose items,
  tells them to the others and takes the take requests for them; copies of another game's people drop nothing (the
  cash through `8245A630`, `CashDropHook`). A copy of another game's item is made within 80 m of this player and goes
  past 120 m or when its owner leaves; copies are keyed by (owner, handle).
- *Owner models.* Every game sends its loaded people models (`kEventHostModels`) to its viewers each second; a viewer
  keeps one set per owner and uses their union where 2.4 used the host's. The people-model quota balancer (option
  c) runs on every game while it shows people copies, the session host too: a level that its own people ask at
  keeps its slots, the idle levels' slots go to level 0 for the copies. The host's park (`ModelBudgetTick`) is off in
  shared mode (each game's own people need its own slots). The copies' model pins are given back as in area mode
  (`SharedModelUpkeep`); the eviction of models outside the owners' sets is not run (it removes this game's own people).
- *Budgets per viewer.* WWA1 now carries (35 bytes) the viewer's share for that owner of what it takes in (36 KB/s
  shared out by the copies it holds of each owner, at least 6 KB/s each), its pool caps shared the same way (at
  least 4 cars / 8 people), the bytes of that owner's world messages it received, and the interest it has room for.
  An owner's upload is shared only by its busy viewers (something created there, or within its interest); a
  client's stream to another client also gets the relay's share (the session host's upload over every relayed
  stream, estimated); the viewer's share caps it, 6-24 KB/s (`world.h` `SharedTickBudget`, `ShareOut`, unit-tested).
  A viewer whose WWA1 says it received under three quarters of what was queued for it gets half the budget for 2 s.
- *Pools.* World copies leave room for the other players' characters and cars not made here yet (a reserved floor);
  when the live pools drop below a margin of 6 people / 3 cars, the interest radii this game asks of every owner
  shrink by 10 % a second (to half at most), and grow back by 5 % a second once there is room.
- *Props.* A prop no player decided (an NPC's car, a stray round) is decided by the nearest active player's game (the
  spawn veto's rule), not by slot 0.
- *The phase 2a review's fixes.* H1: the cutscene end removes the copies on every game that shows them (the session
  host too). H2: a mirror's own mission characters stay out of its registry. M1: a hit by this game's own people on
  another owner's copy (a person or a car) goes to that owner with the attacker's NetId (`kEventHitBy` before the
  damage event), applied there with its copy of the attacker. M2: a create of an owner still marked out is taken
  when its pose already says it is back. M3: interest is tested first, so a copy whose chunk the viewer no longer has
  is removed for interest. M4: let-go crews and copies in the graveyard are left out of the registry. M5 / M6: the
  budgets above. Lows: L1 (a future timestamp), L2 (an owner's leftovers forgotten when it leaves), L3 (no truth
  judged for an owner that is out), L4 (owner models), L6 (NPC projectiles within 200 m only), L10 (the mirror's
  cars); style S1-S3, S5, S6.
- *Not in 2c.* The hand-over (phase 3), host migration (phase 4), mission objects that aren't people (mission cars
  stream only with `mission_cars = 1`, as in 2.4), the relay's own per-destination backlog, biasing the model pick.
- *Log lines (shared mode, every 10 s).* `World (shared) 2c: budgets: busy viewers N of N a tick (relayed N), held to
  a viewer's share N, a viewer behind N (sent N KB, received there N KB); pools here: reserved ..., room for copies
  ..., interest asked people N% / cars N% ...; own people's hits on others' copies sent N (cars N), others' people's
  hits applied here N (attacker not here N); creates of an owner back ... N` and `World (shared) 2c: police: this
  player's group ... (owner slot S, slots M), in a group N s, its owner N s, own response held for another's N, owner
  changes N, arrests by copies refused N, seen told N / taken N; missions: lock free | slot S (mission H), asked N,
  requests N (granted N, denied N), starts refused here N, released N, started N, unseen starts N, run alone N, ended
  for a host that left N; items: copies gone far N, owner left N; owner models sent N, taken N (their union N);
  copies' pins given back N passes; props decided here N, by another N`. One-shots: `Police (shared): ...`, `Mission
  lock (session host): ...`, `Mission lock: granted | denied | let go ...`, `Mission start H refused | held back ...`.

**Phase 3: the hand-over in place** (`world_mode = shared` with `handover = 1`; `area` plays exactly as before).
Design 6 and 12 (9), (10), (15); research: notes/coop-3/handover_research.md and phase3_notes.md. Also the phase 2b
review's fixes (review_phase2b.md: H1, M1-M6 and the cheap lows).
- *What changes hands.* Ambient people on foot and traffic cars with their crews, between the games whose players
  are near them, without a fade: the old owner's object becomes its copy of the new owner's, the new owner's copy
  becomes its own (its AI back on, on the ambient or traffic list). Never mission objects, response units, hood or
  special spawns, parked cars (each game's own until driven: then traffic), followers or the party (a script record:
  `8244BC40` says no), the dead, a person lying, knocked out, getting in or out, in an action, hidden or fading, a
  fighter unless its target is the new owner's player, a car a player sits in (the authority path; it goes to the
  driver's game after 10 s of driving or when that player gets out), a wreck. A car moves with its whole crew.
- *Who and when.* Only the owner starts one. The candidate is the active viewer whose camera is nearest, holding a
  made copy, within its keep radius (people 75 m 2D, cars 120 m): another game must be nearer than the owner's camera
  by 25 m (people) / 40 m (cars) for 2 s, and an object moves at most every 5 s. Urgent (at once): the object past
  80 % of the owner's despawn ring (people 90 m, cars 130 m). Evacuating (death, arrest, teleport, a cutscene's
  start: 2b's `EvacuateNow`): every object an active viewer holds within its interest, at once, to the nearest. At
  most 12 under way at a time (24 while evacuating). `world.h` `HandPick`, unit-tested.
- *Messages (WWH1, `kMsgWorldHand` = 42, reliable; protocol 10 stays: an older build ignores the type).* Offer
  (owner -> candidate: the NetId, its epoch e, its record id, the state: position, velocity, hit points, damage, team,
  combat bits, AI mode, a fighter's target, the crew), Accept (its copies' handles: the ids once adopted) or Decline
  (no copy, no room, no traffic node, not loaded, too far, busy, inactive, handover off, stale), Release (e + 1, the
  final state, the slots that hold a copy), Return (e + 2: the candidate can't adopt at the Release),
  OwnerChanged (the new epoch, the new id, the old key: to everyone). Nobody simulates the object for the trip
  (two hops through the relay). `world.h` `HandRec`, `HandOut`, `HandIn`, unit-tested with lost and late messages.
- *Release (the old owner, one update).* A person off the ambient list (`0x8309ABB4`: the reverse of `82479FD8`),
  category 0 (`82400F10`), `+4244` 0, its order cancelled (`82441308`) and its AI mode 0 (`820D5DA8`), passive, co-op's
  model reference taken (`8250C750` kind 5, as for a made copy); a car off the traffic list (`824105B8(node)`),
  category 3, its vehicle AI idle (`825905D0`). Out of the registry with no remove (`Registry::Detach`: the holders
  keep their copies) and indexed as the new owner's copy where it stands (its follower seeded). With `puppets = 1`
  its brain goes off as any copy's.
- *Adopt (the new owner).* Out of co-op's copy tables first, so no copy hook matches it again (`skipped: adopted`
  counts any that still does: it must stay 0); a person on the ambient list (`82479FD8`, category 1), its nav node
  (`824A3FB0`, as the ambient spawn asks it), its combat bits as the old owner had them (`MakePassive` undone), its hit
  points and team, a fresh default behaviour (`820DFF00`; a fighter keeps its own); a car on the traffic list
  (`82413688`: no free node is a Return), its lane (`82514AF0`'s road segments within 25 m, the nearest heading its
  way: `825B1AC8` + `825B1BF0`, as the traffic creator does) or else its AI idle, its hit points; its crew category 2.
  Into the registry with its NetId, the new epoch and the holders' offers already created (`Registry::Adopt`: no
  creates for them); OwnerChanged to everyone.
- *Everyone else.* A copy's key moves to the new owner on OwnerChanged, or on that owner's create when it comes
  first (a create of a NetId held here is never a second copy); records that came early are taken; for 5 s the old
  key still finds the car (a pose that names it so; the new owner's own pose names it by the old key for 1.5 s).
  The old owner forwards hits it gets for 2 s (`kEventHitFrom` names the attacker). The digest leaves a copy that
  just changed hands alone for 3 s, and waits a digest before it calls one with records under 2 s old extra.
- *Failures.* No Accept within 1 s: the owner keeps it (three silent offers in a row: that game is left alone 60 s,
  an older build). No OwnerChanged or record within 2 s of the Release: the old owner takes it back at e + 2 and says
  so. A game that owns an object and hears of a newer epoch (or the same from a lower slot) yields it into a copy; a
  create from another game of an object owned here is the same test (`two owners seen`). An owner gone (left the
  session, or removed the object while evacuating): each holder that could own it claims it after 2 s + 250 ms x
  its slot unless a claim came first (equal epochs: the lower slot). A copy car this player sits in that its owner
  removes is this game's own at once (`WorldLetGoCar`, owner-aware: no taken car named as the owner's).
- *The phase 2b review's fixes.* H1: the veto, the share, the cull guard and the keep-alive use every present shared
  player with a fresh pose (not the viewers: an inactive game has none), and an inactive game leaves the spots near
  an active player to it. M1: the traffic fill (`82411CC8` -> `82411310`, after a reset) is vetoed too, and surplus
  traffic is culled (`82410F38`, the car furthest behind this camera that nobody may see). M2: a game counts in the
  veto only within its reach too, the reaches are 120 m / 160 m, measured from the cameras' eyes (unit-tested past
  reach). M3: this game's eye as its last sent pose has it, and a player stays in the veto list 1.5 s after its last
  fresh pose. M4: a parked copy hides this game's parked car once, when it is made, the nearest within 2.5 m still on
  its spot and heading its way. M5: the evacuation's start is taken on every pass; held copies are out of the digest's
  check. M6: the hood rule is greedy by slot (unit-tested). Lows: L2 (make room spares a held person; no cull for a
  mission's empty street), L4 (the shared state reset at the session's end), L5 (an inactive game skips the hood rule
  near an active one), L9 (the veto hooks' perf part), L10 (style; the 2b line split in two).
- *Log lines (shared mode, every 10 s).* `World (shared) 3: hand-over: offered N (urgent N, evacuating N, player cars
  N), under way N, accepted here N, declined to this game N (reasons), no answer N, late accepts N; released N people,
  N cars (crew N); adopted here N people, N cars (crew N); returned by this game N (declined here N: reasons), returns
  here N, taken back N (no word N)` and `World (shared) 3: claims: owner left N, removed while evacuating N, a car this
  player drives N (waiting N); yields N, two owners seen N (kept against N); copies moved to a new owner N, creates of a
  copy held under another key N (kept N, older N); hits forwarded N, taken from a forward N; not offered: (bars);
  adopted cars 5 s on: driving N, standing N (lane found N, none N, no traffic node N); skipped (adopted): copy lookups
  N, copy updates N, in both tables N (all must be 0)`. The 2b lines: `traffic fill spots N (left N)`, `culled people N
  (tries N), cars N (tries N)` and a second `World (shared) 2b: keep-alive: ...` line. One-shots: `Hand-over: ... is
  this game's now`, `... released to slot S`, `... yields to slot S`, `Hand-over: adopted car H 5 s on: driving |
  standing`.

**Phase 4: session host migration** (`world_mode = shared` with `host_migration = 1`; `area` plays exactly as before:
the session ends when the host leaves). Design 9 and 12 (17); research: notes/coop-3/phase4_notes.md. Also the phase
2c review's fixes (review_phase2c.md: H1, M1-M9 and the cheap lows).
- *The rule.* The session host is a role, not slot 0. When it leaves (its `kLeave`, now sent by a host too, five
  packets: at once) or goes silent (6 s), every game picks the same successor: the lowest slot still present. Slots
  and names stay; nothing is renumbered. The successor becomes the session host and the relay; every other client
  links to it again (`kRejoin`: its version, its slot, its name) at the address the session host gave every client
  in advance (`kPeerAddr`, reliable, at each join, leave and re-link: every client's address as the host sees it;
  online, the product user ID, which each game maps to its own stand-in address). The new host holds a pending link
  per client for 20 s (reliable messages for it wait, snapshots are dropped); a client tries for 15 s. What a client
  had queued for the old host and never saw acked goes to the new one (relayed as before), the successor's own to
  each pending link. Clients send each other a bare packet once a second (warm links: Epic opens its P2P connection,
  a NAT its port; every receiver drops them), so a re-link is quick. `net.h` `Session`: `EnableMigration`,
  `AddressCodec`, `HostSlot`, `Relinking`, `HostHeardAt`, `HostAddress`; inbox-only `kHostMoved` (origin = the new
  host, then the old one's `kPeerLeft`) and `kRejoined`. net_test case 10 (a host vanishes with 4 clients, its
  successor leaves cleanly, a new player joins the moved session, two one-way link failures).
- *When it fails.* A client whose re-link isn't answered in 15 s joins the old host's address again (2.x's "trying to
  get back in"); a successor nobody links to in 20 s does too (most likely only its own link to the host failed, and
  the host is still there). A client that doesn't know the successor's address does so at once. A re-link the new
  host doesn't wait for (it gave up on it, or another name) is a new join (a new slot: the client starts over).
- *Online (Epic).* The lobby is made with `bDisableHostMigration = EOS_FALSE` in shared mode only (area keeps
  `EOS_TRUE` and the host destroying the lobby at the end). Decision: the session's own rule decides, and the lobby
  follows it (not the other way round, which would wait for Epic's notice on every game and could disagree with the
  net layer's timeouts): EOS picks a new lobby owner when the owner leaves (`EOS_Lobby_AddNotifyLobbyMemberStatusReceived`,
  `EOS_LMS_PROMOTED`, tells every member), and an owner that isn't the session host gives the lobby to it
  (`EOS_Lobby_PromoteMember`, every 10 s until it is), so new players, who connect to the lobby's owner, reach the
  session host. In shared mode every game leaves the lobby at the end (the last one out closes it).
- *What the new host takes over.* `hosting` and `session_host` follow the role (the network thread sets both at
  `kHostMoved`), so the clock, the weather and the pause broadcast, the OwnerMap's writer test and every other
  session-host path move with it. The mission lock: rebuilt on the new host from its copy of the last `kLockState`
  (every game keeps one), with the generation going on from the newest seen; an ask the old host never answered goes
  to the new one. If the old host held the lock, its mission ends for everyone as for any mission host that leaves
  (2c, with the M4 fix below). Its objects follow phase 3's claims (an owner gone). The relay's lost queues: what a
  client never saw acked is carried over (above); after each re-link the broken cars this game decides are sent
  again and every viewer's digest comes 2 s later instead of up to 10 s (what the old relay dropped shows as missing
  or extra there and is asked for again or removed: the existing catch-up).
- *Copies hold through the gap.* Every other game's stream and pose comes through the relay. While a client's link
  to the session host is quiet (1 s) or a migration links the session again, an owner's silent pose isn't taken as
  the owner gone: its copies hold (and an owner keeps a silent viewer as a viewer) until 3 s after the relay is back,
  at most 25 s (`WorldRelayHold`, WorldHostHold's 3 s per owner).
- *Also.* A session host in shared mode places the mission host's markers (`IconPosHook`: not the mission host,
  instead of not the session host) and releases its let-go crews (it never ran `UpdateSharedTraffic`); direct IP with
  migration turns off Windows' `SIO_UDP_CONNRESET` (a warm packet to a closed port must not break the next read).
- *The phase 2c review's fixes.* H1: the cutscene end gives back the quota and the people pins on the session host
  too in shared mode (`!hosting || world_mode_shared`). M1: a mission this game runs on its own (the lock denied)
  takes none of the mission host's objectives, help, groups or end, and isn't ended when that host leaves
  (`MissionHereIsHosts`: mirroring, or a mission of its hash). M2: the lock has a generation (`a[3]` of the grant,
  deny, state and release); the session host frees it only on a release of the generation it holds, and a game that
  let go ignores a state still naming it and lets go of that one again; `MissionLockRelease` resets `lock_active_at`.
  M3: a grant after the 5 s timeout (or one no longer asked for) is let go at once; the pending hash is kept, marked
  late. M4: "the mission host left" ends the mission step by step: a cutscene first (at most 5 s), then `82215360(0, 0)`
  once a second until no mission runs (at most 10 tries). M5: a game leaves its police response to the group's owner
  only while the owner's own report (the group mask, now in `kMsgNotoriety`) names it within 2.5 s. M6: the budget's
  caps (the relay's share, the viewer's share) apply after the 6 KB/s floor, down to 1.5 KB/s (unit-tested). M7: WWA1's
  `received` is a running total, compared with what the owner had queued a round trip (+ 150 ms) before; a viewer is
  behind only after two windows in a row over a quarter short (a lost WWA1 costs nothing). M8: the pool reserve counts
  only the players within 150 m. M9: in shared mode cash drops from a prop's death or a car's destroy only on the game
  that decides it (its own prop, its own car), never inside another game's explosion or round played here. Lows: L1
  (the lock's per-session state at file scope, reset at the end), L2 (a granted start isn't made twice), L3 (a new
  mission host's mission id starts as the lock's hash), L4 (no notice for a mirror's chained start), L5 (an NPC
  attacker by its game's epoch), L7 (64 of this game's own items, copies not counted), L9, L13; `kEventHitBy` taken in
  shared mode only.
- *Wire.* Protocol 10 stayed here (co-op 3 is one unreleased build); the phase 4 review's fixes (below) make it 11.
  `NotorietyPacket` is 4 bytes longer (M5), so every game needs this build.
- *Log lines.* One-shots: `Session: host left (slot S), successor slot N (...), N players; the others link to it
  again`, `Session: NAME (slot S) linked again to this game, the new session host` / `Session: linked again to the new
  session host (slot S)`, the net layer's `Session: host (slot S) timed out | left: slot N is the session host now`,
  `Session: session lost: ...`, `Mission lock: this game is the new session host: the lock rebuilt (...)`, `EOS: the
  lobby's owner changed: ...`, `EOS: the lobby given to the session host: ...`. Every 10 s when anything happened:
  `Session (10 s): the session host is slot S; host moves N, links made again N (given up N), reliable messages carried
  to the new host N, warm packets N; relay gaps here N (copies held N owner passes); lobby given to the session host N
  (session: moves N, links again N)`. The 2c lines gain `stale states N, late grants N, rebuilt by a new session host
  N` and `(an owner that hasn't named this player yet N refreshes)`.
- *Not in 4.* Direct links between clients for the world stream (phase 5: the warm links are half of it); a joiner
  who connects during a migration's few seconds to a lobby owner that isn't the session host yet gets no answer and
  keeps asking there (it joins once it tries again later); with direct IP over the internet the successor must be
  reachable at the address the host saw (a LAN, or ports the warm packets opened).

**The phase 3 review's fixes** (review_phase3.md: M1-M6 and the cheap lows; shared with `handover = 1` only).
- M1: a copy car this player got into while it faded and is then destroyed (`WorldDestroy`) becomes a taken car as in
  2c; only a copy that stays in the table (`WorldFade`'s path) is claimed in place (`WorldLetGoCar(..., may_claim)`).
- M2: an offer that came to nothing (declined, no answer, barred at the Accept) isn't made again for that object for
  1 s (urgent, a player's car), 2 s (evacuating) or 5 s (`WorldObj::offer_retry_at`): no more 250 ms repeats.
- M3: any Accept or Decline from a slot ends its silence count (only offers that got nothing back count towards the
  60 s rest). An Accept that brings no Release (the object changed since the offer, or the Accept was late) is
  answered with a Cancel (WWH1 type 7, `kHandCancel`, unit-tested): the candidate drops its accepted offer at once,
  and the owner's offer is over.
- M4: the people the spawn-region manager makes (`824290D0`: its cops are category 1, as the street's) go as hood
  people (`WorldPersonKind`: kind hood, marked by `HumanCreateHook` inside `HoodSpawnsHook`'s call), so they are never
  handed over or claimed and don't count in the density share.
- M5: a take-back after the object moved on (its copy here follows another owner or a newer epoch) doesn't happen;
  an OwnerChanged from any game at e + 1 or later ends a release (`HandOut::OnOwner`, unit-tested).
- M6: a parked copy is matched with this game's own parked car on its spot every 250 ms for its first 60 s, and marked
  matched only on a real match (this game's car may spawn there later, when this player comes nearer).
- Lows: L1 (a person that sat down since the offer is barred at the Accept; a seated person still on the ambient list
  is taken off it at the release), L3 (an adopted object's position at once), L4 (`HandOnCreate` only with
  `handover = 1`), L5 (a forwarded hit's attacker is taken whether the hit applies here or not; a forwarded hit isn't
  forwarded again), L7 (`kEventHitFrom` names a person only within 1 s, as the damage path), L11 (an adopted traffic
  car's engine bit, `+53976` 0x08000000), L12 (a damaged ambient list is left alone), L13 (`handover`'s default is 1,
  as mod.ini ships it; style). Left: L2 (crew epochs), L6 (a hit by the new owner's own player in the release window),
  L8 (fades instead of a claim's wait), L9 / L10 (cars with no traffic node).
- Log: the first hand-over line gains `(... not taken back, moved on since N)`, the second `accepts cancelled by this
  game N, here N`.

**The phase 4 review's fixes** (review_phase4.md: M1-M8, the cheap lows and the style points; host migration is
`world_mode = shared` with `host_migration = 1` only, apart from the wire changes). Protocol 11.
- *At most once, in order (M1).* Every reliable game message carries its origin's own count (`MessageHeader.oseq`,
  u16, kept through relays; the header is 10 bytes, so a message holds 1,144 bytes). Each game delivers from each
  origin only what is newer than the last it delivered (cleared when a slot is a new player). What a client carries
  to the new host that the old one had relayed already is dropped where it arrived before (counted), and a carried
  older message can't land after a newer one. Losses stay losses.
- *The succession order (M3).* The session host writes each client's seniority into `kPeerAddr` (u32: 1 is the
  session's first host, the clients count on in the order they joined; a new player is listed once its address has
  been shared for 3 s, `kListMs`). A new player never inherits a slot's seniority (a slot left and taken again starts
  at "not listed"), and a new join gets the slot freed longest ago, never-used slots first (L3: no stranger in the
  old host's slot while its objects are claimed). Every game picks the most senior player still present that it can
  reach (nobody listed: the lowest slot). `kPeerAddr` also lists the clients still linking again after a migration
  (their old address, seniority and token hash), so a second migration right after the first can pick them.
- *Passing over a successor (M2).* A client tries a successor for 7 s (`kRejoinMs`, was 15 s), then the next one in
  the order; a successor that left (its `kLeave`) is passed over at once. Only when nobody is left does it join the
  old host's address again, as 2.x. A session host that stops while clients are still linking to it sends each a
  `kLeave` of its own (five packets, at the address the list gave). So the host and slot 1 dropping together end with
  slot 2 as the session host (net_test 12), and a successor that leaves 3 s after taking over hands on to the next one
  (net_test 13).
- *A split heals (M3).* A session host that took over sends the warm packet too (flagged as a host, with its
  seniority) to the players not linked to it. A session host that hears a more senior one yields: its clients get a
  `kLeave` naming that host and link to it, and it links to it as a client, with what it sent that no client acked
  yet. A false successor (only its own link to the host failed: L5) yields when the old host warms it, or joins the
  old host again once the clients it waits for still warm it as clients 8 s after it took over (was 20 s).
- *Re-links are authenticated (M4).* A client makes a random 64-bit token and sends it in `kJoin`; the session host
  shares only its hash (SHA-256's first 8 bytes, `TokenHash`) in `kPeerAddr`; `kRejoin` carries the token. The new
  host adopts a re-link only from the address the list gave (online: the same product user ID, which Epic
  authenticates; direct IP: the same IP, any port) and, over direct IP, with the token whose hash it has. Anything
  else is ignored (counted), and a `kRejoin` for a slot whose player is linked is ignored too. net_test 15.
- *Direct IP after a migration (M5).* With `host_migration = 1` a client binds the session port (27015) too when it is
  free (two games on one PC: another port), so a successor is joined at its IP as before. `join_ip.txt` takes
  `IP:port` (the session host's port when it isn't 27015). At `kHostMoved` the overlay and the pause menu's line show
  where the session host is now, and the new host's notice says "Friends join at IP[:port]".
- *A new session host that was paused for the old one (M6)* lets go of it: its world runs again (`ReleaseRemotePause`,
  the controls once its own menu is closed); `kHostMoved` clears the old host's pause state.
- *Joining online during or after a migration (M7).* Of the review's two options, the joiner follows the lobby: a game
  that isn't in the session reads the lobby's owner again every 4 s (its own copy of the lobby, no Epic round trip)
  and, when it changed, joins that one (a new join attempt). That covers the joiner that reached the old host, a
  crashed host Epic noticed late, and a player stranded by a failed re-link. The other option (a client answering a
  stranger's `kJoin` with the session host's ID) was not done: it needs the joiner to reach a live member, which the
  first case doesn't. Still open: a lobby owner that isn't in the session itself (it can't hand the lobby on).
- *Cash (M8).* A person of this game's own that another game's replayed explosion kills here drops its cash here
  (`CashDropHook` counts it as this game's, as the prop and car paths do).
- *Lows.* L1: `kRejoin` is ignored with migration off (area mode: no protocol change). L2: the new host's first packet
  on the client's epoch is the adoption; the client's queue flows from then on, the `kJoinOk` follows in it; a second
  `kJoinOk` to a joined client is ignored. L4: a client takes the host's `kLeave` only from a packet that acks one it
  sent in the last 5 s (a blind spoofer can't). L6: `response_group_out` is stored once per refresh (the network
  thread never reads a passing 0). L7: "the mission host left" never ends the mission during a cutscene: the host's
  cutscene is ended first, any other is waited for (within the 20 s). L8: a slot's WWA1 baseline and send history start
  over with a new player in it. L9: a client turns migration on for the session when its host's first `kPeerAddr`
  comes (the session host's setting counts). L10: over direct IP `kPeerAddr` gives every client every other client's
  public IP and port (before, only the host saw them); online it gives product user IDs only. L11: a mission host
  that isn't the new session host asks it for the lock it holds (a rebuilt lock from an older state would read
  "free"). L12: the lobby's owner is read again every 4 s (`eos::RefreshOwner`); the promotion's result is logged 5
  times at most, the ask 3 times then once a minute; the counter is "lobby promotions asked"; its timer is reset with
  the session. L13: `AddressOf` compares handles by their text only for an ID read from the address list (once: Epic's
  handle replaces it), not on every packet. The player overlay names the session host, not slot 0.
- *Style.* Protocol 11 (S1); the slot-0 comments and `kJoinDeny`'s inbox origin (S2); `IsJoin` (S3); `SameUser` for the
  lobby owner (S4); `eos::host_id` is `eos::owner_id` (the lobby's owner) and the promotion timer is at file scope (S5).
- *Wire (protocol 11).* `MessageHeader` + `oseq`; `kJoin` = version, token, name; `kRejoin` = version, slot, token,
  name; `kPeerAddr` entries + seniority + token hash; the host's `kLeave` may name a host to link to; warm packets
  flag a session host (`flags` bit 1, seniority in `ack_bits`). A 10 build is refused at the join with the reason.
- *Log lines.* New: `Session: host (slot S) didn't answer | left: slot N is the session host now`, `Session: successor
  (slot S) ...`, `Session: slot N is a more senior session host: this game links to it` and the game's `Session: slot N
  (NAME) is a more senior session host (the session had split): ...`, `Session: a re-link as NAME (slot S) from another
  address or with the wrong token: ignored` (5 at most), `Session: session lost: the others are still with the old
  session host`, `Session: not in the session yet, and the Epic lobby has another owner: joining that one`, `EOS: the
  lobby's owner is ... (read again)`, `Mission lock: this game holds it: asked the new session host to confirm`, `This
  game is the session host now: this game unpaused`, `Socket ready (port N)`. The `Session (10 s)` line: `host moves
  N, links made again N (successors given up N), reliable messages carried to the new host N (delivered already,
  dropped N), re-links refused N, gave way to a more senior host N, warm packets N; relay gaps ...; lobby promotions
  asked N, joins started again at the lobby's owner N (session: ...)`.
- *Tests.* net_test 10 updated (successor by seniority; the new player gets slot 5, one nobody had; the one-way
  failures pass over successors), new 11 (carry-over duplicates: at most once, in order), 12 (two drops at once,
  with an address codec as online), 13 (the successor leaves while a client is still linking to it), 14 (a join
  while a client still links again), 15 (a stranger's `kRejoin` with the wrong token; `TokenHash` against SHA-256).

**The player list** (any mode; mod.ini `player_list`). Research: notes/coop-3/player_list.md.
- *Hold Back (Select; Tab on the keyboard, the loader's mapping) to see everyone in the session; let go and it is
  gone.* No gameplay action of any control scheme uses Back (the action tables `0x8203A998` / `0x8203A448`: only
  `abort` (56) in menus and `mp score` (79), the MP scoreboard `823D4A70`, which also shows only while held), so there
  is no hold delay.
- *Input* (`InputHook`, the XInput wrapper `827166D0`, buttons at state + 4, Back `0x0020`, per controller). A press
  counts only in the world: connected, the MP byte `0x8370E9F6` clear, no pause menu (the state stack) and no pause
  screen (`0x839E0DF8` != `0x82FFB84C`), and the world HUD drawn with a fade over 0 in the last 250 ms. That press is
  taken out of the game's pad state until the release, so a menu opened meanwhile (Start with Back still down) gets
  no `abort`. Pressed in a menu (pause, shops, the phone's screens) Back passes as before and the list doesn't show;
  held in from a menu that then closes, it doesn't show either (only a press made in the world counts).
- *Drawing* (`HudHook`, after the story HUD `822E1508`, so it sits over the minimap; the HUD fade `822E3C10` is now
  read whenever co-op draws anything there, not only with name tags). A centred panel, 22 % down the screen, 740 px
  wide x the HUD scale (`8263F4D0`, filled, black at 170 / 255 x the fade, params `0x82839AA0`; the screen size from
  the render state `8263D818()` + 0 / + 4). A 30 px title: `Free Roam  CO-OP n/12`, or `NAME's mission  CO-OP n/12`
  while everyone plays NAME's mission. A 20 px row of column names in grey: `PLAYER`, `ROLE`, `STATUS`, `DISTANCE`,
  `PING` (a thin line under the title and under the names). Then a 24 px row per player in slot order (this
  player's row lit): under PLAYER the slot's colour square (12 px, the map arrow's and the name tag's colour) and the
  name in that colour (`(you)` on this player's own: its name as it sends it); under ROLE `HOST` (the session host:
  the role, not slot 0), `MISSION` (the mission host) or `HOST, MISSION`; under STATUS `IN CAR` or `ON FOOT`;
  under DISTANCE the distance from this player (from the map marker: their character here or their last pose; m,
  then km); under PING the round trip to the session host. Text as the name tags (`8264E988` twice, px_thinoutline
  then px_thin, HUD scale x 0.87; the column names x 0.75), the distance and the ping right-aligned by `8264EF48`'s
  width; the colour white again at the end.
- *Ping.* Each row's player's round trip to the session host: on the session host its own links (`LinkStats`); a
  client knows its own link and gets the others' from the session host's roster (`kEventRoster` = 49, once a second
  with 3 players or more: 12 x u16 in `a[0..5]`, 0xFFFF none). `-` for the session host's own row and for anything not
  known (no roster for 3 s).
- *Not checked yet: the phone.* If the phone is a HUD overlay while the world HUD stays on (`0x827AEDF2` set), Back
  pressed with the phone open would show the list instead of closing the phone (B still closes it). The test says;
  the fix would be a phone-open test in the press's conditions.
- *Log lines.* One-shot `Player list: shown (Back held), screen W x H, HUD scale S, panel X, Y, W x H, N players`; with
  `diagnostics = 1` every 10 s `Player list: shown N frames, Back held in the world N times, passed to a menu N times;
  draw calls failed N since the start`; failures (`Player list: the rectangle 8263F4D0 failed here`, ...) at most 3.

**The final pre-test reviews' fixes** (review_final_area.md, review_final_shared.md; notes/coop-3/final_fixes_notes.md).
- *Area mode keeps 2.4's hooks* (area M1). The hooks only shared mode uses go in only with `world_mode = shared`: the
  cash drop `8245A630` and the spawn veto's `8240DD40` / `82411310`; the puppet brain `820D5B48` only with
  `puppets = 1`. The copy despawn hook `8244BC40` stays on in every mode. Area also drops the per-origin duplicate
  filter without migration (L1), and takes `kEventHitFrom` in shared mode only (L6).
- *A player that left, claimed at the right time* (shared H1). `kPeerLeft` says why (u8 slot, u8 why: `kLeftLeave`
  for a `kLeave`, `kLeftTimedOut` for a time-out, a fall behind, a new join from the same address, a missed re-link,
  a lost session). `net.h` `ClaimGate`, fed every session message: the old host a `kHostMoved` names is claimed only
  once this game is linked again (`kRejoined`), so a game whose own link failed never claims the world the host still
  owns; a player that timed out is claimed 8 s later than one that left, and not at all if a player with the same
  name joins in that time (its copies fade, and it brings its world back as new objects). A client that still hears
  its session host and gets a `kRejoin` (a game whose link alone failed takes it for a successor) answers with a
  `kLeave` on epoch 0 (u8 its host, u8 1); a game whose move began with the host's silence then joins the host again
  at once instead of trying every successor 7 s each, and the host sees the same name join again before its own
  time-out. net_test cases 16 and 17.
- *The relay gap* (shared M1, M2). While `WorldRelayHold` is on, the views keep the last poses (fresh for the veto,
  the hood rule, the density share and the keep-alive) and the police group stays as it was; the hand-over timers
  stand still, no offers start, and every claim is due 2 s + 250 ms x the slot after the gap at the earliest. An
  Accept or Decline also ends a slot's 60 s rest.
- *Hand-over.* A re-keyed copy drops its evacuation deadline (M3) and takes the new owner's kind (L3); an adoption
  sends the kind it has now. A car goes with its crew only when every seated person is crew or ambient and the game
  would remove it (`8244BC40`, M4). Special spawns and a mission's cars never move, also when a player drives them
  (M5). With `puppets = 1` an adopted fighter that was a puppet gets a fresh default behaviour too (its own brain
  never ran here; M7: chosen over declining the offer, so fights still change hands). A replaced copy always goes to
  the graveyard (L4).
- *Also.* The mission host's markers on a person or car follow that host's copy, not the session host's (M6). The
  rebuilt lock's generation steps 0x100 past the newest seen (L2). An inactive game whose evacuation is over keeps
  nothing alive for viewers (L1). The session host's relay passes every 6th pose (5 a second) to a player over 300 m
  from the pose's sender (shared only, M8). The player's own name is read once per session, not every 10 s (area L5);
  the registry never throws (area L7); the old-build notices say co-op 3.0 (protocol 11).
- *Log lines.* `Session (10 s):` gains `relayed here N messages, N KB/s (far players' poses held back N)` (printed
  whenever the relay passed anything on: 3 players or more) and `a link alone failed (a successor still hears the
  host) N`. `World (shared) 3: claims:` gains `owners held for this game's re-link N, timed out and held 8 s N, called
  off: the player came back N, copies faded for it N; claims pushed past a relay gap N; slots left out of offers 60 s
  (no answers) N`. `World (shared) 2b: spawn veto` gains `views gone stale N`. `Puppets (member)` shows with
  `puppets = 1` without `diagnostics`. `World models` counts people taken over with the copies.

**The clothing pool** (any mode; mod.ini `clothing_pool`). Research: notes/coop-3/clothing_memory.md,
clothing_impl_notes.md.
- *Why.* Each other player's character took a full set of clothing components, one per category (54, table
  `0x8310CF78`), in one 5.9 MB physical block per player-pool slot (`EnsureComponents`), on top of 16 MB for the 11
  body stream slots. On the PC the 4th other player's block failed (`Clothing memory: 5.9 MB block not available`),
  and a player in a 29-33-item outfit still came up short in a few categories: its pieces were "unloaded" and dressed
  again every 3 s. In single player any free component of a category goes to whoever asks (`8248D5F0` looks at the
  owner only with the MP byte set), so the sets were one shared pool per category anyway.
- *Demand-sized.* Right before an outfit goes on (the first dressing and every dressing again, `ClothingPlan`) each
  worn item's category is read the way the game wears it (`WornItemCategory`: `8248DE28`'s unpacking, `82318788` /
  `82312B10`'s item, substitution and piece lookup; the piece's category at +32) and the rings are grown in stages
  (`clothing.h` `GrowRings`; `82488A30`, linked in at the ring's tail as `8248DAA0` does, flag 0x20): this outfit's
  base layer, then the base-layer items of the other players here that hold no component, then the rest of the
  outfit, then a spare in each base-layer category; the last two only with room left after them for one more base
  layer. A stage waits until the ones before it are grown, so nobody's hat comes before someone else's pants. Every
  free component goes on (none is kept back). A category whose ring can't grow (`Max_equipped` 0) is planned like an unknown one:
  its item goes on as in 2.4. A character taken off (or removed past 130 m) frees its pieces in their rings for
  anyone. The base-layer items of every player in the session count, also one without a character here yet (its
  last outfit received); a hat never takes the component another player's base-layer hat needs; the rest leaves room
  for one more base layer per player whose outfit hasn't come yet, plus one. A buffer free again is used for any
  category it is big enough for. After the items went on and before the game's queue loads them, the record's nodes
  say where each item went (`ClothingAfterWear`): a category the plan didn't see coming gets its component then (outside
  the base layer only when the plan grew the rest too); a planned item without a node was dropped by the game.
- *After a session.* The game's cleanup (`8248DBB8`, at a mission start outside a session) takes every 0x20 piece
  away without looking whether it is worn. Co-op's pieces still in use (+132 0x10 / 0x40) or referenced (+20) lose
  0x20 first and stay (`KeepWornPieces`; with 2.4's sets too): the local player or the player copy can be wearing
  one. The buffers of the pieces the game took are used again by the next components (`ClothingReclaim`); the
  arenas, the components left in the rings and the body block are used again by the next session (an online session
  stopped, then a direct-IP one: `Co-op memory kept from earlier sessions: ...`). For 30 s after the last other
  character is gone, co-op's components that nothing uses go back the way the cleanup does it, and an arena none of
  whose buffers a component holds goes back to the game (`827166B0`, the game's XPhysicalFree): `Clothing pool after
  the session: N components and N arenas given back ...; kept for the next session: ...`. The 30 s open once per
  session (armed at the session start) and are never extended, also with a piece still worn; UpdateBody runs them
  once a second before its idle return, so they don't make the session-over block run every frame in single player.
  The game's own cleanup outside a session (each mission start) gets 7 s of the same (`Clothing pool after the
  game's cleanup: ...`, 10 lines). Only a component in its ring, free (+132 without 0x08 / 0x10 / 0x40 / 0x80: the
  game's release clears 0x08 and 0x80 with 0x10), with no wearer (+20 0), that no node of the 12 customization
  records points at, found so for 5 s, with the game's clothing queue idle; a piece still worn keeps its arena. The
  body block is kept (group 3's slots point at it). mod.ini `clothing_release = 0` turns all of this and the memory-low
  state below off: the pool keeps its memory until the game exits.
- *Memory.* The buffers (the category's `Slot_size`, 4 KB-aligned) are cut from 1 MB physical arenas (`82716618`,
  64 KB pages, first fit), not one block per player. No arena is made that leaves less than 12 MB physical memory
  free, nor one that would take the lowest reading of the last 5 min (read every 2 s; each reading counts the arenas
  as free, so arenas made since count against it) below 11 MB; the first 5 min, and a minute after a gap of more than
  10 s in the readings, 16 MB (`MmQueryStatistics`, thunk `0x82789394`; if it refuses, 24 MB of arenas at most).
  Below 8 MB free (or that lowest less the arenas now below 9 MB) memory is low until growth would be allowed again
  (12 MB free and that lowest less the arenas 11 MB; reset at a session start): no component is added, the farthest
  players lose what they wear outside the base layer (then, still below 8 MB, all their items if beyond 60 m, back
  once memory is), and the components nothing uses and empty arenas go back to the game. Nobody is shed while co-op
  holds no arena (the low is the game's own), nor after 30 s of shedding in which no arena went back (an arena freed
  starts the 30 s again; `clothing.h` `MayShed`). (The
  game itself swings 5-6 MB below the level the arenas were made at, and was seen running at 11 MB:
  TEST-3.0-bots12-findings.md.) On the PC that leaves about 6-10 MB of arenas: about 4 basic outfits, or 5 base
  layers (the second 12-bot test: about 6 dressed characters, host included). Dressing 11 others needs the body block
  (below) or the runtime's chunk2. A buffer given back merges with the free buffers right before and after it in the
  same arena (never across two: they go back to the game one by one), one at an arena's end goes back to its free
  tail, and a component takes only the bytes it needs from a bigger free buffer (`clothing.h` `SpareAdd` /
  `SpareTake`): a player's hat, glasses and gloves were cut one after another, so together they can hold a pair of
  pants.
- *The body block* (mod.ini `own_body`, `own_body_max`). Other players' own body models take 11 stream slots of
  ~1.46 MB in one 16 MB block (group 3, made at the first character with its own body), the biggest co-op allocation.
  With more than `own_body_max` players (6) in the session when the first character asks for its body (on the host
  of a session over half that size 5 s after, so the players joining together count: `BodySettles`), it isn't made: `Own body: off past N players (N in the session; ...),
  16.0 MB left for clothing: everyone here shows this game's body shape for this session`; the characters get the
  shared body at once, and the clothing pool sees the 16 MB as free memory. Held for the session; the next one decides
  again. Once made it stays, also when the session grows past `own_body_max` (`Own body: N players, past
  own_body_max N, but the 16.0 MB body block was made with N players in the session and stays`): group 3 loads each
  body model at base + slot x size (`8250D3E0`) and the characters here are drawn from it, co-op's references on
  them (`8250C750`, kind 5) are never released, and nothing in the game frees group 3's memory (its only writers are
  the static initialiser `8277A9E8` and multiplayer's `8247E0E0`), so freeing it would need every such character
  made again with the shared body and every slot unloaded first (not done: memory_fair_notes.md 2). So the first join
  decides: a host whose players come one by one makes it; a game that joins a session of 7 or more doesn't. A failed
  block no longer goes on to a group 3 load (its slots would have been cut from address 0).
- *Nearest first.* An item whose category has no free component is left off, never put on (no "No place to put
  streaming component model", nothing to dress again). While memory is short (a player was left without something
  in the last minute), players within 60 m (kept to 80 m) get their full outfit and farther ones the base layer: hair,
  undershirt, overshirt, underwear, pants, shoes and skin (an item the game drops because something was left off, the
  hair in its hat wear without the hat, makes the hat part of that outfit's base layer). Once a second
  `ClothingPoolTick` picks at most one outfit change (`clothing.h` `PickChange`): a player beyond 80 m wearing
  something outside its base layer that a nearer one (20 m nearer or more) lacks into the base layer (nobody nearer
  loses what it wears for someone else); then, nearest first, a player back into its full outfit or into what it
  lacked once a component is free or an arena may be made (a near one in the base layer at once). A far player goes back into its full outfit only when all it left off has a free
  component (or memory isn't short and an arena may be made); one that came up short then waits 1 min for the next
  try, doubled each time up to 8 min (no base / full flip-flop).
- *Fair sharing* (memory_fair_notes.md 1; the second 12-bot test: 6 players in full, hats and all, the 6 who came later
  bare). While a player here lacks part of its base layer and no new arena may be made: the co-op components nobody
  uses outside those categories go back (their buffers become free buffers any category can take), nobody gets
  anything outside the base layer (no way back into a full outfit, no retry for a hat, a first dressing puts on only
  the base layer), and only the players lacking base-layer items are dressed again, once one of them has a free
  component or a free buffer that holds it. When nothing is free, nothing fits and nothing given back is still on its
  way (5 s), one player's extras (hats, glasses ...) on co-op's components are taken off (into its base layer, never
  less: `clothing.h` `PickTake`): one off this screen before one on it, then the farthest, the nearest last; one
  every 5 s, one per player per 10 s; their memory then goes to the base layer. A component belongs to one category,
  so a hat taken off gives nobody pants: its buffer does. With room for an arena every plan grows the others' missing
  base layers first anyway (stage 2), so a base item missing for another reason never holds everyone's extras back.
  Once every base layer is on, the extras come back as before, the nearest first (also into a free buffer that holds
  them).
- *No dressing loop* (the 12-bot test: 11 players dressed again once a second, 50-90 ms each). The clothes check
  compares with what the slot listed right after the dressing (the game drops some items, and the slot's worn list
  never shrinks: `8248D198` leaves its entry), not the plan's count. With the pool only a new record is dressed again
  at once; a count change or unloaded pieces wait for 2.4's back-off, the player's 10 s, the session's 1 s gap and no
  other dressing under way (`clothing.h` `MayRedress`). A player its last dressing left short is dressed again
  (by the clothes check or the tick) only after memory changed: the memory epoch moves on when an arena is made,
  buffers come back, a character is removed, a dressing puts on less, a category someone lacks has more free
  components than a second before, or an arena may be made again (room for 2 MB after none; at most every 30 s, a
  later one waits). Pieces the game unloaded don't wait for the epoch (2.4's back-off). A dressing whose character is
  gone, or that started more than 20 s ago, holds nobody back. One dressing at a time: none starts while another player is between its take-off and its wear (its pieces
  are free in their rings for those frames; another plan took them and undressed Bot1).
- *Diagnostics* (log only). `ComponentPickHook` on `8248D5F0`: a request that found no component, counted per
  category, the first 30 logged with the model, the player slot, the ring and what the record lists in that category.
  A record listing two nodes in one category with the queue idle is logged once per record and category (20 lines).
- *Log lines.* `Clothing pool: started with N components ...`, `Clothing pool: arena N at X (1024 KB), physical free N
  MB before it`, `Clothing pool: no new arena (N MB physical free, lowest N MB in 5 min; N MB kept free, 11 at the
  lowest) ...`; per dressing `Player N clothing plan (full | base layer): N items, N go on (N not known here), N left
  off for the distance, N for memory (categories); grown: (how far: its base layer short of memory ... all of it and
  a spare) (N base-layer items of the others here held for them); N components added, pool ...`; `... clothing: worn
  but not planned: ...; planned but not worn: ...`; `... clothing: the game didn't put on ... now go with its base
  layer`; `... clothing: into the base layer ...` / `back into the full outfit` / `dressed again: free now: ...`;
  `Co-op memory kept from earlier sessions: body block N MB at X; N clothing arenas (...), N components free in their
  rings` at a session start; every 10 s `Clothing: N players fully dressed, N in the base layer, N short of memory, N
  undressed; pool N components added (MB in use in N arenas, N buffers spare), physical free N MB (lowest N in 5 min,
  room for N MB more); N arena refusals (N this window), N component objects refused; dressings N this window,
  dressing again held back N times (memory epoch N); no place N (categories); in use / in the ring: per category
  grown` (with `N with all items off for memory`, `N components and N arenas given back` and `MEMORY LOW, giving back`);
  `Clothing pool: memory low (...), giving memory back` / `memory back (...)`, `... clothing: into the base layer for
  memory` / `all items off for memory`, `Clothing pool: memory low, but co-op holds no arenas: nobody shed ...` /
  `... but 30 s of shedding gave no arena back ...`, `Clothing pool: arena at X (1024 KB) given back to the game`;
  fair sharing: `... clothing: extras off for the others' base layer (categories; N m, off screen; memory N MB physical
  free; lacking: ...)`, `dressed again: room (a spare, an arena's tail or a new arena) for ...`, `Clothing pool: base
  layer short (...), and nobody left wearing extras on co-op's memory to take` (once a minute), and in the 10 s line
  `N lacking part of the base layer, extras taken off N times for it (N this window)`; the body block: `Own body on up
  to N players ...` at start-up, `Own body: off past N players ...`, `Own body: N players, past own_body_max ...`.
- *Tests.* `tests/clothing_test.cpp` (`clothing.h`): arenas (first fit, alignment, give-back, the 16 MB floor of the
  first 5 min, 12 MB / 11 MB at the lowest after it, the low water and its gaps, arenas made since the low, the 24 MB
  cap when free memory isn't known, memory low and when it clears, an arena given back only when unused, spares across
  categories, the headroom, a reserved base-layer hat), the shed (nobody within 60 m stripped, `MayShed`), tiers and
  hysteresis, what an outfit wants, every free component usable, the base layer (no eyes), which items fit (the base
  layer first, never one without a component, a ring that can't grow), the worn-list filter, `PickChange` (nobody
  within 80 m undressed for another, a far one only for what a nearer one lacks, one per player per 10 s, no change
  when nothing is free, a far player's upgrade only when all of it fits, the upgrade hold, no retry before memory
  changed), the stages (base layers for everyone before a hat, the rest only with room for one more base layer, a
  spare only for the base layer), `Promote` (the hat-wear hair; re-planned at once), the retry with room and no
  free component, and `MayRedress`; free buffers split and merged (`SpareTake` / `SpareAdd`, never across two
  arenas, the tail back to its arena, `BufferRoom`), fair sharing (`MayTakeExtras`, `PickTake`: off the screen first,
  the nearest last, only co-op's components, the 10 s; `PickChange` with a base layer short: only its retry, no hat
  back, no rule 1, a shed player back into its base layer; one take, its memory back, the pants on, nothing more
  taken), and `DecideBodyBlock` (the 5 s, off past `own_body_max` for the session, kept once made, 0 = no limit).

**The fades fix** (any mode; `view_holds = 1`, below). The 3.0 test f77fa75c (area, `puppets = 1`): people and cars faded out on
the member's screen right in front of it. Every one was the owner's own game removing its object for the owner's
camera (each removal a WWR1 the member faded over 1 s wherever it looked), and with puppets the fading people froze
mid-stride. Research: TEST-3.0-f77fa75c-findings.md; notes/coop-3/fades_fix_notes.md.
- *A fading puppet stands.* `WorldFade` turns the puppet off (its brain runs again: `PuppetBrainHook` leaves a fading
  copy alone) and stops it where it stands whether or not `moving` is set (`Place` clears it every update, so the
  stop never came: "fading people stopped" went from 433 to 2).
- *The owner holds what a member sees.* A removal the owner's game asks for again and again is refused while a member
  (shared: an active viewer) shows the object (its offer created) and may see it (`cw::SeenFrom`: within 30 m of its
  eye, or in front of it within 60 degrees and the interest + 20 m), for 20 s at most (`cw::HoldAsk`; a path that
  leaves it alone 2 s starts a new hold, but not within 30 s of the last one's end, `cw::kHoldCoolMs`, so the
  housekeeping can't hold one car in 20 s slices for ever). While the object is being handed over to a member
  (`HandBusy`) it is held and its 20 s stand still, until the hand-over's answer or its own time-out:
  - the off-screen cull `8241C498` (off the owner's screen, past 20 m, its model slot doomed): its destroy from LR
    `8241C680` is refused in `HumanDestroyHook` (people) and `VehicleRemoveHook` (the car of any occupant); so is its
    car pass `8241C7F8` (the same loop over the car lists: an empty, parked or swapped-model car; LR `8241C90C`,
    "off-screen car cull"); the doomed slot is freed once the hold is over. The game's own (priority < 5) model swaps
    wait up to 20 s per held object's doomed slot meanwhile; the mod's own requests (priority 5) never do;
  - the traffic housekeeping `82410B80` (its oldest cars deleted off its screen, else faded): its pick `824108D0` (new
    hook, `TrafficOldPickHook`; its only caller). In its at-once call (off the owner's screen) the pick is asked again
    with a held pick moved behind the count, so the game takes another car; none left: -1, as when it finds none. In
    its fade call (every other candidate is on the owner's screen) a held pick gives -1: no car this pass (traffic
    one over until the next pass, 0.4-0.6 s on), never a fade the owner would watch instead.
  - The game's limits stay first (`WorldHoldRoom`, at a hold's start): at most 8 people and 4 cars held, none for a
    person whose category is at its limit (ambient 26, traffic crews 20) or with the people pool 2 from full, none for
    a car while the traffic list has 12; in shared mode none within 2 of the other games' copies' room either
    (`WorldCopyRoom(false)` for people, `WorldCarRoom` for cars). The make-room path and the allocator's evictions are
    unchanged, so the owner's own spawning still finds room. The distance despawns keep 2.4's rings (area) or
    `WorldKeepShared` (shared): a separate keep on separate paths, so in shared mode one object can be kept 20 s by
    each.
- *The member fades only what it sees.* `WorldFade`: a copy off this screen (the game's own frustum test `823FE380`,
  as its traffic removal uses) or past the copies' interest + 20 m (people 120 m, cars 170 m) goes at once with no
  fade, as the game removes its own traffic out of view; one on the screen fades as before at any distance it can
  have a copy (a 60 m cut-off popped people 60-120 m ahead). This applies in area mode with `puppets = 0` too: it is
  a change on every member, not only with puppets (by design: the owner's removals faded wherever the member looked).
- *Standing copies off their spot.* A puppet whose state isn't in its anim set (2.4's move orders, its brain off) is
  stopped where it stands up to 25 m off its path (it was ordered to walk to the path point, which nothing carried
  out: copies stood 3-10 m off for 30-60 s), its running order cancelled (`82441308(h, 0, 0)`; no new order to
  where it stands, which would never run either), held standing from up to 6 m, and `SmoothWorldPeople` steps a held
  puppet back from up to 6 m (2 m before): 0.35 m a frame per 6 m of gap over 1 m (within 1 m in about half a
  second, not a 2-4 s slide in its idle pose), 5 cm a frame under 1 m. Over 1 m off and off this screen
  (`823FE380`) it is put on its spot at once. One standing 6-25 m off is left to `Drive`'s placing, counted and
  logged (`Puppet H standing D m off its path: A m after Drive's placing (state +516 N, +508 N)`, 20 lines) to settle
  whether `MoveHuman` places a brain-off copy at all.
- *Log lines.* Owner, with members: `World (owner) removals: people: <path> N (on a member N: in its view N, within 30
  m N), ...; cars: ...; held for a member's view (...): people now N (begun N, their 20 s over N, not held at the
  game's limits N, asks held for a hand-over N), cars ... (...; housekeeping passes that removed none N)`, the paths
  by the destroy's return address or the deciding hook (a hook's hint counts only within 3 s): off-screen cull,
  off-screen car cull, model unload (`8244BCE8`), make room, distance despawn, traffic housekeeping, region check,
  corpse clean-up, chunk clear, other. Once per name: `World (owner): story character NAME (person H, kind K) left out of the stream (host-only,
  2.1's rule), D m from slot S's camera` (log only, to decide which to stream). Member: `World (member) removes by
  this camera: cars faded in view N, at once behind N / far N; people ...`; `Puppets (member)` gains `stopped 1-25 m
  off the path N, pulled back standing: put back unseen N; standing 6-25 m off N, still 6 m off after Drive N`.
- *Area mode with `puppets = 0`* is not 2.4 plus the hold: the member's at-once removal off the screen (above) and the
  player list layout (`IN CAR` / `ON FOOT`; 2.4's `DOWN` is gone) apply there too. Both are intended. Fixes 1 and 5
  (the fading puppet, the standing puppets) are puppet-only.
- *Tests.* world_test: `SeenFrom`, `HoldAsk` (20 s whatever the asks, then none until a 2 s gap and 30 s after its
  end; the housekeeping's re-arm), `SightOf`.

**The fades fix, round 2** (any mode; `view_holds = 1`, below). The 3.0 test 6d607dde (area, `puppets = 1`, a police level 5
fight): the member faded 160 people in view in 6.9 min, about half of them living people on foot; the biggest owner
path was unnamed ("other"), the cull's people holds never began (refused at the ambient category's limit, where the
cull always runs), and the 2.1 story-character rule left most of the SWAT out of the stream. Research:
TEST-3.0-6d607dde-findings.md; notes/coop-3/fades_fix2_notes.md.
- *The removal paths named.* New hooks on the fades themselves, `824517A0` (a person) and `82537D40` (a car): for
  this game's own objects the caller names the path the owner counts when it sees the fade (`WorldFadePath`): the
  distance despawns (`8240FBB0`, the traffic update `824129C8`), the housekeeping, "corpse fade" (the corpses' own
  timer, `82476FA8`), "wreck clean-up" (`8253F9D8`, the 7th wreck's `82568908`), "the car's own update"
  (`8253E938`) and "script" (`team_despawn` `824DD0B0`, a group's cars `8256B428`, `824C4B58`'s script functions).
  What no path names is split by the object: a person in a car or taken by its car's removal (`825324D0`'s seat
  passes) is "a removed car's crew", a dead one "dead (no named path)", a wrecked car's fade "wreck clean-up"; a fade
  from another caller is "a fade the game began (other caller)". The rest stay "other", and those and the unnamed
  fades are counted by return address (16 a window, the 8 most in a new line).
- *Holds at the category's limit.* `WorldHoldRoom`: a person whose spawn category is at its limit is still held, up to
  `cw::kHoldAtLimit` (4) of that category at once (the cull only runs on a full street: a model swap into it); the
  pool test (2 from full), the caps (8 people, 4 cars) and the shared mode's margin are unchanged.
- *The distance despawn in a member's view* (area mode). Past `PeopleDespawnHook`'s 90 m ring, a person a member shows
  and may see (`cw::SeenFrom`: in front within 120 m, or within 30 m) is held as the cull's are (`WorldHoldSeen`: 20
  s at most, the same limits and cool-down), at most `cw::kHoldFar` (4) of the 8 at once, so the cull's in-view
  holds near the members keep the rest. Shared mode keeps `WorldKeepShared`.
- *Dead bodies linger on the member.* `WorldFade`: a dead copy (not seated) removed while on this screen isn't faded:
  it lies on (no record moves a fading copy) until it is off this screen, then goes at once, or `cw::kCorpseLingerMs`
  (30 s) pass, then it fades; `cw::kCorpseLinger` (4: the game's corpse manager keeps 5 bodies) at once at most, the
  rest fade as before (`WorldLingerOver` each apply pass; a session's end fades one still on the screen). Chosen over
  holding the owner's corpse clean-up `824793C0`: the owner's game and its pools stay as they are, and a corpse can't
  desync. On the member, `CorpseCleanHook` keeps a lingering body from that game's own corpse clean-up until its 30 s
  are over (the corpse manager `82476FA8` sets its fade bits on the body's timer, or past 5 bodies), with the same
  loop guard as a kept dead copy.
- *Story characters* (2.1's rule, `kWhyNamed`): the name test ignores case (`lw_w_m_swat01` counted as one), and the
  rule never takes response units, crews or spawn-region people: SWAT and police stream.
- *Dead copies' positions.* A corpse lying with its ragdoll slot gone to a newer fall (`+784` -1: the game has 12)
  is put on the owner's corpse once (the teleport `8243E6F8`), when the owner's lies still, it is over 2 m off and
  off this screen. The sync checker counts a person dead or in its ragdoll on the owner apart (`position: dead or in
  its ragdoll`, its line with the copy's `lying` and `slot`).
- *Log lines.* Owner: `World (owner) removals` gains the new paths and `begun N: at the category's limit N, from the
  distance despawn N (refused, 4 of them held: N)`, `in a hold's 30 s cool-down N` (people) and `cooling down N` (cars); new `World (owner)
  removals by return address (other, and fades the game began from an unnamed caller): [fade from ]LR people N (in
  view N) / cars N (N), ...; more N`. Member: new `World (member) people removed in view: living on foot N, dead N,
  lying N, seated N; past 60 m N, past 90 m N; dead bodies left lying N (then off the screen, gone at once N; faded
  after 30 s N; taken by this game first N, its removal let through N; faded at once, 4 already lying: N)`; with
  diagnostics, `Corpses (member)` counts the clean-up's runs skipped for lingering bodies; `Deaths and wrecks` ends with `corpses with no ragdoll slot put
  on the owner's unseen N`.
- *Tests.* world_test: the despawn hold's cone (90-120 m ahead, not behind, not past 120 m) and the new constants.

**The overnight review's fixes** (coop3-17; review_overnight.md, notes/coop-3/overnight_fixes_notes.md).
- *The session-end clothing release arms again* (M1). A Host / Join press stops the last session first, and that
  Stop's clean-up ran `ClothingSessionEnd` while nobody was connected yet: the new session's arm was used up before
  anyone joined, and after the session nothing went back. The clean-up now ends the clothing session only when the
  session stopped (`!running`), not on a lost link; a host whose clients all left keeps its arm until its own Stop.
- *`ClothingSessionStart` on the game thread* (L1). It ran on the frame thread inside `StartSession`, where the game's
  own cleanup (`StripRemotePiecesHook`, the guest thread) could change the same vectors. The press now sets a flag,
  and the next `UpdateBody` (with `game_mutex`, on the game thread) runs it.
- *A car back in a member's interest keeps its damage* (M2). The pass-1 fix forgets a car's parts on the member when
  a create comes with no copy held (a new car in a reused handle), and that also hit the same car made again after
  leaving the member's interest: its dents, panels, glass and tyres were gone until it was hit again. The owner now
  sends a broken car's parts in full (`SendCarParts(key, v, true)`, broadcast) 1 s after it queues a create of it for
  any member (`CarPartsCreated`, `kCarPartsAfterCreateMs`), and a member keeps parts taken in the last 5 s at a create
  (`kCarPartsFreshMs`: they are that car's, whichever came first). The NetId the review suggested can't tell the two
  apart: its nonce is the origin's per session, so a reused handle gets the same NetId.
- *The view holds can be turned off, and pause* (M3). `view_holds = 0` (mod.ini) turns off what the two fades fixes
  hold or change on purpose: `WorldHoldSeen` (the cull, the car cull, the housekeeping pick, the far despawn hold) holds
  nothing, and on the member a copy off the screen fades as before and a dead body isn't left lying. The rest of the
  fades fixes (the paths named, the story-character rule, the corpse put-back, the puppet stop) stay. With holds on,
  none begins or goes on during a cutscene (here, or `host_cutscene`) or for 10 s after `MissionStartHook`
  (`cw::HoldsPaused`, `kViewHoldQuietMs`): those loads may wait on a held object's doomed slot. Counted in `World
  (owner) removals` (`asks not held for a cutscene or a mission start: people N, cars N`).
- *The pool's own changes every 5 s* (M4). A change `ClothingPoolTick` begins (rule 1 into the base layer, a retry, an
  upgrade, a shed) waits for `clothing_next_pool` (the last one + `coopclothing::kPoolGapMs`, 5 s) as well as the 1 s
  gap (`PoolChangeDue`). A new record and a first dressing keep the 1 s gap, so a player who joins is dressed as
  before. Items are taken from a player off this screen first (`View::on_screen`, the game's frustum test `823FE380`;
  `TakeFirst` in `PickChange` rule 1 and `PickShed`), then the farthest. Rule 2 (giving items back) stays nearest
  first.
- *Lows.* L2: `world_seen_hold` loses a handle in `WorldObjectGone` and is cleared in `WorldRegistryOff`
  (`WorldSeenHoldForget`). L3: a lingering body counts toward the member's people cap. L4: the dressing lines (`Dress
  slot`, `clothing: N bytes`, `dressed again`, `clothing lost`) at most 40 a minute (`DressLog`, the rest counted in the
  next). L6: the `player_list` comment in mod.ini. L7: `clothing_pool = 0` is 2.4's clothing plus the worn-piece guard
  (`KeepWornPieces` runs after a session that made 2.4 sets too: the game's cleanup outside a session keeps 2.4 pieces
  still worn or referenced instead of putting them on its free list). L8: `Clothing pool: arena N at ...` and
  `memory low` / `memory back` at most 50 a process. L5 (the item-category cache kept across sessions) is not done:
  the cost is one-off, and the session-start clear is the safe side if the item table can change with the same count
  (not checked).
- *Tests.* All three run after each change: `tests/net_test.cpp`, `tests/world_test.cpp` and
  `tests/clothing_test.cpp` (`clang++ -std=c++20 -O1 -I core/WhompaysCoop core/WhompaysCoop/tests/<name>.cpp -lws2_32`;
  exit code 0 = passed). world_test: `HoldsPaused`. clothing_test: `PoolChangeDue`, and `PickChange` / `PickShed`
  taking from a player off the screen first.

**Players' vaults and climbs** (any mode; mod.ini `climb_sync`, no protocol change). Research:
notes/coop-3/vault_climb_research.md; notes/coop-3/vault_audio_impl_notes.md. In the 3.0 f2534683 test another
player's character slid over a wall in its run pose. Their 5/x climb states were replayed on a character with no climb
record of its own (its own ClimbStart is blocked), so 824623C8 dropped it to 1/0 on its next update, while the jump's
3 s hold (`jumping`, ended only by 4/E) kept it placed on their path in steps.
- *Phase 1, the receiver's holds.* A 5/x on the character is never played (`climb_states_left`). Without a replayed
  climb it starts the fallback (`climb_anim_only`): their climb's actions (0x218, 0x217, 0x213, 0x219) play while it
  follows their path, `GameFrame` smoothing it each frame (no longer skipped for this hold), until their 6/E or 0/0
  (or 1/x after their drop), 2.5 s at most. A jump's hold also ends on 6/E and 0/0 (`jump holds ended`), and Drive's
  ">2 m/s path cancels a hold" rule leaves climbs alone (a vault is done at a run).
- *Phase 2, the climb replayed* (`kEventClimb = 50`, kinds up to 49 are taken; an older build ignores the kind:
  `ApplyEvents` tests kinds one by one). `ClimbStartHook` for our player (`82444270` returning 1): the position just
  before, the climb point, `PackDir` of the frame's row 2, `coopworld::PackClimbInfo` (flag, state / sub before, the
  new sub-state, the caller from the return address: `8216AC90` ground, `823F6BB0` jump grab, `824634A4` fall grab)
  and the yaw. The receiver holds it by the follower's delay like the states (`ReplayClimb`): within 3 m of the start
  the character is put there (`MoveHuman`, rows from the yaw with its own handedness, `coopworld::ClimbRows`), then
  under `replicated_depth` the game's own finder `8234A1C8(point, frame, &flag, human)` (as the game's MP apply
  `823AFA88` calls it) and, when its ledge is theirs (`coopworld::ClimbLedgeMatches`: within 0.5 m, facing within
  ~37 degrees), `82444270(character, point, frame, flag, this game's ledge)`. On 1: `climb_replayed`. Drive leaves it
  alone up to 4 m from the path, `GameFrame` skips it, and their 0 / 1 / 5 / 6 states and actions 0x20B-0x219, 0x2A,
  0x2E are not played (`climb_actions_left`): its own 824623C8 / 82463100 play them. `UpdateClimbHold` hands it back
  (`coopworld::ClimbHoldStep`) once the character is out of 5 / 6 (past 150 ms; M3 below), 400 ms after their end, or
  at 2.5 s; placed on the path when more than 1.5 m off (`snapped`). Watchdog: out of state 5 within 150 ms, the
  fallback. No ledge, another ledge, refused or too far: the fallback too. A second `kEventClimb` within 300 ms is
  ignored. Lines: `Climb sent: ...`, `Climb on Player N: replayed / no ledge here / another ledge / refused / too far /
  aborted`, `Climb on Player N ended after N ms: ...` (40 each side), the sender's states and actions for 2.5 s after
  its climb (`Climb: our player sends ...`), and a `Climbs:` line with the Perf line.

**Sound after cutscenes** (any mode; mod.ini `audio_repair`). Research: notes/coop-3/audio_cutscene_research.md. The
player report: "audio seems to disappear and randomly come back after cutscenes". A joiner's unlock popup (state
enter `822A7800`, after a mission end) pauses the world without sound (`8220C778(0)`, from `822A784C`) and then the
sound on its own (`821007C0(500)`); its exit's unpause (`8220C868`) takes the sound count `0x8370CF84` down only on
the world counter's 1 -> 0. PauseHook skipped the world pause, so the count stayed at 1 until the next cutscene's end.
- *The root fix.* A skipped pause from `822A784C` or with r3 = 0 (only `822A7800` passes 0) owes a sound pause
  (`unlock_sound_owed`). `UnpauseHook` (`8220C868`): an unpause about to do nothing (world counter 0) with one owed
  pays it back after the original, the game's way: `8220C778(0)` then `8220C868` under `pause_passthrough` (the
  world counter 0 -> 1 -> 0 in one update; not `821F73A8`, which also takes one off the s16 `0x83710230`).
  `Unlock screen: its sound pause handed back (sound count N -> N, N owed)`.
- *The watchdog, on every game (the session host too).* `UpdateSoundWatch` logs each change with the reasons that
  hold (200 a session): `Sound pause count now N (was N): pause, cutscene D991/D990/D99C step D998, loading, video,
  death, menu, owed, last skipped pause`. The game's reasons (`SoundReasons`): the world pause counter
  `0x8370DAE4`, a cutscene (`0x8370D991` / `D990`, `D99C`, a deferred step at `D998`), the loading counter
  (big-endian at the unaligned `0x8370DB34`), a video (`0x8370D9B2`), the death / busted sequence (`0x840B5E00`), the
  pause menu or game state 12 on the state stack (its enter has its own sound pause), an owed popup pause (60 s at
  most). Above 0 with none for 5 s (`coopworld::SoundStuck`): repaired with the same pair, up to 8 times.
- *Safe points* (the same test without the 5 s): `CutsceneEndHook` after the original, the end of
  `ReleaseRemotePause`, and the session's end (`SoundSessionOver`, which pays any owed pause first and logs
  `Session over: sound pause count N -> N. Sound: N changes, max count N, N unlock pauses handed back, N watchdog
  repairs, N cutscene ends held for their begin`).
- *Co-op's forced cutscene ends* (`821F9700` in `UpdateSharedCutscene`, the HUD's mission end, the mission host left)
  wait while the start's deferred begin is pending (`0x8370D998 == 821F7640`, or `0x8370D990` still 0), up to 1 s
  (`CutsceneEndNow`, `coopworld::CutsceneEndWaits`): an end in that window left the start's sound pause on, and the
  begin still ran after it. `Cutscene end held: its start's begin is still pending`.
- *Tests.* world_test: `PackClimbInfo`, `ClimbCallerOf`, `ClimbLedgeMatches`, `ClimbRows`, `ClimbDropsState` /
  `ClimbDropsAction`, `ClimbOwnerEnds`, `ClimbHoldStep`, `SoundStuck`, `CutsceneEndWaits`.

**The memory-fair / climb / audio review's fixes** (coop3-21; review_memfair_vault_audio.md; the progress logs in
notes/coop-3/memory_fair_notes.md and vault_audio_impl_notes.md, "Review fixes").
- *M1.* Fair sharing gives co-op's free components back only with `clothing_release = 1` (with 0 no memory goes back
  until exit, and so no extras are taken off either: their memory would stay with their components), and never an
  arena: `ClothingGiveBack(..., free_arenas = false)` turns the buffers into spares and keeps the arenas for them.
  Arenas go back in memory low and after the session only, as before (no free / re-allocate churn at the floor).
- *M2.* A component's idle time must be seen at every look (`clothing.h` `IdleWatch`: `IdleSeen`, `IdleBusy`,
  `IdleLongEnough`): a look missed for over 1.5 s (its category left out of a partial give-back, a busy clothing
  queue) starts the 5 s again, so a piece worn and taken off meanwhile never goes back on a stale start.
- *M3.* A replayed climb is handed back once the character's own state is out of 5 and 6 after the first 150 ms,
  whatever the owner sent (a climb that ends on top sends 5/x -> 1/0 with no drop: it was held 2.5 s). The owner's
  5/x and drop still to come are the climb's tail (`coopworld::ClimbTailStep`): not played and no fallback started,
  until the owner's end or the climb's 2.5 s.
- *M4.* A `kEventClimb` arriving while the character is still in its own state 5 ends the hold and takes the
  fallback (`still climbing` in the `Climbs:` line): `82444270` is never called on a climbing character (no caller
  in the game does). State 6 is fine (`82463100` grabs from it).
- *M5.* The body block's decision waits 5 s only on the session host with 3 or more players and over half of
  `own_body_max` (`clothing.h` `BodySettles`); a 2-player session and a joiner (it gets the whole player list at
  once) decide at once. A character's 8 s wait for its body starts only once the block is decided, and a failed block
  counts as held off (`OwnBodyHeldOff`): the characters after it get the shared body at once.
- *Lows.* L1: `clothing_base_pressure` cleared in memory low. L2: fair sharing keys on an arena that can really be
  made (`room` and not refused by `82716618`). L3: a component idle for 15 s or more no longer counts as freeing
  (`IdleFreeing`: one that can't be unlinked never holds the takes). L5: the path 4 m or more ahead during a replayed
  climb ends the hold at the next update (placed on the path) instead of the path's moves fighting the climb. L6: an
  event with bad numbers no longer drops a jump's hold. L9: `CutsceneEndNow` holds nothing with `audio_repair = 0`.
  L12: the cutscene end's safe point only repairs a count with no reason; the owed popup pauses are left to
  `UnpauseHook` and the watchdog (not paid inside `821F9700`). L13: the sound watchdog runs 30 s past the session's
  end (also with co-op stopped), so a count left above 0 by a session that ended during a reason is repaired. L14:
  mod.ini `climb_sync`. Not changed: L4 (a take may hit a donor wearing the game's own extras; bounded by the donors),
  L7 (the opening action played by the fallback and again by the replay comes in the same pass: no frame drawn in
  between), L10 (past 60 s an owed popup pause is no longer a reason: the world keeps running in co-op, so the
  watchdog unmuting a popup left open that long is wanted), L11 (any no-op unpause while the popup is open repays a
  little early; the count stays right).
- *Tests.* clothing_test: `DecideBodyBlock` with the settle flag, `BodySettles`, `IdleWatch` (continuous, a missed or
  busy look starts again, freeing for under 15 s). world_test: `ClimbHoldStep` (out of 5 / 6 after 150 ms hands
  back), a climb ending on top (5/5 -> 5/9 -> 1/0, no 6), `ClimbTailStep`.

**Pose bandwidth, protocol 12** (any mode; mod.ini `pose_tiers`, `send_pacing_ms`). Research:
notes/coop-3/pose_bandwidth_design.md; notes/coop-3/bandwidth_impl_notes.md. The 12-bot test relayed 1.08-1.10 MiB/s:
the 392 B pose was mostly a model name, a look block and car fields that never change, and worn items, sliders and
colours went to everyone every second. `net.h` `kVersion` 12: 3.0 builds before this (11) are refused at join.
- *The compact pose* (`WPP2`, world.h `coopworld::PosePacket`, 48 B; + `PoseCar`, 27 B, in a car): sequence, sender
  time, the float position at offset 12 (the relay reads it), rows / camera as `QRot` (smallest three), the eye and
  the car's position as `QPos` 2 cm offsets from the player, car velocity `QVel`, spin `QAng`, flags as before, a form
  byte and the sender's appearance version. Mirrored rows (the right row negated, the `kPedSign` convention: det < 0)
  are negated into a rotation and flagged, for the character, the camera and the car. An eye or car over 655 m from
  the player goes as none / at the player. `EncodePose` / `DecodePose`; the receiver takes exactly 48 B, or 75 with
  the car flag. The game side keeps `PlayerPose`: only `SendDue` and `OnGameMessage` changed. The pose goes on a 33 ms
  schedule (at most 30.3 a second; it was `now + 33`).
- *The appearance* (`WAP1` on the free type 23, `kMsgAppearance`: version, model, look; 21 B for `StyleTest_PC`):
  reliable to everyone when the model or look changes (its version +1, never 0), reliable to each player who joins
  (`kPeerJoined`) and to everyone after this game's own join or re-link (`kJoinOk`, `kRejoined`), unreliable every
  5 s. The receiver keeps it in `remote_in[o].pose.model / look` (where the game side reads it). A character is made
  only once their WAP1 is here (and is the version their pose names), up to 3 s (`Player N: no appearance ... after
  3 s`), then from what is here (`kFallbackModel` when nothing is).
- *Worn items, sliders, colours* the same way (reliable on a change, at most every 250 ms per kind; to joiners; every
  5 s unreliable; they were every second). Notoriety unchanged. *Car looks* (`WVL1`): reliable when one is new or
  changed and to a joiner, one a second unreliable, cycling (it was 3 every 100 ms); `PruneCarLooks` drops cars that
  are gone or no longer this game's (the set never lost one: ~10 KiB/s per player who had driven 3 cars).
- *The relay's tiers* (`RelayPoseFilter` = `coopworld::RelayTiers`, both world modes, the session host's own pose
  too: `Session::Send` asks the filter per link for its own unreliable messages, and every client's unreliable
  message is noted once with target `kEveryone`, so a lone client's position is known). By the 2D distance from the
  pose to the target's last pose through the relay: every pose within 60 m, 10 a second to 170 m, 5 to 500 m, 3
  beyond, 10 m hysteresis, by the sender's clock (a pose passes when the tier's interval less 8 ms passed since the
  last one passed to that target). At once at any tier: a change of the state bits (in the car, shared, active,
  ragdoll, dead, host world, world stream / evacuate: `kPoseStateBits`, asserted against `kPose*`), a car entered or
  left, a jump over 40 m; a car faster than 20 m/s goes one tier nearer. A pose older than the last one passed is held
  (the receiver drops it anyway); over 2 s back the sender restarted. Reset at `kHostMoved` (a new relay passes
  everything until it knows both positions), a slot forgotten at `kPeerJoined` / `kPeerLeft`.
- *Receivers.* `Follower::jump_up` (the players' characters and cars): two sender gaps in a row over the delay less
  20 ms raise the delay at once (gap + half the spread + 20, capped), so a move to a slower tier doesn't run the path
  past its newest record. `FollowVehicle`: base delay 30, sampled at `max(30, delay - 40)` behind the newest pose (30
  ms as before at 30 a second; at 10 / 5 a second the fixed 30 ran up to 170 ms past the data and snapped).
- *Send pacing* (`Session::PaceUnreliable`): a link's packets with only unreliable messages go at most once per
  `send_pacing_ms` (10); a reliable message due (the oldest's resend, the newest unsent) goes at once.
- *Lines.* `Bandwidth (10 s): out N KB/s (N packets/s, headers included; pacing N ms), in N KB/s; pose tiers (to 60 /
  170 / 500 m / past): passed ..., held back ..., at once N; poses refused N, WAP1 refused N, taken N, older ones
  dropped N; camera not sent N (last det X), with no eye N; own by type: ...; relayed by type: ...` on every connected
  game (the order since the review's L12) (net.h `Stats::sent_type` / `relayed_type`). The Session line's
  "far players' poses held back" counts the tiers' holds.
- *Measured* (coop_bot `--bench`, fake host and bots on 127.0.0.1, 30.3 poses a second each, KiB/s; host out is
  `bytes_out` with the 16 B packet headers and the fake host's own small world stream):

  | players, case | relayed before | relayed after | host out before | host out after | a bot in before | after |
  |---|---|---|---|---|---|---|
  | 4, together | 76.7 | 11.7 | 145 | 47 | 48 | 16 |
  | 8, together | 537 | 82 | 703 | 164 | 101 | 23 |
  | 12, together | 1406 | 214 | 1679 | 343 | 153 | 31 |
  | 12, spread 20 / 100 / 300 / 700 m (area) | 1404 | 67 | 1634 | 140 | 149 | 13 |
  | 12, together, all in cars (75 B) | | 302 | | 432 | | 39 |
- *Tests.* world_test: the WPP2 codec (on foot, mirrored rows / camera, no camera / eye, a far eye, a car, a far
  car, malformed), WAP1, `RelayTiers` (30 / 10 / 5 / 3 Hz at 20 / 100 / 300 / 700 m, flips / car / jump at once, the
  fast car, hysteresis, unknown target, `Forget`). net_test 18: the tiers through a Session (the host's own pose and
  relayed ones), a state flip, `PaceUnreliable`. coop_bot `--selftest`: WPP2 / WAP1 both ways, every WAP1 within 1 s
  of the first pose, the 5 s refreshes, the tiers at 20 / 100 / 300 / 700 m with a car bot.

**The bandwidth review's fixes** (coop3-22; review_bandwidth.md; notes/coop-3/bandwidth_impl_notes.md "Review
fixes"). No wire change (protocol 12 as above); the sender's rules and the receivers changed.
- *M1.* A character waits for the WAP1 its pose names up to 9 s (3 s while the pose names version 0, none sent yet),
  then is made with what is here. One made so (or whose model changed since) is made again as soon as a WAP1 brings
  another model (`Remote::made_model`, through `force_remake`, at most every 5 s: `Player N: their appearance brings
  model '...' (the character was made with '...'): made again`).
- *M2.* The sender orthonormalises the camera (`coopworld::CamUsable`: Gram-Schmidt from the forward row, then up;
  right = up x forward, negated when mirrored) and refuses only a degenerate one (a row under 0.1 long or |det| under
  0.1; it was |det| 0.8-1.25 untried against the real block). No camera goes without the eye (an eye over 655 m from
  the player sent "none", and the receiver wrote eye 0,0,0 into +4112). Counted in the Bandwidth line (`camera not
  sent N (last det X), with no eye N`); one `Camera sample: det X, rows ... long` per session.
- *Lows.* L1: a WAP1 1-8 versions older than the one held is dropped (`coopworld::AppearanceStale`; `older ones
  dropped N`), and a change goes reliably only 30 ms after its kind's last unreliable refresh (the refresh could still
  sit in the paced queue while the change went first). L2: rows that aren't a rotation (|det| under 0.5: all zero for a
  matrix not set yet) go as identity, not a 180-degree roll. L3: `UnqRot` renormalises (a malformed packet's three
  over-long components). L4: a seat of 8 or more, or an owner slot of 12 or more, goes on foot (no car tail), not as
  the driver of slot 0's car. L5: in a car, another seat, car handle or car type passes the relay at once. L6: the
  fast-car rule has hysteresis per pair (one tier nearer over 22 m/s, back under 18). L8: the appearance wait starts
  again when the player leaves the 60 m or their WAP1 version changes. L9: `Follower::jump_up` needs three long gaps
  in a row once the delay is 90 ms or more (two lost poses one apart at 10 a second raised it to ~240 ms for 1.5 s).
  L11: the join texts name the release (3.0 of 2026-10-24 or newer; an earlier 3.0 is protocol 11). L12: the
  Bandwidth line puts the counts before the type lists (2048 B). Not changed: L7 (T3 stale views, noted), L10 (pacing,
  documented), L13 (perf, microseconds), L14 (coop_bot, test tool).
- *Tests.* world_test: `AppearanceStale`, a scaled / skewed camera sent orthonormalised, a degenerate one refused, a
  far eye with no camera, all-zero rows, a malformed rotation, seat 9 / owner 12 on foot, a seat swap / car handle /
  car type at once at 700 m, the fast-car hysteresis. coop_bot `--selftest` passes (it builds on world.h / net.h).

**Memory step 1: the body block of K slots** (mod.ini `own_body_slots`, default 2; research:
notes/coop-3/memory_plan_12.md section 2, notes/coop-3/memory_steps_impl_notes.md).
- *What.* At the first character with its own body, group 3 (the multiplayer player bodies, `kStreamTable + 3 x 204`
  = `0x83E87914`) gets K load slots, not the 11 it was built with at boot, and a block of K x 1.46 MB (1,528,320 B
  each), not 16 MB: (11 - K) x 1.46 MB stay free for clothing whatever the join order (K = 2: 13.1 MB). The first K
  other players' pool indices get their own body shape; the next one gets this game's at once (no 8 s wait:
  `OwnBodyFull`). A body model no character here is drawn with for 5 s gives back its reference (`8250CA48(group, id,
  5, 0, 0)`, `OwnBodyRelease`); the model stays as a cache and the pass can load the next player's into its slot.
- *How (H, the recompiled game).* The pass `8250D3E0` takes free slots from the group's +60 queue and loads into
  `+36 + index x +32`; the boot reset had queued all 11, so writing +0 alone would have let a load past the block.
  Co-op writes +0 = K and runs the group's own reset `8250DC00` (the game runs it from `8250F008` on every unlocked
  group): it clears both queues (`8250BB60`), rebuilds the free-slot queue for +0 slots and sets quota[0] (`*(+196)`) =
  +0; it leaves +32 / +36 alone. Only while the group is idle (`coopclothing::BodyGroupBusy`: the game's 11 slots, all
  in state 4 and queued, the slot lists +12 / +16 / +20 / +24 and the active request records +48 empty, nothing
  queued at +56, not locked): else the block is made with 11 slots as before (logged). The flags' 0x40 is kept.
- *Lines.* Start-up `Own body: the first K other players get their own body shape ...`; at the block `Own body: group
  3 cut to K load slots (now K, quota[0] K, K free queued)` and `Own body: player body streaming memory X MB at ...
  (K of 11 slots ...: Y MB saved ...)`; `Own body: StyleTest_PC_MPn's reference given back ...`.
- *own_body_max* stays (its count and settle wait as above), default 0 now: the K-slot block is small. `own_body_slots
  = 0` is `own_body = 0`; 11 is the old block.
- *Tests.* clothing_test: `BodyGroupBusy` (each condition), `BodySlots`, `BodyRefs` (at most K, a held index keeps its
  body, given back after 5 s unused, used again starts over).

**Memory step 3: item-sized clothing buffers** (mod.ini `clothing_size_log`, `clothing_item_sized`, both 0;
research: memory_plan_12.md section 1). The loader stages a piece in a buffer of its category's `Slot_size`
(`request+80`, `8248A100`) and `8248AD88` copies all of it into the component (`memcpy(component+12, request+92,
request+80)`), so a buffer smaller than `Slot_size` needs both guards below.
- *The item's size S* = peg pointer + peg size - staging (`8248D5F0`'s r6 / r7, `request+92`; the peg is the job's
  last file, 4096-aligned, `825134F8`); not known (full size) for a pointer outside the staging buffer or a morph
  after the peg (`coopclothing::ItemBytes`).
- *Phase A* (`clothing_size_log = N`): the first N picks logged, `Clothing size N: <mesh> (<category>): S ..., Slot_size
  ... (P %); staging ..., mesh +0, peg <name> +X (Y B), morph ...`, and an `item sizes:` part in the Clothing line
  (picks, not known, the largest S / Slot_size, the average S / Slot_size). Compare with the packfile sizes offline
  (`<mesh>.cmesh_xbox2` + `.peg_xbox2`, research `tools/vpp.py`).
- *Phase B* (`clothing_item_sized = 1`, outside multiplayer only): in the pick hook, before the game's pick, the free
  co-op components smaller than S get +132 0x10 for the call (the picker skips them as busy); the record's nodes on a
  co-op component smaller than S get node+12 = 0 for the call (the second chance can't swap into it; with nothing
  else the game fails cleanly, "No place", and the old item stays); with no free component and no node that holds S,
  one of AlignUp(S, 4 KB) is made at once (`AddClothingComponent(..., bytes)`); the free co-op component the picker
  takes (idle: no 0xD8, refs 0) is cut to AlignUp(S), the rest going to the spares that any later component takes by
  size (`ClothingTrim`). `ComponentCopyHook` on `8248AD88`: into a co-op buffer B smaller than `request+80`, B bytes
  are copied when S fits, else the component is passed as 0 (the game's null path: the item is asked again). An item
  of unknown size is guarded as full size. `ClothingPlan` still budgets `Slot_size` (an upper bound). Clothing line:
  `item-sized: N buffers cut (X MB to the spares), N made for one item (N refused), N too small hidden, N swaps
  refused; copies N shortened, N REFUSED`.
- *Tests.* clothing_test: `ItemBytes`, `PlanPick` (hide, clear, trim, grow, the record's own holds it, busy not cut,
  under 4 KB over not cut, S unknown), `CopyGuard`.

**Memory steps 1 and 3: review fixes** (research: notes/coop-3/review_memory_steps.md, memory_steps_impl_notes.md
"Review fixes").
- *H1, the key-5 player copy.* `OwnBodyRelease` counts as drawn every pool object not on the pool's free list
  (`0x8309ABE8`, linked at +4100) whose body definition (+228) is its own `StyleTest_PC_MP<index + 1>`, plus the copy's
  index itself (`coopclothing::BodiesInUse`); a free list that can't be read counts none free. A reference a drawn
  character uses is never given back (the next load would overwrite the mesh it is drawn from).
- *H2, the game's own multiplayer after a co-op session.* `MpBodySetupHook` on `8247E0E0` (from `82370A68`; it gives
  group 3 a new 11-slot block at +36 / +32 and force-unloads the loaded MP body models, but leaves +0 and the queue):
  before it runs, with group 3 below 11 slots and not locked, co-op's references go back (`8250CA48`), the loaded
  `StyleTest_PC_MP` models are unloaded as the original would (`8250CD88` and its byte at `*(0x8370FED0) + (id &
  0xFFFFFF) x 16 + 12`; the reset runs no unload callback), +0 = 11 and `8250DC00` (queue for 11, quota[0] 11).
  Line `Own body: the game's multiplayer body setup: group 3 back to the game's 11 load slots from K (...)`. If the
  hook doesn't install, `own_body_slots` isn't used (11 slots, logged).
- *H3.* Phase B (`clothing_item_sized`) is turned off (logged) when the copy hook on `8248AD88` doesn't install.
- *M1.* On a copy refusal the reserved component's 0x10 is cleared when nothing holds it (no +20 reference, no node of
  the record on it); refusals are counted per item (`coopclothing::CopyRefusals`) and after 2 the item gets no
  component again (`StopPick`: every free one hidden, the record's nodes cleared: the game's clean "No place"; also
  in multiplayer). The pick guard covers the whole ring and record (vectors, not 64-bit masks; `kPickWalkMax` 4096
  is only a broken-link guard).
- *M2.* `ItemBytes` needs the mesh in [staging, peg) (`8248D5F0`'s r4, `request+164`) and a morph inside it; else 0
  (full size). Phase B works with S + 4 KB (`ItemNeed`, at most `Slot_size`) for the hide, clear, trim, grow and copy
  until phase A's S is confirmed against the packfile sizes.
- *Lows.* L3: a reference isn't counted unused during its 8 s load wait (`BodyRefs::Take(index, now)`); L4: a held
  index whose request record a world reset freed is taken again while drawn, else forgotten (`BodyRefs::Drop`); L5:
  `OwnBodyRelease` skips a locked group; L6: `BodyGroupBusy` wants exactly 1 level (only level 0's queues are read);
  L7: `RelayTiers::Pass` reads the car tail only from a pose of `kPoseInCar` bytes. The Clothing line adds `N
  components unreserved, N picks of a twice-refused item failed`.
- *Tests.* clothing_test: `BodiesInUse` (the copy, an active pool object on its own definition, a free one, the free
  list not read), the copy's reference kept, the 8 s wait, `Drop`, `BodyGroupBusy` with 2 levels, `ItemBytes` with the
  mesh, `ItemNeed`, a 100-component ring guarded whole, `StopPick`, `CopyRefusals`. world_test: a 48-byte pose with
  the car form relayed as on foot.

**Player cap and appearance fixes** (research: notes/coop-3/appearance_feedback_research.md, mjq_fixes_a_notes.md).
- *The player cap* (mod.ini `max_players`, 8). `Session::MaxPlayers`: the session host refuses a join once the
  players and the slots waiting for a re-link reach it, with the existing `kJoinDeny` ("the session is full (N players,
  the session host's mod.ini max_players)"; the joiner's notice `Co-op: couldn't join - ...`). `kJoinOk` carries the
  host's cap after the names (part of protocol 13), so a client's player list shows `CO-OP n/N` (`SessionMax`). The
  Epic lobby's `MaxLobbyMembers` is `max_players`. The slots stay 12; after a migration the new session host's own
  setting counts (whoever is in stays; re-links aren't counted against it). Online, the Epic lobby keeps the first
  host's size after a migration, so the cap is the lower of the two, and a joiner Epic refuses gets Epic's error, not
  "the session is full".
- *The look block on any human.* A player whose character is an actor of another definition (the SR Tweak Menu's
  model swap, a female model) gets their look block (`ApplyLook`, 8238B258: race, head, hair, outfit variants), when
  the character was made with their model (not `kFallbackModel`, `Remote::their_model`) and no other model waits to be
  made. Clothing, sliders and colours stay with player objects.
- *Colours with the shared body.* Skin tone and hair colour changes after the first dressing now go on for players
  past `own_body_slots` too. Read in the asm: 82322050 -> 82322328 writes only the colour table entry of the record's
  own slot (`+8`), and the rebuild 8248A900 frees and makes again the render instances of the record's nodes and of
  `*(record+16)+3732` (82230D78 writes the instance); the body model is only read. The sliders still need their own
  body (they bake into the shared mesh).
- *Outfit changes settle, then go by difference* (clothing pool only). The sender sends a new worn list once it held
  1.5 s (`coopclothing::WornSettle`; the first at once, a joiner gets the settled one): the Tweak Menu's hover
  try-ons and presets go out once. The receiver plans the new outfit at its take-off as before (`ClothingPlan`: the
  pool decides what goes on), then takes off only the pieces not in the plan or not on (8248D198 per piece; a piece is
  `WornItemPiece`: the 68 B entry the record's node +0 holds) and wears only what isn't on (8248DE28), `DressDiff`. The
  rest stays on: no bare body. By difference there is no 8248D340 step: it isn't a commit but a loop of 8248D198 on
  the record's first node until none is left (every piece off); the wear waits for the take-off requests
  (`ClothingQueueIdle`). The plan is made once at the take-off and also used when the dressing goes in full. Everything comes off as before for a first dressing, a new record, the clothes check's
  re-dress and the colours' (`full_dress`), an item not known here, and once for an outfit whose new items the game
  didn't take by themselves (`NotWorn`, right after the wear: `diff_failed_for`). Lines `outfit changed: by
  difference, N pieces off, N items on, N stay`, `dressed again (by difference)`. `clothing_pool = 0` is unchanged.
- *The look kept through the game's reverts* (mod.ini `look_restore`, 1). In a session the local worn list, sliders
  and colours are kept once they held 2 s outside missions and cutscenes (`LookKeep`). `SaveLookHook` on `82208640`
  counts a call from the mission cleanup (returns to 82210D3C, `82210BE0`) as a revert, `RevertLookHook` on `82209190`
  one from the mission end (82212030) or restart (82212B1C); any other call (the load request `82208C48`, the
  console's `customization_revert`) is the player's: what was kept is forgotten. 2 s after a revert, outside a
  cutscene with the clothing queue idle, `LookRestoreTick` puts the look back the game's way (82318B38, the colours,
  8248DE28, 8248CFC8, the sliders' wanted values, the colours with the look rebuilt). Lines `Look restored after the
  game's revert to the save (from ADDR, ...)`, `Look: ... it is the one kept, nothing to restore`, `Look: a save was
  loaded ...`. The look the revert left is read once its clothing requests are done; nothing goes back on while a
  mission runs (a restart, the next mission: its own end reverts again), nor when the look changed since the revert
  (a mission's disguise, a script's customization, the player's own change: `Look: ... but it changed since ...: not
  restored`), nor when the model isn't the one the look was kept on (`... the look kept was on model ...: not
  restored`; the model itself is never put back).
- *Tests.* net_test 19 (the cap: a 4th player refused, the clients learn the cap, a leaver makes room, clamped);
  clothing_test (`WornSettle`, `DressDiff`, `NotWorn`).

**The host's mission car a player sits in** (MJQ's Vice Kings 2 report: on the client the host and Aisha stood up
through the car, then the client hit an invisible car and a copy of the host's car appeared; research
notes/coop-3/activity_car_research.md, mjq_fixes_b_notes.md). With `mission_cars = 0` the host's mission cars were
left out of the stream: the client showed the host's car as a pose copy made inside its own mission's twin, with no
passengers, and Aisha's copy froze at the door.
- *Streamed once a player sits in it* (mod.ini `player_mission_cars`, 1). `WorldRefresh`: a `world_mission_cars`
  entry (not a shared mirror's) whose `driver_of` is a player goes into `world_player_mission_cars` and streams to
  the mission's end whatever `mission_cars` says (only while its script hides it and nobody is in it it is left out).
  The pose carries it, its create carries the crew; the member adopts its pose copy (`WorldAdoptCar`). A story
  character seated in it isn't held back by 2.1's named rule.
- *The twin hidden.* `ShareMissionCars` lists those cars with `a[0] | 1 << 9` (no new event) whenever the mission's vehicles don't all stream. A member pairs them with any
  `mission_cars` setting: its own mission car of the model nearest to where the host's car was first seen here or is
  now, within 25 m (`cw::PickTwin`), hidden with 82538A90 as 2.4 does, and kept hidden while listed (no 3 s rule; shown
  again at the mission's end). For both kinds of list the host's car counts as shown when `FindCar` finds it (a car copy
  a pose made too, not only a world copy: a copy let go into `car_copies` used to show the twin again).
- *Hidden cars aren't solid* (mod.ini `hidden_cars_ghost`, 1). 82538A90 takes a hidden car out of the collisions
  only while it is in the world (`+120 0x04000000`): the vehicle vtable+28 `82533740` -> vtable+24 `825338C8` sets the
  body's filter (`*(v+244)+56`) to the no-collision `*0x827E7A20` and keyframes it. A car hidden outside the world
  (the client's own copy of a mission's chase vehicles, hidden at its group's create far off) stayed solid once its
  chunk loaded. Once a second a member gives the same call (`CarUnsolid`) to each hidden car of its mirrored mission
  (`own_mission_cars`) and each twin it hid, when in the world with a solid body; a car it made unsolid that is shown
  again outside the world gets the shown call once it is back in (`CarsSolidAgain`: once a second in every role, so
  also after this game became the mission host, and at once when a twin is shown; 256 remembered at most, logged once
  when full). Only on the known path (vtable+28 `82533740`, `+54664` / `+54684` clear).
- *Crew without its car.* `Scheduler::PlanMember`: a seated person whose car isn't a candidate and that the member
  holds while it doesn't hold the car (left out, declined) is removed there (reason 0) and offered again with its car
  or on foot; one in a car the member holds keeps it as before. Only a car of this owner's registry counts: a person
  in one it doesn't know (a pose's car copy, another owner's car) isn't seated for the plan and streams on foot as
  before. The owner's 10 s line adds `crew without its car N
  entry-ticks, removed for it N`.
- *The seat reset beside the car.* The reset after two refused seat tries puts the character 1.6 m out along the
  car's right row (+32), on the side it stood, else the seat's (0 and 2 on the negative side), not at the car's
  origin (which stood it in the body).
- *Tests.* world_test 7b (a person on foot that sits in a left-out car is removed at once, comes back after its car;
  in a held car that is inactive for 2 s it stays, it goes with the car after kInactiveMs), 7c (`PickTwin`), 7d (a
  person in a car not in the registry isn't removed, it streams on foot).

**Emotes and voice lines, protocol 13** (MJQ: "some emotes don't show on the other screen, voice lines don't work at
all"; research notes/coop-3/emotes_voice_research.md, mjq_fixes_c_notes.md). Two new event kinds, so `kVersion` is 13:
a protocol 12 build is refused at join.
- *The emote's set* (mod.ini `emote_sets`, 1). Most of the SR Tweak Menu's emotes (most poses, every prop emote) are
  states and actions of another animation set: the menu writes that set's entry (12 B at 0x82C06D58, +0 the name,
  count *0x8370DB74) on the player's animation instance `*(+568)+4` while the emote plays. The character in the other
  games tried them with its own set (state refused, "Action N: refused"). `kEventAnimSet` = 52 carries the set
  (`cw::PackAnimSet`: its index, its name's hash, borrowed = not the definition's set (+228, +20) nor a menu `SRT_`
  set), on a change at once (`WatchLocalAnim`: before the state's `kEventAnim` 250 ms later; `PlayActionHook`: before
  the `kEventAction`), and every 5 s. The receiver finds it by index when the name matches, else by name, and
  borrows it on the character before their state (UpdateAvatar) or action (`227 + n`) plays, even when the
  character's own set has it (the prop comes with the other set's state); not when that set lacks it too
  (`BorrowEmoteSet`). Its own set goes back (`EndEmoteSet`: StopMoves when something of it still plays, else
  82440200) when theirs changes, when nothing of theirs has played for 2 s, and when the character goes (a pool
  character goes back with its own). The pose refusals are logged now (once per player and state), and a refused
  action says why. `82444F88(player, -1)` is no longer sent.
- *Held tracks.* A file emote plays with 826F5C20 flag 17 (held: full weight after its end). 826F5BC0 / 826F5B88 skip
  held tracks, so each one played on a character stayed frozen there (after nine none started). Its handle is kept and
  the track's held bit cleared (ap+7260+idx*84 +16, when +0 is its id and 0x400 is set) before the next one and in
  `StopMoves`, as the menu's `free_held_tracks` does. A custom animation not loaded here is tried again every 5 s while
  they play it (was: never after 3 misses).
- *Voice lines* (mod.ini `voice_sync`, 1). Every line ends in SAY_LINE 82219298(human, situation, r5, r6 sound set,
  r7 distance class): pain, death, combat, script and Tweak Menu lines, all on the local player. `SayLineHook`: a
  line our player said (it returned 1) goes as `kEventVoice` = 51 (`cw::PackVoice`: situation, the persona's index in
  0x82B3E420 (960 B each, count *0x82B3E41C, +0 its name) and name hash, r5-r7) to the players within 170 m
  (`cw::VoiceInRange`, the relay's second tier; one event to everyone when all are), at most 4 a second and no repeat
  of a situation within 300 ms (`cw::VoiceGate`). There the character takes that persona (+3552; a gang voice from the
  menu's Voice page goes too) and says it: SAY_LINE on the character, so from where it stands, culled by that game's
  listener, with its talking face (+224 0 around the call). At most 6 a second per player there; none while the
  character is hidden for a cutscene. The characters' own lines are refused (a replayed hit with a persona said its
  pain twice). The persona is put back when the character goes.
- Lines `Player N emote set: PML1`, `emote N: their set borrowed here`, `emote set put back (...)`, `pose N refused
  here: ...`, `custom animation X isn't loaded here: tried again every 5 s ...`, `Player N voice: BM_VK1`, `said line N
  (...): said here / not said here`; the 30 s `Emote sets:` and `Voice:` counts.
- *Tests.* net_test 20 (both events pack and back, refusals, the gate, the range, and through the session: the set
  before its action, the line to its target only).

*Settings* (mod.ini)
- `max_players = 8` (the line missing is 8; 2-12; the session host's counts): the player cap above.
- `emote_sets = 1` (the line missing is 1): the emote's set and the held tracks above. `0`: no set sent or borrowed,
  only emotes in the character's own set show (and file emotes' held tracks stay, as before).
- `voice_sync = 1` (the line missing is 1): voice lines above. `0`: none sent or said here, other players' characters
  stay silent (their own lines are not refused either).
- `player_mission_cars = 1` (the line missing is 1; the host streams, a member hides its twin): the host's mission car
  a player sits in above. `0`: as 2.4.
- `hidden_cars_ghost = 1` (the line missing is 1): hidden cars out of the collisions above. `0`: as the game leaves
  them.
- `look_restore = 1` (the line missing is 1): the look kept through the game's reverts above. `0`: the reverts are
  left as they are (the save's look).
- `pose_tiers = 1` (the line missing is 1; the session host's setting counts): the relay's tiers above. `0`: every
  pose to everyone (to tell a tier problem from another).
- `send_pacing_ms = 10` (the line missing is 10; 0-50): the pacing above. `0`: a packet whenever something is queued.
- `player_list = 1` (the line missing is 1): hold Back for the player list. `0`: Back is never taken, it passes to
  the game as before.
- `clothing_pool = 1` (the line missing is 1): the clothing pool above. `0`: a full set per player-pool slot, as 2.4,
  plus the worn-piece guard (L7 above).
- `view_holds = 1` (the line missing is 1): the fades fixes' holds and the member's off-screen and lying-body rules.
  `0`: copies fade when their game removes them, as coop3-7 (to tell a fades problem from a 2.4 one).
- `audio_repair = 1` (the line missing is 1): the unlock popup's sound pause handed back, the watchdog's repairs and
  the safe points above, and co-op's cutscene end held for its start's begin. `0`: the `Sound pause count` lines only
  (to tell a sound problem from the repair), the end at once.
- `climb_sync = 1` (the line missing is 1): players' climbs sent and replayed (above). `0`: no `kEventClimb` sent or
  replayed, and their climb states play on the character as before 3.0 (no fallback, no jump end on 6/E or 0/0,
  Drive's >2 m/s release as before).
- `own_body_slots = 2` (the line missing is 2; 0-11; with `own_body = 1`): the body block's load slots (memory step 1
  above): the first K other players get their own body shape, K x 1.46 MB set aside at the first join. `0`: own body
  off (`own_body = 0`). `11`: the old 16 MB block.
- `own_body_max = 0` (the line missing is 0; with `own_body = 1`): with more players than this in the session at the
  first join (on the host of a session over half this size, 5 s after the first character asked for its body), no body block: everyone shows this game's body shape
  (the body block above). `0`: no limit (coop3-21 shipped 6, when the block was always 16 MB).
- `clothing_size_log = 0` (the line missing is 0; 0-1000): the first N clothing picks' item sizes logged (memory step
  3, phase A).
- `clothing_item_sized = 0` (the line missing is 0): item-sized clothing buffers (memory step 3, phase B). Turn it on
  only once the `Clothing size` lines show every item at or under its slot.
- `clothing_release = 1` (the line missing is 1, as mod.ini ships it): the pool gives memory back (memory low, after
  a session, after the game's cleanup; fair sharing's free components as spares). `0`: its memory is kept until the game exits, no extras taken off for others, as coop3-14 (to turn it off
  without a rebuild if a crash or broken textures show up).
- `world_mode = area` (default; the line missing is area). `shared`: phases 2a, 2b, 2c and 3 above. Set it the same on
  every game: a game of the other mode gets no world stream and says so in its log.
- `handover = 1` (shared only; the line missing is 1, as mod.ini ships it): phase 3's hand-over. `0`: copies fade when their owner's player
  leaves and the new owner spawns its own (2c), to compare.
- `host_migration = 1` (shared only; the line missing is 1): phase 4's host migration. `0`: the session ends when the
  host leaves (as in area mode, where it always does). The session host's setting counts: a client turns migration
  on for the session when its host shares the address list (review_phase4 L9).
- `puppets = 0` (default; the line missing is 0): 2.4's copies. `1`: puppet copies (above).
- `world` is gone (1b): 2.2's `world = 0` (2.1's streams instead of the world stream) has no build that reads it; a
  `world` line left in an old mod.ini is ignored. `traffic` / `peds = own` as above.
