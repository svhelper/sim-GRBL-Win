simavr - a lean and mean Atmel AVR simulator for linux
======

Origianl repo: https://github.com/buserror/simavr

This repo created for debugging of GRBL project under Windows, using real CNC host applications on the virtual CNC machine.

Steps to create debugging environment:
- prepare MinGW environment (install to `F:\MinGW`)
- prepare AVR toolchain (install to `F:\AVR\bin\Atmel_Toolchain`)
- install `VScode`
- install `Free Virtual Serial Ports`
 - add virtual COM port bridge, for example COM2-COM3
- checkout this repository
- open repository folder in the VScode
- build the project
- start debugging of simulator app:
 - select debugger configuration `(gdb) Launch`
 - start debugging
- start debugging of GRBL:
 - select debugger configuration `Attach to gdbserver`
 - launch in the terminal `grbl_cnc.exe -g \\.\COM2`
 - start debugging
- Launch your preferred CNC host app (g-code sender)
 - select COM3 port (where connected `grbl_cnc.exe`)
- continue debugging as usual

Attention! By default:
- working disk is `F:`
- path to MinGW: `F:\MinGW`
- path to AVR toolchain: `F:\AVR\bin\Atmel_Toolchain`
- path to repository: `F:\AVR\src\simavr`
