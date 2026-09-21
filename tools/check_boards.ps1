# Goal Line Tech - board ping / connectivity check
#
# Talks directly to the ESP32 ROM bootloader (esptool read-mac) on every
# serial port currently present. Read-only - never touches flash contents,
# safe to run any time, on any board, as often as you like.
#
# Usage:
#   .\tools\check_boards.ps1
#
# For each port found it reports: responds or not, chip type, MAC address,
# and (if the MAC matches tools\known_boards.json) which physical board
# and role that is in this project.

$scriptDir = $PSScriptRoot
$knownBoardsPath = Join-Path $scriptDir "known_boards.json"
$knownBoards = @{}
if (Test-Path $knownBoardsPath) {
    $raw = Get-Content $knownBoardsPath -Raw | ConvertFrom-Json
    foreach ($mac in $raw.PSObject.Properties.Name) {
        $knownBoards[$mac.ToLower()] = $raw.$mac
    }
}

$esptool = Get-ChildItem "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\esptool_py" -Recurse -Filter "esptool.exe" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
if (-not $esptool) {
    Write-Host "esptool.exe not found under Arduino15\packages\esp32\tools\esptool_py - is the esp32 core installed?" -ForegroundColor Red
    exit 1
}

$ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
if (-not $ports) {
    Write-Host "No serial ports detected at all." -ForegroundColor Yellow
    Write-Host "Check: USB cable is a data cable (not charge-only), board is plugged into a direct port (not a hub), and the driver is installed (CH340 / CP210x)." -ForegroundColor Yellow
    exit 0
}

Write-Host "Found $($ports.Count) serial port(s): $($ports -join ', ')"
Write-Host ""

foreach ($port in $ports) {
    Write-Host "-- $port --"

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $esptool
    $psi.Arguments = "--port $port read-mac"
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    [void]$proc.Start()
    $finished = $proc.WaitForExit(10000)

    if (-not $finished) {
        try { $proc.Kill() } catch {}
        Write-Host "  TIMEOUT - no response from bootloader. Board may be mid-boot, in use by another program (Serial Monitor?), or unresponsive." -ForegroundColor Red
        Write-Host ""
        continue
    }

    $out = $proc.StandardOutput.ReadToEnd()
    $err = $proc.StandardError.ReadToEnd()

    if ($proc.ExitCode -ne 0) {
        Write-Host "  NOT RESPONDING as an ESP32 (exit code $($proc.ExitCode))." -ForegroundColor Red
        $reason = ($err -split "`n" | Where-Object { $_ -match "fatal error" }) -join " "
        if ($reason) { Write-Host "  $reason" -ForegroundColor DarkYellow }
        Write-Host ""
        continue
    }

    $chipLine = ($out -split "`r?`n" | Where-Object { $_ -match "^Chip type:" }) | Select-Object -First 1
    $macLine  = ($out -split "`r?`n" | Where-Object { $_ -match "^MAC:" }) | Select-Object -First 1
    $mac = ($macLine -replace "^MAC:\s*", "").Trim().ToLower()

    Write-Host "  ALIVE" -ForegroundColor Green
    if ($chipLine) { Write-Host "  $($chipLine.Trim())" }
    Write-Host "  MAC: $mac"

    if ($mac -and $knownBoards.ContainsKey($mac)) {
        $b = $knownBoards[$mac]
        Write-Host "  Identified: $($b.name)  [role: $($b.role), fqbn: $($b.fqbn)]" -ForegroundColor Cyan
    } elseif ($mac) {
        Write-Host "  Not in known_boards.json yet - add it once you've confirmed what this board is." -ForegroundColor DarkYellow
    }
    Write-Host ""
}
