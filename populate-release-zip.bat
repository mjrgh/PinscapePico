@echo off
rem  Usage:
rem    populate-release-zip <versionTag>
rem
rem  <versionTag> is included in the .zip file names; use format
rem  "v1.2.3"

rem  Check arguments
if %1# == # (
  echo Missing version tag
  goto EOF
)
set rlsVersionTag=%1

rem  Construct the release date stamp
set SysDateTime=
for /f "skip=1" %%i in ('wmic os get localdatetime') do if not defined SysDateTime set SysDateTime=%%i
set ReleaseDate=%SysDateTime:~0,8%
set ReleaseTag=%rlsVersionTag%-%ReleaseDate%
echo Building release %ReleaseDate%

rem  Delete any existing zip
set ReleaseZip="%cd%\Releases\PinscapePico-%ReleaseTag%.zip"
if exist %ReleaseZip% del %ReleaseZip%

rem  Save a copy of the firmware link map file in the release folder
copy Firmware\PinscapePico.elf.map "%cd%\Releases\PinscapePico-%ReleaseTag%.elf.map"

rem  Build the new zip
zip %ReleaseZip% License.txt
zip -j %ReleaseZip% Firmware\PinscapePico.uf2 Firmware\PinscapePico.elf.map
zip -j %ReleaseZip% PWMWorker\PWMWorker.uf2
zip -j %ReleaseZip% PWMWorker\exe\SetPWMWorkerAddr.exe

pushd x64\release
zip %ReleaseZip% GUIConfigTool.exe Scintilla.dll MicrosoftEdgeWebview2Setup.exe
zip -r %ReleaseZip% Help ConfigTemplates
zip %ReleaseZip% ConfigTool.exe
popd

rem  Build the Button Latency Tester zip
set BLT2ReleaseZip="%cd%\Releases\ButtonLatencyTester2-%ReleaseTag%.zip"
if exist %BLT2ReleaseZip% del %BLT2ReleaseZip%
zip -j %BLT2ReleaseZip% ButtonLatencyTester2\Firmware\ButtonLatencyTester2.uf2
zip -j %BLT2ReleaseZip% x64\release\ButtonLatencyTester2.exe

:EOF
