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
$licenseRoot = Join-Path $runtimeRoot "Licenses"
$licenseIndex = Join-Path $licenseRoot "FILE_INDEX.txt"
foreach ($requiredSourceFile in @("LICENSE.md", "VERSION")) {
    if (-not (Test-Path -LiteralPath (Join-Path $sonicPiSourceRoot $requiredSourceFile) -PathType Leaf)) {
        throw "The fetched Sonic Pi source tree is missing $requiredSourceFile."
    }
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

if (Test-Path -LiteralPath $licenseRoot) {
    Remove-Item -LiteralPath $licenseRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $licenseRoot -Force | Out-Null
Set-Content -LiteralPath $licenseIndex -Value @(
    "Original upstream path => bundled path"
    "======================================="
) -Encoding utf8

function Copy-LicenseFiles {
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$Component
    )
    if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
        throw "A required source tree for license collection is missing: $SourceRoot"
    }

    $noticeNamePattern = '^(?i)(license|licence|copying|copyright|notice|patents|unlicense|authors|contributors|attribution|credits|third[-_. ]party[-_. ]notices?|bsdl|legal)(?:$|[._-].*)'
    $files = Get-ChildItem -LiteralPath $SourceRoot -File -Recurse -Force |
        Where-Object {
            $_.FullName -notmatch '[\\/]\.git([\\/]|$)' -and
            $_.Name -match $noticeNamePattern
        } |
        Sort-Object FullName
    if (-not $files) {
        throw "No license or copyright notice files were found under $SourceRoot."
    }

    $componentRoot = Join-Path $licenseRoot $Component
    New-Item -ItemType Directory -Path $componentRoot -Force | Out-Null
    $index = 0
    foreach ($file in $files) {
        $index++
        $relativeSourcePath = [System.IO.Path]::GetRelativePath(
            $SourceRoot, $file.FullName).Replace("\", "/")
        $bundledFileName = "{0:D4}-{1}" -f $index, $file.Name
        Copy-Item -LiteralPath $file.FullName `
            -Destination (Join-Path $componentRoot $bundledFileName) -Force
        Add-Content -LiteralPath $licenseIndex `
            -Value "$Component/$relativeSourcePath -> $Component/$bundledFileName" `
            -Encoding utf8
    }
    Write-Host "Collected $index license/notice files for $Component."
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
Copy-Item -LiteralPath (Join-Path $sonicPiSourceRoot "VERSION") `
    -Destination (Join-Path $sonicPiRoot "VERSION") -Force

# These documentation and example trees are not used by the VST runtime and
# have separate upstream attribution terms. Keep the actual runtime assets.
foreach ($relativePath in @(
    "etc\doc",
    "etc\examples",
    "etc\snippets",
    "etc\quickstart",
    "etc\www"
)) {
    $optionalContentPath = Join-Path $sonicPiRoot $relativePath
    if (Test-Path -LiteralPath $optionalContentPath) {
        Remove-Item -LiteralPath $optionalContentPath -Recurse -Force
    }
}

# Capture notices before removing vendor test and documentation payloads below.
Copy-LicenseFiles -SourceRoot $sonicPiRoot -Component "SonicPiRuntime"
$superSonicSourceRoot = Join-Path $buildRoot "_deps\supersonic-src"
$vst3SdkSourceRoot = Join-Path $buildRoot "_deps\vst3sdk-src"
Copy-LicenseFiles -SourceRoot $superSonicSourceRoot -Component "SuperSonic"
Copy-LicenseFiles -SourceRoot $vst3SdkSourceRoot -Component "VST3SDK"

# Vendored Ruby dependencies carry upstream test fixtures that are not used by
# the live server. Some fixture paths exceed Windows Explorer's ZIP path limit.
$sonicPiRubyVendorRoot = Join-Path $sonicPiRoot "app\server\ruby\vendor"
if (Test-Path -LiteralPath $sonicPiRubyVendorRoot -PathType Container) {
    $nonRuntimeSourceDirectories = @("test", "tests", "spec", "specs",
        "doc", "docs", "example", "examples", "benchmark", "benchmarks")
    $sourceDirectoriesToPrune = Get-ChildItem `
        -LiteralPath $sonicPiRubyVendorRoot -Directory -Recurse |
        Where-Object { $_.Name -in $nonRuntimeSourceDirectories } |
        Sort-Object { $_.FullName.Length } -Descending
    $prunedSourceDirectoryCount = @($sourceDirectoriesToPrune).Count
    foreach ($directory in $sourceDirectoriesToPrune) {
        Remove-Item -LiteralPath $directory.FullName -Recurse -Force
    }
    Write-Host "Removed $prunedSourceDirectoryCount non-runtime vendored Ruby directories."
}

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
$excludedRubyDirectories = @(
    (Join-Path $rubyInstallRoot "msys64"),
    (Join-Path $rubyInstallRoot "include")
)
& robocopy $rubyInstallRoot $rubyBundleRoot /E /COPY:DAT /DCOPY:DAT /XJ `
    /XD $excludedRubyDirectories /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) {
    throw "Could not stage the Ruby runtime files (robocopy exit $LASTEXITCODE)."
}

# RubyInstaller's MSYS2 directories include compilers and headers that the
# runtime does not need. Keep only their DLLs, which the live probe verifies
# with the system PATH removed.
foreach ($relativeBinPath in @("msys64\ucrt64\bin", "msys64\usr\bin")) {
    $sourceBin = Join-Path $rubyInstallRoot $relativeBinPath
    if (-not (Test-Path -LiteralPath $sourceBin -PathType Container)) {
        throw "A required Ruby runtime DLL directory is missing: $sourceBin"
    }
    $destinationBin = Join-Path $rubyBundleRoot $relativeBinPath
    New-Item -ItemType Directory -Path $destinationBin -Force | Out-Null
    $runtimeDlls = Get-ChildItem -LiteralPath $sourceBin -Filter "*.dll" -File
    if (-not $runtimeDlls) {
        throw "No runtime DLLs were found in $sourceBin."
    }
    foreach ($dll in $runtimeDlls) {
        Copy-Item -LiteralPath $dll.FullName `
            -Destination (Join-Path $destinationBin $dll.Name) -Force
    }
}

# Preserve Ruby's own legal files and the package notices for the MSYS2 DLLs
# staged above. The full MSYS2 toolchain is not shipped with the instrument.
Copy-LicenseFiles -SourceRoot $rubyBundleRoot -Component "RubyRuntime"
$msys2Root = Join-Path $rubyInstallRoot "msys64"
$msys2LicenseRoots = @(
    (Join-Path $msys2Root "usr\share\licenses"),
    (Join-Path $msys2Root "ucrt64\share\licenses"),
    (Join-Path $msys2Root "mingw64\share\licenses")
) | Where-Object { Test-Path -LiteralPath $_ -PathType Container }
if (-not $msys2LicenseRoots) {
    throw "MSYS2 license directories were not found for the Ruby runtime DLLs."
}
$msys2LicenseRootIndex = 0
foreach ($msys2LicenseRoot in $msys2LicenseRoots) {
    $msys2LicenseRootIndex++
    Copy-LicenseFiles -SourceRoot $msys2LicenseRoot `
        -Component ("MSYS2-{0:D2}" -f $msys2LicenseRootIndex)
}

$rubyBytesBeforePrune = (Get-ChildItem -LiteralPath $rubyBundleRoot -Recurse -File |
    Measure-Object -Property Length -Sum).Sum
Write-Host ("Ruby bundle before documentation cleanup: {0:N2} MB" -f
    ($rubyBytesBeforePrune / 1MB))

# Keep Ruby's interpreter, standard libraries, gems, and native extensions.
# Remove material used only for development, package caching, or documentation.
$rubyPrunePaths = @(
    "share\doc",
    "share\man",
    "share\ri",
    "lib\ruby\gems\3.2.0\cache",
    "lib\ruby\gems\3.2.0\doc",
    "lib\ruby\3.2.0\rdoc",
    "lib\ruby\3.2.0\irb",
    "lib\ruby\3.2.0\test",
    "lib\ruby\3.2.0\tk",
    "lib\ruby\3.2.0\tkextlib"
)
foreach ($relativePath in $rubyPrunePaths) {
    $pathToPrune = Join-Path $rubyBundleRoot $relativePath
    if (Test-Path -LiteralPath $pathToPrune) {
        Remove-Item -LiteralPath $pathToPrune -Recurse -Force
    }
}

$gemPayloadRoot = Join-Path $rubyBundleRoot "lib\ruby\gems\3.2.0\gems"
if (Test-Path -LiteralPath $gemPayloadRoot -PathType Container) {
    $nonRuntimeGemDirectories = @("doc", "docs", "test", "tests", "spec",
        "specs", "examples", "benchmarks")
    $gemDirectoriesToPrune = Get-ChildItem -LiteralPath $gemPayloadRoot `
        -Directory -Recurse |
        Where-Object { $_.Name -in $nonRuntimeGemDirectories } |
        Sort-Object { $_.FullName.Length } -Descending
    foreach ($directory in $gemDirectoriesToPrune) {
        Remove-Item -LiteralPath $directory.FullName -Recurse -Force
    }
}

$rubyDevelopmentExtensions = @(".pdb", ".lib", ".a", ".exp", ".obj", ".ilk")
$rubyDevelopmentFiles = Get-ChildItem -LiteralPath $rubyBundleRoot -Recurse -File |
    Where-Object { $_.Extension -in $rubyDevelopmentExtensions }
foreach ($file in $rubyDevelopmentFiles) {
    Remove-Item -LiteralPath $file.FullName -Force
}
$rubyBytesAfterPrune = (Get-ChildItem -LiteralPath $rubyBundleRoot -Recurse -File |
    Measure-Object -Property Length -Sum).Sum
Write-Host ("Ruby bundle after documentation cleanup: {0:N2} MB" -f
    ($rubyBytesAfterPrune / 1MB))

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

The sonic-pi directory contains the upstream Sonic Pi runtime source and runtime
assets. Tutorials, examples, snippets, and documentation not used by the VST are
excluded. The ruby directory contains Ruby runtime files and required DLLs;
MSYS2 build tools, headers, documentation, test payloads, and package caches
are excluded while the interpreter, standard libraries, gems, and extensions
are retained.
THIRD_PARTY_NOTICES.txt summarizes the bundled components. Licenses contains
the collected upstream license and copyright files; FILE_INDEX.txt maps each
bundled file to its original location. Review all applicable terms before
redistribution.
"@
Set-Content -LiteralPath (Join-Path $runtimeRoot "README.txt") `
    -Value $runtimeManifest -Encoding utf8
Copy-Item -LiteralPath (Join-Path $projectRoot "THIRD_PARTY_NOTICES.txt") `
    -Destination (Join-Path $runtimeRoot "THIRD_PARTY_NOTICES.txt") -Force

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

$bundleArchivePrefix = "build-windows/VST3/Release/sonic_pi_vst3.vst3/"
if ($env:GITHUB_WORKSPACE) {
    $bundleArchivePrefix = [System.IO.Path]::GetRelativePath(
        $env:GITHUB_WORKSPACE, $pluginBundle).Replace("\", "/") + "/"
}
$maxBundleRelativePathLength = (Get-ChildItem -LiteralPath $pluginBundle `
    -Recurse -File | ForEach-Object {
        $_.FullName.Substring($pluginBundle.Length + 1).Replace("\", "/").Length
    } | Measure-Object -Maximum).Maximum
$maxZipEntryPathLength = $bundleArchivePrefix.Length +
    $maxBundleRelativePathLength
Write-Host "Maximum Windows artifact ZIP path length: $maxZipEntryPathLength characters"
if ($maxZipEntryPathLength -gt 240) {
    throw "The VST3 bundle still contains paths too long for reliable Windows ZIP extraction."
}

$bundleSizeMb = [math]::Round(
    ((Get-ChildItem -LiteralPath $pluginBundle -Recurse -File |
        Measure-Object -Property Length -Sum).Sum / 1MB), 2)
Write-Host "Self-contained VST3 bundle size: $bundleSizeMb MB"
Write-Host "Built Windows VST3 bundle: $pluginBundle"
Get-FileHash -LiteralPath $pluginBinary.FullName -Algorithm SHA256
