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
- **`Genome`** — flat `std::vector<float>` of 379 genes in `[-1,1]`: 5 body
  traits (speed/size/metabolism/lifespan/fertility, named via
  `Genome::TraitIndex`) + 374 brain weights (`brainWeights()` span, must stay
  in sync with `brain::kWeightCount` — enforced by a `static_assert` in
  `Genome.cpp`). `random()`/`crossover()`/`mutate()` all take an explicit
  `Rng&` (`std::mt19937`, no global RNG state — deterministic given a seed).
  Random brain weights are drawn from a narrow gaussian (not full `[-1,1]`
  noise) so a fresh creature starts with responsive, non-saturated neurons
  instead of flailing for generations before selection finds anything
  coherent.
- **`Brain`** — stateless: `brain::evaluate(weights, BrainInputs) -> BrainOutputs`,
  fixed topology 16 inputs → 16 hidden (tanh) → 6 outputs (`tanh` for
  move X/Z, sigmoid for the four desire outputs). Sized after comparing to
  the real Creatures Norn brain (952 neurons/~5000 connections/9 lobes) —
  both scales are computationally trivial at thousands of agents, so this
  was a scope call, not a performance one. No lifetime-learning lobe (a Norn
  can learn associations during its life) — deliberately deferred, would be
  an additive input source layered on top of `evaluate()`, not a rewrite.
- **`Creature`** — aggregates `Genome` + `Needs` (hunger/energy/fear/libido/
  social) + position/velocity/age. `tick(dt, SenseInputs)` runs the brain and
  updates needs/movement/age; `SenseInputs` (nearest food/threat/mate
  direction+proximity, crowding) is supplied by the caller — `Creature`
  never queries a world itself, which is the seam the next milestone (world +
  spatial queries) plugs into. `feed()`/`rest()`/`mate()` are world-driven
  mutators (world decides an action succeeded, then calls these). Body traits
  (`maxSpeed()` etc.) are computed on-demand from the genome, not cached.
- Tests: `engine/sim/tests/{genome,brain,creature}_tests.cpp`, assert-based
  (no test framework dependency), run via `ctest --test-dir build/debug`.

## Current state / TODO

- [x] Engine bring-up: window, Vulkan context, ImGui, orbit camera, flat
      ground plane, fixed-timestep loop
- [x] Creature core: genetics, neural-net brain, needs (`engine/sim/`) — not
      yet wired into the running game
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
