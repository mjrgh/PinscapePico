@echo off
rem  Pinscape Pico Release Builder
rem
rem  Prerequisites:
rem  
rem   * Visual Studio path/environment variables set (run path-to-VS\vcvars.bat)
rem   * Pico SDK path/variables set (run path-to-sdk\pico-env.cmd)
rem

goto main

:CleanFirmware
  pushd "%~1"
  if exist Makefile nmake /nologo clean
  if exist CMakeCache.txt del CMakeCache.txt
  if exist CMakeFiles rmdir /s /q CMakeFiles
  popd
  goto EOF

:BuildFirmware
  pushd "%~1"
  cmake -S . -G "NMake Makefiles"
  nmake /nologo
  popd
  goto EOF

:main

rem  Clean old builds
echo ^>^>^> Removing old builds
msbuild PinscapePico.sln -t:Clean -p:Configuration=Release;Platform=x86 -v:q -nologo
if errorlevel 1 goto abort
msbuild PinscapePico.sln -t:Clean -p:Configuration=Release;Platform=x64 -v:q -nologo
if errorlevel 1 goto abort
call :CleanFirmware firmware
call :CleanFirmware PWMWorker

rem  Build the firmware
echo ^>^>^> Building Pinscape Pico firmware
call :BuildFirmware firmware
if errorlevel 1 goto abort

echo ^>^>^> Building PWMWorker firmware
call :BuildFirmware PWMWorker
if errorlevel 1 goto abort

rem  Build the Windows Release configurations
echo.
echo ^>^>^> Building Windows tools (Release^|x86)
msbuild PinscapePico.sln -t:Build -p:Configuration=Release;Platform=x86 -v:q -nologo
if errorlevel 1 goto abort

echo.
echo ^>^>^> Building Windows tools (Release^|x64)
msbuild PinscapePico.sln -t:Build -p:Configuration=Release;Platform=x64 -v:q -nologo
if errorlevel 1 goto abort

rem  Run the release ZIP builder
call .\build-release-zip.bat
if errorlevel 1 goto abort

echo.
echo ^>^>^> Release created successfully
goto EOF

goto EOF

:abort
echo BUILD ERROR - Release files were not created

:EOF
