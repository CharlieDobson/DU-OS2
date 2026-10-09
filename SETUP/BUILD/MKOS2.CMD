@echo off
setlocal
rem ==========================================================================
rem  Build the OS/2 2.x / Warp Dobson Utilities installer (SETUP.EXE, 32-bit
rem  PM) from THIS subdirectory.  Sources are one level up, resources in
rem  ..\RES.  Building from here keeps the object files out of the source
rem  tree.
rem
rem  Cross-built: Open Watcom's NT-hosted compiler emits OS/2 LX objects, so
rem  this runs on the Windows box and produces an executable for OS/2.  It
rem  cannot be run here.
rem
rem  Steps:
rem    1. host tool ICO2OS2.EXE  (converts the Windows icon to OS/2 format;
rem                               source is shared, in ..\..\COMMON\TOOLS)
rem    2. wpp386 -bt=os2         -> the objects
rem    3. wrc    -bt=os2 -r      -> SETUP.RES
rem    4. wlink  sys os2v2_pm    -> SETUP.EXE  (LX, PM, 128 KB stack)
rem    5. wrc                    -> binds SETUP.RES into SETUP.EXE
rem
rem  SETUP.EXE ships with SETUP.INF and DU.CWD (a PKZIP 2.04g archive of the
rem  files listed in the INF) beside it.  Neither is made here.
rem ==========================================================================
rem  Watcom location.  An existing %WATCOM% is honoured -- a normal Open
rem  Watcom install sets it -- so the fallback below only matters when the
rem  variable is unset.  A path containing spaces will not work: Watcom's
rem  -i= option takes no quoting.
if "%WATCOM%"=="" set WATCOM=C:\WATCOM

if not exist "%WATCOM%\binnt\wlink.exe" goto nowatcom

if "%INCLUDE%"=="" set INCLUDE=%WATCOM%\H;%WATCOM%\H\OS2
set PATH=%WATCOM%\BINNT;%WATCOM%\BINW;%PATH%

rem  -3r       : OS/2 2.x runs on a 386, so the code must too
rem  -xs / -xr : no exception tables, no RTTI
rem  No -bm: Setup is single-threaded.  The extraction runs on the PM
rem  thread and pumps messages from its progress callback, the same as the
rem  Windows installers.
rem  These flags are spelled out at the compile line below rather than held
rem  in CFLAGS.  NT 3.51's SET splits on the LAST '=', so it refuses any value
rem  containing one: "set CFLAGS=-bt=..." sets nothing there and reports only
rem  "The syntax of the command is incorrect".

rem  The deepest path is ExtractZip -> ExtractOne -> Inflate -> the progress
rem  callback -> the message pump, a few KB; 128 KB leaves nothing to chance.
set STACK=131072

rem  NT 3.51's cmd.exe has no %~dp0.  Win32 folds "<file>\.."
rem  lexically, so "%0\.." is the directory holding this script, and
rem  pushd - unlike cd - changes the drive as well.
pushd "%0\.."
if not exist MKOS2.CMD goto nohome

del SETUP.EXE >nul 2>&1
del SETUP.res >nul 2>&1
del SETUP.PTR >nul 2>&1

rem --------------------------------------------------------------------------
rem  1. host tool: Windows .ICO -> OS/2 colour icon
rem --------------------------------------------------------------------------
rem  A genuine OS/2 icon in RES\SETUP.OS2 -- one saved by OS/2's own
rem  ICONEDIT -- is used as-is, and is always the safer source.  PM validates
rem  an icon resource strictly and rejects anything it dislikes outright.
rem  Only when there is no native icon do we convert the Windows .ICO.
if exist ..\RES\SETUP.OS2 goto nativeicon

echo Building host tool ICO2OS2...
wcl386 -bt=nt -q -zq -otexan ..\..\COMMON\TOOLS\ICO2OS2.C -fe=ICO2OS2.EXE
if errorlevel 1 goto failed
del ICO2OS2.obj >nul 2>&1

.\ICO2OS2.EXE ..\RES\SETUP.ICO SETUP.PTR
if errorlevel 1 goto failed
goto haveicon

:nativeicon
echo Using the native OS/2 icon RES\SETUP.OS2...
copy /y ..\RES\SETUP.OS2 SETUP.PTR >nul
if errorlevel 1 goto failed

:haveicon

rem --------------------------------------------------------------------------
rem  2. compile
rem --------------------------------------------------------------------------
echo Compiling...
for %%F in (SETUP INIPARSE UNZIP INSTTOOL) do wpp386 ..\%%F.CPP -bt=os2 -3r -otexan -zq -w4 -e25 -xs -xr -i=.. -i=..\RES -i=%WATCOM%\H -i=%WATCOM%\H\OS2 || goto failed

rem --------------------------------------------------------------------------
rem  3. resources  (-i=. so the ICON statement finds SETUP.PTR built above)
rem --------------------------------------------------------------------------
echo Compiling resources...
wrc -bt=os2 -r -q -i=. -i=..\RES -i=%WATCOM%\H\OS2 ..\RES\SETUP.RC -fo=SETUP.res
if errorlevel 1 goto failed

rem --------------------------------------------------------------------------
rem  4. link an OS/2 LX Presentation Manager image
rem --------------------------------------------------------------------------
echo Linking...
wlink sys os2v2_pm name SETUP.EXE ^
  option quiet option map option stack=%STACK% ^
  option description 'Setup - Dobson Utilities for OS/2' ^
  file SETUP.obj,INIPARSE.obj,UNZIP.obj,INSTTOOL.obj
if errorlevel 1 goto failed
if not exist SETUP.EXE goto failed

rem --------------------------------------------------------------------------
rem  5. bind the resources into the LX executable
rem --------------------------------------------------------------------------
echo Binding resources...
wrc -bt=os2 -q SETUP.res SETUP.EXE
if errorlevel 1 goto failed

del *.obj >nul 2>&1

echo.
echo Built BUILD\SETUP.EXE  (OS/2 2.x / Warp, 32-bit Presentation Manager).
popd
endlocal
goto end

:nowatcom
echo.
echo *** Open Watcom was not found at "%WATCOM%".
echo ***
echo *** Set the WATCOM environment variable to your Open Watcom 1.9
echo *** installation directory, or edit the fallback near the top of
echo *** this script.
endlocal
goto failrc

:failed
echo.
echo *** BUILD FAILED - see the wpp386/wrc/wlink errors above.
popd
endlocal
goto failrc

:nohome
echo *** Could not switch to the directory holding MKOS2.CMD.
echo *** Run MKOS2.CMD from that directory; a path with spaces in it
echo *** cannot be found this way on NT 3.51.

:failrc
rem  NT 3.51's cmd.exe has no EXIT /B, and its EXIT ignores an exit code.
rem  A FIND that matches nothing is how this script hands ERRORLEVEL 1
rem  back to whoever called it, so it must be the LAST thing run.
ver | find "@@" > nul

:end
