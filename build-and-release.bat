@echo off
rem  Pinscape Pico Release Builder
rem
rem  Prerequisites:
rem  
rem   * Visual Studio path/environment variables set (run path-to-VS\vcvars.bat)
rem   * Pico SDK path/variables set (run path-to-sdk\pico-env.cmd)
rem

rem  Clean old builds
echo ^>^>^> Removing old builds
msbuild PinscapePico.sln -t:Clean -p:Configuration=Release;Platform=x86 -v:q -nologo
if errorlevel 1 goto abort
msbuild PinscapePico.sln -t:Clean -p:Configuration=Release;Platform=x64 -v:q -nologo
if errorlevel 1 goto abort

rem  Build the release configurations
echo.
echo ^>^>^> Building Release^|x86
msbuild PinscapePico.sln -t:Build -p:Configuration=Release;Platform=x86 -v:q -nologo
if errorlevel 1 goto abort

echo.
echo ^>^>^> Building Release^|x64
msbuild PinscapePico.sln -t:Build -p:Configuration=Release;Platform=x64 -v:q -nologo
if errorlevel 1 goto abort

rem  Run the release ZIP builder
call .\build-release-zip.bat

echo.
echo ^>^>^> Release completed
goto EOF

goto EOF

:abort
echo MSBUILD exited with error - aborted

:EOF
