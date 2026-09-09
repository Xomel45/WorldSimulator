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
               No Vulkan/GLFW dependency (only glm) -> fast to compile and
               directly unit-testable. Not yet wired into Application/game —
               see "Creature core" below.
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

Pure simulation logic, no world/spawning/rendering integration yet:
- **`Genome`** — flat `std::vector<float>` of 695 genes in `[-1,1]`: 6 body/
  behavior traits (speed/size/metabolism/lifespan/fertility/**plasticity**,
  named via `Genome::TraitIndex`) + 689 brain weights, split into
  `fixedBrainWeights()` (500 — input→hidden, "perception", never changes
  after birth) and `initialPlasticWeights()` (189 — hidden→output, "decision
  mapping", just the *starting point* a creature's `learnedWeights` copies at
  birth — see `Brain`/`Creature` below). `Genome::kFixedBrainWeightCount`/
  `kPlasticBrainWeightCount`/`kBrainWeightCount` mirror
  `brain::kInputHiddenWeights+kHiddenCount` / `kHiddenOutputWeights+kOutputCount`
  / `kWeightCount` respectively, enforced by `static_assert`s in `Genome.cpp`.
  `random()`/`crossover()`/`mutate()` all take an explicit `Rng&`
  (`std::mt19937`, no global RNG state — deterministic given a seed). Random
  brain weights are drawn from a narrow gaussian (not full `[-1,1]` noise) so
  a fresh creature starts with responsive, non-saturated neurons instead of
  flailing for generations before selection finds anything coherent.
- **`Brain`** — mostly stateless, with one deliberate exception (see lifetime
  learning below): `brain::evaluate(fixedWeights, plasticWeights, BrainInputs)
  -> BrainEvalResult` (bundles `BrainOutputs` + the hidden/output activations,
  needed by `reinforce()`), fixed topology 24 inputs → 20 hidden (tanh) → 9
  outputs (`tanh` for move X/Z, sigmoid for the desire + communication
  outputs). Sized after comparing to the real Creatures Norn brain (952
  neurons/~5000 connections/9 lobes) — both scales are computationally
  trivial at thousands of agents, so this was a scope call, not a performance
  one.
- **Communication** (`alarmCall`/`matingCall` outputs, `nearbyAlarmLevel`/
  `nearbyMatingCallLevel` inputs) — creatures broadcast a distress or
  mating-readiness signal; nearby creatures sense the aggregate (computed
  externally by the future world/spatial system, same pattern as
  `SenseInputs`'s other fields) and it has real behavioral effect in
  `Creature::tick()`: a neighbor's alarm call raises your own fear even if
  you can't see the threat yourself (herd panic), a neighbor's mating call
  nudges your own libido. This is what makes creatures actually social,
  rather than tracking a private `Needs::social` number nobody else reacts to.
  It's also the raw material for emergent teamwork (e.g. rallying toward an
  alarm instead of fleeing it) — deliberately not hard-scripted, since
  hard-coding "help a neighbor in danger" would undercut the actual goal
  (genuinely emergent, evolved behavior, not a scripted formula wearing a
  brain label).
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
- **Lifetime learning** — the one deliberate exception to "Brain is
  stateless": `Creature::learnedWeights` is a per-creature runtime copy of
  just the *plastic* weight slice (hidden→output+bias, 189 floats), lazily
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
  social) + identity + `learnedWeights` + position/velocity/age.
  `tick(dt, SenseInputs)` runs the brain (with learning) and updates
  needs/movement/age; `SenseInputs` is supplied by the caller — `Creature`
  never queries a world itself, which is the seam the next milestone (world +
  spatial queries) plugs into. `feed()`/`rest()`/`mate()`/`recordChild()` are
  world-driven mutators (world decides an action succeeded, then calls
  these). Body traits (`maxSpeed()` etc.) are computed on-demand from the
  genome, not cached.
- Tests: `engine/sim/tests/{genome,brain,creature}_tests.cpp`, assert-based
  (no test framework dependency), run via `ctest --test-dir build/debug`.

## Current state / TODO

- [x] Engine bring-up: window, Vulkan context, ImGui, orbit camera, flat
      ground plane, fixed-timestep loop
- [x] Creature core: genetics, neural-net brain, needs, communication,
      fight-or-flight, identity/relationships, in-group/out-group, lifetime
      learning (`engine/sim/`) — not yet wired into the running game
- [ ] Procedural terrain (heightmap or voxel — AlphaCraft's Perlin/chunk code
      may be a starting point)
- [ ] World + spatial-grid system (feeds `SenseInputs` to creatures; also
      the fix for the O(n²) nearest-neighbour cost flagged when the hardware
      ceiling was discussed — see `tools/hw_ceiling.py`)
- [ ] Spawning: place `Creature`s in the world, wire `tick()`/`reproduce()`
      into `Application`'s update loop
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
