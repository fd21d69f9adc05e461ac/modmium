#!/bin/bash
# written by fd21d69f9adc05e461ac
# to ensure nix commands are in $PATH, just use the boilerplate all over this project
if [[ -d /usr/local/nix/store ]]; then
  if ! mountpoint -q /nix; then
    mkdir -p /nix
    mount --bind /usr/local/nix /nix
  fi
  source /nix/var/nix/profiles/default/etc/profile.d/nix.sh
  unset LD_LIBRARY_PATH
fi
markdir=/usr/local/.markers

# wayland setup
export XDG_RUNTIME_DIR=/run/user/$(id -u)
mkdir -p -m 0700 "$XDG_RUNTIME_DIR"
export WAYLAND_DISPLAY=/run/chrome/wayland-0
export WINEDEBUG=fixme-all,err-waylanddrv

# cursor setup
export XCURSOR_PATH=/root/.nix-profile/share/icons
export XCURSOR_THEME=Adwaita
export XCURSOR_SIZE=$(cat $markdir/curSize 2>/dev/null)

# get ALSA to work with CRAS
export LD_LIBRARY_PATH=/usr/local/cras-lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

# mesa setup
MESA=$(nix-build '<nixpkgs>' -A mesa --no-out-link | tail -n 1)
export __EGL_VENDOR_LIBRARY_FILENAMES=$MESA/share/glvnd/egl_vendor.d/50_mesa.json
export VK_DRIVER_FILES=$MESA/share/vulkan/icd.d/intel_icd.x86_64.json

# wine setup
export WINEPREFIX=${WINEPREFIX:-/usr/local/wine64}
export WINEARCH=win64
export DXVK_CONFIG="dxvk.maxFrameRate = 60"

# shim v4 wayland calls to v3
export LD_PRELOAD="/lib/wlshim.so"

exec "$@"
