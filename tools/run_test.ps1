# Goal Line Tech - flash a test sketch and capture its serial output
#
# Re-runs one of the existing bring-up test sketches under firmware\ against
# a connected board: compiles it, uploads it, resets the board, and prints
# whatever it prints over Serial for a few seconds. This is the "prove it
# actually works" check (WiFi scan, BLE scan, MAC read, etc.), as opposed to
# check_boards.ps1 which only proves the bootloader responds.
#
# Usage:
#   .\tools\run_test.ps1 -Port COM5 -Test RadioTest
#   .\tools\run_test.ps1 -Port COM4 -Test GetWristMAC
#   .\tools\run_test.ps1 -Port COM5 -Test WifiConnectTest -ListenSeconds 15
#
# Available -Test values are the folder names under firmware\ (each folder
# holds a <name>.ino of the same name). FQBN is auto-detected from the
# board's MAC via known_boards.json; pass -Fqbn to override for a board
# that isn't registered there yet.

param(
    [Parameter(Mandatory=$true)][string]$Port,
    [Parameter(Mandatory=$true)][string]$Test,
    [string]$Fqbn,
    [int]$ListenSeconds = 8
)

$projectRoot = Split-Path -Parent $PSScriptRoot
$sketchDir   = Join-Path $projectRoot "firmware\$Test"
$sketchFile  = Join-Path $sketchDir "$Test.ino"

if (-not (Test-Path $sketchFile)) {
    Write-Host "No such test sketch: $sketchFile" -ForegroundColor Red
    Write-Host "Available tests:" -ForegroundColor Yellow
    Get-ChildItem (Join-Path $projectRoot "firmware") -Directory | ForEach-Object { Write-Host "  $($_.Name)" }
    exit 1
}

$esptool = Get-ChildItem "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\esptool_py" -Recurse -Filter "esptool.exe" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName

if (-not $Fqbn) {
    Write-Host "No -Fqbn given - identifying board on $Port via its MAC ..."
    $knownBoardsPath = Join-Path $PSScriptRoot "known_boards.json"
    $knownBoards = @{}
    if (Test-Path $knownBoardsPath) {
        $raw = Get-Content $knownBoardsPath -Raw | ConvertFrom-Json
        foreach ($mac in $raw.PSObject.Properties.Name) { $knownBoards[$mac.ToLower()] = $raw.$mac }
    }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $esptool
    $psi.Arguments = "--port $Port read-mac"
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    [void]$proc.Start()
    $finished = $proc.WaitForExit(10000)
    if (-not $finished) { try { $proc.Kill() } catch {} }
    $out = $proc.StandardOutput.ReadToEnd()
    $macLine = ($out -split "`r?`n" | Where-Object { $_ -match "^MAC:" }) | Select-Object -First 1
    $mac = ($macLine -replace "^MAC:\s*", "").Trim().ToLower()

    if ($mac -and $knownBoards.ContainsKey($mac)) {
        $Fqbn = $knownBoards[$mac].fqbn
        Write-Host "  MAC $mac -> $($knownBoards[$mac].name) -> fqbn $Fqbn" -ForegroundColor Cyan
    } else {
        Write-Host "  Could not auto-identify the board (MAC: '$mac')." -ForegroundColor Red
        Write-Host "  Pass -Fqbn explicitly, e.g. -Fqbn esp32:esp32:esp32  or  esp32:esp32:esp32cam" -ForegroundColor Yellow
        exit 1
    }
}

Write-Host ""
Write-Host "Compiling and uploading $Test to $Port ($Fqbn) ..."
arduino-cli compile --fqbn $Fqbn "$sketchFile" --upload -p $Port
if ($LASTEXITCODE -ne 0) {
    Write-Host "Compile/upload failed - see output above." -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Listening on $Port for $ListenSeconds seconds ..."
$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$sp.Open()

# Pulse DTR/RTS to force a clean reset so we catch the sketch from boot.
$sp.DtrEnable = $true
$sp.RtsEnable = $true
Start-Sleep -Milliseconds 100
$sp.DtrEnable = $false
$sp.RtsEnable = $false
Start-Sleep -Milliseconds 100
$sp.DtrEnable = $true

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$buffer = New-Object System.Text.StringBuilder
while ($sw.Elapsed.TotalSeconds -lt $ListenSeconds) {
    try {
        $chunk = $sp.ReadExisting()
        if ($chunk) { [void]$buffer.Append($chunk) }
    } catch {}
    Start-Sleep -Milliseconds 100
}
$sp.Close()

Write-Host ""
Write-Host "--- Serial output ---"
Write-Host $buffer.ToString()
