CC=gcc
INCLUDES="/opt/homebrew/include"
LIBRARIES="\
	/opt/homebrew/lib/libxcb.a \
	/opt/homebrew/lib/libXau.a \
	/opt/homebrew/lib/libXdmcp.a"
DYN_LIBRARY_PATH="/opt/homebrew/lib"
DYN_LIBRARIES="-lxkbcommon-x11 -lxkbcommon"

FLAGS=$([[ "$1" == "release" ]] && echo "-Os -flto" || echo "-g")
echo "Building with flags: $FLAGS"

$CC main.c linux.c -o 6502 $FLAGS -Wall -I$INCLUDES $LIBRARIES\
	-L$DYN_LIBRARY_PATH $DYN_LIBRARIES
