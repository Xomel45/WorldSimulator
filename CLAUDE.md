# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

**WorldSimulator** — god-game world simulator, mixing **Creatures** (individual
genetics, simple neural-net brains, needs-driven behaviour) with **WorldBox**
(sandbox scale: god powers, terrain, civilizations, chaos).
Stack: C++23 · Vulkan 1.4 · GLFW 3.4 · CMake 4+ + Ninja · Clang.

Engine plumbing (`engine/renderer`, `engine/ui`, `InputSystem`, `Logger`) was
bootstrapped from a sibling project, **Podjezd** (`../Podjezd`) — a mature
first-person-horror Vulkan engine. Only the generic rendering/windowing/input
layer was reused; `Application` and the camera were rewritten from scratch for
a top-down god-camera instead of Podjezd's FPS mouse-look. Podjezd's
audio (FMOD), physics (AABB character controller), network, and procgen
(stairwell generator) modules were **not** carried over — none of that logic
applies here yet. AlphaCraft (`../AlphaCraft`, voxel/Perlin-terrain engine)
was surveyed but not used; may be worth revisiting for terrain generation
later.

## Build

Besides Vulkan/GLFW/glm: **Lua >= 5.4** (`find_package(Lua)`; Arch: `lua`) and
**nlohmann_json** (Arch: `nlohmann-json`) for mods.

```bash
cmake --preset debug       # → build/debug/
cmake --preset release     # → build/release/

cmake --build --preset debug
cmake --build --preset release

./build/debug/game/worldsim
```

### Windows (cross-compiled on Linux)

```bash
cmake --preset release-windows          # llvm-mingw toolchain: cmake/toolchain-mingw64.cmake
cmake --build --preset release-windows  # -> build/release-windows/game/worldsim.exe
cmake --install build/release-windows --prefix dist   # portable folder
```

Needs llvm-mingw (`/opt/llvm-mingw`) and the Arch mingw sysroot (`/usr/x86_64-w64-mingw32`:
Vulkan loader, GLFW, glm, nlohmann_json). Everything else is linked statically (`-static`), so
the result depends only on system DLLs (`vulkan-1.dll`, UCRT); Lua is compiled from source
(`WSIM_BUNDLED_LUA=ON` in the preset; FetchContent of lua/lua v5.4.7 — also used on Linux
automatically when no system Lua is found). Cross quirks are copied from Podjezd and live in the
root `CMakeLists.txt` under `if(WIN32)`: the mingw glm cmake config is broken (made a clean
INTERFACE target), the sysroot include dir is added as `-idirafter` and zeroed on imported
targets (as `-isystem` it shadows libc++'s headers). The install tree is
`worldsim.exe + shaders/*.spv + assets/fonts + mods + LICENSE`; `game/main.cpp` prefers that
exe-relative layout (`platform/Paths.hpp: exeDir()`) and falls back to the build-tree paths.
Verified: it runs under Wine (Vulkan, mods loaded from the exe folder). Not verified on real
Windows. User paths: `%APPDATA%\WorldSimulator` (settings, disabled mods, user mods).

`WSIM_AUTOEXIT=1` — auto-quits after 5s (non-interactive smoke test of the
full init → render → shutdown cycle).

`WSIM_FRAME=file.ppm` (+ `WSIM_FRAME_AT=seconds`, default 3) — dumps the FINISHED frame including all UI (menu,
settings, power bar, toasts) by copying the swapchain image (it has `TRANSFER_SRC`); `WSIM_MENU_PAGE=load` or
`settings:N` (N = tab 0..5) opens a menu page directly, so every screen can be inspected:
`WSIM_MENU_PAGE=settings:3 WSIM_FRAME=/tmp/x.ppm WSIM_AUTOEXIT=1 ./build/debug/game/worldsim`.

`WSIM_SCREENSHOT=file.ppm` — dumps the scene (offscreen target, before UI/composite,
tonemapped) at ~3 s into a PPM, so visuals can be checked without a screen
grabber (Wayland `grim` doesn't work here): convert with PIL (`Image.open(..).save('x.png')`)
and look at it. `WSIM_DEMO_HILLS=1` raises three hills near the origin (to see
shadows/rock/snow without the mouse). The terrain seed is random every run.

Sound env vars: `WSIM_NOAUDIO=1` (don't open a device), `WSIM_DUMP_SOUNDS=dir` (write every synthesized
sound as WAV so you can listen), `WSIM_AUTOEXIT_SECONDS=N` (length of the AUTOEXIT smoke test, default 5).

More debug env vars (all in `Application.cpp`): `WSIM_NIGHT=1` (no sun, dim ambient),
`WSIM_FLASHLIGHT=1` (lantern on from the start), `WSIM_DEMO_BOULDERS=1`, `WSIM_DEMO_HOUSES=1`.

`WSIM_PERF=1` — every 2 s logs `PERF: fps, ms/frame, CPU render ms, shadow/scene draw counts, chunks`
(measured: release ≈ 500+ fps at radius 64 with shadows, MSAA 8; **debug was 28 fps at `-O0`**, now `-Og` ≈ 240 fps —
the frame is CPU-bound by ~1400 shadow draws; if FPS looks bad, check you run the *release* build first).
GPU choice prefers a discrete device. `WSIM_SEED=N` fixes the world/terrain seed; `WSIM_SHADOW_FULL=1` disables the
shadow-caster culling below (reference for pixel-diffing).

`WSIM_AUTOSTART=1` — skip the menu and start straight in the game (use with
`WSIM_AUTOEXIT=1` to smoke-test the in-game loop: chunks, power bar, etc.).

After first configure, `build/debug/compile_commands.json` → LSP resolves
include paths correctly.

## Architecture

```
engine/
  core/        Logger.hpp (header-only, C++23 std::print)
               InputSystem.hpp/.cpp — keys, mouse delta, mouse buttons, scroll
               Application.hpp/.cpp — game loop (fixed 60 Hz + variable render)
               TerrainChunks.hpp/.cpp — infinite terrain as a ring of chunks
                 around the camera (see "Terrain" below)
  renderer/    Window.hpp/.cpp — GLFW wrapper
               VulkanContext.hpp/.cpp — Vulkan init, swapchain, sync objects,
                 scene pipeline, MSAA, RT shadows (graceful fallback if
                 ray_query unsupported), FSR/NIS upscalers — all inherited
                 from Podjezd, currently unused/dormant except MSAA
               Camera.hpp — OrbitCamera: pivot point (target) + yaw/pitch/
                 distance; same CameraData UBO contract as Podjezd's FPS
                 camera, so VulkanContext needed zero changes
               Mesh/GpuBuffer/ObjLoader/SceneObject/Transform/AABB/Frustum —
                 generic mesh/scene plumbing
  ui/          ImGuiLayer.hpp/.cpp
               MainMenu.hpp/.cpp — main menu drawn entirely via ImDrawList
                 (see "Main menu" below)
               PowerBar.hpp/.cpp — in-game god-power bar (see "God powers")
               Style.hpp/.cpp — shared bioluminescent palette + glow primitives
                 (rgba() sRGB->linear, gradientRing, radialGlow)
               Settings.hpp/.cpp — plain-data user settings + key=value file
                 ($XDG_CONFIG_HOME/WorldSimulator/settings.cfg)
  sim/         Powers.hpp/.cpp — GodContext + PowerDef/PowerRegistry (god powers)
               Genome/Brain/Creature — genetics + neural-net brain + needs.
               World — population + food + spatial grid, orchestrates the
               above. Terrain — heightmap generation (independent of World
               this round). No Vulkan/GLFW dependency (only glm) -> fast to
               compile and directly unit-testable. `Application` owns a
               `World` and a `Terrain` and renders both — see "Creature
               core"/"World"/"Terrain" below.
  audio/       Synth (procedural PCM, no deps) + AudioEngine (miniaudio) — see "Sound"
  platform/    Paths.hpp — header-only: exeDir/configDir/dataDir (Linux + Windows)
  procgen/     MeshBuilder + HouseGen — procedural building meshes (see "Lantern, physics...")
  mods/        LuaSandbox + ModManager — mod loading (see "Mods"). Depends only
               on engine_sim, so it is unit-tested headlessly.
cmake/         toolchain-mingw64.cmake (cross-build for Windows, from Podjezd)
game/
  main.cpp     entry point
assets/
  shaders/     inherited wholesale from Podjezd (scene/blur/composite/vision/
               FSR/NIS/RT) — generic rendering utilities, no game content
  fonts/       LiberationSans-Regular.ttf (ImGui debug UI),
               Unbounded-Bold.ttf + Geologica-Regular.ttf (main menu; static
               instances cut from Google Fonts' variable fonts via fontTools
               `instancer` — ImGui/stb_truetype can't pick a variable-font
               axis — with Cyrillic; OFL licenses alongside)
```

## Camera (god camera)

`OrbitCamera` (`engine/renderer/Camera.hpp`) orbits a movable pivot
(`m_target`) instead of Podjezd's free-fly FPS camera:
- **WASD** — pan the pivot (camera-relative flat XZ, speed scales with
  distance so zoomed-out panning is faster)
- **Right-mouse-drag** — orbit (`yaw`/`pitch`, pitch clamped `[-85°, -20°]` —
  always looking down, never below horizon or near-vertical)
- **Scroll wheel** — zoom (`distance`, clamped `[4, 200]`)
- **Esc** — back to the main menu (camera input is ignored while it's open)

`InputSystem` gained `isMouseButtonDown()` and `scrollDY()` on top of
Podjezd's key/mouse-delta tracking. The scroll GLFW callback is installed
*before* `ImGuiLayer` so Dear ImGui's own `install_callbacks=true` chains to
it correctly (ImGui saves+calls the previously-registered callback). Window's
`glfwSetWindowUserPointer` already holds the `Window*` for resize handling,
so `InputSystem`'s scroll callback uses its own single-instance static
pointer instead (the whole engine assumes exactly one window/one
`InputSystem`).

## Creature core (`engine/sim/`)

Pure simulation logic — `World` (below) is what turns it into a running
population, `Creature` itself still never reaches into anything external:
- **`Genome`** — flat `std::vector<float>` of 863 genes in `[-1,1]`: 13 body/
  behavior/temperament traits (speed/size/metabolism/lifespan/fertility/
  **plasticity**/**boldness**/**sociability**/**bondStrength**/**envy**/
  **territorial**/**curiosity**/**empathy**, named via `Genome::TraitIndex`)
  + 850 brain weights, split into `fixedBrainWeights()` (640 — input→hidden,
  "perception", never changes after birth) and `initialPlasticWeights()`
  (210 — hidden→output, "decision mapping", just the *starting point* a
  creature's `learnedWeights` copies at birth — see `Brain`/`Creature`
  below). `Genome::kFixedBrainWeightCount`/
  `kPlasticBrainWeightCount`/`kBrainWeightCount` mirror
  `brain::kInputHiddenWeights+kHiddenCount` / `kHiddenOutputWeights+kOutputCount`
  / `kWeightCount` respectively, enforced by `static_assert`s in `Genome.cpp`.
  `random()`/`crossover()`/`mutate()` all take an explicit `Rng&`
  (`std::mt19937`, no global RNG state — deterministic given a seed). Random
  brain weights are drawn from a narrow gaussian (not full `[-1,1]` noise) so
  a fresh creature starts with responsive, non-saturated neurons instead of
  flailing for generations before selection finds anything coherent.
  `setTrait(TraitIndex, float)` is a direct, clamped single-gene setter —
  symmetric with `trait()`, used by tests to force a specific temperament and
  eventually by god-tools to set a creature's character directly.
- **`Brain`** — mostly stateless, with one deliberate exception (see lifetime
  learning below): `brain::evaluate(fixedWeights, plasticWeights, BrainInputs)
  -> BrainEvalResult` (bundles `BrainOutputs` + the hidden/output activations,
  needed by `reinforce()`), fixed topology 31 inputs → 20 hidden (tanh) → 10
  outputs (`tanh` for move X/Z, sigmoid for the desire + communication
  outputs). Sized after comparing to the real Creatures Norn brain (952
  neurons/~5000 connections/9 lobes) — both scales are computationally
  trivial at thousands of agents, so this was a scope call, not a performance
  one.
- **Communication** (`alarmCall`/`matingCall`/`dominanceDisplay` outputs,
  `nearbyFamiliarAlarmLevel`/`nearbyStrangerAlarmLevel`/
  `nearbyMatingCallLevel`/`nearbyDominanceLevel` inputs) — creatures
  broadcast a distress, mating-readiness, or status-display signal; nearby
  creatures sense the aggregate (computed externally by the future
  world/spatial system, same pattern as `SenseInputs`'s other fields) and it
  has real behavioral effect in `Creature::tick()`: a neighbor's alarm call
  raises your own fear even if you can't see the threat yourself (herd
  panic — split into familiar/stranger sources so empathy has something to
  modulate, see Empathy below), a neighbor's mating call nudges your own
  libido, a neighbor's dominance display is intimidating (raises fear too,
  dampened by `boldness` — same mechanism as own-threat/herd-alarm, see
  Explicit temperament below). This is what makes creatures actually social,
  rather than tracking private `Needs` numbers nobody else reacts to. It's
  also the raw material for emergent teamwork (e.g. rallying toward an alarm
  instead of fleeing it) — deliberately not hard-scripted, since hard-coding
  "help a neighbor in danger" would undercut the actual goal (genuinely
  emergent, evolved behavior, not a scripted formula wearing a brain label).
- **Respect/status drive** — `Needs::respect` (desire for social standing,
  default `0.3`) drifts upward on its own (like `libido`/`social`), faster
  when a nearby `dominanceDisplay` creates rivalry pressure
  (`nearbyDominanceLevel`). `dominanceDisplay` doubles as both this
  creature's expressed desire for status *and* the broadcast signal others
  sense (same dual role as `alarmCall`/`matingCall` — no separate output
  needed). Satisfied only by `Creature::earnRespect(amount)`, a world-driven
  mutator (same "world decides, `Creature` records" pattern as
  `feed()`/`rest()`/`mate()` — called when a display actually wins a status
  contest, once that world-level mechanic exists). No dedicated heritable
  "ambition" trait yet (mirrors `kAggression` being deferred in the
  temperament round) — worth adding once status contests are real.
- **Envy/resentment (the dark mirror of respect)** — `Genome::kEnvy`
  (`Creature::envy()`, `[0,1]`) is what actually links "fear" and "desire to
  be respected" into a dark side rather than just a light one: a nearby
  `dominanceDisplay` is inherently intimidating (see above), but an envious
  creature converts part of that intimidation into `Needs::resentment`
  instead of pure submission (`fearTarget`'s `nearbyDominanceLevel` term is
  dampened by `envy()`, same slot `boldness` already dampens). `resentment`
  builds only from nearby dominance scaled by `envy()` and decays on its own
  otherwise — at `envy=0` it never leaves `0`, no capacity to resent at all.
  Fed to the brain as `BrainInputs::resentment`; **not** hardcoded into any
  particular reaction — whether resentment turns into `defendDesire`
  (lashing out), confrontation, or nothing is left to evolved weights, same
  "brain expresses intent, don't hard-script the translation" reasoning as
  communication/teamwork. No world-driven satisfier yet (no "revenge" action
  exists to resolve it against). Two related dark-side ideas were
  considered and **not** built this round, because the architecture already
  covers them: **deceptive signaling** — `alarmCall`/`matingCall`/
  `dominanceDisplay` are already free evolved outputs with zero honesty
  constraint tying them to true internal state, so a creature could already
  evolve to bluff; it just has no payoff to do so without a World creating
  real multi-agent stakes. **Betrayal/selfishness** — already the low pole
  of `bondStrength` (a `bondStrength≈0.2` creature already barely reacts to
  a loved one's danger) — a second trait for the same axis would be
  redundant plumbing, not a new feature.
- **Grief** — `Needs::grief`, triggered by the transient
  `SenseInputs::lovedOneDied` (world sets it nonzero the tick(s) it wants to
  notify). No dedicated trait — reuses `bondStrength()` on both ends: a loss
  hits proportional to how attached the creature was
  (`needs.grief = bondStrength()` on trigger), and fades slower the more
  attached it was (`griefFade = 0.005 + 0.01*(1-bondStrength())`). Pure
  need+input like `respect`/`resentment` — no hardcoded downstream effect.
- **Territoriality** — `Genome::kTerritorial` (`Creature::territorial()`).
  `Creature::homePosition` lazily set to the birth/first-wake position in
  the same first-`tick()` branch that already lazily inits `learnedWeights`
  (no new "is this the first tick" flag). `Creature::awayFromHome()`
  (public, computed on demand — not just an internal `tick()` value) is a
  saturating `[0,1)` distance (`homeDistance / (homeDistance +
  kHomeRangeScale)`, `kHomeRangeScale=20` — placeholder scale until
  terrain/world establish real units, same caveat as `lifespan()`'s
  placeholder time unit). Feeds `fearTarget` (homesickness, scaled by
  `territorial()`) and `BrainInputs::awayFromHome`.
- **Curiosity** — `Genome::kCuriosity` (`Creature::curiosity()`).
  `Creature::boredomLevel()` (public, pure function of current `needs` +
  `curiosity()`, no stored state) = `(1 - urgency) * curiosity()` where
  `urgency = max(hunger, 1-energy, fear, libido)` — deliberately excludes
  the slower-burn background needs (`social`/`respect`/`resentment`/`grief`)
  so they don't suppress exploration as strongly. Fed as
  `BrainInputs::boredom` only — no deterministic `tick()` effect, since
  unlike grief/territoriality there's no single obvious Needs-level effect
  worth hardcoding; evolved weights decide whether/how boredom drives
  movement.
- **Empathy** — `Genome::kEmpathy` (`Creature::empathy()`). The
  familiar/stranger alarm split above exists specifically so this has
  something to modulate: a stranger's panic is mostly discounted by default
  (floor `0.2×` in `fearTarget`), but a high-empathy creature feels nearly
  as much concern for a stranger as for family (up to `0.8×` at
  `empathy=1`) — distinct from `sociability` (general company-seeking) and
  `bondStrength` (specific-individual attachment): this is compassion for
  people you don't even know.
- A fifth candidate, **"desire for power"**, was considered and skipped —
  `Needs::respect` (wanting status) + `dominanceDisplay` (asserting it) +
  `Genome::kEnvy`/`Needs::resentment` (resenting others who have it) already
  cover the power/status axis end to end; a dedicated trait would duplicate
  it, same trap as the betrayal/deception skips above.
- **In-group/out-group** — `SenseInputs::familiarCrowding`/`strangerCrowding`
  (world tells a creature how many nearby individuals are "known": id
  matches `bondedPartnerId`/`parentIds`/`childIds`, vs strangers). Family
  satisfies `needs.social` more than strangers do (not zero — some social
  value even among unknowns); a crowd of strangers adds mild background
  wariness to `needs.fear` (well below any real threat — evolved weights
  decide how much a given lineage actually cares).
- **`defendDesire`** output (fight, not just flee) + **`selfSize`** input (a
  creature needs some notion of its own size/strength to make a fight-vs-flee
  decision that's about anything other than luck). Like the other desire
  outputs, the world (later) decides whether a fight actually resolves — the
  brain only expresses intent.
- **Identity & relationships** — `Creature::id`/`bondedPartnerId`/`parentIds`/
  `childIds` (all `std::uint64_t`, `0` as the uniform "none/unassigned"
  sentinel — no `std::optional`). `Creature` never assigns its own `id`;
  that's a population-registry concern for the future World system, same
  "world decides, `Creature` records" pattern as `feed()`/`rest()`/`mate()` —
  a freshly-constructed or freshly-`reproduce()`d `Creature` has `id == 0`
  until the world registers it. `mate(partnerId)` also sets
  `bondedPartnerId`; `recordChild(childId)` — the world calls it on both
  parents after assigning the new child's real id; `reproduce()` sets the
  child's `parentIds` from the parents' existing ids. `childIds` is a
  `std::vector`, which reintroduces heap allocation on reproduction (rare
  relative to per-tick brain evaluation, so an accepted trade-off for now
  over a fixed-capacity container — revisit only if profiling ever shows it
  matters). This is what makes "loved ones" a real sensed thing:
  `SenseInputs::lovedOneDir/Proximity/ThreatLevel` (→
  `BrainInputs::lovedOneDirX/Z/Proximity/ThreatLevel`) is computed externally
  from the family/bond ids above, same seam as every other sensed field, and
  feeds `needs.fear` in `Creature::tick()` (weighted below own-threat and
  above generic herd-alarm) as well as the existing move/defend outputs.
  `SenseInputs::lovedOneDied` extends this to death, not just danger — see
  Grief above.
- **Explicit temperament** — `boldness`/`sociability`/`bondStrength`/`envy`/
  `territorial`/`curiosity`/`empathy` are named `Genome::TraitIndex` genes
  (unlike the other 850 brain weights, individuality here isn't an opaque
  blob — it's inspectable and directly, deterministically wired, not just
  another evolved input the network may or may not learn to use).
  `Creature::boldness()`/`sociability()`/`bondStrength()`/`envy()`/
  `territorial()`/`curiosity()`/`empathy()` lerp them to `[0,1]`
  (`bondStrength()` to `[0.2,1]` — a bonded relationship is never *zero*
  fear) and modulate `tick()`'s `fearTarget`: `boldness` dampens fear from
  own threat, herd alarm, *and* nearby dominance displays (same
  `boldnessFactor`, floored — nothing is fully fearless; `envy` further
  dampens the dominance-display term specifically, see Envy/resentment
  above), `bondStrength` scales how strongly a loved one's danger is felt
  (individualizes what used to be a flat constant), `sociability` dampens
  stranger-crowd wariness and (separately) stranger-*alarm* wariness is
  `empathy`'s job (see Empathy above — these are deliberately distinct
  axes), `territorial` scales homesickness via `awayFromHome()`.
  `sociability` also scales how much company (familiar and stranger)
  satisfies `needs.social` — extroverts gain more from any crowd, introverts
  less (baseline loneliness decay is temperament-independent — everyone
  still gets lonely over time). `curiosity` has no `fearTarget` role — see
  Curiosity above, it only feeds `BrainInputs::boredom`. Considered a
  `kAggression` trait too but skipped: no mechanical hook exists yet since
  fight resolution isn't implemented (`defendDesire` is still just an
  expressed intent) — revisit once combat lands.
- **Lifetime learning** — the one deliberate exception to "Brain is
  stateless": `Creature::learnedWeights` is a per-creature runtime copy of
  just the *plastic* weight slice (hidden→output+bias, 210 floats), lazily
  initialized from `genome.initialPlasticWeights()` on first `tick()`
  (`resetLearning()` does this explicitly too, e.g. for a "forget everything"
  mechanic later). Mechanism: **reward-modulated Hebbian learning**
  (`brain::reinforce()`) — no backprop/gradient framework, matches the
  project's whole no-training-loop approach and is roughly the era/style the
  real Creatures used. `Creature::tick()` computes `reward` from the needs
  delta the tick itself produces (`wellbeing = -hunger -(1-energy) -fear`,
  `reward = wellbeing_after - wellbeing_before`, clamped `[-1,1]`) and calls
  `reinforce()` after applying the tick's outputs — `Δw = learningRate *
  reward * hiddenActivation * outputActivation`, clamped to `[-2,2]` (wider
  than genome's `[-1,1]` — this is runtime state, not gene-space). The
  input→hidden slice (`genome.fixedBrainWeights()`, "perception") is never
  plastic — only "how strongly a perceived pattern drives an action" adapts,
  echoing Creatures' separation of more-fixed perceptual lobes from a
  dedicated learning lobe, without a full multi-lobe rewrite.
  `Genome::kPlasticity` (trait, lerped via `Creature::plasticity()` to
  `[0, 0.05]`) makes the learning rate itself heritable — evolution can
  select for more/less behaviorally-adaptive lineages. Crucially:
  **Darwinian, not Lamarckian** — `reproduce()` crosses over `Genome`s only,
  never `learnedWeights`; a child inherits baseline instinct, never a
  parent's lifetime experience.
- **`Creature`** — aggregates `Genome` + `Needs` (hunger/energy/fear/libido/
  social/respect/resentment/grief) + identity + `learnedWeights` +
  `homePosition` + position/velocity/age.
  `tick(dt, SenseInputs)` runs the brain (with learning) and updates
  needs/movement/age; `SenseInputs` is supplied by the caller — `Creature`
  never queries a world itself, which is the seam the next milestone (world +
  spatial queries) plugs into. `feed()`/`rest()`/`mate()`/`recordChild()`/
  `earnRespect()` are world-driven mutators (world decides an action
  succeeded, then calls these). Body traits (`maxSpeed()` etc.) and derived
  signals (`awayFromHome()`, `boredomLevel()`) are computed on-demand, not
  cached.
- Tests: `engine/sim/tests/{genome,brain,creature,world}_tests.cpp`,
  assert-based (no test framework dependency), run via
  `ctest --test-dir build/debug`.

## World (`engine/sim/World.hpp`/`.cpp`)

Owns the population, food, and a spatial grid; is the only thing allowed to
call `Creature`'s world-driven mutators (`feed()`/`rest()`/`mate()`/
`recordChild()`/`earnRespect()`) and `Creature::reproduce()` — `Creature`
itself still never reaches outward, same invariant the module already had.
Zero Vulkan/GLFW dependency, same as the rest of `engine_sim`.

- **`Inhabitant`** = `Creature` + `lastOutputs` (that creature's `BrainOutputs`
  from the *previous* tick). Lives in `World`, not `Creature.hpp` — keeps
  `Creature` itself unaware it's being watched. `lastOutputs`, not the
  current tick's, is what neighbors sense (`nearbyFamiliarAlarmLevel` etc.)
  — avoids a same-tick chicken/egg dependency between creatures without a
  two-solve-per-tick scheme.
- **Spatial grid** — uniform grid (`kCellSize=15`), rebuilt fresh every
  `tick()` from current positions (`O(n)`, no incremental bookkeeping —
  population is in the hundreds, not millions). One grid for creatures, one
  for food (amount > 0 only). This is the fix for the O(n²) neighbor-query
  cost flagged when the hardware ceiling was discussed (`tools/hw_ceiling.py`).
- **`tick(dt)`**: two passes. Pass 1 — per creature, build `SenseInputs` from
  the grid + `lastOutputs` + a pending "grief ping" map (see below), call
  `creature.tick()`, then resolve `eatDesire`/`restDesire` inline (continuous
  `dt`-scaled consumption/recovery, not instant). Pass 2 — mating needs both
  sides' `mateDesire`, only available once everyone's thought this tick: pairs
  up eligible creatures within interact range, calls `mate()` on both, and
  (under `kMaxPopulation`) spawns a child via `Creature::reproduce()`. Dead
  creatures (`Creature::tick()` already sets `alive=false` at `age >=
  lifespan()` — no new death logic here) get erased and queue a one-shot
  grief ping for their `bondedPartnerId`/`parentIds`/`childIds`, consumed as
  `SenseInputs::lovedOneDied` at the start of the *next* tick.
- **Sensing approximations** (documented, not accidental): loved-one lookups
  ignore `kSenseRadius` entirely (family is always sensed, however far —
  `kFamilySenseRange=200` only shapes the proximity falloff); a creature's
  own senses are built from grid/`lastOutputs` state frozen at the top of the
  tick, but *other* creatures' `needs.fear` (for `lovedOneThreatLevel`) is
  read live, so it's mid-tick-fresh for lower-indexed creatures in
  `m_population` and previous-tick for higher-indexed ones — a same-tick
  ordering wrinkle, imperceptible at a 60Hz step, not worth a second sense
  pass to eliminate.
- **Known gaps** (deliberately out of scope this round; mostly superseded — see "Behavior layer"): no predators/threats
  (`threatProximity` stays `0`) or combat resolution (`fleeDesire`/
  `defendDesire` stay expressed-but-unresolved intents); no starvation death
  (`needs.hunger` caps at `1.0` in `Creature::tick()` but never kills — an
  existing gap, not introduced here; `kMaxPopulation=200` is the safety net
  against unbounded growth in its place); no dominance/status contests
  (`Creature::earnRespect()` stays uncalled); no per-creature visual
  differentiation (all creatures share one mesh/color — see Application
  integration below); **terrain physics is movement-only** (see "Creature
  terrain physics" below): the brain still knows nothing about slopes/water
  (no terrain inputs), the world just constrains what the brain asks for.
- **Application integration**: `Application` owns a `World` (seeded from
  `std::random_device` — real playthroughs vary run to run; determinism for
  tests lives in `world_tests.cpp` with a fixed seed) and a single shared
  pyramid `Mesh` (`buildCreatureMesh()` in `Application.cpp`) that every
  living creature's `SceneObject` references — `Application::update()` calls
  `m_world.tick(dt)` then rebuilds `m_creatureObjects` from
  `m_world.population()` each step (X/Z position + `Terrain::heightAt()` for
  Y — see Terrain below, `size()` scale, yaw from velocity when moving).
  **Destructor note**: since every creature `SceneObject` shares the *same*
  `Mesh`, it's destroyed exactly once explicitly — not by iterating
  `m_creatureObjects` the way `m_scene`'s (distinct) meshes are, which would
  double-destroy the shared GPU buffers.

## Terrain (`engine/sim/Terrain.hpp`/`.cpp`)

Real heightmap geometry, not a flat plane painted by biome — pure logic,
zero Vulkan/GLFW deps, same rule as `Genome`/`Brain`/`Creature`/`World`.
**`Terrain` knows nothing about `World`**; `Application` owns both and hands
`&m_terrain` to `World::tick()`, which passes it to creatures via
`SenseInputs::terrain` (see "Creature terrain physics").

- **`Terrain::heightAt(x, z)`** — pure `O(1)` function, no stored grid (same
  "derived/pure-function" style as `Creature::awayFromHome()`/
  `boredomLevel()`): mesh resolution (a renderer concern) is fully decoupled
  from generation (a sim concern). Two noise layers, both via the same
  fractal/octave Perlin (`octaveNoise2D()`, adapted from
  `../AlphaCraft/src/world/PerlinNoise` but reseeded through `wsim::Rng`
  instead of `std::default_random_engine`, to match the project's RNG
  convention) — no second algorithm needed: a low-frequency/high-amplitude
  layer shapes continent-scale land/ocean, a higher-frequency layer on top
  adds local hill/valley detail. `kHeightBias` shifts the result so most of
  the map is land (not a 50/50 split — empirically ~8-28% water across
  seeds at the tuned constants, checked via `terrain_tests.cpp`), since a
  world meant for future settlements needs somewhere to put them.
  `Terrain::kSeaLevel` (`0.0`) is the water threshold; `isUnderwater(x, z)`
  is just `heightAt(x, z) < kSeaLevel`.
- **Chunks + LOD** (`engine/core/TerrainChunks`): the world is not bounded by
  a pre-generated square -- `heightAt()` is pure, so `TerrainChunks::update()`
  (every frame from `render()`, with the camera pivot) keeps the needed chunks
  loaded. A level-L chunk covers `32<<L` units but always has 16x16 cells
  (cell 2/4/8/16 units, `kMaxLevel=3`), so far terrain is coarser and the
  chunk count stays small: view radius 64 chunks (~2000 units) is ~770
  chunks, whereas a flat grid would be ~12 800 -- far beyond the driver's
  `maxMemoryAllocationCount` (~4096), since `GpuBuffer` does one
  `vkAllocateMemory` per buffer. The needed set is a quadtree around the
  camera (`collect()`: a node splits while closer than `4 * its size`, leaves
  tile the world with no overlap). Cracks between different levels are hidden
  by skirts (vertical strips dropped `cell` units along every chunk edge).
  Transitions are hole-free: an on-screen chunk that is no longer needed is
  kept until everything replacing it is built, and the new chunks under it are
  hidden meanwhile (no z-fight). At most 4 builds per frame, nearest first;
  empty map (start / new world) builds everything at once. Unloaded meshes are
  destroyed after 6 frames (GPU may still use them). Water is ONE shared unit
  quad mesh, a `Transform` per chunk with sub-sea-level vertices. Normals are
  central differences of `heightAt()` itself, colour uses a fixed height range
  (`kColorLow/High`), so same-level seams are continuous. "Новый мир" on a
  started game reseeds `m_terrain` and calls `TerrainChunks::reset()`.
  Gotcha: Perlin permutation table is 256-periodic, so the finest detail
  octave repeats every ~266 units (not noticeable so far). `World` is still
  bounded to `kWorldHalfSize` -- creatures live in the central square only;
  `Terrain::halfSize()` is now just that nominal playfield size.
- **Auto view radius** (`Application::updateAutoViewRadius`, `Settings::
  autoViewRadius`, on by default; `Settings::viewRadius` 4..64 is then the
  cap): start radius by GPU class (discrete 24 / integrated 10 / other 8);
  smoothed frame time > 1/54 s for 1 s -> radius x0.85 and remembered as a
  ceiling; < 1/57 s for 4 s -> +1 chunk up to the cap/ceiling; ceiling is
  forgotten after 45 s. Frames where chunks were built are ignored. With
  vsync the frame time can't show GPU headroom, so growth only stops when FPS
  actually falls below ~54.
- **Water** is opaque (no alpha blending in the scene pipeline -- `fragColor.a`
  is a "concrete" flag); per-chunk quads at `kSeaLevel`, depth-test does
  the shorelines.
- **Winding order**: `cullMode=BACK`, `frontFace=COUNTER_CLOCKWISE`, and
  `Camera.hpp` flips `proj[1][1]` for Vulkan NDC, so triangles must be CCW
  *as seen from outside* (from above, +Y, for terrain/water). The terrain/
  water grids and the creature pyramid's sides originally had inverted
  winding — everything was culled from above and the camera appeared to be
  "inside" the world. `TerrainChunks::build()`/`buildCreatureMesh()` index order is load-bearing; check it first if
  geometry ever looks inside-out again.
- **Directional light** (`LightsData` in `engine/renderer/Lights.hpp` /
  `LightsUBO` in `assets/shaders/include/lighting.glsl`): the lighting model
  was ambient + up to 8 point lights + one camera-following spotlight, with
  no sun/directional concept — added `sunDir` (already-normalized direction
  *to* the light, not a position) + `sunColorIntensity` (`w=0` disables it,
  same convention as `spotColor.w`) as two more `vec4`s, mirrored field-for-
  field in both the C++ struct and the GLSL UBO (std140-safe since
  everything in both structs is already `vec4`-sized, no padding to get
  wrong). `shade()` in `lighting.glsl` adds a sun term reusing the same
  `blinnPhong()`/`shadowFactor()` hooks the point/spot lights already use —
  free ray-traced soft shadows in `scene_rt.frag`'s RT variant, always-lit
  no-op in the rasterized `scene.frag` variant, exactly like the existing
  lights, no new shadow code needed. `Application`'s constructor sets a
  fixed sun angle/color once alongside `m_lights.ambient` — no day/night
  cycle (out of scope).
- **Known gaps** (deliberately out of scope this round; biomes since added, see "Biomes"): **no rivers** —
  agreed in conversation to be a separate round (needs a flow/erosion pass,
  not just more noise, to look like real connected downhill water); no
  brain-level terrain awareness (see "Creature terrain physics");
  no day/night cycle (sun angle is a fixed constant).

## Rendering: shadows, materials, sky (`VulkanContext`, `assets/shaders/`)

- **Frame order**: `beginFrame` (cmd buffer only) -> `updateCamera`/`updateLights` ->
  per cascade `beginShadowCascade(i, lightVP)` / `drawShadowMesh` / `endShadowCascade(i)` ->
  `beginScenePass` (what `beginFrame` used to open) -> `drawMesh` -> `beginUIPass` -> `endFrame`.
  Shadow passes can be skipped entirely (`shadowParams.x = 0`).
- **Sun shadows = cascaded shadow maps** (`Shadows.hpp`, pure glm, `shadows_tests`): 4 cascades x
  2048^2 D32 array, depth ranges 25/80/240/700 along view depth (`kCascadeFar`), each cascade
  is a bounding-sphere ortho fit (rotation-stable) snapped to the texel grid, light box
  extended 400 units toward the sun for off-screen casters. Casters: terrain chunks
  (`TerrainChunks::shadowCasters()`, no water) + creatures, culled per cascade with the
  light frustum. `shadow.vert` depth-only pipeline (no culling, dynamic depth bias).
  Sampling is `sunShadow()` in `lighting.glsl` (only when the shader defines
  `WSIM_RASTER_SHADOWS`, i.e. `scene.frag`): normal-offset + slope-aware depth bias
  derived from the cascade matrix, 12-tap Vogel-disk PCF rotated per pixel on top of
  the hardware 2x2 compare sampler, fade-out at the last split. Other includers
  (RT variant, path tracer) get a ray-based `sunShadow` via their `shadowFactor`.
  The shadow image lives in `READ_ONLY` layout between frames; descriptor binding 4.
  Settings: `Settings::shadows` ("Тени" toggle). **The old "ray-traced shadows" setting is
  gone**: `RtSystem::build` is never called by this game (that machinery is inherited
  from Podjezd), so `setRayTracedShadows` was always a no-op here.
- **Materials are procedural** (`include/materials.glsl`), selected by the vertex alpha
  (`fragColor.a`): 0.25 terrain, 0.5 water, 0.75 creature, else plain vertex colour. Set in
  `TerrainChunks::build`, the water quad and `buildCreatureMesh`. Everything is a function of
  world position (3D value noise), so LOD levels and chunk seams match. Terrain: grass
  (two tones + dry patches + bare dirt), beach sand by height, rock by slope/height with
  strata, snow above ~10-13 units (raised mountains turn white), micro-bump from a noise
  gradient, detail fades with distance (avoids shimmer). Water: animated normal ripples,
  Fresnel mix with the sky colour, sun glint, depth-patch tint (it's an opaque plane, so no
  real depth/foam). Creatures: spotted albedo. The old Podjezd "concrete" material is gone.
- **Sky/fog**: `kSkyColor` in `Application.cpp` is both the fog colour (density 0.0009,
  height falloff 0.015) and, run through the same tonemap+gamma as `tonemapEncode`, the
  clear colour (`setClearColor`), so distant LOD terrain dissolves into the horizon. Sun is
  ~44 degrees above the horizon (`m_lights.sunDir`).
- **Debug screenshot**: `VulkanContext::requestSceneDump(path)` copies the offscreen target to a
  host buffer in `beginUIPass`, `endFrame` waits for the fence and writes the PPM.
- **Far-cascade cache** (optimization, `Application::render`, `ShadowCache`): cascades 2 and 3 are re-rendered only every 4 / 8 frames (or when the camera sphere moved > 10% of its radius, the sun turned > ~1.4°, or the world changed —
  `m_shadowForce` from terrain/tree edits and new worlds); in between the old depth maps and their matrices are reused (`VulkanContext::beginShadowPass(preserve)` keeps the image contents; barriers moved out of the cascade calls into
  `begin/endShadowPass`). Cascades 2/3 get a 30% larger sphere (`computeCascades(..., inflate)`, results also expose `center/radius`) so a small camera drift stays covered. Measured (seed 11, camera at 120): shadow draws **704 → ~90-110**
  per frame, CPU render 4.05 → 3.4 ms, pixel diff vs `WSIM_SHADOW_FULL=1` 0.03% of pixels > 8/255. `WSIM_SHADOW_FULL=1` disables both the culling and the cache.
- **Shadow draw culling** (`Application::render`): far cascades skip what can't be seen there — cascade 2 drops terrain
  chunks whose top is below 2.5 units, cascade 3 below 7 and also trees/creatures/food (texel ~0.7). A caster missing
  from the depth map only loses its own shadow, so flat lowland is safe to skip. Trees are a separate list
  (`TerrainChunks::treeCasters()`). Measured: ~1500 → ~500 shadow draws, 540 → 670 fps, pixel-identical on 3 seeds.
- Known gaps: no shadow-quality setting (map size is the constant `kShadowMapSize`), no
  real water depth/foam, no sky gradient/sun disc/clouds, shorelines are polygonal (cell 2 mesh
  vs the flat water plane), shadows of creatures are small pyramids' shadows.

## Biomes (`engine/sim/Biomes.hpp`, `Terrain::climateAt`, `Trees.cpp`, `TreeGen.cpp`, `materials.glsl`)

Seven biomes from a two-axis **climate**, all pure functions of the terrain seed (nothing stored or saved, loads and
new worlds just rebuild chunks; player terrain edits never move a biome border):
- **`Climate{temperature, moisture}`** in 0..1: `Terrain::climateAt(x,z)` = two independent 2-octave Perlin maps
  (`kFreq=0.0032`, patches ~300 units, so the 200x200 creature field holds 1-4 biomes), stretched by `0.5+0.5·tanh(4.47·n)`
  (a plain clamp left ~5% of the map at exactly 0/1 → solid arctic). **Altitude cools**: `withAltitude(c, h)` lowers
  temperature by `0.035` per unit above `h=3`, so mountains turn taiga → tundra → snow on their own.
- **`classify`** (Whittaker, simplified): T<0.24 cold (M<0.40 Tundra, else Taiga); T<0.68 temperate (M<0.55 Plains, else
  Forest); hot (M<0.35 Desert, <0.60 Savanna, else Jungle). Measured shares over 6 seeds: 8-25% each (plains largest, savanna
  smallest); `terrain_tests` keeps every biome ≥4% and ≤50%. `Terrain::biomeAt(x,z)` includes the altitude term;
  `biomeName` gives the Russian name (debug window shows "Biome: … (T, M)" at the camera target).
- **Rendering**: terrain vertex colour now carries the climate (`r`=T with altitude, `g`=M, `a`=0.25 material code) —
  the old vertex gradient was unused. `terrainMaterial(P,N,dist,climate)` blends a 3×3 palette (`groundPalette`: cold /
  temperate / hot × dry / mid / wet) so borders are smooth colour ramps, adds desert dune ridges, and snow now comes from
  temperature (`smoothstep(0.01,0.07, T+noise)`) instead of a fixed height; beach/rock logic unchanged.
- **Trees by biome** (`Trees.cpp`): per cell the biome is read from the base climate jittered ±0.04 (borders are ragged, not
  ruler-straight) and sets density and species — Forest 1.0× oak/birch (+6% pine), Taiga 1.3× **spruce** (tiered cones) with
  10% birch and **pine** (tall bare trunk + flat crown cap) 5-40% growing as moisture drops (pine barrens on the dry side),
  Jungle 1.15× big oak (scale ×1.25) + 30% palm, Plains 0.35× oak/birch (+12% pine), Tundra 0.35× dwarf spruce (20% pine),
  Savanna 0.3× acacia, Desert 0.14× cactus (the sparse biomes get a small floor chance so there are lone trees). `suitable()` then applies the altitude climate: below
  `kMinTemperature=0.07` no trees (ice), cold oak/birch become spruce, heat-loving species die out above their zone; conifers
  may climb to height 13 (others 10). New models in `TreeGen.cpp` (`TreeSpecies::{Spruce,Acacia,Cactus,Palm,Pine}`, 4 variants each,
  enum value = species number in the model salts, so `Pine` is last;
  same flat-shaded style; cactus arms are straight prisms + elbow blob because a tube bent 90° twists its quads — the
  `checkMesh` normal test catches that); `TerrainChunks` bakes all `kTreeSpeciesCount` templates.
- Birds are rarer in desert (0.2×) and tundra (0.4×). Debug: `WSIM_CAM_AT=x,z[,dist[,pitch]]` (e.g. `0,0,200,-50` for an
  overview), `WSIM_CAM_TREE=oak|birch|spruce|pine|acacia|cactus|palm`.
- Tests: climate determinism/range/edit-independence/lapse/thresholds/biome shares (`terrain_tests`); per-biome species sets
  and density order on "interior" points over 6 seeds (`trees_tests`); every new model passes `checkMesh` (`procgen_tests`).
- Not done: creatures/food do not care about biomes yet (bushes, regrowth, metabolism, speed in snow/sand), no rivers or
  lakes tied to moisture, no swamp/mountain/beach biomes (beach and rock are still slope/height rules), no biome-specific
  water colour/ice or fog, no seasons, houses/buildings ignore the biome, biome borders are not shown to the player
  (no map/overlay).

## Berry bushes (`engine/sim/Berries.*`, `World::growBerryBushes`, `Powers`, `Application`)

Food bushes are now biome-aware and come in eleven berry kinds (six safe ones, five with different harmful effects — see "Poison, effects and taste memory" below) (tests: `berries_tests`, plus bush cases in `powers_tests`,
`save_tests`, `mods_tests`):
- **Kinds** (`BerryKind`; the list below is the six safe ones): Raspberry (the baseline), Blueberry (0.85× nutrition, 1.35× regrow), Blackberry (1.3×, 0.75×),
  Cloudberry (1.5×, 0.5× — richest and rarest), Tropical (1.15×, 1.2×), Cactus fruit (0.9×, 0.6×). `BerryDef` also has the
  berry/leaf colours and the mod id. `FoodSource::kind` (default Raspberry, so `World` without a terrain — tests, the menu
  colony — behaves exactly as before). **Nutrition** multiplies the hunger relief per bite (`self.feed(bite * nutrition)`),
  **regrow** multiplies `kFoodRegrowRate`.
- **Where they grow** (`pickBerryKind`, weights per biome): tundra cloudberry+blueberry, taiga blueberry/cloudberry/raspberry,
  plains raspberry/blackberry/blueberry, forest blackberry/raspberry/blueberry, savanna cactus fruit+tropical, desert cactus
  fruit, jungle tropical+blackberry. `bushDensity(biome)` scales how many (jungle 1.25×, forest 1.0, plains 0.85, taiga 0.7,
  savanna 0.5, tundra 0.35, desert 0.25).
- **Generation (superseded by "Unbounded living world" below: bushes are now procedural per cell, streamed and unbounded; the text here describes the first version)** `World::growBerryBushes(terrain)` (called after `settleOnLand` at start and on "Новый мир"; replaces the old
  uniformly random 30): a jittered 18-unit grid over the playfield, each cell becomes a *patch* of 1-3 bushes of one kind (radius 3.5)
  with chance `0.24 * density(biome)`; land only (height > 0.4), spacing ≥ `kBushSpacing=1.6`, at least 18 bushes, cap
  `kMaxFood=150`. Uses the world RNG (reproducible, saved). Measured 26-46 bushes per seed (the old world had 30); a 20-minute
  headless run on 6 seeds grew populations to ~50-200 (cap reached only on the richest seed, 52 bushes), so the ecology is in the
  old range but now depends on where you start.
- **God power "Кусты"** (`bush`, key 7; hold to plant in the brush circle, kind by the biome of each point; `GodContext::bush`,
  `plantNaturalBush`) and **"Стереть"** now also removes bushes. **"Осмотр"** falls back to the nearest bush: a panel with the
  kind, a berry bar, nutrition/regrow factors (`drawBushInspector`).
- **Mods**: JSON type `bush` (`berry`: kind id, optional) and Lua `ctx:bush(x, z [, kind])`, `ctx:erase_bushes(x, z, r)`; the
  mods README lists the ids.
- **Saves**: new optional section `BRRY` (kind per bush, in `WRLD.food` order). Old files lack it → every bush is a raspberry
  bush; an old game ignores it. Not covered by a test: loading a genuinely old file (we only write new ones).
- **Rendering**: one mesh per kind (`buildBlobMesh` with the kind's leaf colour, berry colour and berry-face share —
  blueberries many small faces, cloudberry few), shrinks as it is eaten. Debug: `WSIM_CAM_BUSH=<kind id>`, `WSIM_INSPECT_BUSH=1`.
- **Poison, effects and taste memory** (`Creature::ingestBerry/tasteBerry/berryAppeal`, `World::tick` food choice, save v4):
  - **Eleven kinds** (`kBerryKindCount=11`): the six safe ones plus five harmful, each with its own *effect profile* in
    `BerryDef` (`lethal`, `diarrhea`, `vomit`, `hallucination`, each a share of a full dose): **Nightshade** (Волчья ягода: death,
    +0.15 hallucination), **Baneberry** (Вороний глаз: vomiting + diarrhea), **Buckthorn** (Крушина: diarrhea), **Honeysuckle**
    (Жимолость: vomiting), **Henbane** (Белена: hallucinations). `BerryDef::harm()` = `2·lethal + 0.9·diarrhea + vomit +
    0.6·hallucination` is how bad a kind tastes in memory. Harmful kinds are ~15-20% of a biome's weights (`Berries.cpp`).
  - **Effects** (all in `Creature::tick`, dose per bite `bite·share·4`, levels 0..2, saved, never inherited):
    `toxin` — health -0.02/s and energy -0.01/s per unit, decays 0.03/s, death with toxin>0.05 is `deathCause 4`
    (`WorldStats::deathsPoison`); `diarrhea` — hunger +0.03/s and energy -0.012/s per unit, speed ×(1-0.3·level), decays 0.025/s,
    never kills by itself; `nausea` — at level 1 a **vomiting fit**: `vomitStun` 1.6 s (stands still, `Behavior::Sick` "Тошнит", no eating/mating),
    hunger +0.22 (empty stomach), toxin halved, nausea 0.25; a creature with nausea > 0.6 or in a fit ignores food;
    `hallucination` — decays 0.02/s; with probability ≤0.5 per half-second `visionTimer` 1-2.5 s: it sprints in a random direction
    from a phantom (`Behavior::Flee`), fear ≥ 0.7 and **alarm call 0.8 so neighbours panic for real**; otherwise the heading
    drifts by up to ±0.8·level rad. `ingestBerry(def, bite)` applies a profile; `addToxin` stays public as the hook for future
    deliberate poisoning (nothing uses it that way yet).
  - **Taste memory is per LOOK, not per plant** (`BerryLook`: Red, Blue, Dark, Orange, Violet, Pink, White; `taste[7]`, `tasteSamples[7]`):
    mimics share a look — blackberry/nightshade (dark), blueberry/buckthorn (blue), raspberry/honeysuckle (red), cloudberry/henbane
    (orange) — so a creature that loved blackberries happily takes nightshade, then distrusts *all* dark berries (tested: `fan`).
    Each eating tick moves the look's taste toward `nutrition - harm` (~3/s × plasticity factor, **bad news 3× faster**); taste fades
    to neutral 0.5 at 0.002/s (~13 min); unknown look attracts with `0.6 + 0.5·curiosity`.
  - **Choice** (`World::tick`): `max appeal / (1 + dist/12)`; look with appeal < `Creature::kMinAppeal=0.15` is ignored except by a
    creature with hunger > 0.85, which accepts anything above -0.4 (never what it knows kills). One kind in the world → old
    nearest-bush behaviour.
  - Measured (20 min, 6 seeds, all kinds): populations 33-163, poison deaths 0-1.
  - **UI**: creature inspector has a "Яд" bar, "Самочувствие: яд, понос, рвота/тошнит, галлюцинации" and "Любит <вид> · обходит <вид>"
    (look names); bush inspector lists effects ("смертельна · рвота · понос · галлюцинации" / "безопасна"). **Saves v4**
    (`kSaveVersion=4`): v3 = toxin only (v3 taste records were per plant and are ignored on load), v4 adds look-group taste and the
    effect states; v2/v3 files load with the new fields zeroed (tested). Tests: `creature_tests` (profiles, mimic learning, each effect,
    non-inheritance), `intelligence_tests` (poison-only bush, poison+cloudberry, each harmful kind's effect and survival, hallucination
    panic spreading), `save_tests`, `berries_tests`.
  - **Social learning** (`World::tick` pass 3b; `Creature::observeLook`, `mealLook/sinceBite/sickEvent`): a creature remembers the look
    and time of its last meal. A *sick event* — toxin > 0.3, diarrhea > 0.4, hallucination > 0.4 (crossing, in `ingestBerry`), a
    vomiting fit, or **recoiling** (its own taste for the look dropping below `kMinAppeal`) within 25 s of the meal — makes every
    creature within 12 units (not itself) learn that LOOK is bad: `observeLook(look, -1, 0.9·w)` with
    `w = (kin/partner/parent/child ? 1 : 0.35) · (0.5+sociability)/1.5 · (1 - 0.5·dist/12)`; strangers are believed less, kin more,
    sociable ones more. Watching someone eat calmly within 8 units nudges **unknown** looks (or mildly liked ones, until 150 ticks)
    toward the nutrition value (`0.06·w` per tick; a bad opinion is never flipped by watching). Tested: watcher who never ate
    distrusts the look, far creature knows nothing, stranger trusts less than kin, calm eating raises appeal.
  - **Grudge and revenge** (the "revenge" satisfier the resentment need was missing): an envious creature (`envy > 0.35`, `resentment > 0.3`) takes
    the neighbour with the most visible `dominanceDisplay` (> 0.55) as `grudgeId`; `grudge` grows `0.05·resentment/s` while the display goes on,
    fades 0.003/s otherwise, and clears when the target dies. `Behavior::Sabotage` ("Мстит", score `2.4·(grudge-0.25)·max(0.1,1.3-empathy)`,
    needs grudge > 0.5, health > 0.5, hunger < 0.6): go to the nearest bush whose LOOK it knows as bad (taste < -0.3 — by look, so it can
    pick a harmless mimic), **pick** 0.12 of its berries (`carryKind/carryAmount`, lost after 45 s), walk to the grudge target and **serve**
    within 2.6 units: the victim gets `ingestBerry` with that dose and learns the taste (and observers see it), the avenger's grudge -0.6
    and resentment -0.6, `WorldStats::sabotages++`. Pick/serve are requested by `Creature::tick` (`pickRequest/serveRequest`) and executed by
    the world, like eating. Empathic creatures (empathy → 1) essentially never do it (tested). It is **rare**: 1 act in 6 × 20 simulated
    minutes on the probe seeds — it needs an envious creature, a visible rival and a learned bad look — so expect it as an occasional story,
    not a mechanic that reshapes the ecology.
  - **Inspector**: "Мстит" as behaviour, "Зуб на №N (x%) · несёт ягоды". **Saves v5** add last meal, grudge and carried berries after the v4 block
    (v2-v4 files load with them zeroed; tested).
  - Not done: creatures can't learn to tell mimics apart, no deliberate *lethal* planning (the dose is one pick), victims don't retaliate or
    remember who poisoned them, no gossip beyond direct observation, no antidotes/healing herbs, hallucinations have no visual effect, taste is not passed on.
- **Not clustering around bushes** (foraging dispersal; measured with a headless probe over seeds 5/11/29/1234, 10 simulated minutes). Symptom
  reported: creatures "just stand around the berries". Measured before: 50-97% of the population within 6 units of a bush, 30-45% of near-bush
  time in `SeekFood`, **56-71% of near-bush time at an EMPTY bush** (crumbs regrew in 3 s, so everyone queued for the trickle), 26% resting
  under the bush; the world was food-saturated (every bush fed ~1 creature, population always hungry). Changes:
  - **Regrowth** is no longer linear: `da/dt = 0.055 · regrow · (0.25 + 0.75·amount)` — an emptied bush stays barren ~7.5 s and needs ~34 s to
    refill (`kFoodRegrowRate`), so there is nothing to queue for.
  - **Crowd penalty** in bush choice: every other creature within 3.5 units of a bush divides its score by `1 + 0.4·n` (`occupancy[]`, computed
    once per tick) — hungry creatures spread over bushes instead of all taking the nearest.
  - **Dispersal** (`Behavior.cpp: beginExplore`, `Creature::exploreTimer`, saved in v6): when a creature stops eating (full / bush empty) or
    arrives at a remembered spot that is empty it heads away from it for 14 s on a nearly straight course (Wander gets +0.6 while exploring;
    SeekFood without a target walks straight, `wander(0.35, 0.65)`, to new places instead of circling).
  - **Rest away from food**: a tired creature within 7 units of food walks off (speed 0.35) before lying down; **separation** is weakened
    (×0.3) near the target of SeekFood/SeekMate, so the arrivals aren't pushed back out of the 2-unit eating radius.
  - **Births need real abundance**: `canMate` hunger ≤ 0.4 (was 0.65). This is what makes the population settle below the food limit.
  - Measured after: 8-35% within 3 units (was 30-75%), 14-20% of near-bush time at an empty bush (was 56-71%), resting under bushes 7-16%
    (was 26%); the stationary near-bush time is now mostly legitimate eating. **Side effect:** populations settle at ~20-100 instead of 80-170
    (starvation deaths roughly halved, mean hunger ≈ 0.35-0.45). Tests: `intelligence_tests` (two-bush crowd split, rest-away, empty-bush
    exploration), `berries_tests` (regrowth law), `save_tests` (v6/v5 compat).
  - Not done: creatures keep one remembered bush, not a map; no seasons or day/night rhythm that would move them elsewhere (sleep, shelters, houses
    are still decor); no territory use besides `GoHome`.
- Not done: berries aren't tied to season, no bush-planting by creatures (seed dispersal), bushes don't care about moisture
  inside a biome, no bush sounds beyond a pop, trees/houses spacing from bushes is only the existing tree blocker.

## Wildlife (`engine/sim/Fauna.*`, `procgen/AnimalGen.*`, `World`, `Application`)

Wild animals — a separate subsystem from the genome/brain "creatures" (the Norn-like protagonists). Simple innate behaviour, biome-bound like
berries and trees. Tests: `fauna_tests` (species table, population, flight, hunting, creature bites, breeding/caps, starvation, migration, water),
world-level cases in `intelligence_tests`, `save_tests` (FAUN section, bit-exact continuation with wildlife), `procgen_tests` (models), `powers_tests`,
`mods_tests`.
- **Species** (`Species`, `SpeciesDef`, `speciesDef()`): herbivores **Hare, Deer, Antelope, Tapir** and predators **Wolf, Fox, Lion, Jaguar**. Each has
  size, speed, sense/panic radius, group size, per-biome presence weights, population cap, maturity, breed cooldown, lifespan, prey mask, whether it
  hunts creatures, whether it swims. Habitats: hare plains/forest/taiga/tundra, deer forest/plains/taiga, antelope savanna (+desert/plains), tapir
  jungle, wolf taiga/tundra/forest, fox plains/forest/taiga/tundra, lion savanna, jaguar jungle (`pickSpeciesFor`; predators are ×0.12 as likely
  as herbivores in a biome pick).
- **`Fauna`** (no dependency on `World`; gets `CreatureView`s, returns `Bite`s; deterministic through the world RNG): `populate` (14 herbivore groups by
  biome, then one pack per predator species if ≥ 3 prey exist and 85% roll), `spawn/spawnNatural/removeInCircle/nearest`, `tick`, `exportState/importState`.
  `kMaxAnimals = 160`, caps per species (hare 14, deer 10, antelope 12, tapir 5, wolf 4, fox 3, lion 3, jaguar 2). Settles at ~40-50 animals.
- **Herbivore AI**: nearest predator within `panic` (or a creature within 0.3·panic) → **Flee** (full speed, stamina 0.07/s, 2.5 s after the threat is
  gone); hunger > 0.55 → **Graze** (hunger -0.06/s anywhere — grass is not a modelled resource) until 0.2; else Wander (herd cohesion towards the centre
  of same-species within 18 units if > 7 away) alternating with Rest. **Predator AI**: hunger grows 0.0025/s; > 0.5 → picks the nearest prey animal in
  `sense` (else, with `huntsCreatures` and hunger > 0.7, a creature within 0.8·sense), **Hunt** up to 25 s at full speed (stamina -0.12/s, exhausted =
  0.3× speed), gives up beyond 1.4·sense (15 s search cooldown); in reach it attacks: animal prey is killed with probability 0.55 per 0.7 s attempt
  (hunger -0.55, **Feed** 6 s); a creature is **bitten** for 0.28 every 1.1 s (`Bite` → `World`), and when the creature vanishes within 4 s of a bite
  the wolf counts the meal (hunger -0.5). Foxes never hunt creatures; a predator with animal prey nearby never goes for a creature.
- **Life cycle**: hunger 1 → health -0.012/s → starvation death; lifespan death; mature (age ≥ maturity), hunger < 0.4 (0.45 predators), health ≥ 0.7,
  cooldown over, under the species cap, a mature same-species mate within 9 units → birth after ~6 s on average (hare litters of 1-2); **immigration**
  every 25 s: a herbivore species below `max(2, cap/6)` gets a new group at the playfield edge in a suitable biome (70%), a missing predator comes (10%)
  only if ≥ 6 prey animals exist. Movement: look-ahead around water/cliffs/trunks (`Trees::blocksPoint`) **plus a hard block** on each step (an animal never
  enters water unless it swims — the look-ahead alone let hares wade), soft playfield bounds, separation within 1.4·size.
- **World integration**: `World` owns `Fauna m_fauna`; `growWildlife(terrain)` at start and on "Новый мир"; `Fauna::tick` runs first each tick;
  **bites** reduce `Creature::health` (fear = 1) and a killed creature gets `deathCause 5` (`WorldStats::deathsPredation`). Creatures **sense predators
  as threats** through the existing hazard channel: a hunting predator is felt from 28 units, a calm one from 12 (`kPredatorSense/kPredatorNear`) —
  fear, Flee, alarm call, herd panic all come for free. Saves: new `FAUN` section (older files simply have no animals; immigration refills them).
- **Measured** (20 min, 8 seeds, all systems on): ~39-49 animals, 11-40 animal kills, 4-43 creature bites and **1-12 creatures mauled** per world; creature
  populations stay in their usual 20-80 range. Also fixed while measuring: `growBerryBushes` could make 45% of bushes poisonous in small worlds (one seed
  starved to extinction) — poisonous bushes are now capped at a quarter.
- **Rendering**: `AnimalGen::buildAnimal(species)` — flat-shaded low-poly (body + belly blobs, 4 tapered-prism legs with dark hooves, neck, head, snout, ears,
  tail; deer antlers, antelope horns, lion mane, jaguar spots, fox/wolf bushy tails, tapir snout/saddle), model faces +Z, feet on y = 0. `Application` builds
  one mesh per species, yaw from `heading`, juveniles smaller (`0.55 + 0.45·age/maturity`), a little bob when running, `kAnimalVisualScale = 1.6` (true
  sizes are too small from the god camera), shadows and temporal-AA motion vectors (`trackId` = animal id) like creatures. Debug: `WSIM_ZOO=1` (one of each
  species in a row at the camera pivot), the debug window shows "Animals: N (herbivores, predators), kills, bitten, mauled".
- **Tools**: god power **"Звери"** (key 5; `GodContext::animal`, biome-appropriate species per point; "Стереть" removes animals too; the 10th power is on key `0`),
  **"Осмотр"** picks the nearest animal/creature/bush (`drawAnimalInspector`: species, diet · state, health, hunger, age); mods: JSON type `animal`
  (`species`: hare, deer, antelope, tapir, wolf, fox, lion, jaguar), Lua `ctx:animal(x, z [, species])`, `ctx:erase_animals(x, z, r)`.
- Not done: **no animal sounds** (howls, bleats), no leg/head animation (models are rigid, only a bob), no sexes/antler growth, animals ignore berries, houses and
  boulders (no flee from hazards), creatures don't hunt, herd or domesticate animals, no corpses/scavengers, no fish/birds/swimming species except tapir and
  jaguar wading, predators don't coordinate as packs, no day/night rhythm for hunting.

## Unbounded living world (streamed bushes, wildlife and creatures)

Until this round everything "alive" (bushes, creatures, animals) lived in a bounded 200x200 start square (`World::kWorldHalfSize = 100`) while terrain and trees were
infinite pure functions of the seed — so berries stopped at the square's edge and creatures were pushed back from it. Now the simulated world has no edge:
- **Bushes are a pure function of the world** (`Berries::proceduralBushes(terrain, cx, cz)`, cell `kBushCell = 18`): per cell a hash-driven patch of 1-3 bushes of one kind
  (chance `0.26·bushDensity(biome)`, biome from `classify(climateAt(centre))` — *without altitude*, so terrain edits never change which berry grows where; poisonous kinds
  are re-rolled 60% of the time → ~8% of bushes, tests require < 15%). Each candidate has a `local` index (0-2) that does not depend on terrain edits; a candidate on water
  just isn't materialized. Tests: determinism, land only, biome fit, density 0.12-1.0 bushes/cell, edit-robust indices.
- **World materializes only awake cells** (`World::streamBushes`, every 0.5 s): cells within 54 units of any creature and 110 of the camera (`setFocus`, set by `Application` each
  frame from the camera pivot) are loaded (`loadCell` → `FoodSource{cellX, cellZ, local}`); a cell asleep for good (beyond radius + 26) is unloaded (`unloadCells`) leaving only a
  **`CellRecordData`**: eaten bushes (`local → amount`), erased ones (`amount < 0`), player-planted ones, and the world time it fell asleep. On waking an eaten bush regrows in closed
  form (`regrownAmount`, the same `da/dt = r(0.25+0.75a)` law) for the time it slept; erased bushes stay erased; planted bushes return. `plantBush` makes `local = -1` bushes owned by the
  cell of their position; `ensureBushesAt` wakes cells before planting at a far cursor. `kMaxFood` is now 8000 (materialized bushes). `bushNear(terrain, x, z, r)` answers the tree
  blocker from the procedural list (small cache) so trees never grow where a sleeping cell's bush will be.
- **Creatures** have no bounds in a streaming world (`senses.worldHalfSize = 0`; a World without terrain — unit tests — keeps the old soft edge). Measured (20 sim-min, 4 seeds):
  creatures reach 120-230 units from the start, 40-140 alive, ~300-450 awake cells, 60-150 bushes, 60 animals, **0.17-0.5 ms per tick** at release.
- **Wildlife** (`Fauna` with `halfSize <= 0`): the start square is populated as before and its 48-unit cells are marked *explored*; around creatures and the camera (reach 96) every
  newly seen cell is populated once (80% a herbivore group by biome, 6% a predator pack if ≥ 3 prey animals within 90 units) and remembered (`State::explored`); a dying species is refilled
  by a ring 80-110 units from a random focus point. Species caps grew (hare 20, deer 14, antelope 16, tapir 7, wolf 5, fox 4, lion 4, jaguar 3), animals far from everyone keep simulating.
- **Saves**: new `STRM` section (streaming flag, world time, loaded cells, sleeping cell records, origin `(cell, local)` of every materialized bush, in `WRLD.food` order) and
  `FAUN` now ends with the explored cells. An old save (no `STRM`, or counts that don't match) loads its bushes as player-planted and marks the start square loaded, so procedural
  bushes don't duplicate there; elsewhere the world streams normally. Tested incl. bit-exact continuation (`save_tests`).
- `growBerryBushes(terrain)` now means "switch to streamed bushes and wake the start area" (it replaces the old random 30/jittered grid; `kMinBushes` is gone — a barren start just
  means the creatures explore). Debug window: "Food sources: N (awake cells M)".
- Not done: creatures far from the camera still simulate every tick (no level of detail for the sim); animals have no sleep state and do not despawn far away; houses, boulders and
  terrain edits are not streamed (they are global lists / sparse tiles); the creature `homePosition` pull keeps most of them near their birthplace (mean distance ~80-100).

## Resources and tools (stage 1 of civilization: logging, spear, shield)

Creatures now cut trees down, carry logs, craft tools and fight predators with them. Code: `Creature` (fields), `Behavior::Chop/Craft`, `World::tick`, `Terrain::fellTree`, `Trees::nearest`, tests in `resources_tests`.
- **Felled trees**: trees are a pure function of the seed, so a felled tree is stored as a *set of `Trees::kCell` cells* in `Terrain` (`fellTree/treeFelled/exportFelled/importFelled`, next to the edit tiles; `Trees::candidate`
  returns "no tree" for a felled cell). `fellTree` returns the rect to rebuild; `World` queues it (`takeDirty()`), `Application` calls `TerrainChunks::invalidate` after each tick. **`World::tick` now takes a non-const `Terrain*`**.
  Saved in the `TERR` section tail (list after the edit tiles; old files without it load fine). `TreeInstance` got `cx, cz` (its cell).
- **Inventory/tools on `Creature`**: `wood` (max `kMaxWood=4`), `spearDur/shieldDur` (1 = new, 0 = none), `skills` mask (`kSkillSpear`, `kSkillShield`; *chopping is innate*), `dangerMemory` (120 s after seeing a predator;
  also set by a bite), tree-search cache (`treeKnown/treeCx/Cz/X/Z/treeSearch`, managed by the world). Save format **v7** (creature tail); everything is clamped on load.
- **Behavior**: `Chop` (score `0.55 + 1.3·danger`, only if it knows a recipe it lacks the tool for and needs wood, not hungry/afraid/tired) walks to the nearest tree (`World` searches `Trees::nearest` radius 34 every 1.5 s, only for creatures
  that want wood) and chops 3 s (`chopRequest`; yield 1-3 logs by tree scale); `Craft` (score `1.2 + 1.2·danger`) stands 4 s and turns 2 logs into a spear or 3 into a shield (`Creature::tick`).
- **Knowledge** (`World::tick`, hash rolls, no RNG consumption): inspiration `(0.0004 + 0.012·danger)·(0.5+curiosity)` per second (shield only after the spear, half rate); learning from a neighbour within 8 units
  `0.02·(0.5+curiosity)` per second, ×3 for family. No forgetting; children learn from parents by proximity (not inherited).
- **Combat**: `Bite` carries `attackerId`. A shield cuts bite damage to 35% and loses 0.34 durability per block; a spear stabs back for 0.5 (`Fauna::hurt`: predator flees for 6 s, dies at 0 health, deathCause 5) and loses 0.25 per hit.
- Measured (20 sim-min, 4 seeds): 20-60% of creatures know a recipe after 5 min, 30-290 trees felled, ~0-5 stabs (predators are rare), no population collapse (seed 11 climbs towards the 200 cap).
- Debug window: `Trees felled / spears / shields / stabs / blocks / learned`; inspector shows logs, tools and known recipes.
- Not done (next stages): warehouse/stockpile, barricades, traps, bridges, buckets (+ thirst), hunting for meat with the spear, visuals of carried logs/tools on the creature model, mods API for felling (`ctx:fell_trees`), replanting, sounds.

### Stage 2: bridges, barricades, traps, visible gear

- **Skills** grew to five (`kSkillSpear/Shield/Bridge/Barricade/Trap`, mask `kAllSkills = 31`): shield, barricade and trap need the spear first; the **bridge** is invented by hungry creatures whose heading ends at water
  (`0.006/s`, else `0.0002/s`). Teaching/inspiration is one table (`Learn[]` in `World::tick`); same neighbour/family learning as before.
- **Bridges live in `Terrain`**: a set of `kBridgeCell = 2` cells (`addBridge/bridgeAt/exportBridges/importBridges`, saved in the `TERR` tail after the felled trees). `Terrain::isUnderwater` is now
  "below sea level **and no planks**" — so creatures *and animals* walk over bridges with no other change; `surfaceAt(x,z)` = ground, or sea level + 0.14 on planks (creature/animal `position.y`, rendering).
  The water mesh is unaffected (chunks use `heightAt`).
- **`Behavior::Build`** (`Creature::buildRequest/buildProgress/buildTimer`, executed by `World::tick` after `Creature::tick`):
  *bridge* (score 2.6) when its goal (food target if hungry, else the exploring heading) meets water within 3.2 units and land lies within 18 beyond it → lays one plank (1 log, 1.2 s) on the first wet point, so it
  extends the bridge plank by plank as it walks on (measured: crosses a ~6 unit lake with 4 logs); *barricade* (2.4; 3 logs, 3 s, cooldown 25 s) when a predator is 4-26 units away, perpendicular to its direction;
  *trap* (2.4; 2 logs, 2 s, cooldown 40 s) while danger is remembered. `wantsWood` now uses `woodGoal()` (bridge-knowers keep 2 logs, danger-aware barricade/trap makers 3/2).
- **Structures** (`sim/Structures.hpp`, `World::placeStructure/structures`, cap 240, ≥1.6 apart, ≤3 of a kind within 8 units, never on water): *Barricade* — a 3.6 x 1.1 segment; **animals cannot cross it**
  (hard step block + look-ahead in `Fauna::tick`; creatures walk through — only animals are fenced); *Trap* — a **predator** within 1.1 units springs it: 0.45 damage, stunned 6 s (`Animal::stun`, not saved),
  trap consumed (`FaunaStats::trapsTriggered`). `Fauna::tick` got `std::vector<Structure>*`. Saves **v8**: `STRC` section (structures), bridges in `TERR`, creature `buildProgress/buildTimer`.
- **Visible**: `procgen/StructGen` (plank, barricade of 7 stakes + rails, spiked trap, spear, shield, 1-4 log bundles; `procgen_tests`). `Application` draws bridge cells (cached by seed+count), structures, and per creature a
  spear (right hand), shield (left) and carried logs (back) in the creature's frame (`m_gearObjects`, casts shadows). Inspector shows logs/tools/known recipes; debug window shows planks/barricades/traps.
  Debug: `WSIM_DEMO_CIV=1` (gear on all creatures near the origin + barricades, traps and a bridge over the nearest lake; `WSIM_CAM_AT` to look).
- Measured (20 sim-min, 4 seeds): planks 0-28, barricades 0-8, traps 3-82 (a few sprung), predator kills of creatures still 0-2 per run.
- Not done: **stockpile/warehouse** (logs are carried and crafted in place), buckets and thirst, barricade durability (predators don't attack it), creatures don't avoid their own traps (traps only affect animals),
  bridges never decay and cannot be removed by powers, meat hunting, mods API, building sounds, creatures do not plan routes using bridges (they only build when blocked).

### Stage 3: thirst, buckets, spear hunting, stockpile

- **Thirst** (`Creature::thirst`, 0..1, **only in a world with terrain** — flat test worlds never get thirsty): +0.0022/s (~7.5 min to full from 0); at 1 health drains 0.006/s (`deathCause 6`, `WorldStats::deathsThirst`);
  health regeneration pauses above 0.95. `Behavior::Drink` (score `0.5 + 2.6·thirst`, from 0.65, kept until 0.08 or a full bucket): `World::tick` finds the shore by 16 rays up to 44 units (cache `waterKnown/waterX/Z`, kept until
  reached; the point is 1.5 units before the water), drinking within 2.2 units of it lowers thirst 0.3/s. Without known water a thirsty creature wanders more. Measured: nobody dies of thirst, populations stay in the old range
  (an earlier 0.004/s rate caused an extinction on one seed — that is why it is slow).
- **Bucket** (`kSkillBucket`, 2 logs, `Creature::hasBucket`, `water` 0..3 sips, filled while drinking or topping up when `thirst > 0.25`): a sip from the bucket anywhere when thirst > 0.5 (-0.45); while eating, a bucket with water pours on a bush
  below 0.45 (+0.3, `bushesWatered`). Invented mostly while thirsty.
- **Spear hunting** (`Behavior::Hunt`, score `1.2 + 1.6·hunger`, hunger > 0.5, spear, no bush within 10): the world gives the nearest herbivore within 22 (`SenseInputs::preyId/preyDist`); within 2 units a stab (cooldown 1.2 s, `Fauna::hurt` 0.55,
  spear -0.06) — the second kills; a kill feeds 0.7 (`meatKills`). Animals still flee, so kills are rare (1-3 per 20 min on the probe seeds) — it is a supplement, not a food chain yet.
- **Stockpile** (`StructureKind::Stockpile`, `Structure::stock` up to 40, `kSkillStockpile` needs the spear; 3 logs, built at the stand near home when none within 60 units; one per 25 units): holders of the skill chop spare logs while the pile is
  below 24 and **deposit** what exceeds their goal (`Behavior::Haul`, `stockRequest` 1); anyone who wants wood and sees a stocked pile within 45 units **withdraws** instead of chopping (`stockRequest` 2). Counters: `logsStored/logsTaken`.
- **Saves v9**: creature thirst/water/hunt timer/bucket/water cache; new `STR2` section (structures incl. `stock`, kinds 1-3; the older `STRC` is still read). `kAllSkills = 127`.
- **Visible**: stockpile (platform + log stack + roof; height follows the stock), bucket in the creature's hand; inspector shows thirst, bucket and all seven recipes; debug window lists meat/buckets/watered/stock/thirst deaths.
- Not done: houses/shelters (the stockpile is the only community building; creatures do not live in it), sharing meat, bucket-watering by plan (only while eating), thirst of animals, wells/irrigation, droughts, animals carrying
  off stock, a second stockpile for a far-away group beyond the 25/60-unit rule being tuned, trade.

### Stage 4: day and night, sleep, huts

- **Day/night** (`World::sunElevation(time)`, `nightness(time)`, `kDayLength = 300 s`; the world starts in the morning, `sin(2π(t/300+0.08))`; "night" = smoothstep of elevation -0.1..-0.4, ~40% of the cycle). `Application` lights each frame from it:
  sun (warm at dusk/dawn) by day, a weak bluish "moon" from the mirrored side by night (never below horizon, so cascaded shadows keep working), ambient 0.6 → 0.16 and tinted, sky/fog/clear colour fade to a dark blue
  with an orange dusk term; the menu is always day. `WSIM_TIME=seconds` renders a given moment (frame dumps), `WSIM_NIGHT=1` still kills the sun. Debug window shows time/sun/night.
- **Sleep** (`Behavior::Sleep` "Спит", `SenseInputs::night`, set **only in a world with terrain**, so flat test worlds never sleep): score `night·(1.3 + 1.4·(1-energy))` when night > 0.5, fear < 0.3, hunger < 0.65 (kept until 0.85);
  walks to the nearest hut within 40 units (stops within 2.4) or sleeps where it stands; sleeping is `Rest` (restDesire → energy) with a 3x slower hunger/thirst (`Decision::sleep`). Measured: 80-94% of creatures asleep at night, populations
  and starvation unchanged within noise, nobody dies of thirst.
- **Hut** (`StructureKind::Hut`, `kSkillHut`, 4 logs, 5 s, cooldown 90 s; `kAllSkills = 255`): built where it stands when no hut is within 12 units, ≥6 apart (`placeStructure`); invented mostly at night (rate `0.0008 + 0.008·night`, needs the spear).
  Creatures that know it want 4 logs while no hut is near (`woodGoal` via `Creature::hutNear`, saved in **v10**, so a save is still bit-exact) and chop for it (score 1.3). A hut blocks animals within `kHutRadius = 1.5` (same hard block + look-ahead as the barricade)
  and **softens bites to 25%** for creatures within 3.2 units of its centre (`bitesBlockedByWalls`). Measured: 0-6 huts per 20 sim-min across the probe seeds (slow: it needs 4 logs and the skill chain).
- Model `procgen/StructGen::buildHut` (log walls with a doorway, gabled roof); the demo (`WSIM_DEMO_CIV=1`) places two huts on the nearest land.
- Not done: **division of labour** (no roles — everyone builds/chops/hunts by the same rules), creatures do not live in or "own" huts (no family homes, no sleeping inside the model, no door), night makes no difference to predators or
  animals (no nocturnal species, no sleeping animals), windows do not glow, no seasons/weather, no light sources (campfire) for creatures at night, sleep does not wake on noise except by fear > 0.3.

### Stage 5: campfire, roles, hut ownership

- **Campfire** (`StructureKind::Campfire`, `kSkillFire = 256`, 2 logs, 2 s, cooldown 100 s, ≥10 apart, `Structure::hp` = fuel 1→0 over `kFireBurnSeconds = 200`, then removed in `World::tick`): built at dusk (`night > 0.1`) by a creature that knows it and sees no fire within 15 units;
  creatures that know it chop for it in the evening. A burning fire **repels animals within `kFireRadius = 3`** (same step block/look-ahead as barricades and huts), **warms sleepers within 5 units** (extra rest 0.03/s) and is
  **stoked** by a knowing creature with logs within 3 units when `hp < 0.35` (+0.5, `firesStoked`; rare in practice — a fire normally outlasts the night as lit). A sleeper with no hut walks to the nearest fire (within 40) and sleeps near it.
  Rendering: `StructGen::buildCampfire` (stone ring, logs, tiered flame; flame height follows the fuel and flickers) **and a warm point light** per fire (the 6 nearest to the camera, radius 20, flicker; `LightsData::counts.x` was unused before).
- **Roles** (`Creature::role()`, pure function of the genes, so inherited): `Hunter` (bold + big), `Builder` (curious + sociable + not bold), `Woodcutter` (the rest). Effects (priorities only, nothing is forbidden): hunters start hunting at hunger 0.32 (others 0.5) with
  ×1.35 score; woodcutters chop ×1.6; builders build huts/stockpiles/fires ×1.5 and **discover building skills twice as fast** (this is why huts became common: 0-15 per 20 sim-min). Inspector shows the role.
- **Hut ownership** (`Structure::owner`): the builder owns it and moves their `homePosition` there; a sleeper picks the best hut by `distance + 30 if owned by a non-relative` (own, partner, parent, child count as "mine"; unowned is free), so families gather at their house
  (tested: goes to its own farther hut and not the nearer foreign one). Not a real "household": no capacity, no eviction, no inheritance.
- **Skills are 16-bit now** (`kAllSkills = 511`); saves **v11** (high byte after the v10 tail; new `STR3` section with the owner; STR2/STRC still read; kinds 1-5).
- Measured (20 sim-min, 9 seeds): fires 3-18 per run, huts 0-15; populations are small (3-45 alive at 20 min — lower than before stages 3-4; the extra chopping/sleeping time costs foraging; nobody went extinct, but seed 7 ended at 3, so expect to tune food/sleep next).
- Not done: real division of labour (no assignment by need, no teaching of roles), shared meals, fire as cooking/warmth need (no cold), fire spread/danger, fire-fearing creatures, animals that use fire-lit areas, torches, night ambience, seasons.

### Stage 6: ecology rebalance and night sounds

- **Why**: after stages 3-5 populations had sunk (10 seeds x 20 sim-min: mean 19 alive, 17 at the end, 3 seeds ending at ≤ 3). Measured causes of death over those runs: old age 177, **starvation 201**, predators 10, thirst 1, poison 0 — i.e. the world was not
  "dangerous", the **food economy was razor-thin** (a creature eats ~3% of the time at 0.35/s, which exactly balances hunger 0.01/s; mean hunger 0.41; 33% of the time asleep, 16% resting, 6% chopping, 6% drinking), so any extra chore tipped it.
- **Changes**: bush regrowth `kFoodRegrowRate` 0.055 → **0.075** (an emptied bush is edible after ~5.5 s, full after ~25 s), mating needs hunger ≤ **0.5** (was 0.4). Result (same 10 seeds): **mean 35 alive, 60 at the end, no extinctions, births 935 (was 319)**, starvation 313 deaths (more people, same share);
  bushes per creature 3.3 (was 6.3). Tests `berries_tests` re-based on the new constants.
- **Sounds**: two new seamless loops — `LoopCrickets` (6 s, two pulsed voices, periodic by construction; volume `0.4·nightness`, fades with camera height, quiet over water) and `LoopFire` (6 s of hush + crackle; volume by distance to the nearest burning campfire, `30 + 0.4·camDistance`);
  `AudioEngine` now has 6 loops; `SoundDirector::ambient` takes the `World`.
- Not done: seasons/weather, nocturnal animals, sounds for building/chopping/animals, a systematic balance harness in CI (the probes live in the session scratchpad only; the numbers above are from them).

### Stage 7: seasons, weather, night predators, ecology regression test

- **Seasons and weather are pure functions of the world time** (nothing stored or saved): `kYearLength = 4 days = 1200 s`, world starts in spring (`season` by year phase: <0.2 spring, <0.5 summer, <0.7 autumn, else winter);
  `coldness` (cosine peaking at phase 0.8, smoothstep → 0 in summer, ~1 deep in winter), `foodFactor = 1 + 0.3·summer − 0.4·cold` (bush regrowth multiplier, also applied to the closed-form regrowth of sleeping cells), `rain(t)`
  (smooth hashed noise over ~90 s windows, ≈27% of the time rainy, dry for the first 200 s). **All of it applies only in a world with terrain** (flat test worlds are unchanged).
- **Effects**: bushes regrow ×`foodFactor`×`(1 + 0.3·rain)`; **cold** (`SenseInputs::cold`, reduced to 20% beside a burning fire within 6 or a hut within 3.2) raises hunger ×`1+0.15·cold` and energy drain ×`1+0.2·cold`; rain quenches thirst (0.012/s)
  and burns campfires 3.5× faster; winter/dusk both trigger campfire building (`cold > 0.5` counts like `night > 0.1`). **Predators are hungrier and sharper at night** (`Fauna::tick(…, night)`: hunger growth ×`1+0.8·night`, sense radius ×`1+0.3·night`).
- **Visible/audible**: sky/fog tint (cold pale blue, autumn warm, rain grey with denser fog and a dimmer sun), `drawWeather` rain streaks or winter snowflakes as an ImGui overlay (hash-based, 40-400 particles), new loop `LoopRain` (volume `0.6·rain`);
  debug window shows season/cold/rain; `WSIM_TIME=seconds` also drives season and weather in frame dumps (e.g. 700 = autumn rain, 3490 = winter night snow).
- **Balance**: the first cut (cold ×1.25 hunger, food −0.6) dropped the mean population to 25 with one seed ending at 1; softened to the values above → 10 seeds × 20 sim-min: mean 32 alive, 36 at the end, no extinctions, 764 births.
- **`ecology_tests`** (new, ~13 s in debug): two seeds × 8 sim-min with terrain, everything on; asserts no extinction (end ≥ 5, mean ≥ 12), no cap blow-up, births ≥ 10, thirst deaths < 5 — the guard against another silent balance slide.
  `resources_tests` covers seasons, rain, cold/fire hunger, rain on fire, night wolves.
- **Save fix found by this stage**: `World::m_streamTimer` and `Fauna::m_streamTimer` were reset to 100 on load, so continuation was only *accidentally* bit-exact; both timers are now saved (`STRM`/`FAUN` tails) and `save_tests` stays green with the seasonal hunger change. Still unsaved: `Animal::stun` (a few seconds).
- Not done: snow on the terrain (the ground colour does not change), seasonal berry visibility/leaf colours, rain filling water or flooding, wind, lightning/fires, nocturnal species, hibernation/migration.

### Stage 8: seasons on the ground, work and animal sounds

- **Ground by season** (`materials.glsl: terrainMaterial(…, snowCover, autumn)`, fed through the previously reserved `LightsData::fogParams.z/.w`): autumn shifts grass/soil towards yellow-orange (`autumn` = bump around year phase 0.6),
  winter lays **snow cover** in patches on gently sloped ground (`(cold − 0.3)/0.5`, noise-broken, less on beaches, slopes and rock stay clean) and it melts with the cold. Frame dumps: `WSIM_TIME=100` spring, `640` autumn, `920` winter.
  Trees do not change colour yet (leaf and creature share one material code). `scene_rt.frag` passes 0/0 (that path is dormant).
- **World events** (`World::takeEvents()`, `WorldEvent{type,pos}`; `TreeFelled, Built, Plank, Stab, Bitten`, capped at 64, not state, never saved) → `SoundDirector::worldEvents`: falling tree = low thud, building = knock (planks quieter/higher),
  spear stab = short zap, a bite = thud; audible within `40 + 2.5·camDistance`, ≤ 4 per frame.
- **Animal voices** (`SoundDirector::animalVoices`): on the edge into `Hunt` a predator growls (pitch lower for larger species), on the edge into `Flee` a herbivore gives an alarm call; per-animal cooldown 8-16 s, budget 1.5/s, same hearing range.
- Not done: tree colours/bare winter trees, wet ground in rain, footsteps, creature death/birth sounds, distinct species voices (the three growl/alarm timbres are shared), ice on water, snow on roofs/structures.

### Stage 9: tribes (plus a population rebalance)

- **Rebalance first**: bush patch chance `kBaseChance` 0.26 → **0.36** (10 seeds × 20 sim-min: mean 37 alive, end 63, no extinctions, births 1012; before 32/36).
- **Tribes** (`Creature::tribe` 1..3, 0 = none; `World::kMaxTribes = 4`, `formTribes(terrain, count = 3)`, called at world start and in "Новый мир"): the initial creatures are dealt round-robin into three camps placed far apart on land (farthest of 60 random candidates),
  settled in ~10-unit clusters with home at the camp; each camp gets a **stockpile with 12 logs whose `owner` is the tribe**; relations between tribes (`World::relation/setRelation`, symmetric, -1 enmity..1 friendship) start random in [-0.6, 0.4] and slowly fade towards 0
  (0.03%/s). Children inherit the **mother's tribe**; god-spawned creatures stay tribe 0. Saved (**v12**: creature byte + `STRB` relation matrix; stockpile owner rides in `STR3`).
- **Cohesion**: same-tribe creatures count as *familiar* in crowding/alarm senses (so company soothes, strangers stay wary). A stockpile only serves its own tribe (tribe 0 uses unowned piles); a built pile gets its builder's tribe. Measured: tribes cluster naturally and the mean population rose
  (≈ 42 vs ≈ 37 without; noisy).
- **Raiding** (`Behavior::Raid`, "Грабит чужой склад"): a bold (> 0.45) creature that is armed or a Hunter, healthy, calm, not starving, with room to carry, and a **hostile** tribe's stockpile (relation < -0.15, stock ≥ 1) within **110** units walks there and takes up to 2 logs;
  that costs 0.15 relation (`raids`, `logsStolen`). **Defence** (`Behavior::Defend`, "Гонит чужака"): near their own stockpile (< 25) a creature chases a foreign-tribe creature within 12 whose tribe is not friendly (relation ≤ -0.1) and strikes within 1.8 (cooldown 1 s): 0.12 damage
  (×2 with a spear, ×0.4 vs a shield that loses 0.2), the victim's fear 0.75, relation -0.04 per blow; death in conflict is `deathCause 7` (`deathsConflict`). Both sides are tested (including "friendly tribe = no strikes").
  Measured (10 seeds × 20 sim-min): **raids are rare (0-5 per run), skirmishes common on hostile pairs (up to ~100 blows, ≤ 14 deaths)** — the starter stock is usually consumed by its own tribe before anyone marches 60-110 units.
- **Visible/audible**: coloured banner (red, blue, yellow; `StructGen::buildBanner`) on every tribe member's back and on a tall pole at each tribe's stockpile; inspector shows "<colour> племя · <role>"; debug window shows tribe sizes, relations, raids/strikes/conflict deaths; raid = whoosh, blow = thud (`EventType::Raid/Strike`).
  `WSIM_CAM_TRIBE=1..3` frames a camp.
- `ecology_tests` now run with `formTribes` (as in the game).
- Not done: **trade and alliances** (relations never rise through contact except the slow fade), migration/founding of new camps, tribe-specific culture/knowledge boundaries (skills spread across tribes through proximity), leaders, war parties, tribe extinction handling (a tribe with 0 members just stays on the map),
  banners in the menu colony, tribe colours on the creature body.

### Stage 10: trade, gifts, alliances

- **Open piles** (`World::pileOpenTo`): a tribe may use an unowned, its own, or a **friendly** foreign stockpile (relation > 0.3). The existing deposit/withdraw behaviours (`Haul`) therefore turn into trade with no new behaviour:
  *depositing* surplus logs into a friendly pile is a **gift** (`+0.06` relation per log, `giftLogs`, `EventType::Gift` → chime); *withdrawing* from it is **trade** (`-0.03` per log, `tradeLogs`) — friends supply you, but not for free.
  Hostile piles stay closed (they are raid targets). Allies also **defend** each other's piles (the "near a permitted pile" test of `Behavior::Defend`).
- **Contact diplomacy**: for every foreign-tribe creature within 6 units a creature nudges the relation by `+0.0004/s` (if > -0.3, i.e. not open enemies) or `-0.0002/s` (if enemies), ×min(contacts, 3). Peaceful neighbours drift together, enemies stay hostile.
- **Alliances** (`World::allied`, hysteresis: formed above 0.6, broken below 0.3; statistics only — `alliancesFormed/Broken`, shown in the debug window): relations are not state-dependent on this flag, so saves need nothing extra.
- **Camp founding is emergent**: a creature that knows the stockpile skill and sees no *permitted* pile within 60 units builds one for its own tribe (2 of 10 probe seeds ended with a fourth pile).
- Measured (10 seeds × 20 sim-min): alliances formed on 4 seeds, 3-4 logs traded on 2, no gifts in the probe (surplus carriers rarely coincide with a friendly pile in range), mean population ≈ 35. Tests cover trade with friends vs none with the cold, gifts, contact drift both ways and alliance formation.
- Not done: barter of other goods (only logs exist), treaties/war declarations as events, tribe leaders, joint tribe projects, skill sharing speed-ups between allies, relation effects on mating across tribes, UI for diplomacy beyond the debug window.

### Stage 11: leaf colours by season, leaders, mixed marriages

- **Leaf colour by season** (smooth, triangular profiles over the year phase; `materials.glsl: leafMaterial`, material code `MAT_LEAF = 0.85` / `MeshBuilder::kLeaf` for the *deciduous* trees' foliage — oak, birch, acacia;
  spruce, pine and palm stay evergreen on the old mottled code 0.75). The year phase `p` (0..1) rides in the previously unused `LightsData::shadowParams.w` (menu: fixed 0.3, summer). Two **triangle waves** on the circular year distance:
  *autumn colour* peak at p = 0.62, half-width 0.20 (linear rise and fall): green → yellow (first half of the rise) → red-brown (peak); *wilting* peak at p = 0.85, half-width 0.15: leaves go brown and **thin out** (fragments are `discard`ed by
  noise up to 80% at the peak, so winter trees look nearly bare). Spring (p ≈ 0) is back to full green. `WSIM_TIME` previews: 300 summer, 560 early autumn, 740 peak autumn, 1020 bare winter (`WSIM_CAM_TREE=oak`). Shadow maps are not
  thinned (a bare tree still throws a full crown shadow); bushes/houses/creatures are unchanged.
- **Leaders** (`World::leaderId(tribe)`, `electLeaders`, every 5 world-seconds — a slot of the world time, so nothing but the ids needs saving): the mature member with the best score
  `0.6·maturity (best at 40% of lifespan) + 0.3·boldness + 0.3·sociability + 0.15·children + 0.2·respect + 0.2·health`, the incumbent +0.15. Followers who drift > 22 units away from their leader get `Behavior::Follow`
  ("Идёт за вождём", score `0.55 + 0.5·sociability`, beats idle wandering, loses to real needs), so a tribe stays together around its leader. The leader wears a **golden crown** (`StructGen::buildCrown`); inspector prefix "вождь ·".
  Saves: leaders in the `STRB` tail (old files without it load with no leaders until the first election). Measured: ~16-26 changes per 20 sim-min (deaths and ageing).
- **Mixed marriages**: a mating pair of different tribes is allowed unless their relation is below 0 ("enemies do not marry"); a mixed birth gives the child one parent's tribe (coin flip, the mother if the father is tribeless) and **warms the pair's relation by 0.05**
  (`mixedBirths`). Genes already crossed over, so this is gene flow plus diplomacy. Measured: 0-60 per run (the growing seeds mix most).
- Not done: leaders issuing orders (raids and camp founding are still individual), succession by inheritance, leader-specific traits/culture, wilting visible in shadows, spring blossoms, tribe-coloured clothing.

### Stage 12: squads, nocturnal animals, migration (+ the shadow-cache optimization)

- **Optimization pass**: profiled first — the simulation costs **0.3-0.4 ms/tick** (worst spikes 2 ms) at 80-150 creatures, 60 animals and ~50 structures, i.e. ~2% of a 60 Hz frame (hot spot: `Terrain::noise2D` through `heightAt` calls, 45%),
  while uncapped the game runs ~390 fps with 2.45 ms CPU render, 700 of it shadow draw calls. The win was therefore in rendering: the **far shadow-cascade cache** (see "Rendering": 704 → ~100 shadow draws, negligible image difference).
  Not done on purpose: a height cache (sim is not the bottleneck; interpolation would diverge from the rendered terrain), AVX/`-march` flags (portability, no hot arithmetic loop), SIMD for the brain.
- **Squads** (`SquadData`, `World::updateSquads`/`disbandSquad`, `Creature::squad`, `Behavior::March` "В походе с отрядом"): at most one per tribe. Every 12 s a tribe whose **leader** is healthy and whose tribe has ≥ 8 members looks for a hostile stockpile (relation < -0.2, owner's pile
  ≥ 2 logs, ≤ 160 units from the leader); fighters = armed (or bold Hunters), healthy, not hungry, not asleep, within 70 units, at most `min(8, 40% of the tribe)`, ≥ 3 with the leader. **Phases**: 0 *gather* around the leader (until 70% are within 8 units or 25 s) → 1 *march*
  to the enemy pile and raid it (existing raid mechanics, ≤ 150 s, ends when 70% carry ≥ 3 logs or the pile is empty) → 2 *return* to the nearest own pile (≤ 90 s) → disband, 240 s cooldown. Members who starve (> 0.9) or fall below 30% health drop out; < 2 left disbands it.
  The target's defenders fight them with the usual `Defend`. Saved (**v13**: creature byte + `SQAD` section). Measured (10 seeds × 20 sim-min): squads on 5 seeds, squad raids 0-10, no extinctions (an unrestricted first version let one poor seed die out; the 8-member / 40% / 240 s limits fixed it).
- **Nocturnal animals** (`isNocturnal`: fox, jaguar): by day (night < 0.25) they doze (`Rest`, no hunting) and hunt at dusk and night; all herbivores are drowsy at night (speed ×(1−0.5·night)). Tested (a fox never hunts at night = 0, hunts at night = 1).
- **Migration** (`isMigratory`: deer, antelope, hare; `Fauna::tick(…, night, cold)`): every ~3 s (gated by the animal's age, so no stored state) a wandering migrant samples 8 directions 70 units away and bends its heading 45% towards the best one — in cold (> 0.5) the warmest spot
  (altitude-corrected temperature), otherwise the nearest suitable biome (it returns in spring). Tested: after 4 minutes of cold the herd stands on warmer ground than the same herd without cold.
- Debug window: squads (active/phase/formed/disbanded/raids); inspector "в отряде".
- Not done: hibernation, birds/flight migration, squad orders other than raiding (hunting parties, escorts, settling new camps), squad morale/retreat, leader dying mid-march (the squad just disbands), nocturnal herbivores.

## Temporal upscaler and frame generation (`VulkanTemporal.cpp`, `taau.comp`, `framegen.comp`)

Own implementation (not AMD's FSR 2/3): the official FidelityFX SDK builds with MSVC/`.bat` scripts, has HLSL shaders that
need `dxc` (AUR only on Arch) and no Linux/mingw support, so it would not build for everyone. This one is GLSL, built by
`glslc` like every other shader, works on any GPU, in the Windows cross build too. Settings → Качество → Апскейлер →
**Temporal** (`Settings::upscaler=3`; at render scale 100% it is plain TAA); next to it the **Генерация кадров** toggle.
- **Inputs it needed**: sub-pixel **jitter** (Halton 2,3; 8..32 phases by scale; applied in `scene.vert` to `gl_Position`,
  not to `proj`, so shadows/picking/culling keep the unjittered matrices), **motion vectors** (third scene attachment
  `kMotionFormat` RG16F, UV space `cur - prev`, MSAA-resolved like `viewDist`) and the existing `viewDist` as depth.
  `CameraData` got `prevViewProj` + `jitter` (filled by `VulkanContext::updateCamera`, `updateCameraHistory`).
  `MeshPC` is still 128 B (the portable minimum): `mat4 model`, `normalMatrix` as three `vec4` columns (a `mat3` in std430),
  `vec4 prevDelta` = how far a moving object went in world space since the previous DRAWN frame
  (`SceneObject::motion/trackId`, `Application::trackMotion`; creatures by id, boulders by index; > 30 units = teleport = 0).
  Static terrain/trees/houses get motion only from the camera.
- **`taau.comp`**: Lanczos2 reconstruction of the current frame around the jittered sample positions, motion taken from the
  nearest-to-camera neighbour (dilation), Catmull-Rom history fetch, variance clipping in YCoCg (2 sigma, tuned: 1.25 was
  softer, 2.5 gave the same sharpness for more ghosting risk), blend weight 0.10 + more with motion and with how far the history had to be
  clipped. History is RGBA16F ping-pong at output size; then the FSR 1.0 **RCAS** pass (sharpness slider) writes `m_upscaled`.
  Works in the tonemapped (perceptual) space like the rest of the pipeline. Measured (seed 7, 70% scale): sharper than the
  spatial path and visibly close to native; the correct jitter sign was verified by sharpness metrics (wrong signs blur).
  `resetTemporalHistory()` is called on new world/load. Not done: reactive masks for water/particles (water ripples are
  procedural noise in the pixel shader, so they shimmer slightly), history lock for thin features, motion for rotation
  of dynamic objects (translation only), depth-based disocclusion test (only colour clipping).
- **Frame generation** (`framegen.comp`, `endFrame`): needs Temporal **and vsync** (without vsync MAILBOX would drop the
  intermediate frame, so it is refused). Each real frame is preceded by an intermediate one: previous shown frame `a`
  (copied each frame to `m_prevImage`) and current `b` are sampled at `uv ∓ 0.5·mv`, blended when they agree, plain
  blend otherwise. `beginUIPass` composites the intermediate frame on the image acquired in `beginFrame`;
  `endFrame` acquires a SECOND image (`m_imageAvailable2`), composites the real frame + UI on it and presents both in order
  (one submit, two presents). The UI is drawn twice via `setUiRecorder` (so ImGui's per-call vertex buffer ring is
  `kMaxFrames*2`). Swapchain gets one extra image when it is on. Costs ~1 frame of latency and halves the real frame rate
  at a given vsync (60 Hz → 30 real + 30 generated); the point is smoother image when the GPU can't reach the refresh rate.
  Not done: optical flow (it uses engine motion vectors only, UI is not interpolated), frame pacing without vsync, HUD-less
  interpolation (the UI is re-drawn, not warped).
- Debug env: `WSIM_UPSCALER=0..3`, `WSIM_RENDER_SCALE=0.5..1`, `WSIM_FRAMEGEN=1`, `WSIM_CAM_SPIN=deg/s` (camera rotates by
  itself — checks motion), `WSIM_FG_SHOW=1` (the real image also shows the intermediate frame, so `WSIM_FRAME` can dump it).
  Compare native/FSR1/temporal on one seed with the PSNR + gradient-energy script pattern from this session (crops of
  `WSIM_FRAME` dumps).

## Behavior layer, ecology, inspection (`engine/sim/Behavior.*`, `intelligence_tests`)

Before this, creatures were pure random-weight brains acting directly (moveX/Z = brain output, thresholds on
desire outputs) with no consequences, so they drifted to a corner, piled up and — because mating only needed
two `mateDesire` outputs above 0.5 within 2 units, with no age/cooldown/cost, and children spawned at the
parents' midpoint — **24 became 200 in about a second at one spot** (children mated with their parents
instantly). Now:
- **Consequences**: `Creature::health` (drains at hunger >= 0.95 and at energy ~0, regenerates when fed and
  rested; `deathCause` 1 age / 2 starvation / 3 exhaustion), `World::stats()` counters. Mating needs
  `canMate()` = mature (age >= 10% of lifespan) + cooldown over + libido >= 0.4 + energy >= 0.45 + hunger <=
  0.65 + health >= 0.6; success is a per-second roll (`0.9 * mean fertility`), then libido resets,
  cooldown 45–85 s, mother -0.22 energy / father -0.08; the child appears 0.8–2.2 units from the mother
  (never in water), starts at hunger 0.25 / energy 0.8. Initial and god-spawned creatures are adults of
  random age. Food: 30 bushes, regrow ~0.075/s (see Stage 6), a bush with < 0.12 left is "empty" (invisible as food), eat
  rate 0.35/s, rest rate 0.06/s → measured carrying capacity ~80–150 with boom/bust and starvation as the
  main death cause (headless probe over seeds 5/11/29/77, 20 min).
- **`behavior::decide`** (utility AI, called from `Creature::tick`): scores Wander / SeekFood / Eat / Rest /
  Flee / SeekMate / Socialize / GoHome from needs, traits and senses; the current behavior gets +0.35 and a
  0.8 s minimum commit (Flee interrupts instantly), Eat/Rest have hysteresis (eat from hunger 0.2 until 0.04,
  rest until 90% energy). **The neural brain is now a modulator, not the driver**: its desire outputs scale
  priorities (x0.65..1.35) and its move outputs add a 0.25 personal bias to the heading; the genome layout
  (863 genes) is unchanged. `BrainOutputs` returned by `tick` now means what the creature DECIDED
  (`eatDesire=1` only when actually eating, `mateDesire=1` only next to a ready partner, `alarmCall = fear`
  above 0.5, `matingCall` while seeking a mate); the raw brain output is kept inside for the Hebbian learning.
- **Steering**: heading toward the target (food — or *food memory*: the last seen bush location, forgotten
  after 180 s or on arrival; flee direction away from the hazard or the alarming neighbour; mate; group
  centre; home), arrival slow-down near food/mate, **separation** from neighbours closer than 3 units (the
  anti-pile term), soft world-bounds push, then **terrain look-ahead** (`steerAround`: probes 2 and 4.5 units
  ahead for water/cliffs and turns 35/70/105/140° — consistent side per creature — or stops and re-rolls the
  wander heading). Wander heading is a persistent random walk (deterministic hash of id and age).
- **New senses** (`SenseInputs`: `foodDist/foodPos`, `mateDist` (only *ready* partners), `threatDist`,
  `alarmDir/alarmNear`, `groupDir/groupCount/groupDist`, `separation`, `worldHalfSize`) are filled by
  `World::tick`; the brain still sees only the compact old inputs.
- **Hazards**: `World::setHazards()` (not saved) — `Application::stepPhysics` feeds rolling boulders
  (speed > 1.5) and fresh impact spots (2.5 s); creatures feel them from 18 units off the edge, fear rises,
  Flee wins, alarm calls spread the panic (and sound via `SoundDirector`). Flee needs fear > 0.4 so mild
  homesickness/crowding can't trigger it.
- **Inspection**: power "Осмотр" (`inspect`, click): picks the nearest creature within the brush radius;
  `drawInspector` shows a ring on it and a panel (behavior, health/hunger/energy/fear/libido/social bars,
  age, size, speed, children, boldness/sociability). Food is now visible: bushes (lumpy green blobs with
  red berry faces) shrink as they are eaten.
- **Saves**: format v2 stores the new creature state (health, cooldown, behavior, timers, heading, food
  memory); v1 files still load (defaults). `save_tests` keeps proving bit-exact continuation.
- **Tests**: `intelligence_tests` (ecology over 5 min on two seeds: no avalanche — pop at 30 s <= 40, no
  extinction, never at the cap, mean hunger < 0.75, births/matings/starvation happen, children never have
  children; no stacking; hungry creature seeks, eats and returns; remembered food is approached; tired
  creature rests; timid group flees a hazard and calms down after it's gone; ready pair breeds, hungry pair
  doesn't).
- Not done: predators/combat, real use of `defendDesire`/`dominanceDisplay`, learning of *which* behavior
  works (the Hebbian loop only tunes the modulators), seasons/day-night rhythm (sleep), pathfinding beyond
  look-ahead (a creature can still dither at a concave coast), species/diets, creature visuals per state.

## Trees (`engine/sim/Trees.*`, `procgen/TreeGen`, `TerrainChunks::buildTrees`; `trees_tests`, `procgen_tests`)

(Since the biomes round there are seven species — spruce, pine, acacia, cactus, palm were added; see "Biomes".) Two original species, **oak** (thick trunk, two side branches, wide dark-green crown of 3 big + 2 small lumpy blobs) and
**birch** (slender tall white trunk with dark bands, slight lean/bend, narrow light-olive crown of 3 elongated
blobs, a few thin branches), 4 model variants each. Low-poly flat-shaded like houses/boulders; leaves use the
"mottled" material (`alpha 0.75`, world-space speckle), bark is plain vertex colour.
- **Placement is a pure function of the terrain seed** (like the terrain itself): `Trees::inRect(terrain, rect,
  blocker)` — a 7-unit jittered grid, at most one tree per cell, chance from smooth "forest density" noise
  (two octaves: ~80-unit groves + ~23-unit patchiness → groves and clearings), birch groves from a second
  noise (and birches above height 7 become oaks). A tree exists only where the ground is suitable *now*:
  height 0.9..10 (not beach/water/snow), slope <= 0.55 — checked against the current height incl. player
  edits, so lowering land below the shore or raising a mountain removes trees there automatically.
  Nothing is stored or saved; loads/new worlds just rebuild chunks. ~2–4 trees per 32x32 chunk on average.
- **Rendering**: only the finest-LOD (level 0) chunks get trees; they are *baked into one extra mesh per
  chunk* (template transformed by yaw/scale/ground height on the CPU) instead of thousands of draw calls
  (the engine has no instancing), so they share the chunk's hidden/stale/rebuild logic, draw and cast shadows
  (`objects()` and `shadowCasters()`). Terrain edits invalidate the chunk → trees are re-placed.
  `TerrainChunks::setTreeBlocker` — `Application` forbids trees on houses (`radius + 4.2`) and on food bushes;
  placing a house flattens a pad (dirty rect) so the chunk rebuilds and trees disappear there.
- **Collision**: `Trees::blocksPoint` (checks the 3x3 cells around a point, cheap distance test before the
  expensive terrain checks) — creatures' steering look-ahead treats trunks (+0.7) as obstacles and the physics
  step blocks entering a trunk (radius 0.3; slides along it; a creature already inside is not trapped).
  Measured: 10 simulated minutes of ~100 creatures run in ~2.7 s in release, so it's cheap.
- Debug: `WSIM_CAM_TREE=oak|birch` points the camera at the nearest such tree.
- **Hidden-face culling** (`procgen/MeshBuilder.hpp`): `addBox(..., skip)` takes a `BoxFace` mask — houses omit the plinth
  bottom (underground), wall top/bottom (under roof / on plinth), door/window/chimney faces pressed against the wall or
  roof. `removeBuriedTriangles(mesh, solids)` drops triangles whose 3 vertices lie inside another crown blob's *solid core*
  (radius shrunk for roughness and for facet inscription, see `blobT` in `TreeGen.cpp`) — oak −10% triangles, image
  pixel-identical. Not done on purpose: terrain skirts between same-level neighbours (would need a per-chunk neighbour
  mask + rebuild when a neighbour's level changes, risks cracks, saves ~20% of terrain triangles that aren't the bottleneck).
- Not done: chopping/removing trees by powers (they're procedural; would need a "removed" set saved with the
  world), boulders/fire interacting with trees, wind sway (needs time in the vertex shader), tree LOD/billboards
  beyond level-0 chunks (far trees are simply absent, hidden by fog), seasons, fruit/wood as a resource,
  more species (add to `TreeSpecies` + `TreeGen` + species rule in `candidate()`).

## Saves (`engine/sim/SaveGame.*`, `ByteIO.hpp`, `save_tests`, `Application::saveToSlot/loadFromSlot`)

- **What's saved**: terrain (seed + player edits as non-zero 32x32 tiles, sorted), the whole `World`
  (`WorldState`: mt19937 state as text, next id, every creature with genome/needs/position/velocity/age/
  home/ids/relatives/`learnedWeights`/last brain outputs, food, buildings, pending grief pings), boulders
  (`RigidBody`), camera. NOT saved: settings, selected power, mods (not world state), the decorative menu
  world. Loading = `Terrain(seed)` + `importEdits` + `World::importState` + `PhysicsWorld::setBodies`,
  then `TerrainChunks::reset()`; `restoreSave` builds new objects and swaps them in only on success.
  `save_tests` proves the point: after load the world continues **bit-for-bit identically** to the
  original (snapshots compared after 600 more ticks).
- **Format** (`kSaveVersion=1`): `"WSIMSAVE"`, u32 version, u32 meta size + meta (name, unix time, seed,
  play seconds, counts — enough for the list without parsing the world), u32 payload size, u32 CRC-32,
  payload = tagged sections (`TERR`,`WRLD`,`PHYS`,`CAMR`: tag + length + data; unknown tags are skipped, so
  newer minor additions stay readable). Explicit little-endian via `ByteWriter/ByteReader`; the reader is
  bounds-checked and every count has a ceiling *and* must fit in the remaining bytes, so corrupt or forged
  files give a Russian error ("файл повреждён (…)", "сохранено более новой версией игры") instead of
  crashes/huge allocations (tests: truncation, flipped bytes, 400 random payload mutations with a
  recomputed CRC, a 4-billion-creature count). A newer format version is refused with a clear message.
- **`SaveStore`**: folder `<dataDir>/WorldSimulator/saves/*.wsim`; writes `slot.wsim.tmp` then renames
  (atomic replace); slot names are `[A-Za-z0-9_-]` only (no path tricks); `list()` returns newest first and
  includes unreadable files (`valid=false` + reason) so they can be deleted.
- **Game flow**: menu items now Новый мир / Продолжить / **Сохранить** (enabled once a game exists) /
  **Загрузить** (page `Page::Load`: 5 rows + wheel scroll, click a row to load, "Удалить" asks "Точно
  удалить?" for 3 s) / Настройки / Выйти. "Сохранить" creates `world_YYYYMMDD_HHMMSS` the first time, then
  overwrites that slot (`m_currentSlot`); worlds loaded from `quick`/`autosave` start with no current slot
  (they never overwrite those). **F5** quick-save (slot `quick`), **F9** quick-load. **Autosave** to slot
  `autosave` every `Settings::autosaveMinutes` (0/2/5/10, Интерфейс tab, default 5) and on "Выйти". Toasts
  (`drawToast`) confirm/report. World name = "Мир №<seed % 10000>"; play time is tracked per world.
- Debug: `WSIM_SAVE_SMOKE=slot` (save at 2.5 s), `WSIM_LOAD_SLOT=slot` (load at start).
- Not done: thumbnails in the load list (needs an ImGui texture upload), renaming saves, save compression,
  cloud sync, saving mod enable/disable per world, migration code (only v1 exists).

## Sound (`engine/audio/`, `core/SoundDirector`, `synth_tests`)

All sounds are procedural (the approach of Podjezd's `AudioSystem`: oscillators, envelopes, filtered
noise — same toolbox, but backend-free so it's unit-testable and the repo needs no proprietary SDK).
Backend = **miniaudio 0.11.22** (FetchContent of `miniaudio.h` only, `SOURCE_SUBDIR none`; built with
`MA_NO_DECODING/ENCODING/GENERATION/RESOURCE_MANAGER` — these change struct layouts, so they are
`PRIVATE` definitions on the whole `engine_audio` target; the implementation lives in
`miniaudio_impl.cpp`).
- **`Synth`** (`SoundId`, `synthesize`, `synthesizeAll`, `writeWav16`): 27 sounds — UI (hover/click/back/
  toggle), god effects (pop, whoosh, thud, knock, zap, chime), creature voices (3 timbres each of
  alarm/coo/growl), bird chirps, and 4 seamless loops (wind, water, music pad, terrain rumble).
  Deterministic (own xorshift noise). Loops: noise ones use an equal-power crossfade of the tail into the
  head (`makeSeamless`); the 24 s music pad is periodic *by construction* (all frequencies multiples of
  1/24 Hz, cos² chord windows that sum to 1 on a circle, phases accumulated in double). Tests check
  finite/non-clipping/non-silent/deterministic/loop-seam/no-click-at-end for every sound.
- **`AudioEngine`** (pimpl over miniaudio): engine at the device's native rate, 3 `ma_sound_group`s
  (Ambient, Creatures, Effects) + engine master volume, a pool of 32 one-shot voices (each = an
  `ma_audio_buffer_ref` over the shared PCM + its own `ma_sound`; reaped via `ma_sound_at_end`), 4
  looping sounds whose volume eases to a target (`setLoop`). The bank is synthesized on a background
  thread at startup (<1 s release); until then `play()` is silent. 3D: linear attenuation from `minDist`
  to 0 at `maxDist`, no doppler. No device / `WSIM_NOAUDIO` → everything no-ops, `status()` says why
  (shown on Настройки → Звук together with the device name).
- **`SoundDirector`** (`core/`, also the `SoundSink` for mods): the game-side logic.
  Listener sits on the ground under the camera pivot (not in the sky — from god-camera height everything
  would be far/quiet), facing the camera's horizontal direction so panning matches the screen. Wind grows
  with camera distance + gusts; water loop ∝ fraction of water in a point ring around the pivot; music
  always (also in the menu), world ambience only while playing; birds every 3–11 s near the pivot on land;
  terrain rumble while sculpting (`raise/lower/flatten` held). **Creature voices**: on the *rising edge* of
  a brain output over a threshold (alarm/mating 0.65, dominance 0.75 — brain outputs hover ~0.45±0.2, so
  edges, not levels), per-creature cooldown 4–9 s, global budget ~3 new voices/s, only within hearing range
  (`40 + 2.5·camDistance`); timbre = `id % 3`, pitch from body size (small = high). Boulder impacts → thud
  scaled by size; power sounds: spawn pop, erase whoosh, house knock.
- **UI sounds**: `MainMenu`/`PowerBar` emit `UiSound` events through a callback (`ui/UiSound.hpp`) — they
  know nothing about audio; hover ticks on selection change, click/toggle/back, slider ticks throttled to
  70 ms. Settings: Звук tab = master / ambience / creatures / effects (+ device line), persisted.
- **Mods**: `ctx:sound(name [, x, z [, volume]])` with `thud, pop, whoosh, zap, chime, knock`
  (`SoundSink::playNamed`; mods can't ship audio files yet).
- Windows: miniaudio loads WASAPI dynamically, no extra libs; works under Wine (PulseAudio).
- Not done: sound for creature death/birth/eating, footsteps, ambience variety by biome, reverb, mod-
  provided sounds, day/night soundscape (hooks: `SoundDirector::ambient`).

## Lantern, physics, buildings, upscaler

- **Lantern (F)**: the dormant spot light of `LightsData` (inherited from Podjezd's flashlight) is now a
  spotlight from the camera onto the point under the cursor (or the view centre), cone sized to
  the brush circle; intensity is scaled by `1 + 0.05 d^2` to cancel the shader's `1/(1+0.05 d^2)`
  falloff so it works at god-camera distances. Meant for the future day/night cycle (no shadows for
  it: `shadowFactor` is 1 in the raster shader).
- **Physics** (`engine/sim/Physics.*`, `physics_tests`): sphere bodies ("boulders") on the heightfield.
  Ported *ideas* from Podjezd's `PhysicsWorld` (sleep by speed/timer, penetration slop + positional
  correction, impulse events for effects), but a different solver — Podjezd is AABB-vs-static-AABB,
  here it's sphere vs `Terrain::heightAt` (normal from central differences, depth `r - (y-h)*n.y`) plus
  sphere-sphere. Rolling resistance `0.14*g*cos` (slopes under ~8° stop a boulder), spin only for
  rendering (`RigidBody::orient`), water drag, auto-removal after 90 s asleep, cap 48 bodies,
  sleeping bodies wake if the terrain under them changes. Impacts (> 6 u/s) → `ImpactEvent`.
  `Application::stepPhysics` turns them into consequences through `GodContext`: impact kills creatures
  (`1.8r`) and buildings, digs a crater (`raise` negative, scaled by impulse), and a boulder moving
  > 2 u/s crushes creatures/buildings along its path. Power "Валун" (`GodContext::boulder`, Lua
  `ctx:boulder(x,z,r)`). Podjezd's character controller was NOT ported (god camera).
  Rendering: `buildBoulderMesh()` = lumpy icosphere (flat shaded), `Transform` gained an optional
  quaternion (`useQuat`).
- **Buildings** (`engine/procgen/`, `procgen_tests`): `MeshBuilder.hpp` (`addBox`, robust-orientation
  `addQuad/addTri`, `addGabledRoof`; closed meshes with outward normals, vertex alpha = material code)
  and `HouseGen` (`houseVariant(seed)` → `buildHouse`: plinth sunk 1.2 under the ground, walls, gabled roof,
  door, windows, chimney) — the `addBox` idea and parametric-building style are from `PodjezdGen`.
  `World::buildings()` holds `Building{x,z,yaw,seed}` (data only, 300 max, 6 units apart);
  `Application` pre-builds 6 variants (`kHouseVariants`, seed % 6) and rebuilds the instance list each
  update (houses follow the terrain height). Power "Дом" (`GodContext::build`: land only, 4 corners
  must be dry, flattens a pad ×3) — Lua `ctx:build(x,z[,yaw])`, `ctx:erase_buildings(x,z,r)`; the
  built-in "Стереть" removes buildings too. Creatures ignore houses; they're decor + boulder targets
  until the civilization systems exist.
- **Upscaler UI**: Настройки → Качество: FSR 1.0 / NIS / Temporal (own, see "Temporal upscaler and frame generation"), render scale 50–100%, sharpness
  (`Settings::upscaler/renderScale/sharpness` → `VulkanContext::setUpscaler/setSharpness`; the
  machinery was inherited and dormant). Applied on mouse release, not while dragging (it recreates
  render targets). Tabs now: Графика, Качество, Управление, Звук, Интерфейс, Моды.
- The power bar shrinks its pills (down to 55%) when many mod powers would overflow the screen.

## Creature terrain physics (`Creature::tick`, `World::tick(dt, const Terrain*)`)

`SenseInputs::terrain` (nullable `const Terrain*`; nullptr = flat world with no
limits — unit tests and the decorative menu colony) is set by `World` for every
creature. In `Creature::tick`, after the brain's `moveDir`:
- **Slope** is sampled 1.5 units ahead along `moveDir` (`slope` = rise/run, 1.0 = 45°).
  Uphill: speed x `1/(1+2.5*slope)`, movement energy cost x `(1+4*slope)`;
  steeper than `kMaxClimbSlope=1.2` (~50°) is a cliff: speed 0. Downhill gives up
  to +20% speed.
- **Water**: a creature on land can't step into water (`isUnderwater`); hitting
  the shore at an angle it slides along it (x-only/z-only fallback). One already
  in water (god-spawned, or the sea rose via Lower) can still move out but at 0.35x
  speed and 3x energy cost; no drowning death yet.
- `position.y` follows the ground; `velocity` is the *actual* displacement/dt.
- The brain is not told about any of this (no new inputs — that would change the
  genome layout): it only expresses desire, the world limits it, same split as the
  other "world decides" mechanics. Expect creatures to bunch up at shores/cliffs.
- `World::settleOnLand(terrain)` moves creatures/food that are underwater to random
  land in the bounds (called after construction and on "Новый мир");
  newborns that would spawn in water start at a parent's position instead.
- Constants are at the top of `Creature.cpp`; covered by `world_tests` (nobody ever
  underwater over 1800 ticks x 3 seeds, y on the ground).

## God powers (`engine/sim/Powers.*`, `engine/ui/PowerBar.*`, `Application`)

Player's hands in the world. LMB (outside UI) applies the selected power at the
point under the cursor; bar at the bottom: keys 1-9 select, `[` `]` / Ctrl+wheel /
the bar slider change brush radius. A mint ring is drawn on the terrain under
the cursor (`drawBrushRing`, ImGui foreground list, points projected with
`proj*view`).

- **`GodContext`** (World&+Terrain&) is the ONLY way a power touches the world:
  `height/raise/flatten/spawn/erase/countNear`; it collects dirty terrain
  rects (`takeDirty()`). Built-in powers and Lua scripts use the same object,
  so a mod can do exactly what the game can.
- **`PowerDef`** = id/name/description/source(`"base"` or mod id)/radius range/
  `interval`/`apply(GodContext&, PowerCall)`. `interval`: 0 every fixed step
  while held, >0 every N seconds, <0 once per press. `PowerCall.first` marks the
  first call after a press (flatten remembers the start height). Data
  constructors `makeRaisePower/makeFlattenPower/makeSpawnPower/makeErasePower`
  build both the built-ins (`makeBuiltinPowers`: Поднять, Опустить, Выровнять,
  Существа, Стереть) and JSON mod powers. `PowerRegistry::add` rejects duplicate
  ids, `removeFromSource` drops a mod's powers.
- **Terrain edits**: `Terrain::heightAt = baseHeightAt + sparse edit grid`
  (1-unit vertices in 32x32 tiles, bilinear, clamp +-60). `raise()`/`flatten()`
  return the touched `Rect`; the base procedural function is untouched, so edits
  are cheap and `m_edits.empty()` keeps the old cost. "Новый мир" makes a fresh
  `Terrain` (edits gone).
- **Chunks after an edit**: `TerrainChunks::invalidate(rect)` marks overlapping
  chunks of every LOD level; `update()` rebuilds <= 3 per frame, nearest to the
  camera first, old mesh stays until replaced (no holes).
- **Picking** (`Application::pickTerrain`): ray through the pixel (two unprojected
  points at ndc z=0.25/0.75 -> works for both depth conventions), march with a
  growing step until below `heightAt`, bisect 24 times.
- **World**: `spawnCreature(x,z,traits)` (respects `kMaxPopulation`, mind is
  random, traits override genes), `removeCreatures`, `countCreatures`.
  `Genome::traitFromName` maps names for mod data.
- Camera: far plane 4000 / near 0.5 so LOD chunks up to ~2000 units show up.

## Mods (`engine/mods/`, `mods/`, guide in `mods/README.md`)

A mod = folder in `mods/` (next to the game, `WSIM_MODS_DIR`) or in
`$XDG_DATA_HOME|~/.local/share/WorldSimulator/mods`, with `mod.json` and optional
`scripts/*.lua`. Enable/disable on Настройки -> Моды (tab "Моды"; list of
DISABLED ids persists in `<config>/WorldSimulator/disabled_mods.cfg`). Mods can
currently only add powers (no sim-rule changes) — by design for the first version.

- **Manifest**: `id` (a-z0-9_), `name`, `version`, `author`, `description`,
  `api_version` (must equal `kModApiVersion`=1 or the mod is shown as invalid with
  the reason), `powers[]` of type raise/lower/flatten/spawn/erase/script. Power ids
  become `<modid>.<powerid>`; a mod can't override built-in or other mods' powers.
  Script paths are confined to the mod folder (`insideMod`).
- **`LuaSandbox`** (one Lua state per mod, Lua headers private to the .cpp):
  only base (minus dofile/loadfile/load/loadstring/require/collectgarbage/pcall/xpcall — a caught budget error would let a script loop on), table,
  string (minus dump/find/match/gmatch/gsub — pattern matching runs in C past the instruction hook), math, utf8 — NO io/os/package/debug. Text chunks only
  (mode "t"). Memory cap 32 MB (custom allocator), instruction budget via count
  hook (4M per power call, 20M on load), all numeric args to `ctx` clamped/finite
  checked. Script returns a function or a table with `apply`; `ctx` is a table of
  C functions reading the current `GodContext*` from the state's extraspace
  (nullptr outside a call -> error). A runtime error disables only that power
  (`broken` flag) and appends to `ModInfo::errors` (shown in the mods list).
- Written against the stable subset of the C API; builds with Lua 5.4 and 5.5
  (`lua_newstate` signature differs — `#if LUA_VERSION_NUM >= 505`).
- **Tests**: `engine/mods/tests/mods_tests.cpp` builds temp mods incl. malicious ones
  (infinite loop, `os.execute`, `io.open`, `load`, path escape, syntax error, bad
  JSON, wrong API version) and checks sandboxing/persistence.
- Not done: install-from-archive/UI button, hot reload while the game runs (reload
  happens at start and on enable/disable), mod-provided shaders/models/species
  beyond spawn presets, mod load order/dependencies, Windows packaging.

## Main menu (`engine/ui/MainMenu.hpp`/`.cpp`)

Game starts in `Application::Screen::MainMenu`; Esc toggles menu ↔ game once
a game has been started (`m_started`). Design was picked from HTML mockups
(layout "D" — diagonal split — in the "bioluminescence" style, Unbounded
title + Geologica list, "isoline + focus" hover); the mock palette is the
source of truth for the hex constants at the top of `MainMenu.cpp`.

- **Pure ImGui draw-list UI**, no Vulkan/sim dependency: `MainMenu::draw(dt,
  canContinue, colony)` returns a `MenuAction` (`NewWorld`/`Continue`/`Quit`).
  Colony is passed as positions in the unit circle, so the menu knows nothing
  about `World`. Sizes are in `u` = 1% of window width (like `cqw` in the mock).
- **sRGB gotcha**: the swapchain is `VK_FORMAT_B8G8R8A8_SRGB` and ImGui writes
  colors as if they were already sRGB, so everything came out washed out.
  `rgba()` in `MainMenu.cpp` converts each hex channel sRGB→linear. The rest
  of the ImGui style (debug window) is *not* converted — same mild washout,
  not fixed.
- **Soft glow**: ImGui has no blur, so glows are `gradientRing()`/
  `radialGlow()` — triangle strips with per-vertex alpha (stepped `AddCircle`
  rings showed visible banding). ImGui can only clip to a rectangle, so
  creatures' placement radius is shrunk so their halo stays inside the dish.
- **Hover/selection**: `m_sel` is `-1` (nothing highlighted, nothing dimmed)
  until the mouse enters an item or an arrow/W/S is pressed; moving the mouse
  off all items clears it again. Per-item focus `t` and global `m_dim` are
  exponentially smoothed; selected item scales 1.12×, gets a wavy underline
  drawn left→right, others dim to 30%. Disabled items ("Продолжить"/"Сохранить" before a game exists) can't be selected;
  "Продолжить" is enabled only after a game has started.
- **Colony** is a *separate decorative `m_menuWorld`* (first
  `kInitialPopulation` creatures shown), ticked only while the menu is open;
  the real `m_world` is paused then. Ticking the real world behind the menu
  made it hit `kMaxPopulation` (no starvation death yet) before the player
  pressed play. "Новый мир" on an already-started game reseeds `m_world` (no
  confirmation — no saves yet); terrain stays (its mesh is built once).
- **Cursor scares the colony** (visual only — brains/needs not involved):
  per-creature `Runner` (offset from the sim position, velocity, fear). Fear
  rises fast near the cursor, decays slowly; flee accel is away from the
  cursor at a per-creature angle/speed/sight radius ("nerve" from the index),
  slides along the dish rim, springs back home when calm. Real flee-from-
  threat behaviour would need the sim's `threatProximity`/`fleeDesire`
  (still unresolved intents, see World Known gaps).
- **Settings page** (`MainMenu::drawSettings`, `Page::Settings`; Esc goes back
  via `MainMenu::back()`): sections Графика (vsync, MSAA 0/2/4/8 clamped to
  GPU, auto view radius + its cap 4-64 chunks, shadows on/off), Управление (pan
  speed, orbit sensitivity, zoom step, invert), Звук (disabled placeholders,
  "скоро"), Интерфейс (debug-window scale, debug window on/off). Same hover
  language as the main list: hovered row grows slightly, glows, gets the
  wavy isoline, other rows dim (`m_rowDim`); active tab = focused. Toggles,
  segmented buttons and slider thumbs ease toward their value through
  `m_anim` (key -> float, first sight snaps to target). `Application` owns
  `Settings m_settings`; `applySettings()` pushes diffs to `VulkanContext`
  (`setVsync`/`setMsaa`/`setRayTracedShadows`), `TerrainChunks::setViewRadius`
  and the ImGui `FontScaleMain`; it saves to disk once the mouse button is
  released (not while dragging a slider) and in the destructor.
  Camera speeds in `processInput` read from `m_settings` (the old `kPanSpeed`
  etc. constants are gone). No language option (no localisation).
- Known gaps: no "new world" seed/size choice, no gamepad,
  menu fonts load at size 0 and rely on ImGui 1.92 dynamic sizing.

## Current state / TODO

- [x] Engine bring-up: window, Vulkan context, ImGui, orbit camera,
      fixed-timestep loop (ground plane since replaced by real terrain,
      see below)
- [x] Creature core: genetics, neural-net brain, needs, communication,
      fight-or-flight, identity/relationships, in-group/out-group, lifetime
      learning, explicit temperament, respect/status drive, envy/resentment,
      grief, territoriality, curiosity, empathy (`engine/sim/`)
- [x] World + spatial-grid system (feeds `SenseInputs` to creatures; also
      the fix for the O(n²) nearest-neighbour cost flagged when the hardware
      ceiling was discussed — see `tools/hw_ceiling.py`)
- [x] Spawning: population/food live in `World`, rendered as simple pyramid
      markers, `World::tick()` wired into `Application::update()` — running
      in the actual game for the first time (see "World" above)
- [x] Procedural terrain: heightmap (not voxel) generated with fractal Perlin
      noise, sea-level water, directional light (see "Terrain" above) —
      rivers still pending; creatures obey slopes and water (movement only)
- [x] Main menu: ImGui, bioluminescent dish with a living colony that flees
      the cursor (see "Main menu" above)
- [x] God-power tools: raise/lower/flatten terrain, spawn/erase creatures,
      brush UI, terrain edits with chunk rebuild (see "God powers")
- [x] Mods v1: Lua-scripted and data-defined powers, sandbox, mod list UI
      (see "Mods")
- [x] Lantern, boulder physics, houses (procgen), upscaler settings, Windows cross-build
- [x] Saves (see "Saves")
- [x] Creature intelligence v1: behavior layer, ecology (starvation, real mating), hazards, inspector
- [x] Resources/tools stage 1: logging, spear, shield (see "Resources and tools")
- [x] Civilization stage 2: bridges, barricades, traps, visible gear (see "Stage 2")
- [x] Civilization stage 3: stockpile, buckets + thirst, meat hunting (see "Stage 3")
- [x] Civilization stage 4: day/night, sleep, huts (see "Stage 4")
- [x] Civilization stage 5: campfire, roles, hut ownership (see "Stage 5")
- [x] Stage 6: ecology rebalance, night/fire ambience (see "Stage 6")
- [x] Stage 7: seasons, weather, night predators, ecology regression test (see "Stage 7")
- [x] Stage 8: seasonal ground (autumn colour, snow), work and animal sounds (see "Stage 8")
- [x] Stage 9: tribes, banners, raids and defence (see "Stage 9")
- [x] Stage 10: trade, gifts, alliances (see "Stage 10")
- [x] Stage 11: seasonal leaves, leaders, mixed marriages (see "Stage 11")
- [x] Stage 12: squads, nocturnal animals, migration, shadow cache (see "Stage 12")
- [ ] Next: hibernation/birds, hunting parties and settling expeditions, spring blossoms, leader succession
- [ ] More powers (fire, rain, disasters, food, smite individuals), undo
- [x] Sound: procedural synth + miniaudio (see "Sound")
- [x] Day/night cycle (see "Stage 4"; night ambience sound still missing)
- [ ] Civilization-scale systems (out of scope until the above lands)

## Key conventions

- Namespace `wsim::` for all engine/game code (renamed from Podjezd's `pdz::`
  when the engine was forked).
- Include paths: `${CMAKE_SOURCE_DIR}/engine` → `#include "renderer/Window.hpp"`,
  `#include "core/Logger.hpp"`, etc.
- `Logger` is header-only; `engine_renderer` includes it via include path,
  does not link `engine_core` (avoids circular dependency).
- Shaders compile automatically to `build/*/shaders/*.spv`; path passed via
  `WSIM_SHADER_DIR` (`target_compile_shaders()` in root `CMakeLists.txt`).
