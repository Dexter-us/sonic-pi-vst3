$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildRoot = Join-Path $projectRoot "build-windows"

$rustInfo = rustc -vV
if ($LASTEXITCODE -ne 0) {
    throw "Rust is unavailable. Install the x64 MSVC Rust toolchain."
}
$rustInfoText = $rustInfo -join "`n"
if ($rustInfoText -notmatch "(?m)^host: x86_64-pc-windows-msvc$") {
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

$previousBuildTreeOverride = $env:SONIC_PI_VST_ALLOW_BUILD_TREE_RUNTIME
try {
    $env:SONIC_PI_VST_ALLOW_BUILD_TREE_RUNTIME = "1"
    cmake --build $buildRoot --config Release `
        --target sonic_pi_vst3 sonic_pi_live_probe sonic_pi_render_probe `
        --parallel 2
    if ($LASTEXITCODE -ne 0) {
        throw "The Windows VST3 build or SDK validator failed."
    }
} finally {
    $env:SONIC_PI_VST_ALLOW_BUILD_TREE_RUNTIME = $previousBuildTreeOverride
}

$renderProbe = Join-Path $buildRoot "Release\sonic_pi_render_probe.exe"
if (-not (Test-Path -LiteralPath $renderProbe -PathType Leaf)) {
    throw "The Windows render probe was not built."
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

$runtimeRoot = Join-Path $pluginBundle "Contents\Resources\SonicPiVstRuntime"
$sonicPiRoot = Join-Path $runtimeRoot "sonic-pi"
$sonicPiSourceRoot = Join-Path $buildRoot "_deps\sonic_pi_runtime-src"
if (-not (Test-Path -LiteralPath (Join-Path $sonicPiSourceRoot "LICENSE.md") -PathType Leaf)) {
    throw "The fetched Sonic Pi source tree is missing."
}
if (Test-Path -LiteralPath $sonicPiRoot) {
    Remove-Item -LiteralPath $sonicPiRoot -Recurse -Force
}

function Copy-RuntimeTree {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        throw "A required Sonic Pi runtime directory is missing: $Source"
    }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    & robocopy $Source $Destination /E /COPY:DAT /DCOPY:DAT /XJ `
        /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) {
        throw "Could not stage Sonic Pi runtime files from $Source (robocopy exit $LASTEXITCODE)."
    }
}

Copy-RuntimeTree `
    (Join-Path $sonicPiSourceRoot "app\server\ruby") `
    (Join-Path $sonicPiRoot "app\server\ruby")
Copy-RuntimeTree `
    (Join-Path $sonicPiSourceRoot "app\server\native") `
    (Join-Path $sonicPiRoot "app\server\native")
Copy-RuntimeTree `
    (Join-Path $sonicPiSourceRoot "etc") `
    (Join-Path $sonicPiRoot "etc")
Copy-Item -LiteralPath (Join-Path $sonicPiSourceRoot "LICENSE.md") `
    -Destination (Join-Path $sonicPiRoot "LICENSE.md") -Force

$rubyCommand = Get-Command ruby.exe -ErrorAction Stop
$rubyExecutable = $rubyCommand.Source
$rubyInstallRoot = Split-Path (Split-Path $rubyExecutable -Parent) -Parent
$rubyBundleRoot = Join-Path $runtimeRoot "ruby"
if (-not (Test-Path -LiteralPath (Join-Path $sonicPiRoot "app\server\ruby\bin\spider-server.rb") -PathType Leaf)) {
    throw "The Sonic Pi source tree was not staged inside the VST3 bundle."
}
if (Test-Path -LiteralPath $rubyBundleRoot) {
    Remove-Item -LiteralPath $rubyBundleRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $rubyBundleRoot -Force | Out-Null
Copy-Item -Path (Join-Path $rubyInstallRoot "*") `
    -Destination $rubyBundleRoot -Recurse -Force
$bundledRuby = Join-Path $rubyBundleRoot "bin\ruby.exe"
if (-not (Test-Path -LiteralPath $bundledRuby -PathType Leaf)) {
    throw "Ruby 3.2 was not copied into the VST3 bundle."
}
$bundledRubyInfo = & $bundledRuby --version
if ($LASTEXITCODE -ne 0 -or ($bundledRubyInfo -join " ") -notmatch "ruby 3\.2") {
    throw "The Ruby runtime copied into the VST3 bundle is not runnable."
}

$runtimeManifest = @"
Self-contained Sonic Pi runtime for the Sonic Pi VST3 prototype.
Sonic Pi source: v5.0.0
Ruby: $($bundledRubyInfo -join " ")

The sonic-pi directory contains the upstream Sonic Pi runtime source, assets,
and license. The ruby directory contains the Ruby installation used by the plug-in.
See the included upstream license files before redistribution.
"@
Set-Content -LiteralPath (Join-Path $runtimeRoot "README.txt") `
    -Value $runtimeManifest -Encoding utf8

$liveProbe = Join-Path $buildRoot "Release\sonic_pi_live_probe.exe"
if (-not (Test-Path -LiteralPath $liveProbe -PathType Leaf)) {
    throw "The Windows live audio probe was not built."
}
$previousRuntimeRoot = $env:SONIC_PI_VST_RUNTIME_ROOT
$previousPath = $env:PATH
try {
    $env:SONIC_PI_VST_RUNTIME_ROOT = $runtimeRoot
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    & $liveProbe
    if ($LASTEXITCODE -ne 0) {
        throw "The bundled Sonic Pi runtime failed the live audio probe."
    }
} finally {
    $env:SONIC_PI_VST_RUNTIME_ROOT = $previousRuntimeRoot
    $env:PATH = $previousPath
}

$tempDirectory = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }
$renderPath = Join-Path $tempDirectory "sonic-pi-windows-render-probe.wav"
& $renderProbe $renderPath 2
if ($LASTEXITCODE -ne 0 -or
    -not (Test-Path -LiteralPath $renderPath -PathType Leaf)) {
    throw "The host-buffer render probe failed."
}

$bundleSizeMb = [math]::Round(
    ((Get-ChildItem -LiteralPath $pluginBundle -Recurse -File |
        Measure-Object -Property Length -Sum).Sum / 1MB), 2)
Write-Host "Self-contained VST3 bundle size: $bundleSizeMb MB"
Write-Host "Built Windows VST3 bundle: $pluginBundle"
Get-FileHash -LiteralPath $pluginBinary.FullName -Algorithm SHA256
