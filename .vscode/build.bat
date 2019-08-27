@%~d0%
@cd %~p0%\..
@ECHO Set CWD to %CD%
@SET CURDRV=%~d0%
:: @SET AVR32_HOME=%CURDRV%\AVR\bin\WinAVR-20100110
@SET AVR32_HOME=%CURDRV%\AVR\bin\Atmel_Toolchain\AVR8_GCC\Native\3.4.1061\avr8-gnu-toolchain
@SET PATH=%AVR32_HOME%\bin;%AVR32_HOME%\utils\bin;%PATH%
@ECHO WinAvr added to the PATH
@SET MINGW_HOME=%CURDRV%\MinGW
@SET PATH=%MINGW_HOME%\msys\1.0\bin;%MINGW_HOME%\bin;%PATH%
@ECHO MinGW added to the PATH
@SET CC=gcc

make %*%
