simavr - a lean and mean Atmel AVR simulator for linux
======

Origianl repo: https://github.com/buserror/simavr

This repo created for debugging of GRBL project under Windows, using real CNC host applications on the virtual CNC machine.

Steps to create debugging environment:
1. prepare MinGW environment (install to `F:\MinGW`)
2. prepare AVR toolchain (install to `F:\AVR\bin\Atmel_Toolchain`)
3. install `VScode`
4. install `Free Virtual Serial Ports`
  - add virtual COM port bridge, for example COM2-COM3
5. checkout this repository
6. open repository folder in the VScode
7. build the project
8. start debugging:
  - start debugging of simulator app:
    a. select debugger configuration `(gdb) Launch`
    b. start debugging
  - start debugging of GRBL:
    a. select debugger configuration `Attach to gdbserver`
    b. launch in the terminal `grbl_cnc.exe -g \\.\COM2`
    c. start debugging
9. Launch your preferred CNC host app (g-code sender)
  - select COM3 port (where connected `grbl_cnc.exe`)
10. continue debugging as usual

Attention! By default:
 - working disk is `F:`
 - path to MinGW: `F:\MinGW`
 - path to AVR toolchain: `F:\AVR\bin\Atmel_Toolchain`
 - path to repository: `F:\AVR\src\simavr`
