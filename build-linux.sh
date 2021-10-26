CC=gcc
INCLUDES="/opt/homebrew/include"
LIBRARIES="\
	/opt/homebrew/lib/libxcb.a \
	/opt/homebrew/lib/libXau.a \
	/opt/homebrew/lib/libXdmcp.a"
DYN_LIBRARY_PATH="/opt/homebrew/lib"
DYN_LIBRARIES="-lxkbcommon-x11 -lxkbcommon"

$CC main.c linux.c -o 6502 -g -Os -Wall -I$INCLUDES $LIBRARIES\
	-L$DYN_LIBRARY_PATH $DYN_LIBRARIES
