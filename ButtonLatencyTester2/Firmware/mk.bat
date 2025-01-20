@echo off
echo %DATE% %TIME% > BuildTime.txt
if exist CMakeFiles\ButtonLatencyTester2.dir\Version.cpp.obj del CMakeFiles\ButtonLatencyTester2.dir\Version.cpp.obj
nmake %*
