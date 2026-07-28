#!/bin/bash

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/laytonhd"
cd "$GAMEDIR"

exec > "$GAMEDIR/log.txt" 2>&1

mkdir -p "$GAMEDIR/conf"
mkdir -p "$GAMEDIR/cache"
export XDG_DATA_HOME="$GAMEDIR/conf"
export XDG_CONFIG_HOME="$GAMEDIR/conf"

# libbsd ships with the port; SDL2 comes from the device
export LD_LIBRARY_PATH="$GAMEDIR/libs:$LD_LIBRARY_PATH"

chmod a+x "$GAMEDIR/laytonloader"
pm_platform_helper "$GAMEDIR/laytonloader"

# The loader reads controllers, touch and its own on-screen keyboard directly
# through SDL, so no gptokeyb mapping is needed.
"$GAMEDIR/laytonloader" layton.toml

pm_finish
