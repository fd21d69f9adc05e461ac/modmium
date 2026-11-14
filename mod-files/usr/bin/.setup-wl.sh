#!/bin/bash
# written by fd21d69f9adc05e461ac

# -- Pre TUI init --
stty -echo
source /usr/lib/libmosh.sh
if [[ -d /usr/local/nix/store ]]; then
  if ! mountpoint -q /nix; then
    mkdir -p /nix
    mount --bind /usr/local/nix /nix
  fi
  source /nix/var/nix/profiles/default/etc/profile.d/nix.sh
  unset LD_LIBRARY_PATH
fi

# -- MAIN SCRIPT --
tput civis # :whale:

# wine setup
export WINEPREFIX=${WINEPREFIX:-/usr/local/wine64}
export WINEARCH=win64

# markers
markdir=/usr/local/.markers
basicMark=$markdir/gui
wineMark=$markdir/wineGui
steamMark=$markdir/steam
curSize=$(cat $markdir/curSize 2>/dev/null)
[[ -n $curSize ]] || curSize=30

basic(){
  if [[ -f $basicMark ]]; then
        echo -e "${G}Already Installed!${N}"
        sleep 3
        exit
    fi
  ln -sf /usr/bin/.wl /usr/bin/wl
  source /usr/bin/wl
  echo -e "${B}Installing drivers...${N}"
  mix install adwaita-icon-theme gcc # adwaita for icons, gcc to compile the shim
  nix-build '<nixpkgs>' -A mesa -o /usr/local/.nixroots/mesa # mesa for graphics drivers, installed like this so 32 bit mesa can be installed for steam
  gcc -O2 -shared -fPIC -o /lib/wlshim.so /lib/wlshim.c # I wrote a shim to translate certain v4 wayland protocols to their v3 counterpart and spoof exo as v4 so clients requiring v4 don't break
  echo "30" > $markdir/curSize
  mkdir -p /usr/local/cras-lib
  ln -sf /usr/lib64/libcras.so /usr/local/cras-lib/libcras.so
  ln -sf /usr/lib64/libcras.so /usr/local/cras-lib/libcras.so.0
  touch $basicMark
  echo -e "${G}Done!${N}"
  sleep 3
  exit
}
wine(){
  if [[ -f $wineMark ]]; then
        echo -e "${G}Already Installed!${N}"
        sleep 3
        exit
    fi
  echo -e "${B}Installing wine, dxvk...${N}"
  mix install wineWow64Packages.full dxvk # wine, dxvk setup script
  WINEPREFIX=/usr/local/wine64
  WINEARCH=win64
  rm -rf $WINEPREFIX # In case user is reinstalling to repair a broken install
  chown root:root /usr/local &>/dev/null # Wine will refuse to create a prefix in a directory you don't own.
  wl wineboot -u # initialize wineprefix
  DXVK=$(nix-build '<nixpkgs>' -A dxvk --no-out-link | tail -n 1)
  sleep 3 # evidently there is some kind of timing issue otherwise, need to give time for wineboot to actually wrap up
  wl $DXVK/bin/setup_dxvk.sh install -p $WINEPREFIX # install dxvk dll's
  touch $wineMark
  echo -e "${G}Done!${N}"
  sleep 3
  exit
}
steam(){
  if [[ -f $steamMark ]]; then
      echo -e "${G}Already Installed!${N}"
      sleep 3
      exit
  fi
  echo -e "${B}Installing steam, this will take a ${R}LONG TIME and a LOT OF SPACE (around 6GB).${N}"
  echo -e "${R}You have 3 seconds to cancel (Ctrl+C)...${N}"
  sleep 3;
  echo -e "${Y}Continuing...${N}"
  export NIXPKGS_ALLOW_UNFREE=1
  mix install dejavu_fonts dxvk openbox python3 steam util-linux.bin wayland-utils wineWow64Packages.full xdpyinfo xrdb xsetroot xwayland
  for pkg in pkgsi686Linux.mesa pkgsCross.mingwW64.stdenv.cc pkgsCross.mingw32.stdenv.cc; do
    nix-build '<nixpkgs>' -A "$pkg" -o /usr/local/.nixroots/"$pkg" # Installed like this to prevent conflicts, as we need both 64bit and 32bit versions for steamworks
  done
  for script in steam-launch steam-patch-bwrap steam-set-compat steam-wine-daemon; do
    ln -sf /usr/bin/."$script" /usr/bin/"$script" # Symlinks, so updates to the dot versions will actually update the ones visible in $PATH
  done
  touch $steamMark
  echo -e "${G}Done!${N}"
  sleep 3
  exit
}
cursor(){
  stty echo
  echo -e "${P}Current cursor size: ${curSize}${N}"
  echo -ne "${B}Enter integer cursor size (default 30): ${N}"
  read -re newSize
  [[ $newSize =~ ^[0-9]+$ ]] || (echo -e "${R}Invalid input.${N}"; sleep 3; exit)
  echo $newSize > $markdir/curSize
  echo -e "${G}Done!${N}"
  sleep 3
  exit
}

menu_reset(){
  if [[ -f $basicMark ]]; then
    basicText="Linux-native GUI ${G}(Installed)${N}"
  else
    basicText="Linux-native GUI"
  fi
  if [[ -f $wineMark ]]; then
    wineText="Wine GUI support ${G}(Installed)${N}"
  else
    wineText="Wine GUI support"
  fi
  if [[ -f $steamMark ]]; then
    steamText="Steam ${G}(Installed)${N}"
  else
    steamText="Steam"
  fi
  if [[ -f $basicMark && $(arch) == "x86_64" ]]; then
    options=("$basicText" "$wineText" "$steamText" "Set cursor size" "Go Back")
    functions=("basic" "wine" "steam" "cursor" "quit")
  else
    options=("$basicText" "Set cursor size" "Go Back")
    functions=("basic" "cursor" "quit")
  fi
  num_options=${#options[@]}
  menuText=$(cat <<EOF

This is a setup wizard to enable running GUI applications installed via Nix.
Installs a wrapper called \`wl\` (usage, \`wl <program>\`).

If using wine, default WINEPREFIX is /usr/local/wine64, \`wl\` will use a given WINEPREFIX if specified in environment vars.
Call applications like \`wl wine path_to\\program.exe\`

Note: installing Steam can be done without installing wine. Command is \`steam-launch\` when installed.

${R}Steam and wine support are x86_64 only!${N}\n
EOF
  )
  num_options=${#options[@]}
}

menu_reset
clear
full_menu
tput cnorm
selector
