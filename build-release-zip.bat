@echo off

rem  Construct the release date stamp
set SysDateTime=
for /f "skip=1" %%i in ('wmic os get localdatetime') do if not defined SysDateTime set SysDateTime=%%i
set ReleaseDate=%SysDateTime:~0,8%
echo Building release %ReleaseDate%

rem  Delete any existing zip
set ReleaseZip="%cd%\Releases\PinscapePico-%ReleaseDate%.zip"
if exist %ReleaseZip% del %ReleaseZip%

rem  Build the new zip
zip %ReleaseZip% License.txt
zip -j %ReleaseZip% Firmware\PinscapePico.uf2
zip -j %ReleaseZip% PWMWorker\PWMWorker.uf2
zip -j %ReleaseZip% PWMWorker\exe\SetPWMWorkerAddr.exe

pushd x64\release
zip %ReleaseZip% GUIConfigTool.exe Scintilla.dll MicrosoftEdgeWebview2Setup.exe
zip -r %ReleaseZip% Help ConfigTemplates
zip %ReleaseZip% ConfigTool.exe
popd
