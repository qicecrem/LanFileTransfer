param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$QtRoot = $env:QT_ROOT,
    [string]$MinGwBin = $env:MINGW_BIN,
    [string]$BuildDirectory,
    [switch]$Clean,
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$projectRoot = $PSScriptRoot

function Resolve-QtRoot {
    param([string]$RequestedRoot)

    if ($RequestedRoot) {
        $resolved = (Resolve-Path -LiteralPath $RequestedRoot -ErrorAction Stop).Path
        if (-not (Test-Path -LiteralPath (Join-Path $resolved "bin\qmake.exe"))) {
            throw "QtRoot does not contain bin\qmake.exe: $resolved"
        }
        return $resolved
    }

    $qmake = Get-Command qmake.exe -ErrorAction SilentlyContinue
    if ($qmake) {
        return Split-Path -Parent (Split-Path -Parent $qmake.Source)
    }

    throw "Qt was not found. Pass -QtRoot or set QT_ROOT to a desktop Qt kit, for example C:\Qt\6.8.3\mingw_64."
}

function Resolve-MinGwBin {
    param([string]$RequestedBin, [string]$ResolvedQtRoot)

    if ($RequestedBin) {
        $resolved = (Resolve-Path -LiteralPath $RequestedBin -ErrorAction Stop).Path
        if (-not (Test-Path -LiteralPath (Join-Path $resolved "g++.exe"))) {
            throw "MinGwBin does not contain g++.exe: $resolved"
        }
        return $resolved
    }

    $compiler = Get-Command g++.exe -ErrorAction SilentlyContinue
    if ($compiler) { return Split-Path -Parent $compiler.Source }

    $qtInstallRoot = Split-Path -Parent (Split-Path -Parent $ResolvedQtRoot)
    $toolsRoot = Join-Path $qtInstallRoot "Tools"
    if (Test-Path -LiteralPath $toolsRoot) {
        $candidate = Get-ChildItem -LiteralPath $toolsRoot -Directory -Filter "mingw*_64" |
            ForEach-Object { Join-Path $_.FullName "bin" } |
            Where-Object { Test-Path -LiteralPath (Join-Path $_ "g++.exe") } |
            Sort-Object -Descending |
            Select-Object -First 1
        if ($candidate) { return $candidate }
    }

    throw "MinGW was not found. Pass -MinGwBin or set MINGW_BIN to the compiler bin directory."
}

$resolvedQtRoot = Resolve-QtRoot $QtRoot
$resolvedMinGwBin = Resolve-MinGwBin $MinGwBin $resolvedQtRoot
if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $projectRoot "build\desktop-$($Configuration.ToLowerInvariant())"
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $projectRoot $BuildDirectory
}

if ($Clean -and (Test-Path -LiteralPath $BuildDirectory)) {
    $resolvedBuildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
    $expectedBuildRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot "build"))
    if (-not $resolvedBuildDirectory.StartsWith($expectedBuildRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a directory outside the project build directory: $resolvedBuildDirectory"
    }
    Remove-Item -LiteralPath $resolvedBuildDirectory -Recurse -Force
}

$env:Path = "$resolvedMinGwBin;$resolvedQtRoot\bin;" + $env:Path

cmake -S $projectRoot -B $BuildDirectory -G "MinGW Makefiles" `
    -DCMAKE_BUILD_TYPE=$Configuration `
    -DCMAKE_PREFIX_PATH=$resolvedQtRoot `
    -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed with exit code $LASTEXITCODE" }

cmake --build $BuildDirectory --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

$applicationPath = Join-Path $BuildDirectory "appLanFileTransfer.exe"
$deployTool = Join-Path $resolvedQtRoot "bin\windeployqt.exe"
if (-not (Test-Path -LiteralPath $applicationPath)) {
    throw "Application executable was not produced: $applicationPath"
}
if (-not (Test-Path -LiteralPath $deployTool)) {
    throw "windeployqt.exe was not found in the selected Qt kit: $deployTool"
}

$deployMode = if ($Configuration -eq "Debug") { "--debug" } else { "--release" }
& $deployTool $deployMode --qmldir $projectRoot --no-translations --verbose 0 $applicationPath
if ($LASTEXITCODE -ne 0) { throw "Runtime deployment failed with exit code $LASTEXITCODE" }

if (-not $SkipTests) {
    ctest --test-dir $BuildDirectory --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Tests failed with exit code $LASTEXITCODE" }
}

Write-Host "Build completed: $BuildDirectory"
