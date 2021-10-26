CC=x86_64-w64-mingw32-gcc
LIBRARIES="\
	/opt/homebrew/Cellar/mingw-w64/9.0.0_2/toolchain-x86_64/mingw/lib/libcomdlg32.a\
	/opt/homebrew/Cellar/mingw-w64/9.0.0_2/toolchain-x86_64/mingw/lib/libgdi32.a"

$CC main.c windows.c -o 6502.exe -g -Os -Wall -static -mwindows $LIBRARIES
