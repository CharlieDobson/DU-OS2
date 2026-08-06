@echo off
setlocal
rem ==========================================================================
rem  Build the OS/2 2.x / Warp Explorer (EXPLORER.EXE, 32-bit PM) from THIS
rem  subdirectory.  Sources are one level up, resources
rem  in ..\RES.  Building from here keeps the object files out of the source
rem  tree, matching the convention the DiskStat and Enhanced Notepad ports use.
rem
rem  Cross-built: Open Watcom's NT-hosted compiler emits OS/2 LX objects, so
rem  this runs on the Windows box and produces an executable for OS/2.  It
rem  cannot be run here.
rem
rem  Steps:
rem    1. host tool ICO2OS2.EXE  (converts the Windows icon to OS/2 format;
rem                               source is shared, in ..\..\COMMON\TOOLS)
rem    2. wpp386   -bt=os2       -> one .OBJ per module
rem    3. wrc      -bt=os2 -r    -> EXPLORER.res
rem    4. wlink    sys os2v2_pm  -> EXPLORER.EXE  (LX, PM, 256 KB stack)
rem    5. wrc                    -> binds EXPLORER.res into EXPLORER.EXE
rem ==========================================================================
rem  Watcom location.  An existing %WATCOM% is honoured -- a normal Open
rem  Watcom install sets it -- so the fallback below only matters when the
rem  variable is unset.  A path containing spaces will not work: Watcom's
rem  -i= option takes no quoting.
if not defined WATCOM set WATCOM=C:\WATCOM

if not exist "%WATCOM%\binnt\wlink.exe" goto nowatcom
set INCLUDE=%WATCOM%\H;%WATCOM%\H\OS2
set PATH=%WATCOM%\BINNT;%WATCOM%\BINW;%PATH%

rem  -xs / -xr : no exception tables, no RTTI
rem  -bm       : MULTITHREADED runtime.  Copy/move/delete (FILEOPS.CPP) and
rem              the search (SEARCH.CPP) each run on a worker thread via
rem              ..\..\COMMON\PMWORKER.C, and _beginthread needs the
rem              multithreaded libraries.  EVERY module must have it -- mixing
rem              gives an image whose errno and heap are not thread-safe, and
rem              the resulting corruption is intermittent rather than loud.
set CFLAGS=-bt=os2 -bm -otexan -zq -w4 -e25 -xs -xr -i=.. -i=..\RES -i=..\..\COMMON -i=%WATCOM%\H -i=%WATCOM%\H\OS2

rem  Same flags minus -xs / -xr, which wcc386 rejects: they are C++ switches
rem  (exception tables, RTTI) and PMWORKER.C is plain C.
set CFLAGS_C=-bt=os2 -bm -otexan -zq -w4 -e25 -i=.. -i=..\RES -i=..\..\COMMON -i=%WATCOM%\H -i=%WATCOM%\H\OS2

rem  !! STACK !!  FILEOPS.CPP's CopyTree / DeleteTree / TreeSize and
rem  SEARCH.CPP's DoSearch all recurse one frame per directory level, and
rem  each frame carries a CCHMAXPATH buffer or two.  256 KB, not the default.
set STACK=262144

pushd "%~dp0"

del EXPLORER.EXE >nul 2>&1
del EXPLORER.res >nul 2>&1
del EXPLORER.PTR >nul 2>&1

rem --------------------------------------------------------------------------
rem  1. host tool: Windows .ICO -> OS/2 colour icon
rem
rem     wrc accepts an ICON statement pointing at a Windows .ICO and embeds it
rem     without converting it, which PM then cannot load.  The bitmaps do NOT
rem     need this: a 40-byte-header Windows DIB already is a valid OS/2 2.0
rem     bitmap.  See ..\..\README.md.
rem --------------------------------------------------------------------------
rem  A genuine OS/2 icon in RES\EXPLORER.OS2 -- one saved by OS/2's own
rem  ICONEDIT, or lifted from \OS2\BITMAP on the target machine -- is used
rem  as-is, and is always the safer source.  PM validates an icon resource
rem  strictly and rejects anything it dislikes outright: WinLoadPointer fails,
rem  that fails FCF_ICON, and that fails the whole WinCreateStdWindow with PM
rem  error 0x1034 PMERR_INVALID_RESOURCE_FORMAT.  Only when there is no native
rem  icon do we fall back to converting the Windows .ICO with the host tool.
if exist ..\RES\EXPLORER.OS2 goto nativeicon

echo Building host tool ICO2OS2...
wcl386 -bt=nt -q -zq -otexan ..\..\COMMON\TOOLS\ICO2OS2.C -fe=ICO2OS2.EXE
if errorlevel 1 goto failed
del ICO2OS2.obj >nul 2>&1

.\ICO2OS2.EXE ..\RES\EXPLORER.ICO EXPLORER.PTR
if errorlevel 1 goto failed
goto haveicon

:nativeicon
echo Using the native OS/2 icon RES\EXPLORER.OS2...
copy /y ..\RES\EXPLORER.OS2 EXPLORER.PTR >nul
if errorlevel 1 goto failed

:haveicon

rem --------------------------------------------------------------------------
rem  2. compile
rem --------------------------------------------------------------------------
echo Compiling...
wpp386 ..\PMMAIN.CPP  %CFLAGS%
if errorlevel 1 goto failed
wpp386 ..\DRIVES.CPP  %CFLAGS%
if errorlevel 1 goto failed
wpp386 ..\DIRVIEW.CPP %CFLAGS%
if errorlevel 1 goto failed
wpp386 ..\PAINT.CPP   %CFLAGS%
if errorlevel 1 goto failed
wpp386 ..\TOOLBAR.CPP %CFLAGS%
if errorlevel 1 goto failed
wpp386 ..\STATBAR.CPP %CFLAGS%
if errorlevel 1 goto failed
wpp386 ..\SEARCH.CPP  %CFLAGS%
if errorlevel 1 goto failed
wpp386 ..\FILEOPS.CPP %CFLAGS%
if errorlevel 1 goto failed
wpp386 ..\PROPS.CPP   %CFLAGS%
if errorlevel 1 goto failed
wpp386 ..\NETWORK.CPP %CFLAGS%
if errorlevel 1 goto failed

rem  The shared worker is plain C (XArchive and Enhanced Notepad link it too),
rem  so it goes through wcc386 rather than wpp386.
wcc386 ..\..\COMMON\PMWORKER.C %CFLAGS_C%
if errorlevel 1 goto failed

rem --------------------------------------------------------------------------
rem  3. resources
rem
rem     -i=.     so the ICON statement finds EXPLORER.PTR built above
rem     -i=..\RES so the BITMAP statements find SM\ MED\ LRG\ TOOLBAR\
rem --------------------------------------------------------------------------
echo Compiling resources...
wrc -bt=os2 -r -q -i=. -i=..\RES -i=%WATCOM%\H\OS2 ..\RES\EXPLORER.RC -fo=EXPLORER.res
if errorlevel 1 goto failed

rem --------------------------------------------------------------------------
rem  4. link an OS/2 LX Presentation Manager image
rem --------------------------------------------------------------------------
echo Linking...
wlink sys os2v2_pm name EXPLORER.EXE ^
  option quiet option map option stack=%STACK% ^
  option description 'Explorer - Dobson Utilities file manager' ^
  file PMMAIN.obj,DRIVES.obj,DIRVIEW.obj,PAINT.obj,TOOLBAR.obj,STATBAR.obj ^
  file SEARCH.obj,FILEOPS.obj,PROPS.obj,NETWORK.obj,PMWORKER.obj
if errorlevel 1 goto failed
if not exist EXPLORER.EXE goto failed

rem --------------------------------------------------------------------------
rem  5. bind the resources into the LX executable
rem --------------------------------------------------------------------------
echo Binding resources...
wrc -bt=os2 -q EXPLORER.res EXPLORER.EXE
if errorlevel 1 goto failed

del *.obj >nul 2>&1

echo.
echo Built BUILD\EXPLORER.EXE  (OS/2 2.x / Warp, 32-bit Presentation Manager).
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
