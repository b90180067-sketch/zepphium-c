# Installs the Rust toolchain that builds the native ad-blocking engine
# (blocker/rust -> static library linked into windows/zephium_win.exe).
#
# Idempotent: run it before the first mingw32-make, and it is a no-op afterwards.
# The engine is built for the GNU Windows target because the shell is linked with
# MinGW-w64 g++; a MSVC-targeted Rust library cannot be linked into it.
#
# Usage: powershell -ExecutionPolicy Bypass -File fetch_rust.ps1 [-Toolchain stable-x86_64-pc-windows-gnu]
param([string]$Toolchain = 'stable-x86_64-pc-windows-gnu')

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$cargoBin = Join-Path $env:USERPROFILE '.cargo\bin'
$rustup = Join-Path $cargoBin 'rustup.exe'
$cargo = Join-Path $cargoBin 'cargo.exe'
$rustc = Join-Path $cargoBin 'rustc.exe'
# rustup-init: the standard MSVC-hosted installer; it installs the GNU toolchain
# requested below just fine (host and target are independent).
$rustupUrl = 'https://static.rust-lang.org/rustup/dist/x86_64-pc-windows-msvc/rustup-init.exe'
# blocker/rust/Cargo.toml declares rust-version = "1.85"; refuse older compilers
# instead of failing later with a confusing syntax error inside a dependency.
$minVersion = [version]'1.85'
$minCargoVersion = [version]'1.85'

Write-Host "Zephium ad-block engine toolchain: $Toolchain"

# --- rustup ------------------------------------------------------------------
if (-not (Test-Path $rustup)) {
    $existing = Get-Command rustup -ErrorAction SilentlyContinue
    if ($existing) {
        $rustup = $existing.Source
        $cargoBin = Split-Path -Parent $rustup
        $cargo = Join-Path $cargoBin 'cargo.exe'
        $rustc = Join-Path $cargoBin 'rustc.exe'
        Write-Host "Using rustup from PATH: $rustup"
    } else {
        $installer = Join-Path $root 'rustup-init.exe'
        Write-Host "Downloading rustup-init ..."
        Invoke-WebRequest -Uri $rustupUrl -OutFile $installer -UseBasicParsing
        # --no-modify-path: do not touch the user PATH; make calls cargo by its
        # absolute %USERPROFILE%\.cargo\bin path when it is not on PATH.
        & $installer -y --no-modify-path --default-toolchain none --profile minimal
        if ($LASTEXITCODE -ne 0) { throw "rustup-init failed with exit code $LASTEXITCODE" }
        Remove-Item -Force $installer
        if (-not (Test-Path $rustup)) { throw "rustup was not installed into $cargoBin" }
    }
}

# --- toolchain ---------------------------------------------------------------
$list = & $rustup toolchain list 2>$null
$installed = $false
foreach ($line in $list) {
    if ($line -match ('^\s*' + [regex]::Escape($Toolchain) + '(\s|$)')) { $installed = $true }
}
if ($installed) {
    Write-Host "Toolchain already installed: $Toolchain"
} else {
    Write-Host "Installing toolchain $Toolchain ..."
    & $rustup toolchain install $Toolchain --profile minimal --no-self-update
    if ($LASTEXITCODE -ne 0) { throw "rustup toolchain install failed with exit code $LASTEXITCODE" }
}

# --- version gates -----------------------------------------------------------
$rustcVersionText = (& $rustc "+$Toolchain" --version) -join ' '
if (-not ($rustcVersionText -match 'rustc\s+(\d+\.\d+\.\d+)')) {
    throw "Could not parse the rustc version: $rustcVersionText"
}
$rustcVersion = [version]$Matches[1]
if ($rustcVersion -lt $minVersion) {
    Write-Host "rustc $rustcVersion is too old (need >= $minVersion), updating ..."
    & $rustup toolchain install $Toolchain --force --no-self-update
    $rustcVersionText = (& $rustc "+$Toolchain" --version) -join ' '
    if (-not ($rustcVersionText -match 'rustc\s+(\d+\.\d+\.\d+)')) {
        throw "Could not parse the rustc version after the update: $rustcVersionText"
    }
    $rustcVersion = [version]$Matches[1]
    if ($rustcVersion -lt $minVersion) {
        throw "rustc $rustcVersion is too old for blocker/rust (needs >= $minVersion)"
    }
}

$cargoVersionText = (& $cargo "+$Toolchain" --version) -join ' '
if (-not ($cargoVersionText -match 'cargo\s+(\d+\.\d+\.\d+)')) {
    throw "Could not parse the cargo version: $cargoVersionText"
}
if ([version]$Matches[1] -lt $minCargoVersion) {
    throw "cargo $($Matches[1]) is too old for blocker/rust (needs >= $minCargoVersion)"
}

# The shell links the staticlib with MinGW g++; a GNU-targeted Rust std needs an
# external linker and Rust uses the "gcc" it finds on PATH.
$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $gcc) {
    Write-Warning "gcc was not found on PATH: the Rust GNU target needs the MinGW-w64 gcc linker. Install it or add it to PATH before building."
}

$defaultLine = (& $rustup default) -join ' '
if ($defaultLine -notmatch [regex]::Escape($Toolchain)) {
    Write-Warning "The default rustup toolchain is '$defaultLine', not $Toolchain."
    Write-Warning "windows/Makefile builds the staticlib with the default toolchain; run:"
    Write-Warning "  rustup default $Toolchain"
} else {
    Write-Host "Default rustup toolchain: $defaultLine"
}

Write-Host "Rust ready: rustc $rustcVersion, cargo at $cargo"
Write-Host "Next: mingw32-make -C windows   (builds blocker/rust first, then the shell)"
Write-Host "Or:   mingw32-make -C windows adblock-test   (console self test)"
