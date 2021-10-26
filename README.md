
# 6502js But C

A hobby project that replicates the [6502js](https://github.com/skilldrick/6502js) emulator, but with the speed of C. Why? For fun and learning :D

 - Simple on the outside: Replicates the simple and educational 6502js environment.
 - Simple on the inside: Plain C, and almost no libraries, not even SDL2.
 - Fast: Runs up to 28Mhz on my machine.
 - Correct: Passes Klaus functional tests, and Bruce Clark's decimal mode tests.
 
For programmers, the OS layer contains an example on how to use X11, Cocoa/Quartz2D or Win32 to draw stuff without a library like SDL2. See [for programmers](#for-programmers) for more info.

This emulator does not have a built-in assembler. Check out [writing your own binaries](#writing-your-own-binaries) for a recommended third-party assembler. Alternatively, check out [6502asm](6502asm.com) or [easy6502](https://skilldrick.github.io/easy6502/).

Finally, do note that this emulator does not have accurate cycle emulation (1 instruction = 1 cycle).

## Installation

Download binaries for Windows or Mac on the [releases page](https://gitlab.com/limdingwen/6502js-but-c/-/releases).

For Linux, please [compile from source](#compilation).

## Usage

Downloads come with a folder of demo binaries which you can use to try out the emulator with. If you wish to write your own, please see
[writing your own binaries](#writing-your-own-binaries). (Note: Most of the demos don't belong to me -- check the attribution file in the folder for more info.)

For Windows and Mac, simply run by double-clicking the app. However, if you want to change the speed, you'll need to run via command line.

Command line options:

    Usage: 6502 file.bin [options]
    Options:
    -unlimited: Run with no speed limiter (default: limited)
    -s(speed_in_khz): Set speed limit (default: 30)

For Linux, you'll need to run via command line.

## Writing your own binaries

Use any assembler for this that can produce simple binaries. I would recommend [Virtual 6502 Assembler](https://www.masswerk.at/6502/assembler.html).

The emulator will load your code into `$0600`. Specify this in your assembler by putting in `*=$600` or similar.

The screen is from memory address `$0200` to `$05FF`, from the top-left to the bottom-right corner for a total of 32 x 32 pixels. Colours repeat after `$F`. Colour list:

    $0 Black
    $1 White
    $2 Red
    $3 Cyan
    $4 Purple
    $5 Green
    $6 Blue
    $7 Yellow
    $8 Orange
    $9 Brown
    $A Light Red
    $B Dark Gray
    $C Gray
    $D Light Green
    $E Light Blue
    $F Light Gray

To get user input, read from `$FF`, which will be changed to the ASCII value of the character pressed, when the user presses a keyboard button.

To get a random number, read from `$FE`, which will change every cycle.

## Compilation

### Configuration (all platforms)

Please check [`main.c`](https://gitlab.com/limdingwen/6502js-but-c/-/blob/main/main.c) for a bunch of fun configuration options, such as the ability to change memory locations, the screen size, FPS, and various debug utilities.

### Windows

You'll need MingW. (Not tested on actual Windows, I cross-compile via Mac instead.)

Edit `build-windows-mingw.sh` to change `CC` to your compiler and `LIBRARIES` to point to the MingW versions of `libcomdlg32` and `libgdi32`. Compile by running `sh build-windows-mingw.sh`.

### Mac

You'll need XCode. Compile by running `sh build-mac.sh`, or just compile `mac6502` from XCode.

You may try compiling without XCode. Just make sure to compile both `main.c` and `mac.c` and include `-framework Cocoa`. However, you will only get a Unix binary, and the menu bar may not work properly.

### Linux

You'll need [XCB](https://xcb.freedesktop.org/) and [XKBCommon](https://xkbcommon.org/). Edit `build-linux.sh` to point to your installation of XCB and XKBCommon. Compile by running `sh build-linux.sh`.

## For programmers

 - `main.c` contains all of the emulator code.
 - `os.h` contains a common interface for all 3 OSes, inspired by SDL2.
 - `windows.c`, `linux.c`, and `mac6502/mac6502/mac.m` contain working examples of how to create a window, receive user input and draw rects using Win32, X11 (via XCB/XKBCommon) and Cocoa/Quartz2D.

It's not the best code (I'm still learning it myself), and it's not hardware accelerated, but some of this information was *hard* to find, so I hope my code can help you here too.

## Contributing

Merge requests are welcome! Issues are too, but this is just a hobby project for me, so please be patient.

## License

[AGPL v3.0](https://www.gnu.org/licenses/agpl-3.0.en.html)

