#!/bin/bash
export LIBVA_DRIVER_NAME=hobot
export LD_LIBRARY_PATH=/usr/hobot/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

# Check if user requested X11 mode
USE_X11=0
for arg in "$@"; do
    if [[ "$arg" == *"--vo=x11"* ]] || [[ "$arg" == *"--profile=x11"* ]]; then
        USE_X11=1
        break
    fi
done

# If DRM mode is active and Xorg is holding DRM master, temporarily stop LightDM
WAS_LIGHTDM_RUNNING=0
if [ $USE_X11 -eq 0 ] && systemctl is-active --quiet lightdm; then
    WAS_LIGHTDM_RUNNING=1
    sudo systemctl stop lightdm
    while pgrep -x Xorg >/dev/null 2>&1; do
        sleep 0.1
    done
fi

restore_lightdm() {
    if [ $WAS_LIGHTDM_RUNNING -eq 1 ]; then
        sudo systemctl start lightdm
    fi
}
trap restore_lightdm EXIT INT TERM

/usr/local/bin/mpv.bin "$@"
EXIT_CODE=$?
exit $EXIT_CODE
