# Startup and recovery options

Configured Stage 1 images boot their selected Stage 2 immediately by default.
`tools/fill_stage1_config.py` sets the proxy window to zero unless
`--window-ms` explicitly requests a development interception window.
Changing this default does not modify an already enrolled boot object:
configure and enroll a new Stage 1 to replace an older 15-second window.

The boot-argument tokens are matched exactly:

- `aura-debug` selects DebugUSB early and prevents CDC from taking its carrier.
- `aura-proxy` makes Stage 1 wait up to 15 seconds for a host. A host that
  connects gets the proxy; otherwise Stage 1 boots its Stage 2 as normal. An
  ESP-loaded Stage 2 ignores it.
- An `aura-proxy` file at the root of the ESP Stage 1 boots from does the same,
  so a development Mac can be switched from Linux without touching Stage 1 or
  NVRAM: `touch <esp>/aura-proxy` to turn it on, `rm <esp>/aura-proxy` to turn
  it off. Without either, Stage 1 boots straight through.

The options may be combined. `aura-debug` alone does not request a proxy pause.
Normal payload failure still follows the existing recovery fallback.

## Common build profiles

Stage 1: `make RELEASE=1 CHAINLOADING=1 BUILTIN_LOGO=aurora`.
Stage 2: add `ESP_STAGE2=1` to that command. Preserve each stage before rebuilding.
These profiles are shared across boards. The embedded ESP configuration is
consumed on every supported chip, and normal boot attempts its payload before
initializing the USB proxy. An explicit proxy request or missing payload starts
the platform's USB transport; `aura-debug` retains DebugUSB instead.

The former Neo CDC/KIS make switches and Neo Stage 2 alias have been removed.
The raw image links both USB protocol engines; the PHY chooses the engine at
runtime. The native Aura image retains its native USB/DART implementation.
Physical register and firmware handoff differences remain runtime driver logic.

Cross-platform hardware qualification is required before replacing installed
boot objects; host tests and successful links do not establish that qualification.
