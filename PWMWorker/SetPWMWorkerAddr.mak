# Pinscape Pico - PWM Worker Program makefile
# Copyright 2024 Michael J Roberts / BSD-3-Clause license / NO WARRANTY

OBJ = .\obj\          # trailing backslash
BIN = .\exe\          # trailing backslash
WINAPI = ..\WinAPI\   # trailing backslash
CFLAGS = /EHsc /std:c++17 /c
LD = link

OBJS = \
    $(OBJ)SetPWMWorkerAddr.obj \
    $(OBJ)RP2BootLoaderInterface.obj

all: $(BIN)SetPWMWorkerAddr.exe

clean:
    del $(OBJS) $(BIN)SetPWMWorkerAddr.exe

$(OBJ)RP2BootLoaderInterface.obj : $(WINAPI)RP2BootLoaderInterface.cpp

$(BIN)SetPWMWorkerAddr.exe: $(OBJS)
    $(LD) /out:$@ $(OBJS)

.SUFFIXES: .obj.cpp

.cpp{$(OBJ)}.obj:
    $(CC) $(CFLAGS) /Fo$(OBJ) $<

{$(WINAPI)}.cpp{$(OBJ)}.obj:
    $(CC) $(CFLAGS) /Fo$(OBJ) $<
