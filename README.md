# Tankaq

A Steam-based multiplayer tank arena with rasterized Direct3D 11 / Direct3D 12 rendering.
Drive the hull with WASD, aim the turret with the mouse, shoot your friends.

## Features

- **GPU detection & backend selection** — at startup the game enumerates GPUs through DXGI
  (preferring the high-performance adapter), probes Direct3D 12 feature level 12_0 support,
  and picks **D3D12** when available, falling back to **D3D11** otherwise. `d3d12.dll` is
  delay-loaded so D3D11-only machines still start. Override with `--renderer=d3d11|d3d12`.
  The window title and HUD show the active backend; details land in `tankaq_log.txt`.
- **Rasterized forward renderer** on both backends from one shared HLSL file
  (`shaders/Basic.hlsl`, compiled at runtime): textured lit meshes, CPU-generated mip
  chains, anisotropic filtering, distance fog, screen-space UI.
- **Real mesh tank** — "Tank" by Quaternius (CC0, via Poly Pizza), baked offline into
  `assets/tank/tank_baked.glb` with separate **hull** and **turret+gun** meshes, an
  embedded palette texture, turret pivot and muzzle metadata. Hull and turret rotate
  independently, exactly like the big tank games.
- **Gameplay** — fixed 60 Hz simulation: screen-relative WASD movement (W up, S down,
  A left, D right; the hull turning to face the travel direction is purely visual),
  world-absolute turret aim, shells with cooldown, health/damage, kills, respawns,
  obstacle & tank collision.
- **Screen-space GI + SSAO** on both backends (`shaders/Post.hlsl`): the scene renders
  into a small G-buffer (lit color / normals / albedo / depth), SSGI cosine-samples the
  hemisphere and ray-marches the depth buffer for one bounce of indirect light (misses
  pick up sky), SSAO adds contact occlusion, and a temporal pass reprojects history by
  world position with distance-based disocclusion rejection so accumulated GI survives
  camera motion. Effective sample count = GI rays (1-16 per frame) x temporal samples
  (2-16 frames). Settings menu + `F5`/`F6` toggles + HUD readout.
  - The march interpolates the projected clip-space segment (clip coords are affine in
    world position) instead of re-projecting every step, and only reconstructs world
    positions for steps whose NDC depth flags a candidate hit.
  - GI traces at **half resolution** by default and is upsampled bilaterally in the
    composite using per-texel camera distance stored in the GI alpha (GI RES setting
    switches to full). Measured at 1440p, 16 rays x 4 temporal on an RTX 5060:
    ~100 FPS originally -> 190 FPS full-res -> **~600 FPS half-res**.
  - SSAO: 12-sample golden-angle spiral over a cosine hemisphere with per-pixel
    rotation, followed by a depth-aware separable blur (7 taps per axis).
- **Sun shadows** — an orthographic shadow map whose 58-unit box follows the camera
  focus (translation snapped to the texel grid so edges don't shimmer while driving),
  sampled with PCF in the mesh shader. A shadow map was chosen over screen-space
  shadows because off-screen geometry keeps casting (screen-space shadows pop as
  casters leave the frame). Settings: SHADOW RES 1024/2048/4096 and SHADOW FILTER
  sharp (1 tap) / soft (3x3) / softer (5x5); toggle via `F7` / `--shadows=0|1`,
  `--shadowres=`, `--shadowfilter=0|1|2`.
- **GPU VFX with G-buffer collisions** (`shaders/Vfx.hlsl`) — stateless particles:
  every smoke puff's position is a pure function of (burst, particle, age) evaluated
  in the vertex shader, which then collides the particle against the depth+normal
  G-buffer and pushes it out along the surface plane, so smoke slides around walls
  instead of clipping them; the pixel shader depth-fades near geometry (soft
  particles) and the pass depth-tests read-only so walls occlude smoke. Explosions
  also stamp **scorch decals**: a deferred pass reconstructs world position from
  depth and multiplies burn darkening into BOTH the lit color and the albedo
  G-buffer, so SSGI bounces less light off burnt spots — and since smoke is drawn
  into the scene color before the GI pass, GI "sees" the smoke too. Explosion events
  are inferred from projectile slots going inactive, which works for host and
  clients without protocol changes. `--boom` spawns test explosions.
- **Ambience** — hemisphere ambient (cool sky light from above, warm ground bounce
  from below) replaces the flat constant term, and the composite ends with a gentle
  filmic finish (small saturation lift + highlight rolloff).
- **Edge anti-aliasing** — a contrast-adaptive tent pass (deliberately
  direction-free so per-frame GI noise can't wobble the blur direction) kills the
  stair-step crawl on geometry edges during camera motion while leaving flat areas
  sharp. Settings toggle / `--aa=0|1`. Explosion VFX origins are pushed out of
  walls/obstacles on spawn so smoke never starts embedded in geometry, and burn
  decals only stick to static world geometry — tanks/projectiles tag themselves as
  dynamic in the G-buffer (normal alpha) and the scorch pass skips them.
- **Display settings** — live D3D11/D3D12 renderer switch (recreates the renderer +
  reuploads assets in place), borderless fullscreen / windowed toggle
  (`--fullscreen`), windowed resolution picker, VSYNC toggle (tearing appears only
  with vsync off), and swapchains use `DXGI_SCALING_NONE` so a size mismatch cuts
  the image instead of stretching it. GI temporal accumulation clamps per-frame
  changes against converged history to stop ray-noise shimmer.
- **Steam multiplayer** — host-authoritative listen server over the Steamworks SDK
  (SteamNetworkingSockets). Clients send sequence-numbered inputs (60 Hz), the host
  simulates and broadcasts snapshots (20 Hz) that ack the last input simulated per
  player. **Client-side prediction**: clients integrate their own tank locally with
  the exact host movement code, then on each snapshot adopt the authoritative state
  and replay all unacknowledged inputs — zero input latency, server stays boss.
  Remote tanks and projectiles interpolate between the last two snapshots. The whole
  sim runs on a fixed 60 Hz tick and **rendering interpolates between ticks**, so
  motion is smooth at any frame rate (the camera is rigidly locked to the
  interpolated tank). Friends join with the **game code** (host SteamID64, relayed
  P2P — click the HUD banner to copy it) or `ip:port` on LAN. Dev AppID 480.

- **Stats system** — upgrades never touch gameplay directly; each `UpgradeType`
  carries `StatMod`s (`amount` additive, `factor` multiplicative, one upgrade may mix
  both across several stats, tradeoffs included). Finals are rebuilt from scratch as
  `final = (base + sum(amounts)) * product(factors)` — additions/subtractions first,
  then multiplications/divisions — with the two accumulation maps stored as flat
  Stat-indexed arrays. The host recalculates every 8 ticks and on every purchase;
  the server runs at 64 tick. Final stats replicate in snapshots so client
  prediction integrates the exact host values.
- **Offer conveyor** (`TAB`) — players start with zero offers; a random one (rarity-
  weighted, no cap, cost +25%/copy owned) arrives every 5 s at slot 0 of a 2x3
  panel, pushing the rest along like a conveyor; every card's position lerps to its
  new slot. Icons are real images from an atlas texture (generated pixel-art by
  default, per-icon overrides via `assets/icons/<name>.png`), drawn by a textured
  UI pass. Purchasing burns the card (pixel-dissolve from the click point, icon
  burns with it). **Overflow ejects instead of burning**: the tail card smashes
  through breakable slats on the panel's lower-right border (which shatter into
  spinning, falling debris), flies right at high velocity, bounces off the screen
  edge with a velocity-scaled impulse and spin, and tumbles off-screen. Offers are
  host-authoritative with rolling ids (protocol v5). `--rich`, `--shoptest`.
- **Camera lean** — strafing tilts the camera slightly around Y, forward/back
  pitches around its local X; dt-exponential smoothing with epsilon snaps, targets
  only from held input, so no twitching at rest or near-target.
- **Quick match** — FIND MATCH searches the free Steam lobby directory
  (worldwide, filtered to this game + protocol version + open slots). Found: the
  host's SteamID is read straight off the lobby data and joined over P2P/SDR.
  Not found: you become the host and a public lobby advertises your game (player
  count + phase kept fresh, closed on leave). The Steam lobby is a directory
  entry only -- nobody joins it, so no membership bookkeeping. No servers, $0.
- **Sound** — XAudio2, zero assets: every effect is synthesized at startup
  (square/saw/noise + envelopes + a bitcrush for the low-bit techno style).
  Deep bass shoot + explosion, fire-crackle purchase burn, glass-shard slat
  break, hover/click blips, and a quiet seamless-loop engine hum that follows
  movement input (volume + pitch), tuned to never mask the effects.

## Controls

- `W A S D` / arrows — move up / down / left / right (screen-relative)
- Mouse — aim the turret (world-space cursor aim)
- Left mouse / `Space` — fire
- `F5` / `F6` / `F7` — toggle GI / SSAO / shadows
- `TAB` — upgrade shop, `R` — ready up in the lobby
- `ESC` — pause overlay (resume / settings / leave game); the game keeps
  running behind it — only LEAVE GAME disconnects
- Join screen accepts typing and `Ctrl+V` paste

## Build

Requirements: Windows 10/11, Visual Studio 2022+ with Desktop C++ workload, CMake 3.24+,
and the Steamworks SDK extracted to `third_party/steamworks/sdk` (not redistributable,
hence gitignored — grab it from https://partner.steamgames.com).

```
cmake -S . -B build -A x64
cmake --build build --config Release
bin\Release\TankaqClient.exe
```

The post-build step copies `steam_api64.dll`, writes `steam_appid.txt` (480), and copies
`assets/` + `shaders/` next to the exe.

## Playing with a friend

1. Both machines: Steam running and logged in, game built (same protocol version).
2. Host clicks **HOST GAME** and shares the yellow **GAME CODE** from the HUD.
3. Friend clicks **JOIN GAME**, pastes the code, **CONNECT** (relayed P2P — no port
   forwarding). On the same LAN you can instead enter the host's `ip:27500`.

Same-machine test: run one instance with `--host`, another with `--join=127.0.0.1:27500`
— or just paste your own game code into JOIN; the game detects it and reroutes to
loopback automatically (Steam cannot relay P2P to the same account).

Join troubleshooting: the client shows relay warm-up status while connecting and times
out after 30 s with the Steam end-reason; each process writes `tankaq_log_<pid>.txt`
next to the exe with the full connection trace. The SteamID64 relay path between two
*different* Steam accounts still needs a second account/machine to verify.

## Command-line flags (testing)

`--renderer=d3d11|d3d12`, `--solo`, `--host`, `--join=<code|ip[:port]>`, `--port=N`,
`--drive` (auto-drive + auto-fire), `--frames=N --screenshot=file.png` (dump backbuffer
and exit), `--winpos=x,y`, `--winsize=WxH`, `--novsync`, `--gi=0|1`, `--ao=0|1`,
`--girays=1..16`, `--temporal=2..16`, `--gires=half|full`, `--shadows=0|1`,
`--shadowres=1024|2048|4096`, `--shadowfilter=0|1|2`, `--aa=0|1`, `--boom`.

## Layout

```
src/            Main.cpp (loop/menu/camera), Game.* (sim), GpuDetect.*, AssetLoad.*, Ui.*
src/render/     IRenderer.h + RendererD3D11.cpp + RendererD3D12.cpp
src/net/        Protocol.h (wire format) + Net.* (Steam transport, host/client)
shaders/        Basic.hlsl (shared by both backends)
assets/tank/    tank_baked.glb + tank_meta.txt + LICENSE.md (CC0 attribution)
third_party/    cgltf, stb_image, stb_image_write, stb_easy_font, steamworks/ (gitignored)
tools/          bake_tank.py (model bake: split/reorient/palette-texture)
```
