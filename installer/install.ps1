# obs-audio-router installer
# Run this on BOTH your Gaming PC and Streaming PC after downloading the zip.
# Usage: Right-click -> "Run with PowerShell"  (or run from an admin terminal)

param(
    [string]$ZipPath = ""
)

$ErrorActionPreference = "Stop"

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  OBS Audio Router - Installer" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# ── Find OBS installation ─────────────────────────────────────────────────────
$obs_paths = @(
    "$env:ProgramFiles\obs-studio",
    "$env:ProgramFiles(x86)\obs-studio",
    "$env:LOCALAPPDATA\Programs\obs-studio"
)

$obs_root = $null
foreach ($p in $obs_paths) {
    if (Test-Path "$p\bin\64bit\obs64.exe") {
        $obs_root = $p
        break
    }
}

if (-not $obs_root) {
    Write-Host "ERROR: OBS Studio not found." -ForegroundColor Red
    Write-Host "Please install OBS Studio from https://obsproject.com first."
    Read-Host "Press Enter to exit"
    exit 1
}
Write-Host "Found OBS at: $obs_root" -ForegroundColor Green

# ── Find the plugin zip ───────────────────────────────────────────────────────
if (-not $ZipPath) {
    # Look for zip next to this script
    $script_dir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $zips = Get-ChildItem -Path $script_dir -Filter "obs-audio-router*.zip" -ErrorAction SilentlyContinue
    if ($zips.Count -eq 1) {
        $ZipPath = $zips[0].FullName
    } elseif ($zips.Count -gt 1) {
        Write-Host "Multiple zips found. Please specify one:" -ForegroundColor Yellow
        $zips | ForEach-Object { Write-Host "  $_" }
        $ZipPath = Read-Host "Enter full path to zip"
    } else {
        Write-Host "No obs-audio-router*.zip found next to this script." -ForegroundColor Yellow
        $ZipPath = Read-Host "Enter full path to the downloaded zip file"
    }
}

if (-not (Test-Path $ZipPath)) {
    Write-Host "ERROR: Zip not found at: $ZipPath" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
}
Write-Host "Using zip: $ZipPath" -ForegroundColor Green

# ── Extract to temp ───────────────────────────────────────────────────────────
$tmp = Join-Path $env:TEMP "obs-audio-router-install"
if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

Write-Host "Extracting..."
Expand-Archive -Path $ZipPath -DestinationPath $tmp -Force

# ── Copy DLL ──────────────────────────────────────────────────────────────────
$dll_src  = Join-Path $tmp "obs-plugins\64bit\obs-audio-router.dll"
$dll_dst  = Join-Path $obs_root "obs-plugins\64bit"

if (-not (Test-Path $dll_src)) {
    Write-Host "ERROR: obs-audio-router.dll not found inside zip." -ForegroundColor Red
    Write-Host "Expected path inside zip: obs-plugins\64bit\obs-audio-router.dll"
    Read-Host "Press Enter to exit"
    exit 1
}

New-Item -ItemType Directory -Force -Path $dll_dst | Out-Null
Copy-Item $dll_src $dll_dst -Force
Write-Host "Installed DLL to: $dll_dst" -ForegroundColor Green

# ── Copy locale data ──────────────────────────────────────────────────────────
$data_src = Join-Path $tmp "data\obs-plugins\obs-audio-router"
$data_dst = Join-Path $obs_root "data\obs-plugins\obs-audio-router"
if (Test-Path $data_src) {
    New-Item -ItemType Directory -Force -Path $data_dst | Out-Null
    Copy-Item "$data_src\*" $data_dst -Recurse -Force
    Write-Host "Installed locale data." -ForegroundColor Green
}

# ── Firewall rules ────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "Adding Windows Firewall rules for UDP ports 9001 and 9002..." -ForegroundColor Cyan

$fw_rules = @(
    @{Name="OBS Audio Router Track 1 (UDP 9001)"; Port=9001},
    @{Name="OBS Audio Router Track 2 (UDP 9002)"; Port=9002}
)

foreach ($rule in $fw_rules) {
    # Remove old rule if present
    netsh advfirewall firewall delete rule name="$($rule.Name)" | Out-Null

    # Inbound rule (needed on Streaming PC to receive)
    netsh advfirewall firewall add rule `
        name="$($rule.Name)" `
        dir=in `
        action=allow `
        protocol=UDP `
        localport=$($rule.Port) `
        profile=private `
        description="OBS Audio Router plugin" | Out-Null

    Write-Host "  Added inbound rule for port $($rule.Port)" -ForegroundColor Green
}

# ── Cleanup ───────────────────────────────────────────────────────────────────
Remove-Item $tmp -Recurse -Force

# ── Done ──────────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Installation complete!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. Restart OBS Studio on this PC."
Write-Host ""
Write-Host "  ON YOUR GAMING PC:"
Write-Host "    - Right-click any audio source -> Filters"
Write-Host "    - Click '+' -> 'Audio Router: Send Track'"
Write-Host "    - Enter your Streaming PC's IP and choose Track 1 or Track 2"
Write-Host ""
Write-Host "  ON YOUR STREAMING PC:"
Write-Host "    - Add a new source -> 'Audio Router: Receive Track'"
Write-Host "    - Choose the track number to match what the Gaming PC is sending"
Write-Host ""
Write-Host "  Make sure both PCs are on the same local network."
Write-Host ""
Read-Host "Press Enter to exit"
