@echo off
setlocal
rem ==========================================================================
rem  Build and run OS2T.EXE - the backend unit test for the OS/2 port.
rem
rem  WHY THIS EXISTS.  The OS/2 build cannot be run on this machine, and the
rem  one bug it most needs to not regress - a doubled path separator, which
rem  made extracting to the root of a drive fail - is invisible to BOTH
rem  emulators available here.  DOSBox forgives "D:\\FILE.TXT" and so does
rem  Windows NT; real DOS reads it as a UNC name (see TransPath in the MS-DOS
rem  6 source) and OS/2 rejects it too.  So the observable thing is the STRING
rem  BuildPath builds, not the file that appears afterwards - and that can be
rem  checked here, on the host, against the OS/2 tree's OWN sources.
rem
rem  It compiles ..\SZARC.C through ..\windows.h, the same shim the OS/2 build
rem  uses, as an ordinary NT console program.  SZARC.C is #included rather
rem  than linked because BuildPath and SzDictCost are both static.
rem
rem  ARCFILE.C and COMPAT.C are deliberately NOT linked - see the stub block
rem  in OS2T.C for why (the _WIN32 branch collides with the shim, and COMPAT's
rem  calendar half calls DosGetDateTime, which is not here).
rem
rem  This is a UNIT test of the two 2026-08-28 backend fixes.  It is not a
rem  substitute for running XARCHIVE.EXE on OS/2; it is what can be checked
rem  without one.
rem ==========================================================================
if not defined WATCOM set WATCOM=C:\WATCOM
if not exist "%WATCOM%\binnt\wcl386.exe" goto nowatcom

set INCLUDE=%WATCOM%\H
set PATH=%WATCOM%\BINNT;%WATCOM%\BINW;%PATH%

pushd "%~dp0"

rem  wcl386 exits 0 even on compile errors, so deleting the target first is
rem  the only reliable success signal (the same trap as MKTUI.CMD).
del OS2T.EXE >nul 2>&1

rem  -i=%WATCOM%\H\OS2 is needed because the shim includes <os2def.h> for its
rem  base typedefs; the headers alone compile fine for an NT target.
wcl386 -bt=nt -l=nt -zq -w0 -i=.. -i=%WATCOM%\H\OS2 ^
  OS2T.C ..\LZMADEC.C ..\CRC32.C -fe=OS2T.EXE

del *.obj >nul 2>&1

if not exist OS2T.EXE goto failed

echo.
rem  Explicitly relative: the current directory is not always on the command
rem  search path (NoDefaultCurrentDirectoryInExePath), and a bare name then
rem  fails with "not recognized" AFTER a perfectly good build.
.\OS2T.EXE
if errorlevel 1 goto testfailed

echo.
echo Backend unit test PASSED.
popd
endlocal
goto :eof

:testfailed
echo.
echo *** BACKEND UNIT TEST FAILED - see the FAIL lines above.
popd
endlocal
exit /b 1

:failed
echo.
echo *** BUILD FAILED - see the wcl386 errors above.
popd
endlocal
exit /b 1

:nowatcom
echo.
echo *** Open Watcom was not found at "%WATCOM%".
endlocal
exit /b 1
