$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildRoot = Join-Path $projectRoot "build-windows"

$rustInfo = rustc -vV
if ($LASTEXITCODE -ne 0) {
    throw "Rust is unavailable. Install the x64 MSVC Rust toolchain."
}
if ($rustInfo -notmatch "host: x86_64-pc-windows-msvc") {
    throw "Use the x64 MSVC Rust toolchain to match 64-bit Windows DAWs."
}

$rubyInfo = ruby --version
if ($LASTEXITCODE -ne 0) {
    throw "Ruby 3.2 is unavailable. Install it and make ruby.exe available on PATH."
}
if (($rubyInfo -join " ") -notmatch "ruby 3\.2") {
    throw "Ruby 3.2 is required to launch the Sonic Pi runtime."
}

cmake -S $projectRoot -B $buildRoot -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed."
}

cmake --build $buildRoot --config Release `
    --target sonic_pi_vst3 sonic_pi_live_probe sonic_pi_render_probe `
    --parallel 2
if ($LASTEXITCODE -ne 0) {
    throw "The Windows VST3 build or SDK validator failed."
}

$liveProbe = Join-Path $buildRoot "Release\sonic_pi_live_probe.exe"
if (-not (Test-Path -LiteralPath $liveProbe -PathType Leaf)) {
    throw "The Windows live audio probe was not built."
}
& $liveProbe
if ($LASTEXITCODE -ne 0) {
    throw "The live Sonic Pi audio probe failed."
}

$renderProbe = Join-Path $buildRoot "Release\sonic_pi_render_probe.exe"
if (-not (Test-Path -LiteralPath $renderProbe -PathType Leaf)) {
    throw "The Windows render probe was not built."
}
$tempDirectory = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }
$renderPath = Join-Path $tempDirectory "sonic-pi-windows-render-probe.wav"
& $renderProbe $renderPath 2
if ($LASTEXITCODE -ne 0 -or
    -not (Test-Path -LiteralPath $renderPath -PathType Leaf)) {
    throw "The host-buffer render probe failed."
}

$pluginBundle = Join-Path $buildRoot "VST3\Release\sonic_pi_vst3.vst3"
if (-not (Test-Path -LiteralPath $pluginBundle -PathType Container)) {
    throw "The VST3 bundle was not produced at $pluginBundle."
}
$pluginBinary = Get-ChildItem -LiteralPath $pluginBundle -Recurse -File |
    Where-Object { $_.Extension -in @(".vst3", ".dll") } |
    Select-Object -First 1
if (-not $pluginBinary) {
    throw "The VST3 bundle contains no Windows plug-in binary."
}

Write-Host "Built Windows VST3 bundle: $pluginBundle"
Get-FileHash -LiteralPath $pluginBinary.FullName -Algorithm SHA256
