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
curSize=$(cat $markdir/curSize 2>/dev/null)
[[ -n $curSize ]] || curSize=30

basic(){
  [[ -f $basicMark ]] && (echo -e "${G}Already installed!${N}"; sleep 3; exit)
  cp /usr/bin/.wl /usr/bin/wl
  source /usr/bin/wl
  echo -e "${B}Installing drivers...${N}"
  mix install mesa adwaita-icon-theme gcc # mesa for drivers, icon for icons, gcc to compile the shim
  gcc -O2 -shared -fPIC -o /lib/wlshim.so /lib/wlshim.c # I wrote a shim to translate certain v4 wayland protocols to their v3 counterpart and spoof exo as v4 so clients requiring v4 don't break
  echo "30" > $markDir/curSize
  mkdir -p /usr/local/cras-lib
  ln -sf /usr/lib64/libcras.so /usr/local/cras-lib/libcras.so
  ln -sf /usr/lib64/libcras.so /usr/local/cras-lib/libcras.so.0
  touch $basicMark
  echo -e "${G}Done!${N}"
  sleep 3
  exit
}
wine(){
  [[ -f $wineMark ]] && (echo -e "${G}Already installed!${N}"; sleep 3; exit)
  echo -e "${B}Installing wine, dxvk...${N}"
  mix install wineWow64Packages.full dxvk # wine, dxvk setup script
  WINEPREFIX=/usr/local/wine64
  WINEARCH=win64
  rm -rf $WINEPREFIX # in case user is reinstalling to repair a broken install
  wl wineboot -u # initialize wineprefix
  DXVK=$(nix-build '<nixpkgs>' -A dxvk --no-out-link | tail -n 1)
  sleep 3 # evidently there is some kind of timing issue otherwise, need to give time for wineboot to actually wrap up
  wl $DXVK/bin/setup_dxvk.sh install -p $WINEPREFIX # install dxvk dll's
  touch $wineMark
  echo -e "${G}Done!${N}"
  sleep 3
  exit
}
cursor(){
  stty echo
  echo -e "${P}Current cursor size: ${curSize}${N}"
  echo -e "${B}Enter integer cursor size (default 30): ${N}"
  read -re newSize
  [[ newSize =~ ^[0-9]+$ ]] || (echo -e "${R}Invalid input.${N}"; sleep 3)
  echo $newSize > $markdir/CurSize
  echo -e "${G}Done!${N}"
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
  if [[ -f $basicMark ]]; then
    options=("$basicText" "$wineText" "Set cursor size" "Go Back")
    functions=("basic" "wine" "cursor" "quit")
  else
    options=("$basicText" "Set cursor size" "Go Back")
    functions=("basic" "cursor" "quit")
  fi
  num_options=${#options[@]}
  menuText=$(cat <<EOF

This is a setup wizard to enable running GUI applications installed via Nix.
${R}CURRENTLY IN BETA, IF YOU DON'T KNOW WHAT WAYLAND OR WINE MEANS THEN DON'T USE THIS.${N}
Installs a wrapper called \`wl\` (usage, \`wl <program>\`).

If using wine, default WINEPREFIX is /usr/local/wine64, \`wl\` will use a given WINEPREFIX if specified in environment vars.
Call applications like \`wl wine path_to\\program.exe\`

EOF
  )
  num_options=${#options[@]}
}

menu_reset
clear
full_menu
tput cnorm
selector
