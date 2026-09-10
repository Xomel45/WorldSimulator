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

```bash
cmake --preset debug       # → build/debug/
cmake --preset release     # → build/release/

cmake --build --preset debug
cmake --build --preset release

./build/debug/game/worldsim
```

`WSIM_AUTOEXIT=1` — auto-quits after 5s (non-interactive smoke test of the
full init → render → shutdown cycle).

After first configure, `build/debug/compile_commands.json` → LSP resolves
include paths correctly.

## Architecture

```
engine/
  core/        Logger.hpp (header-only, C++23 std::print)
               InputSystem.hpp/.cpp — keys, mouse delta, mouse buttons, scroll
               Application.hpp/.cpp — game loop (fixed 60 Hz + variable render)
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
  sim/         Genome/Brain/Creature — genetics + neural-net brain + needs.
               World — population + food + spatial grid, orchestrates the
               above. No Vulkan/GLFW dependency (only glm) -> fast to compile
               and directly unit-testable. `Application` owns a `World` and
               renders it — see "Creature core"/"World" below.
game/
  main.cpp     entry point
assets/
  shaders/     inherited wholesale from Podjezd (scene/blur/composite/vision/
               FSR/NIS/RT) — generic rendering utilities, no game content
  fonts/       LiberationSans-Regular.ttf (ImGui)
```

## Camera (god camera)

`OrbitCamera` (`engine/renderer/Camera.hpp`) orbits a movable pivot
(`m_target`) instead of Podjezd's free-fly FPS camera:
- **WASD** — pan the pivot (camera-relative flat XZ, speed scales with
  distance so zoomed-out panning is faster)
- **Right-mouse-drag** — orbit (`yaw`/`pitch`, pitch clamped `[-85°, -20°]` —
  always looking down, never below horizon or near-vertical)
- **Scroll wheel** — zoom (`distance`, clamped `[4, 200]`)

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
- **Known gaps** (deliberately out of scope this round): no predators/threats
  (`threatProximity` stays `0`) or combat resolution (`fleeDesire`/
  `defendDesire` stay expressed-but-unresolved intents); no starvation death
  (`needs.hunger` caps at `1.0` in `Creature::tick()` but never kills — an
  existing gap, not introduced here; `kMaxPopulation=200` is the safety net
  against unbounded growth in its place); no dominance/status contests
  (`Creature::earnRespect()` stays uncalled); no per-creature visual
  differentiation or non-ambient lighting (see Application integration below).
- **Application integration**: `Application` owns a `World` (seeded from
  `std::random_device` — real playthroughs vary run to run; determinism for
  tests lives in `world_tests.cpp` with a fixed seed) and a single shared
  pyramid `Mesh` (`buildCreatureMesh()` in `Application.cpp`, next to the
  existing `buildGroundPlane()`) that every living creature's `SceneObject`
  references — `Application::update()` calls `m_world.tick(dt)` then rebuilds
  `m_creatureObjects` from `m_world.population()` each step (position, `size()`
  scale, yaw from velocity when moving). Ground plane bounds now come from
  `World::kWorldHalfSize` (previously a separately-hardcoded constant).
  **Destructor note**: since every creature `SceneObject` shares the *same*
  `Mesh`, it's destroyed exactly once explicitly — not by iterating
  `m_creatureObjects` the way `m_scene`'s (distinct) meshes are, which would
  double-destroy the shared GPU buffers.

## Current state / TODO

- [x] Engine bring-up: window, Vulkan context, ImGui, orbit camera, flat
      ground plane, fixed-timestep loop
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
- [ ] Procedural terrain (heightmap or voxel — AlphaCraft's Perlin/chunk code
      may be a starting point; flat ground plane for now, sized from
      `World::kWorldHalfSize`)
- [ ] God-power tools (raise/lower terrain, spawn/remove creatures, click
      interactions)
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
