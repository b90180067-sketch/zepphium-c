# Downloads the Microsoft WebView2 SDK (headers + WebView2Loader.dll) into windows/sdk
# Usage: powershell -ExecutionPolicy Bypass -File fetch_sdk.ps1 [-Version 1.0.4370.26]
param([string]$Version = '1.0.4370.26')
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $root 'sdk'
New-Item -ItemType Directory -Force -Path $sdk | Out-Null

$zip = Join-Path $root 'webview2.zip'
$url = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$Version/microsoft.web.webview2.$Version.nupkg"

Write-Host "Downloading WebView2 SDK $Version ..."
Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
Expand-Archive -Path $zip -DestinationPath $sdk -Force
Remove-Item $zip -Force

$header = Join-Path $sdk 'build/native/include/WebView2.h'
$loader = Join-Path $sdk 'build/native/x64/WebView2Loader.dll'
if (-not (Test-Path $header)) { throw "WebView2.h not found after extraction" }
if (-not (Test-Path $loader)) { throw "WebView2Loader.dll not found after extraction" }

$versionLine = Select-String -Path (Join-Path $sdk 'build/native/include/WebView2EnvironmentOptions.h') -Pattern 'CORE_WEBVIEW_TARGET_PRODUCT_VERSION (L"[^"]+")' | Select-Object -First 1
Write-Host "SDK ready: $sdk"
if ($versionLine) {
    Write-Host "Target product version in SDK: $($versionLine.Matches[0].Groups[1].Value)"
    Write-Host "If you bump the SDK, update ZephiumEnvOptions::kTargetVersion in zephium_win.cpp to match it,"
    Write-Host "otherwise CreateCoreWebView2EnvironmentWithOptions fails with E_INVALIDARG."
}
