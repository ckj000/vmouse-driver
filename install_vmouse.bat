@echo off
setlocal

:: Check admin rights
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Run as Administrator  (right-click -> Run as administrator)
    pause
    exit /b 1
)

cd /d "%~dp0"

:: Check vmouse.sys
if not exist "core\vmouse_drv\vmouse.sys" (
    echo.
    echo [ERROR] core\vmouse_drv\vmouse.sys not found.
    echo.
    echo  Steps to download:
    echo  1. https://github.com/ckj000/vmouse-driver/actions
    echo  2. Latest run ^> Artifacts ^> vmouse-driver ^(download^)
    echo  3. Extract zip ^> copy ALL files to core\vmouse_drv\
    echo     ^(vmouse.sys, vmouse.inf, vmouse.cat, VMouseTestCert.cer^)
    echo  4. Re-run this script
    echo.
    pause
    exit /b 1
)

echo [OK] vmouse.sys found.
echo.

:: Step 1: Enable test signing
echo [1/3] Enabling test signing...
bcdedit /set testsigning on
if %errorlevel% neq 0 (
    echo [WARN] bcdedit failed - may already be on, or Secure Boot is blocking it.
)
echo.

:: Step 2: Install test certificate
echo [2/3] Installing test certificate...
if exist "core\vmouse_drv\VMouseTestCert.cer" (
    certutil -addstore "Root" "core\vmouse_drv\VMouseTestCert.cer"
    if %errorlevel% neq 0 (
        echo [WARN] Root store install failed
    )
    certutil -addstore "TrustedPublisher" "core\vmouse_drv\VMouseTestCert.cer"
    if %errorlevel% neq 0 (
        echo [WARN] TrustedPublisher store install failed
    )
    echo [OK] Certificate installed.
) else (
    echo [WARN] VMouseTestCert.cer not found - skipping
    echo        Download the full artifact from GitHub Actions and copy all 4 files.
)
echo.

:: Step 3: Install driver
echo [3/3] Installing driver...
pnputil /add-driver "core\vmouse_drv\vmouse.inf" /install
if %errorlevel% equ 0 (
    echo [OK] Driver registered. Reboot to activate.
) else (
    echo [WARN] pnputil failed (exit %errorlevel%)
    echo        If error says "no digital signature" - reboot first, then re-run.
    echo        Trying alternate syntax...
    pnputil /install-driver "core\vmouse_drv\vmouse.inf"
)

echo.
echo ================================================================
echo  Done! Next steps:
echo  1) REBOOT  (test signing + certificate take effect on reboot)
echo  2) After reboot, check Device Manager for "Virtual Mouse Driver"
echo ================================================================
echo.
pause
