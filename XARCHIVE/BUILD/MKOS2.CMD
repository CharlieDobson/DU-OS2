@echo off
setlocal
rem ==========================================================================
rem  Build XArchive for OS/2 2.x / Warp (XARCHIVE.EXE, 32-bit PM) from THIS
rem  subdirectory.  Sources are one level up, resources
rem  in ..\RES.  Building from here keeps the object files out of the source
rem  tree, matching the BUILD\ convention the other OS/2 ports use.
rem
rem  Cross-built: Open Watcom's NT-hosted compiler emits OS/2 LX objects, so
rem  this runs on the Windows box and produces an executable for OS/2.  It
rem  cannot be run here.
rem
rem  Steps:
rem    1. host tool ICO2OS2.EXE  (converts the Windows icon to OS/2 format;
rem                               source is shared, in ..\..\COMMON\TOOLS)
rem    2. wcc386   -bt=os2       -> one .OBJ per source
rem    3. wrc      -bt=os2 -r    -> XARCHIVE.res
rem    4. wlink    sys os2v2_pm  -> XARCHIVE.EXE  (LX, PM)
rem    5. wrc                    -> binds XARCHIVE.res into XARCHIVE.EXE
rem ==========================================================================
rem  Watcom location.  An existing %WATCOM% is honoured -- a normal Open
rem  Watcom install sets it -- so the fallback below only matters when the
rem  variable is unset.  A path containing spaces will not work: Watcom's
rem  -i= option takes no quoting.
if not defined WATCOM set WATCOM=C:\WATCOM

if not exist "%WATCOM%\binnt\wlink.exe" goto nowatcom

set INCLUDE=%WATCOM%\H;%WATCOM%\H\OS2
set PATH=%WATCOM%\BINNT;%WATCOM%\BINW;%PATH%

rem  -i=.. matters twice over: it finds the headers, and it is what makes the
rem  format backends pick up the local windows.h compatibility shim when they
rem  say #include <windows.h>.  That shim is why those sources are byte-
rem  identical to their Win32s originals in the DU32 XArchive tree.
rem  -bm : MULTITHREADED runtime.  Extraction, the integrity test and the
rem        archive-header parse each run on a worker thread via
rem        ..\..\COMMON\PMWORKER.C, and _beginthread needs the multithreaded
rem        libraries.  Every module must have it, backends included.
set CFLAGS=-bt=os2 -bm -otexan -zq -w4 -e25 -i=.. -i=..\RES -i=..\..\COMMON -i=%WATCOM%\H -i=%WATCOM%\H\OS2

rem  256 KB, as the other OS/2 ports use.  The archive backends were made
rem  heap-based rather than stack-hungry for the 64 KB DOS build, so there is
rem  plenty of head-room here; the deepest chain is dialog -> extract -> the
rem  RAR solid sink.
set STACK=262144

pushd "%~dp0"

del XARCHIVE.EXE >nul 2>&1
del XARCHIVE.res >nul 2>&1
del XARCHIVE.PTR >nul 2>&1

rem --------------------------------------------------------------------------
rem  1. host tool: Windows .ICO -> OS/2 colour icon
rem
rem     wrc accepts a Windows .ICO for -bt=os2, exits 0 and embeds it verbatim
rem     as RT_POINTER, which PM then cannot load - you get the default program
rem     icon and no error anywhere.  This converter rewrites the same pixels
rem     in the OS/2 bitmap-array format.
rem --------------------------------------------------------------------------
rem  A genuine OS/2 icon in RES\XARCHIVE.OS2 -- one saved by OS/2's own
rem  ICONEDIT, or lifted from \OS2\BITMAP on the target machine -- is used
rem  as-is, and is always the safer source.  PM validates an icon resource
rem  strictly and rejects anything it dislikes outright: WinLoadPointer fails,
rem  that fails FCF_ICON, and that fails the whole WinCreateStdWindow with PM
rem  error 0x1034 PMERR_INVALID_RESOURCE_FORMAT.  Only when there is no native
rem  icon do we fall back to converting the Windows .ICO with the host tool.
if exist ..\RES\XARCHIVE.OS2 goto nativeicon

echo Building host tool ICO2OS2...
wcl386 -bt=nt -q -zq -otexan ..\..\COMMON\TOOLS\ICO2OS2.C -fe=ICO2OS2.EXE
if errorlevel 1 goto failed
del ICO2OS2.obj >nul 2>&1

.\ICO2OS2.EXE ..\RES\XARCHIVE.ICO XARCHIVE.PTR
if errorlevel 1 goto failed
goto haveicon

:nativeicon
echo Using the native OS/2 icon RES\XARCHIVE.OS2...
copy /y ..\RES\XARCHIVE.OS2 XARCHIVE.PTR >nul
if errorlevel 1 goto failed

:haveicon

rem --------------------------------------------------------------------------
rem  2. compile
rem
rem     PMMAIN / OS2PLAT / COMPAT are the port; everything else is the shared
rem     backend, copied from the Win32s tree unchanged.
rem --------------------------------------------------------------------------
echo Compiling...
for %%F in (PMMAIN OS2PLAT COMPAT ARCFILE ARCPREF SZARC ZIPARC RARARC RAR3DEC RAR5ARC DISKARC LZMADEC CRC32) do (
  wcc386 ..\%%F.C %CFLAGS%
  if errorlevel 1 goto failed
  if not exist %%F.obj goto failed
)

rem  The shared worker, from the tree the other OS/2 ports use.
wcc386 ..\..\COMMON\PMWORKER.C %CFLAGS%
if errorlevel 1 goto failed

rem --------------------------------------------------------------------------
rem  3. resources  (-i=. so the ICON statement finds XARCHIVE.PTR built above)
rem --------------------------------------------------------------------------
echo Compiling resources...
wrc -bt=os2 -r -q -i=. -i=..\RES -i=%WATCOM%\H\OS2 ..\RES\XARCHIVE.RC -fo=XARCHIVE.res
if errorlevel 1 goto failed

rem --------------------------------------------------------------------------
rem  4. link an OS/2 LX Presentation Manager image
rem --------------------------------------------------------------------------
echo Linking...
wlink sys os2v2_pm name XARCHIVE.EXE ^
  option quiet option map option stack=%STACK% ^
  option description 'XArchive - Dobson Utilities' ^
  file PMMAIN.obj,OS2PLAT.obj,COMPAT.obj,ARCFILE.obj,ARCPREF.obj,SZARC.obj,ZIPARC.obj, ^
       RARARC.obj,RAR3DEC.obj,RAR5ARC.obj,DISKARC.obj,LZMADEC.obj,CRC32.obj, ^
       PMWORKER.obj
if errorlevel 1 goto failed
if not exist XARCHIVE.EXE goto failed

rem --------------------------------------------------------------------------
rem  5. bind the resources into the LX executable
rem --------------------------------------------------------------------------
echo Binding resources...
wrc -bt=os2 -q XARCHIVE.res XARCHIVE.EXE
if errorlevel 1 goto failed

del *.obj >nul 2>&1

echo.
echo Built BUILD\XARCHIVE.EXE  (OS/2 2.x / Warp, 32-bit Presentation Manager).
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
