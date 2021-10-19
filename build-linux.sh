CC=gcc
I_XCB=/opt/homebrew/include
L_XCB="/opt/homebrew/lib/libxcb.a \
	/opt/homebrew/Cellar/libxau/1.0.9/lib/libXau.a \
	/opt/homebrew/Cellar/libxdmcp/1.1.3/lib/libXdmcp.a"

$CC main.c linux.c -o 6502 -g -Wall -I$I_XCB $L_XCB
