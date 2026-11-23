# Build the new-tab ASCII core.
#   ascii_core.wasm  - the C17 core compiled freestanding for the page
#   ascii_demo.exe   - native terminal preview of the same C file
#   ascii_core_embed.js - base64 fallback for file:// pages (fetch is blocked there)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $here
try {
    $clang = (Get-Command clang -ErrorAction Stop).Source
    $clangArgs = @(
        '--target=wasm32', '-O3', '-ffreestanding', '-fno-builtin', '-nostdlib',
        '-Wl,--no-entry',
        '-Wl,--export=ascii_render', '-Wl,--export=ascii_cells',
        '-Wl,--export=ascii_cols', '-Wl,--export=ascii_rows', '-Wl,--export=ascii_version',
        '-Wl,--export-memory',
        '-Wl,--initial-memory=1048576', '-Wl,--max-memory=1048576',
        '-Wl,--strip-all',
        '-o', 'ascii_core.wasm', 'ascii_core.c'
    )
    & $clang $clangArgs
    if ($LASTEXITCODE -ne 0) { throw "clang failed with $LASTEXITCODE" }

    node embed.mjs
    if ($LASTEXITCODE -ne 0) { throw "embed.mjs failed with $LASTEXITCODE" }

    $gcc = (Get-Command gcc -ErrorAction Stop).Source
    & $gcc '-std=c17' '-O2' '-Wall' '-Wextra' 'ascii_core.c' 'ascii_demo.c' '-o' 'ascii_demo.exe' '-lm'
    if ($LASTEXITCODE -ne 0) { throw "gcc failed with $LASTEXITCODE" }

    $wasm = Get-Item ascii_core.wasm
    Write-Host ("ascii_core.wasm  {0:N0} bytes" -f $wasm.Length)
    Write-Host "ascii_demo.exe  built"
} finally {
    Pop-Location
}
