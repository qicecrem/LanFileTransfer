param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$QtRoot = $env:QT_ROOT,
    [string]$MinGwBin = $env:MINGW_BIN,
    [string]$BuildDirectory,
    [switch]$Clean,
    [switch]$SkipTests,
    [switch]$SkipPackage
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
if (-not (Test-Path -LiteralPath $applicationPath)) {
    throw "Application executable was not produced: $applicationPath"
}
$requiredRuntimeFiles = @(
    "Qt6Core.dll",
    "Qt6Gui.dll",
    "Qt6Network.dll",
    "Qt6Qml.dll",
    "Qt6Quick.dll",
    "Qt6Sql.dll",
    "Qt6Multimedia.dll",
    "platforms\qwindows.dll",
    "sqldrivers\qsqlite.dll",
    "libgcc_s_seh-1.dll",
    "libstdc++-6.dll",
    "libwinpthread-1.dll"
)
$missingRuntimeFiles = @($requiredRuntimeFiles | Where-Object {
    -not (Test-Path -LiteralPath (Join-Path $BuildDirectory $_))
})
if ($missingRuntimeFiles.Count -gt 0) {
    throw "CMake runtime deployment is incomplete. Missing: $($missingRuntimeFiles -join ', ')"
}

if (-not $SkipTests) {
    ctest --test-dir $BuildDirectory --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Tests failed with exit code $LASTEXITCODE" }
}

if ($Configuration -eq "Release" -and -not $SkipPackage) {
    $cachePath = Join-Path $BuildDirectory "CMakeCache.txt"
    $versionEntry = Select-String -LiteralPath $cachePath `
        -Pattern '^CMAKE_PROJECT_VERSION:STATIC=(.+)$' | Select-Object -First 1
    if (-not $versionEntry) { throw "Unable to read project version from $cachePath" }
    $version = $versionEntry.Matches[0].Groups[1].Value
    $packageName = "LanDrop-$version-windows-x64"
    $packageWorkRoot = Join-Path $BuildDirectory "_package"
    $stagingDirectory = Join-Path $packageWorkRoot $packageName
    $resolvedBuildRoot = [System.IO.Path]::GetFullPath($BuildDirectory)
    $resolvedStaging = [System.IO.Path]::GetFullPath($stagingDirectory)
    if (-not $resolvedStaging.StartsWith($resolvedBuildRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to prepare a package outside the build directory: $resolvedStaging"
    }
    if (Test-Path -LiteralPath $stagingDirectory) {
        Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
    }
    New-Item -ItemType Directory -Path $stagingDirectory -Force | Out-Null

    $packagedApplication = Join-Path $stagingDirectory "appLanFileTransfer.exe"
    Copy-Item -LiteralPath $applicationPath -Destination $packagedApplication
    $windeployqt = Join-Path $resolvedQtRoot "bin\windeployqt.exe"
    if (-not (Test-Path -LiteralPath $windeployqt)) {
        throw "windeployqt was not found: $windeployqt"
    }
    & $windeployqt --release --compiler-runtime --qmldir $projectRoot `
        --no-translations --verbose 0 --dir $stagingDirectory $packagedApplication
    if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }

    $licensesDirectory = Join-Path $stagingDirectory "licenses"
    New-Item -ItemType Directory -Path $licensesDirectory -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $projectRoot "img\MATERIAL_ICONS_LICENSE.txt") `
        -Destination $licensesDirectory

    & $packagedApplication --smoke-test
    if ($LASTEXITCODE -ne 0) {
        throw "Packaged application smoke test failed with exit code $LASTEXITCODE"
    }

    $distDirectory = Join-Path $projectRoot "dist"
    New-Item -ItemType Directory -Path $distDirectory -Force | Out-Null
    $archivePath = Join-Path $distDirectory "$packageName.zip"
    if (Test-Path -LiteralPath $archivePath) { Remove-Item -LiteralPath $archivePath -Force }
    Compress-Archive -Path (Join-Path $stagingDirectory "*") `
        -DestinationPath $archivePath -CompressionLevel Optimal
    $archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $checksumPath = "$archivePath.sha256"
    Set-Content -LiteralPath $checksumPath -Encoding ascii `
        -Value "$archiveHash  $([System.IO.Path]::GetFileName($archivePath))"
    Write-Host "Release package: $archivePath"
    Write-Host "SHA-256: $archiveHash"
}

Write-Host "Build completed: $BuildDirectory"
