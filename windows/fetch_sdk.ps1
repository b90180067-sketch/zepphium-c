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

# Ad blocking is implemented at the network level, in WebResourceRequested; the
# shell no longer loads browser extensions. Refuse an SDK that does not expose the
# interfaces the shell uses (WebResourceRequested + frames: 1.0.1108) instead of
# failing at runtime with E_NOINTERFACE.
$minVersion = [version]'1.0.1108'
if ([version]$Version -lt $minVersion) {
    throw "WebView2 SDK $Version is too old for network-level ad blocking (need >= $minVersion)"
}
foreach ($symbol in @('ICoreWebView2WebResourceRequestedEventArgs', 'ICoreWebView2Frame2')) {
    if (-not (Select-String -Path $header -Pattern $symbol -Quiet)) {
        throw "WebView2 SDK $Version does not expose $symbol - update fetch_sdk.ps1 to a newer version"
    }
}

$versionLine = Select-String -Path (Join-Path $sdk 'build/native/include/WebView2EnvironmentOptions.h') -Pattern 'CORE_WEBVIEW_TARGET_PRODUCT_VERSION (L"[^"]+")' | Select-Object -First 1
Write-Host "SDK ready: $sdk"
if ($versionLine) {
    Write-Host "Target product version in SDK: $($versionLine.Matches[0].Groups[1].Value)"
    Write-Host "If you bump the SDK, update ZephiumEnvOptions::kTargetVersion in zephium_win.cpp to match it,"
    Write-Host "otherwise CreateCoreWebView2EnvironmentWithOptions fails with E_INVALIDARG."
}
