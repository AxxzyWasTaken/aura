# Startup and recovery options

Configured Stage 1 images boot their selected Stage 2 immediately by default.
`tools/fill_stage1_config.py` sets the proxy window to zero unless
`--window-ms` explicitly requests a development interception window.
Changing this default does not modify an already enrolled boot object:
configure and enroll a new Stage 1 to replace an older 15-second window.

The boot-argument tokens are matched exactly:

- `m1n1-debug` selects DebugUSB early and prevents CDC from taking its carrier.
- `m1n1-proxy` skips Stage 1 payload loading and enters the proxy. It is ignored
  by an ESP-loaded Stage 2, so a deliberately chainloaded Stage 2 can boot.

The options may be combined. `m1n1-debug` alone does not request a proxy pause.
Normal payload failure still follows the existing recovery fallback.
