<#
.SYNOPSIS
    XSched Windows (CUDA) one-click setup / restore script.
.DESCRIPTION
    This script manages the XSched interception proxy for the Windows
    C:\Windows\System32\nvcuda.dll driver. It supports automatic privilege
    elevation, proxy setup, native driver restore and status checks.
.EXAMPLE
    .\manage_xsched.ps1 -Action setup
    .\manage_xsched.ps1 -Action restore
    .\manage_xsched.ps1 -Action status
#>

[CmdletBinding()]
param (
    [Parameter(Position=0)]
    [ValidateSet("setup", "enable", "restore", "disable", "status", "menu", "help")]
    [string]$Action = "menu"
)

$ErrorActionPreference = "Stop"

# Check whether the current session has administrator privileges.
function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Require administrator privileges, elevating automatically if needed.
function Demand-Admin {
    if (-not (Test-IsAdmin)) {
        Write-Host "[!] Administrator privileges are required to modify the system driver in C:\Windows\System32." -ForegroundColor Yellow
        Write-Host "[*] Attempting to elevate automatically..." -ForegroundColor Cyan
        try {
            $scriptPath = $MyInvocation.MyCommand.Path
            $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$scriptPath`" -Action $Action"
            Start-Process powershell.exe -Verb RunAs -ArgumentList $argList
            Exit
        } catch {
            Write-Host "[X] Failed to elevate automatically. Please re-run this script from an elevated PowerShell." -ForegroundColor Red
            Exit 1
        }
    }
}

$System32Dir = "C:\Windows\System32"
$NvcudaOriginal = Join-Path $System32Dir "nvcuda_original.dll"
$NvcudaTarget = Join-Path $System32Dir "nvcuda.dll"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$XSchedDll = Join-Path $ScriptDir "output\bin\nvcuda.dll"

function Show-Status {
    Write-Host ""
    Write-Host "=== XSched Windows configuration status ===" -ForegroundColor Cyan
    if (Test-Path $NvcudaOriginal) {
        Write-Host " Current state: [ XSched interception proxy enabled ]" -ForegroundColor Green
        Write-Host " Backed-up native driver: $NvcudaOriginal"
        Write-Host " Current System32 driver: $NvcudaTarget (XSched Proxy)"
    } else {
        Write-Host " Current state: [ Native NVIDIA driver (XSched proxy disabled) ]" -ForegroundColor Yellow
        Write-Host " Current System32 driver: $NvcudaTarget (Original)"
    }

    if (Test-Path $XSchedDll) {
        Write-Host " XSched build artifact: $XSchedDll (ready)" -ForegroundColor Green
    } else {
        Write-Host " XSched build artifact: $XSchedDll (not found; run 'make cuda' first)" -ForegroundColor Red
    }
    Write-Host "===================================="
    Write-Host ""
}

function Enable-XSched {
    Demand-Admin

    if (-not (Test-Path $XSchedDll)) {
        Write-Host "[X] Error: the compiled nvcuda.dll was not found at '$XSchedDll'!" -ForegroundColor Red
        Write-Host "[*] Please build XSched with 'make cuda' in the repository root first." -ForegroundColor Yellow
        return
    }

    if (Test-Path $NvcudaOriginal) {
        Write-Host "[!] Warning: an existing backup '$NvcudaOriginal' was found; the XSched proxy may already be configured." -ForegroundColor Yellow
        $confirm = Read-Host "Overwrite the existing configuration with the latest XSched nvcuda.dll? (y/n)"
        if ($confirm -ne 'y' -and $confirm -ne 'Y') {
            Write-Host "[*] Operation cancelled." -ForegroundColor Cyan
            return
        }
    } else {
        Write-Host "[1/3] Taking ownership and granting modify permission on C:\Windows\System32\nvcuda.dll..." -ForegroundColor Cyan
        & takeown /f $NvcudaTarget | Out-Null
        & icacls $NvcudaTarget /grant administrators:F | Out-Null

        Write-Host "[2/3] Backing up the native nvcuda.dll as nvcuda_original.dll..." -ForegroundColor Cyan
        Move-Item -Path $NvcudaTarget -Destination $NvcudaOriginal -Force
    }

    Write-Host "[3/3] Copying the XSched nvcuda.dll into C:\Windows\System32..." -ForegroundColor Cyan
    Copy-Item -Path $XSchedDll -Destination $NvcudaTarget -Force

    Write-Host ""
    Write-Host "[OK] XSched CUDA interception proxy enabled successfully!" -ForegroundColor Green
    Show-Status
}

function Disable-XSched {
    Demand-Admin

    if (-not (Test-Path $NvcudaOriginal)) {
        Write-Host "[!] Backup file '$NvcudaOriginal' not found; the system may already be running the native NVIDIA driver." -ForegroundColor Yellow
        return
    }

    Write-Host "[1/2] Restoring the native nvcuda.dll driver..." -ForegroundColor Cyan
    Copy-Item -Path $NvcudaOriginal -Destination $NvcudaTarget -Force

    Write-Host "[2/2] Cleaning up the backup file nvcuda_original.dll..." -ForegroundColor Cyan
    Remove-Item -Path $NvcudaOriginal -Force

    Write-Host ""
    Write-Host "[OK] XSched configuration removed; the native NVIDIA driver is restored!" -ForegroundColor Green
    Show-Status
}

function Show-Menu {
    Show-Status
    Write-Host "Select an operation:" -ForegroundColor White
    Write-Host " [1] Enable the XSched proxy (setup / enable)" -ForegroundColor Green
    Write-Host " [2] Remove the configuration and restore the native driver (restore / disable)" -ForegroundColor Yellow
    Write-Host " [3] Check the current configuration status (status)" -ForegroundColor Cyan
    Write-Host " [Q] Exit"

    $choice = Read-Host "`nEnter an option [1-3/Q]"
    switch ($choice) {
        "1" { Enable-XSched }
        "2" { Disable-XSched }
        "3" { Show-Status }
        "Q" { exit }
        "q" { exit }
        default { Write-Host "Invalid option" -ForegroundColor Red }
    }
}

switch ($Action.ToLower()) {
    "setup"   { Enable-XSched }
    "enable"  { Enable-XSched }
    "restore" { Disable-XSched }
    "disable" { Disable-XSched }
    "status"  { Show-Status }
    default   { Show-Menu }
}
