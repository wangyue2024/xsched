import sys

script_content = r'''<#
.SYNOPSIS
    XSched Windows (CUDA) 一键配置与取消配置脚本
.DESCRIPTION
    本脚本用于管理 XSched 对 Windows C:\Windows\System32\nvcuda.dll 的拦截代理配置。
    支持自动提权、配置拦截（setup）、恢复原生驱动（restore）、检查状态（status）。
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

# 检查管理员权限
function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# 强制要求管理员权限
function Demand-Admin {
    if (-not (Test-IsAdmin)) {
        Write-Host "[!] 需要管理员权限来修改 C:\Windows\System32 中的系统驱动文件。" -ForegroundColor Yellow
        Write-Host "[*] 正在尝试自动提升权限..." -ForegroundColor Cyan
        try {
            $scriptPath = $MyInvocation.MyCommand.Path
            $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$scriptPath`" -Action $Action"
            Start-Process powershell.exe -Verb RunAs -ArgumentList $argList
            Exit
        } catch {
            Write-Host "[X] 无法自动提升权限，请以管理员身份重新打开 PowerShell 并运行本脚本。" -ForegroundColor Red
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
    Write-Host "=== XSched Windows 配置状态检查 ===" -ForegroundColor Cyan
    if (Test-Path $NvcudaOriginal) {
        Write-Host " 当前状态: [ 已配置 XSched 拦截代理 ]" -ForegroundColor Green
        Write-Host " 备份的原生驱动: $NvcudaOriginal"
        Write-Host " 当前 System32 驱动: $NvcudaTarget (XSched Proxy)"
    } else {
        Write-Host " 当前状态: [ 原生 NVIDIA 驱动 (未启用 XSched 代理) ]" -ForegroundColor Yellow
        Write-Host " 当前 System32 驱动: $NvcudaTarget (Original)"
    }
    
    if (Test-Path $XSchedDll) {
        Write-Host " XSched 编译目标库: $XSchedDll (已就绪)" -ForegroundColor Green
    } else {
        Write-Host " XSched 编译目标库: $XSchedDll (未找到，请先运行 make cuda 进行编译)" -ForegroundColor Red
    }
    Write-Host "===================================="
    Write-Host ""
}

function Enable-XSched {
    Demand-Admin

    if (-not (Test-Path $XSchedDll)) {
        Write-Host "[X] 错误: 未在 '$XSchedDll' 找到编译好的 nvcuda.dll！" -ForegroundColor Red
        Write-Host "[*] 请先在项目根目录下运行 make cuda 编译 XSched。" -ForegroundColor Yellow
        return
    }

    if (Test-Path $NvcudaOriginal) {
        Write-Host "[!] 警告: 发现已存在备份驱动 '$NvcudaOriginal'，XSched 代理可能已经配置过。" -ForegroundColor Yellow
        $confirm = Read-Host "是否覆盖现有配置并重新复制最新的 XSched nvcuda.dll？(y/n)"
        if ($confirm -ne 'y' -and $confirm -ne 'Y') {
            Write-Host "[*] 操作已取消。" -ForegroundColor Cyan
            return
        }
    } else {
        Write-Host "[1/3] 正在获取 C:\Windows\System32\nvcuda.dll 的文件所有权与修改权限..." -ForegroundColor Cyan
        & takeown /f $NvcudaTarget | Out-Null
        & icacls $NvcudaTarget /grant administrators:F | Out-Null

        Write-Host "[2/3] 正在备份原生 nvcuda.dll 为 nvcuda_original.dll..." -ForegroundColor Cyan
        Move-Item -Path $NvcudaTarget -Destination $NvcudaOriginal -Force
    }

    Write-Host "[3/3] 正在将 XSched nvcuda.dll 复制到 C:\Windows\System32..." -ForegroundColor Cyan
    Copy-Item -Path $XSchedDll -Destination $NvcudaTarget -Force

    Write-Host ""
    Write-Host "[✓] 成功启用 XSched CUDA 代理拦截！" -ForegroundColor Green
    Show-Status
}

function Disable-XSched {
    Demand-Admin

    if (-not (Test-Path $NvcudaOriginal)) {
        Write-Host "[!] 未找到备份文件 '$NvcudaOriginal'，当前系统可能已是原生 NVIDIA 驱动。" -ForegroundColor Yellow
        return
    }

    Write-Host "[1/2] 正在恢复原生 nvcuda.dll 驱动..." -ForegroundColor Cyan
    Copy-Item -Path $NvcudaOriginal -Destination $NvcudaTarget -Force

    Write-Host "[2/2] 正在清理备份文件 nvcuda_original.dll..." -ForegroundColor Cyan
    Remove-Item -Path $NvcudaOriginal -Force

    Write-Host ""
    Write-Host "[✓] 成功取消 XSched 配置，已恢复原生 NVIDIA 驱动！" -ForegroundColor Green
    Show-Status
}

function Show-Menu {
    Show-Status
    Write-Host "请选择要执行的操作:" -ForegroundColor White
    Write-Host " [1] 配置并启用 XSched 代理 (setup / enable)" -ForegroundColor Green
    Write-Host " [2] 取消配置并恢复原生驱动 (restore / disable)" -ForegroundColor Yellow
    Write-Host " [3] 检查当前配置状态 (status)" -ForegroundColor Cyan
    Write-Host " [Q] 退出 (Exit)"
    
    $choice = Read-Host "`n请输入选项 [1-3/Q]"
    switch ($choice) {
        "1" { Enable-XSched }
        "2" { Disable-XSched }
        "3" { Show-Status }
        "Q" { exit }
        "q" { exit }
        default { Write-Host "无效选项" -ForegroundColor Red }
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
'''

with open('manage_xsched.ps1', 'w', encoding='utf-8-sig') as f:
    f.write(script_content)

print("Saved manage_xsched.ps1 with utf-8-sig encoding.")
