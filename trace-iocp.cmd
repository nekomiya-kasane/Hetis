@echo off
setlocal

rem ===== Run as Administrator (auto self-elevate) =====
net session >nul 2>&1
if errorlevel 1 (
    echo Re-launching as Administrator...
    powershell -NoProfile -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"

set TARGET=G:\Teaching\Vulkan\build\x64-asan\bin\Demo.Playground.IocpTimer.exe
set OUT=%~dp0trace-iocp.etl

echo === Cleaning previous sessions ===
xperf -stop "NT Kernel Logger" >nul 2>&1
logman stop "MpWppTracing-20260625-001732-00000003-fffffffeffffffff" -ets >nul 2>&1
logman stop "MpWppCoreTracing-20260625-081732-00000003-100000000" -ets >nul 2>&1

echo === Starting kernel trace ===
xperf -on PROC_THREAD+LOADER+CSWITCH+DISPATCHER -stackwalk CSWITCH+READYTHREAD -BufferSize 1024 -MinBuffers 16 -MaxBuffers 64 -MaxFile 512 -FileMode Circular
if errorlevel 1 (
    echo [!] xperf -on failed. Check that nothing else is using NT Kernel Logger.
    pause
    exit /b 1
)

echo === Running target ===
"%TARGET%"
set RC=%errorlevel%

echo === Stopping trace ===
xperf -d "%OUT%"
if errorlevel 1 (
    echo [!] xperf -d failed. Trying abort/merge fallback...
    xperf -stop
    xperf -merge "%TEMP%\kernel.etl" "%OUT%" 2>nul
)

echo.
echo === Done. Trace: %OUT% (target exit code %RC%) ===
echo Open with: wpa "%OUT%"
echo.
pause
