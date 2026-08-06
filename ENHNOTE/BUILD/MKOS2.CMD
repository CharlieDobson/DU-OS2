@echo off
setlocal
rem ==========================================================================
rem  Build the OS/2 2.x / Warp Enhanced Notepad (ENHNOTE.EXE, 32-bit PM) from
rem  THIS subdirectory.  Sources are one level up,
rem  resources in ..\RES.  Building from here keeps the object files out of
rem  the source tree, matching the BUILD\ convention the other OS/2 ports use.
rem
rem  Cross-built: Open Watcom's NT-hosted compiler emits OS/2 LX objects, so
rem  this runs on the Windows box and produces an executable for OS/2.  It
rem  cannot be run here.
rem
rem  Steps:
rem    1. host tool ICO2OS2.EXE  (converts the Windows icon to OS/2 format;
rem                               source is shared, in ..\..\COMMON\TOOLS)
rem    2. wcc386   -bt=os2       -> PMMAIN.OBJ, EDITCTL.OBJ, GAPBUF.OBJ
rem    3. wrc      -bt=os2 -r    -> ENHNOTE.RES
rem    4. wlink    sys os2v2_pm  -> ENHNOTE.EXE  (LX, PM, 256 KB stack)
rem    5. wrc                    -> binds ENHNOTE.RES into ENHNOTE.EXE
rem ==========================================================================
rem  Watcom location.  An existing %WATCOM% is honoured -- a normal Open
rem  Watcom install sets it -- so the fallback below only matters when the
rem  variable is unset.  A path containing spaces will not work: Watcom's
rem  -i= option takes no quoting.
if not defined WATCOM set WATCOM=C:\WATCOM

if not exist "%WATCOM%\binnt\wlink.exe" goto nowatcom

set INCLUDE=%WATCOM%\H;%WATCOM%\H\OS2
set PATH=%WATCOM%\BINNT;%WATCOM%\BINW;%PATH%

rem  -xs / -xr are C++ switches; this tree is C, so the flags are just the
rem  optimiser, quiet mode and the warning level the other ports use.
set CFLAGS=-bt=os2 -otexan -zq -w4 -e25 -i=.. -i=..\RES -i=%WATCOM%\H -i=%WATCOM%\H\OS2

rem  A gap-buffer editor keeps its document on the heap, so the stack only
rem  has to cover PM's callback depth -- but 256 KB matches the other OS/2
rem  ports and costs nothing but address space.
set STACK=262144

pushd "%~dp0"

del ENHNOTE.EXE >nul 2>&1
del ENHNOTE.res >nul 2>&1
del ENHNOTE.PTR >nul 2>&1

rem --------------------------------------------------------------------------
rem  1. host tool: Windows .ICO -> OS/2 colour icon
rem
rem     wrc accepts a Windows .ICO for -bt=os2 and embeds it verbatim, which
rem     PM then cannot load.  This converter rewrites the same pixels in the
rem     OS/2 bitmap-array format.
rem --------------------------------------------------------------------------
rem  A genuine OS/2 icon in RES\ENHNOTE.OS2 -- one saved by OS/2's own ICONEDIT,
rem  or lifted from \OS2\BITMAP on the target machine -- is used as-is, and is
rem  always the safer source.  PM validates an icon resource strictly and
rem  rejects anything it dislikes outright: WinLoadPointer fails, that fails
rem  FCF_ICON, and that fails the whole WinCreateStdWindow with PM error
rem  0x1034 PMERR_INVALID_RESOURCE_FORMAT.  Only when there is no native icon
rem  do we fall back to converting the Windows .ICO with the host tool.
if exist ..\RES\ENHNOTE.OS2 goto nativeicon

echo Building host tool ICO2OS2...
wcl386 -bt=nt -q -zq -otexan ..\..\COMMON\TOOLS\ICO2OS2.C -fe=ICO2OS2.EXE
if errorlevel 1 goto failed
del ICO2OS2.obj >nul 2>&1

.\ICO2OS2.EXE ..\RES\ENHNOTE.ICO ENHNOTE.PTR
if errorlevel 1 goto failed
goto haveicon

:nativeicon
echo Using the native OS/2 icon RES\ENHNOTE.OS2...
copy /y ..\RES\ENHNOTE.OS2 ENHNOTE.PTR >nul
if errorlevel 1 goto failed

:haveicon

rem --------------------------------------------------------------------------
rem  2. compile
rem --------------------------------------------------------------------------
echo Compiling...
wcc386 ..\PMMAIN.C  %CFLAGS%
if errorlevel 1 goto failed
wcc386 ..\EDITCTL.C %CFLAGS%
if errorlevel 1 goto failed
wcc386 ..\GAPBUF.C  %CFLAGS%
if errorlevel 1 goto failed

rem --------------------------------------------------------------------------
rem  3. resources  (-i=. so the ICON statement finds ENHNOTE.PTR built above)
rem --------------------------------------------------------------------------
echo Compiling resources...
wrc -bt=os2 -r -q -i=. -i=..\RES -i=%WATCOM%\H\OS2 ..\RES\ENHNOTE.RC -fo=ENHNOTE.res
if errorlevel 1 goto failed

rem --------------------------------------------------------------------------
rem  4. link an OS/2 LX Presentation Manager image
rem --------------------------------------------------------------------------
echo Linking...
wlink sys os2v2_pm name ENHNOTE.EXE ^
  option quiet option map option stack=%STACK% ^
  option description 'Enhanced Notepad - Dobson Utilities' ^
  file PMMAIN.obj,EDITCTL.obj,GAPBUF.obj
if errorlevel 1 goto failed
if not exist ENHNOTE.EXE goto failed

rem --------------------------------------------------------------------------
rem  5. bind the resources into the LX executable
rem --------------------------------------------------------------------------
echo Binding resources...
wrc -bt=os2 -q ENHNOTE.res ENHNOTE.EXE
if errorlevel 1 goto failed

del *.obj >nul 2>&1

echo.
echo Built BUILD\ENHNOTE.EXE  (OS/2 2.x / Warp, 32-bit Presentation Manager).
popd
endlocal
goto :eof

:nowatcom
echo.
echo *** Open Watcom was not found at "%WATCOM%".
echo ***
echo *** Set the WATCOM environment variable to your Open Watcom 1.9
echo *** installation directory, or edit the fallback near the top of
echo *** this script.
endlocal
exit /b 1

:failed
echo.
echo *** BUILD FAILED - see the wcc386/wrc/wlink errors above.
popd
endlocal
exit /b 1
