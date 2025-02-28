@echo off
rem  Pinscape Pico Release Builder
rem
rem  Usage:
rem    build-and-release <arguments>
rem
rem  Arguments:
rem    --version <version-tag>    required; set the version tag for the zip files
rem                               names, of the form "v1.2.3"
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

rem  Parse arguments
set rlsVersionTag=
:optionLoop
if not %1# == # (
  if "%1" == "--version" (
    if %2# == # (
      echo Missing version tag for --version
      goto EOF
    )
    set rlsVersionTag=%2
    shift
    shift
  ) else (
    echo Invalid option "%1"
    goto EOF
  )

  goto optionLoop
)

rem  Check for mandatory arguments
if %rlsVersionTag%# == # (
  echo No version specified - use --version ^<tag^>
  goto EOF
)


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

echo ^>^>^> Building ButtonLatencyTester2 firmware
call :BuildFirmware ButtonLatencyTester2\Firmware
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

rem  Create the ZIP files
call .\populate-release-zip.bat %rlsVersionTag%
if errorlevel 1 goto abort

echo.
echo ^>^>^> Release created successfully
goto EOF

goto EOF

:abort
echo BUILD ERROR - Release files were not created

:EOF
