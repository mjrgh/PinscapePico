@echo off
echo %DATE% %TIME% > BuildTime.txt
del CMakeFiles\PinscapePico.dir\Version.cpp.obj
nmake %*
