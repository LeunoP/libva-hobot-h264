# MPV helpers for RDK-X5

These optional files configure MPV for this board's VA-API/VPU and DRM/GBM setup. Review the device, display mode, audio, and service settings before installing them on another system.

| File | Purpose |
|---|---|
| `mpv.conf` | DRM output, VA-API decode, an X11 `vaapi-copy` profile, and a software-decoded HLS/live profile. The DRM connector and mode are configured in the file. |
| `fallback-restart.lua` | Probes hardware playback for five seconds. One watchdog anomaly or a decode error requests software decoding; during probing it restarts from the beginning, and later it switches at the current position. |
| `mpv-launcher-wrapper.sh` | Sets the Hobot library environment. In non-X11 mode, it can stop and restart LightDM to release DRM master. It expects `/usr/local/bin/mpv.bin` and uses `sudo systemctl`; review those assumptions before use. |

The fallback script requires the driver watchdog event file and a working MPV software decoder. A software fallback is a recovery path, not hardware codec support.

## Install user configuration

```bash
mkdir -p ~/.config/mpv/scripts
cp fallback-restart.lua ~/.config/mpv/scripts/
mkdir -p ~/.config/mpv
cp mpv.conf ~/.config/mpv/mpv.conf
```

The launcher wrapper changes system display-service state. Do not install it as `/usr/local/bin/mpv` until its `mpv.bin` target, sudo policy, and LightDM behavior are verified for the target system.
