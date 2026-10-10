# ============================================================================
#  tools/flash.ps1 —— ESP32-C3 内置 USB-Serial/JTAG 专用烧录脚本
#
#  为什么不能直接 pio run -t upload：
#    本板的 USB 直连芯片内置的 USB-Serial/JTAG。当固件已经在正常运行时，esptool
#    默认的 RTS/DTR 复位序列无法把芯片带回 ROM 下载模式，于是报：
#        A fatal error occurred: Failed to connect to ESP32-C3:
#        Wrong boot mode detected (0xd)! The chip needs to be in download mode.
#    PlatformIO 自己会拼上 --before default_reset（追加在我们的设置之后，会覆盖
#    设置），所以只能绕开它，直接用 esptool 的 USB-JTAG 专用复位序列 --before usb_reset。
#
#  用法（在项目根目录下执行）：
#    powershell -NoProfile -ExecutionPolicy Bypass -File tools\flash.ps1
#    powershell -NoProfile -ExecutionPolicy Bypass -File tools\flash.ps1 -Port COM7
#    powershell -NoProfile -ExecutionPolicy Bypass -File tools\flash.ps1 -SkipBuild
#
#  如果连 usb_reset 都进不去（极少见）：按住板上 BOOT(GPIO9) -> 点一下 RESET ->
#  松开 BOOT，然后带上 -SkipReset 再执行本脚本。
# ============================================================================
#  注意：本文件必须保存为 "UTF-8 with BOM"。
#  Windows PowerShell 5.1 读无 BOM 的 .ps1 会按 GBK 解码，中文注释里的引号会被
#  吃掉并报 "Unexpected token" 语法错误。用编辑器改完请确认编码是 UTF-8 with BOM。
# ============================================================================
[CmdletBinding()]
param(
    [string]$Port = "",
    [switch]$SkipBuild,
    [switch]$SkipReset,
    [int]$Baud = 460800
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$pio         = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
$python      = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
$esptool     = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\esptool.py"
$framework   = Join-Path $env:USERPROFILE ".platformio\packages\framework-arduinoespressif32"
$buildDir    = Join-Path $projectRoot ".pio\build\esp32c3"
$bootApp0    = Join-Path $framework "tools\partitions\boot_app0.bin"

foreach ($f in @($pio, $python, $esptool, $bootApp0)) {
    if (-not (Test-Path $f)) { throw "找不到必要文件: $f" }
}

# ---- 没指定端口就自动找一个 USB 串口 ----
if ([string]::IsNullOrWhiteSpace($Port)) {
    $cands = @()
    try {
        $cands = Get-CimInstance Win32_PnPEntity -ErrorAction Stop |
                 Where-Object { $_.Name -match '\(COM\d+\)' -and $_.Name -match 'USB|Serial|JTAG' } |
                 ForEach-Object { [regex]::Match($_.Name, '\((COM\d+)\)').Groups[1].Value }
    } catch { }
    if (-not $cands -or @($cands).Count -eq 0) {
        $cands = [System.IO.Ports.SerialPort]::GetPortNames()
    }
    $cands = @($cands | Select-Object -Unique)
    if ($cands.Count -eq 0) {
        throw "没找到串口。请插好 USB，或用 -Port COMxx 手动指定。"
    }
    if ($cands.Count -gt 1) {
        Write-Host "发现多个串口: $($cands -join ', ')" -ForegroundColor Yellow
    }
    $Port = $cands[0]
}

if (-not $SkipBuild) {
    Write-Host "[1/2] 编译..." -ForegroundColor Cyan
    & $pio run -d $projectRoot | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "编译失败" }
} else {
    Write-Host "[1/2] 跳过编译" -ForegroundColor DarkGray
}

foreach ($f in @("bootloader.bin", "partitions.bin", "firmware.bin")) {
    $p = Join-Path $buildDir $f
    if (-not (Test-Path $p)) { throw "缺少构建产物: $p（先去掉 -SkipBuild 跑一次）" }
}

$before = if ($SkipReset) { "no_reset" } else { "usb_reset" }

Write-Host "[2/2] 烧录到 $Port (--before $before)..." -ForegroundColor Cyan
$esptoolArgs = @(
    $esptool,
    "--chip", "esp32c3",
    "--port", $Port,
    "--baud", "$Baud",
    "--before", $before,
    "--after", "hard_reset",
    "write_flash", "-z",
    "--flash_mode", "dio",
    "--flash_freq", "80m",
    "--flash_size", "4MB",
    "0x0",     (Join-Path $buildDir "bootloader.bin"),
    "0x8000",  (Join-Path $buildDir "partitions.bin"),
    "0xe000",  $bootApp0,
    "0x10000", (Join-Path $buildDir "firmware.bin")
)
& $python @esptoolArgs
if ($LASTEXITCODE -ne 0) {
    throw "烧录失败。若提示进不了下载模式：按住 BOOT(GPIO9) 点一下 RESET 再松开，然后加 -SkipReset 重试。"
}
Write-Host "烧录完成。看日志： pio device monitor --port $Port --dtr 0 --rts 0" -ForegroundColor Green