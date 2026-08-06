@echo off
setlocal
rem ==========================================================================
rem  Build the OS/2 2.x / Warp DiskStat (DISKSTAT.EXE, 32-bit PM) from THIS
rem  subdirectory.  Sources are one level up, resources
rem  in ..\RES.  Building from here keeps the object files out of the source
rem  tree.
rem
rem  Cross-built: Open Watcom's NT-hosted compiler emits OS/2 LX objects, so
rem  this runs on the Windows box and produces an executable for OS/2.  It
rem  cannot be run here.
rem
rem  Steps:
rem    1. host tool ICO2OS2.EXE  (converts the Windows icon to OS/2 format;
rem                               source is shared, in ..\..\COMMON\TOOLS)
rem    2. wpp386   -bt=os2       -> PMMAIN.OBJ, DRAW.OBJ, SCAN.OBJ
rem    3. wrc      -bt=os2 -r    -> DISKSTAT.RES
rem    4. wlink    sys os2v2_pm  -> DISKSTAT.EXE  (LX, PM, 256 KB stack)
rem    5. wrc                    -> binds DISKSTAT.RES into DISKSTAT.EXE
rem ==========================================================================
rem  Watcom location.  An existing %WATCOM% is honoured -- a normal Open
rem  Watcom install sets it -- so the fallback below only matters when the
rem  variable is unset.  A path containing spaces will not work: Watcom's
rem  -i= option takes no quoting.
if "%WATCOM%"=="" set WATCOM=C:\WATCOM

if not exist "%WATCOM%\binnt\wlink.exe" goto nowatcom

if "%INCLUDE%"=="" set INCLUDE=%WATCOM%\H;%WATCOM%\H\OS2
set PATH=%WATCOM%\BINNT;%WATCOM%\BINW;%PATH%

rem  -xs / -xr : no exception tables, no RTTI
rem  -bm       : MULTITHREADED runtime.  The scan runs on a worker thread
rem              (SCAN.CPP + ..\..\COMMON\PMWORKER.C), and _beginthread needs
rem              the multithreaded libraries.  Without -bm the program still
rem              links, but errno, strtok and the heap are not thread-safe and
rem              the corruption is intermittent -- every module must have it.
set CFLAGS=-bt=os2 -bm -otexan -zq -w4 -e25 -xs -xr -i=.. -i=..\RES -i=..\..\COMMON -i=%WATCOM%\H -i=%WATCOM%\H\OS2

rem  Same flags minus -xs / -xr, which wcc386 rejects: they are C++ switches
rem  (exception tables, RTTI) and PMWORKER.C is plain C.
set CFLAGS_C=-bt=os2 -bm -otexan -zq -w4 -e25 -i=.. -i=..\RES -i=..\..\COMMON -i=%WATCOM%\H -i=%WATCOM%\H\OS2

rem  !! STACK !!  SCAN.CPP's ScanDirectory recurses one frame per directory
rem  level.  256 KB, not the default.
rem
rem  This sizes the MAIN thread ONLY.  The scan worker recurses down the same
rem  ScanDirectory, and its stack comes from the size PmWorkerStart is given
rem  (PMWORKER_STACK, the same 256 KB) -- not from here.  Changing one without
rem  the other is how a deep tree finds the shorter of the two.
set STACK=262144

pushd "%~dp0"

del DISKSTAT.EXE >nul 2>&1
del DISKSTAT.res >nul 2>&1
del DISKSTAT.PTR >nul 2>&1

rem --------------------------------------------------------------------------
rem  1. host tool: Windows .ICO -> OS/2 colour icon
rem --------------------------------------------------------------------------
rem  A genuine OS/2 icon in RES\DISKSTAT.OS2 -- one saved by OS/2's own
rem  ICONEDIT, or lifted from \OS2\BITMAP on the target machine -- is used
rem  as-is, and is always the safer source.  PM validates an icon resource
rem  strictly and rejects anything it dislikes outright: WinLoadPointer fails,
rem  that fails FCF_ICON, and that fails the whole WinCreateStdWindow with PM
rem  error 0x1034 PMERR_INVALID_RESOURCE_FORMAT.  Only when there is no native
rem  icon do we fall back to converting the Windows .ICO with the host tool.
if exist ..\RES\DISKSTAT.OS2 goto nativeicon

echo Building host tool ICO2OS2...
wcl386 -bt=nt -q -zq -otexan ..\..\COMMON\TOOLS\ICO2OS2.C -fe=ICO2OS2.EXE
if errorlevel 1 goto failed
del ICO2OS2.obj >nul 2>&1

.\ICO2OS2.EXE ..\RES\DISKSTAT.ICO DISKSTAT.PTR
if errorlevel 1 goto failed
goto haveicon

:nativeicon
echo Using the native OS/2 icon RES\DISKSTAT.OS2...
copy /y ..\RES\DISKSTAT.OS2 DISKSTAT.PTR >nul
if errorlevel 1 goto failed

:haveicon

rem --------------------------------------------------------------------------
rem  2. compile
rem --------------------------------------------------------------------------
echo Compiling...
wpp386 ..\PMMAIN.CPP %CFLAGS%
if errorlevel 1 goto failed
wpp386 ..\DRAW.CPP   %CFLAGS%
if errorlevel 1 goto failed
wpp386 ..\SCAN.CPP   %CFLAGS%
if errorlevel 1 goto failed

rem  The shared worker is plain C (XArchive and Enhanced Notepad link it too),
rem  so it goes through wcc386 rather than wpp386.
wcc386 ..\..\COMMON\PMWORKER.C %CFLAGS_C%
if errorlevel 1 goto failed

rem --------------------------------------------------------------------------
rem  3. resources  (-i=. so the ICON statement finds DISKSTAT.PTR built above)
rem --------------------------------------------------------------------------
echo Compiling resources...
wrc -bt=os2 -r -q -i=. -i=..\RES -i=%WATCOM%\H\OS2 ..\RES\DISKSTAT.RC -fo=DISKSTAT.res
if errorlevel 1 goto failed

rem --------------------------------------------------------------------------
rem  4. link an OS/2 LX Presentation Manager image
rem --------------------------------------------------------------------------
echo Linking...
wlink sys os2v2_pm name DISKSTAT.EXE ^
  option quiet option map option stack=%STACK% ^
  option description 'Disk Statistics - Dobson Utilities disk statistics' ^
  file PMMAIN.obj,DRAW.obj,SCAN.obj,PMWORKER.obj
if errorlevel 1 goto failed
if not exist DISKSTAT.EXE goto failed

rem --------------------------------------------------------------------------
rem  5. bind the resources into the LX executable
rem --------------------------------------------------------------------------
echo Binding resources...
wrc -bt=os2 -q DISKSTAT.res DISKSTAT.EXE
if errorlevel 1 goto failed

del *.obj >nul 2>&1

echo.
echo Built BUILD\DISKSTAT.EXE  (OS/2 2.x / Warp, 32-bit Presentation Manager).
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
echo *** BUILD FAILED - see the wpp386/wrc/wlink errors above.
popd
endlocal
exit /b 1
