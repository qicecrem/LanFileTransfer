param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [ValidateSet("Apk", "Aab", "Both")]
    [string]$Package = "Apk",
    [string]$QtAndroidRoot = $env:QT_ANDROID_ROOT,
    [string]$QtHostRoot = $env:QT_HOST_ROOT,
    [string]$AndroidSdkRoot = $env:ANDROID_SDK_ROOT,
    [string]$AndroidNdkRoot = $env:ANDROID_NDK_ROOT,
    [string]$JavaHome = $env:JAVA_HOME,
    [string]$NinjaPath = $env:NINJA_PATH,
    [string]$BuildDirectory,
    [switch]$Clean,
    [switch]$Sign,
    [switch]$AllowUnsignedRelease,
    [switch]$SkipDist
)

$ErrorActionPreference = "Stop"
$projectRoot = $PSScriptRoot

function Resolve-RequiredDirectory {
    param([string]$Path, [string]$Name, [string]$Hint)
    if (-not $Path) { throw "$Name was not provided. $Hint" }
    $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    if (-not (Test-Path -LiteralPath $resolved -PathType Container)) {
        throw "$Name is not a directory: $resolved"
    }
    return $resolved
}

function Resolve-Ninja {
    param([string]$RequestedPath, [string]$ResolvedQtAndroidRoot)
    if ($RequestedPath) {
        $resolved = (Resolve-Path -LiteralPath $RequestedPath -ErrorAction Stop).Path
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            throw "NinjaPath is not a file: $resolved"
        }
        return $resolved
    }
    $command = Get-Command ninja.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $qtInstallRoot = Split-Path -Parent (Split-Path -Parent $ResolvedQtAndroidRoot)
    $candidate = Join-Path $qtInstallRoot "Tools\Ninja\ninja.exe"
    if (Test-Path -LiteralPath $candidate) { return $candidate }
    throw "Ninja was not found. Pass -NinjaPath or set NINJA_PATH."
}

if ($Configuration -eq "Debug" -and $Sign) {
    throw "-Sign is reserved for Release builds. Remove -Sign or use -Configuration Release."
}
if ($Configuration -eq "Release" -and -not $Sign -and -not $AllowUnsignedRelease) {
    throw "Refusing to create an unsigned Release package. Use -Sign, or explicitly pass -AllowUnsignedRelease for diagnostics only."
}

$resolvedQtAndroidRoot = Resolve-RequiredDirectory $QtAndroidRoot "QtAndroidRoot" `
    "Pass the Qt Android kit root, for example C:\Qt\6.8.3\android_arm64_v8a."
$qtCmake = Join-Path $resolvedQtAndroidRoot "bin\qt-cmake.bat"
if (-not (Test-Path -LiteralPath $qtCmake -PathType Leaf)) {
    throw "QtAndroidRoot does not contain bin\qt-cmake.bat: $resolvedQtAndroidRoot"
}

if (-not $QtHostRoot) {
    $qtVersionRoot = Split-Path -Parent $resolvedQtAndroidRoot
    $hostCandidate = Join-Path $qtVersionRoot "mingw_64"
    if (Test-Path -LiteralPath (Join-Path $hostCandidate "bin\qmake.exe")) {
        $QtHostRoot = $hostCandidate
    }
}
$resolvedQtHostRoot = Resolve-RequiredDirectory $QtHostRoot "QtHostRoot" `
    "Pass the matching desktop host Qt kit, for example C:\Qt\6.8.3\mingw_64."
if (-not (Test-Path -LiteralPath (Join-Path $resolvedQtHostRoot "bin\qmlimportscanner.exe"))) {
    throw "QtHostRoot does not contain bin\qmlimportscanner.exe: $resolvedQtHostRoot"
}

$resolvedAndroidSdkRoot = Resolve-RequiredDirectory $AndroidSdkRoot "AndroidSdkRoot" `
    "Pass the Android SDK root or set ANDROID_SDK_ROOT."
if (-not (Test-Path -LiteralPath (Join-Path $resolvedAndroidSdkRoot "platforms\android-36\android.jar"))) {
    throw "Android SDK platform 36 is not installed under: $resolvedAndroidSdkRoot"
}

if (-not $AndroidNdkRoot) {
    $preferredNdk = Join-Path $resolvedAndroidSdkRoot "ndk\26.1.10909125"
    if (Test-Path -LiteralPath $preferredNdk) { $AndroidNdkRoot = $preferredNdk }
}
$resolvedAndroidNdkRoot = Resolve-RequiredDirectory $AndroidNdkRoot "AndroidNdkRoot" `
    "Install NDK 26.1.10909125 or set ANDROID_NDK_ROOT."
if (-not (Test-Path -LiteralPath (Join-Path $resolvedAndroidNdkRoot "build\cmake\android.toolchain.cmake"))) {
    throw "AndroidNdkRoot does not contain the CMake toolchain: $resolvedAndroidNdkRoot"
}

if (-not $JavaHome) {
    $jarsignerCommand = Get-Command jarsigner.exe -ErrorAction SilentlyContinue
    if ($jarsignerCommand) {
        $JavaHome = Split-Path -Parent (Split-Path -Parent $jarsignerCommand.Source)
    } else {
        $java = Get-Command java.exe -ErrorAction SilentlyContinue
        if ($java) { $JavaHome = Split-Path -Parent (Split-Path -Parent $java.Source) }
    }
}
$resolvedJavaHome = Resolve-RequiredDirectory $JavaHome "JavaHome" `
    "Pass a JDK 17 directory or set JAVA_HOME."
if (-not (Test-Path -LiteralPath (Join-Path $resolvedJavaHome "bin\jarsigner.exe"))) {
    throw "JavaHome does not contain bin\jarsigner.exe: $resolvedJavaHome"
}

$resolvedNinja = Resolve-Ninja $NinjaPath $resolvedQtAndroidRoot
$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
if (-not $cmakeCommand) {
    $qtInstallRoot = Split-Path -Parent (Split-Path -Parent $resolvedQtAndroidRoot)
    $cmakeCandidate = Join-Path $qtInstallRoot "Tools\CMake_64\bin\cmake.exe"
    if (Test-Path -LiteralPath $cmakeCandidate) {
        $cmakeCommand = Get-Item -LiteralPath $cmakeCandidate
    }
}
if (-not $cmakeCommand) {
    throw "cmake.exe was not found in PATH or the Qt Tools directory."
}

if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $projectRoot "build\android-arm64-v8a-$($Configuration.ToLowerInvariant())"
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

if ($Sign) {
    foreach ($name in @(
        "QT_ANDROID_KEYSTORE_PATH",
        "QT_ANDROID_KEYSTORE_ALIAS",
        "QT_ANDROID_KEYSTORE_STORE_PASS",
        "QT_ANDROID_KEYSTORE_KEY_PASS")) {
        $value = [Environment]::GetEnvironmentVariable($name, "Process")
        if ([string]::IsNullOrWhiteSpace($value)) {
            throw "Android signing requested, but $name is not set in the process environment."
        }
    }
    $keystorePath = [Environment]::GetEnvironmentVariable("QT_ANDROID_KEYSTORE_PATH", "Process")
    if (-not (Test-Path -LiteralPath $keystorePath -PathType Leaf)) {
        throw "QT_ANDROID_KEYSTORE_PATH does not exist: $keystorePath"
    }
}

$signApk = if ($Sign -and $Package -in @("Apk", "Both")) { "ON" } else { "OFF" }
$signAab = if ($Sign -and $Package -in @("Aab", "Both")) { "ON" } else { "OFF" }
$deploymentType = $Configuration.ToUpperInvariant()

$env:ANDROID_SDK_ROOT = $resolvedAndroidSdkRoot
$env:ANDROID_NDK_ROOT = $resolvedAndroidNdkRoot
$env:JAVA_HOME = $resolvedJavaHome
$env:Path = "$(Split-Path -Parent $resolvedNinja);$resolvedJavaHome\bin;$resolvedQtHostRoot\bin;" + $env:Path

$configureArguments = @(
    "-S", $projectRoot,
    "-B", $BuildDirectory,
    "-G", "Ninja",
    "-DCMAKE_MAKE_PROGRAM=$resolvedNinja",
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DANDROID_ABI=arm64-v8a",
    "-DANDROID_PLATFORM=android-28",
    "-DQT_HOST_PATH=$resolvedQtHostRoot",
    "-DQT_ANDROID_DEPLOYMENT_TYPE=$deploymentType",
    "-DQT_ANDROID_SIGN_APK=$signApk",
    "-DQT_ANDROID_SIGN_AAB=$signAab",
    "-DBUILD_TESTING=OFF"
)
& $qtCmake @configureArguments
if ($LASTEXITCODE -ne 0) { throw "Android CMake configuration failed with exit code $LASTEXITCODE" }

$targets = switch ($Package) {
    "Apk" { @("apk") }
    "Aab" { @("aab") }
    "Both" { @("apk", "aab") }
}
foreach ($target in $targets) {
    & $cmakeCommand.Source --build $BuildDirectory --target $target
    if ($LASTEXITCODE -ne 0) { throw "Android $target build failed with exit code $LASTEXITCODE" }
}

$variant = $Configuration.ToLowerInvariant()
$artifacts = @()
if ($Package -in @("Apk", "Both")) {
    $apkOutputSegment = [System.IO.Path]::DirectorySeparatorChar +
        "build\outputs\apk\$variant" + [System.IO.Path]::DirectorySeparatorChar
    $apkArtifacts = @(Get-ChildItem -LiteralPath $BuildDirectory -Recurse -File -Filter "*.apk" |
        Where-Object { $_.FullName.IndexOf($apkOutputSegment, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 })
    if ($apkArtifacts.Count -ne 1) {
        throw "Expected exactly one $variant APK output, found $($apkArtifacts.Count) under $BuildDirectory"
    }
    $artifacts += $apkArtifacts[0].FullName
}
if ($Package -in @("Aab", "Both")) {
    $aabOutputSegment = [System.IO.Path]::DirectorySeparatorChar +
        "build\outputs\bundle\$variant" + [System.IO.Path]::DirectorySeparatorChar
    $aabArtifacts = @(Get-ChildItem -LiteralPath $BuildDirectory -Recurse -File -Filter "*.aab" |
        Where-Object { $_.FullName.IndexOf($aabOutputSegment, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 })
    if ($aabArtifacts.Count -ne 1) {
        throw "Expected exactly one $variant AAB output, found $($aabArtifacts.Count) under $BuildDirectory"
    }
    $artifacts += $aabArtifacts[0].FullName
}
foreach ($artifact in $artifacts) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "Expected Android package was not produced: $artifact"
    }
}

if ($Sign) {
    $jarsigner = Join-Path $resolvedJavaHome "bin\jarsigner.exe"
    foreach ($artifact in $artifacts | Where-Object { $_.EndsWith(".aab", [System.StringComparison]::OrdinalIgnoreCase) }) {
        & $jarsigner -verify $artifact
        if ($LASTEXITCODE -ne 0) { throw "AAB signature verification failed: $artifact" }

        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $bundle = [System.IO.Compression.ZipFile]::OpenRead($artifact)
        try {
            $hasSignatureBlock = $null -ne ($bundle.Entries | Where-Object {
                $_.FullName -match '^META-INF/[^/]+\.(RSA|DSA|EC)$'
            } | Select-Object -First 1)
        } finally {
            $bundle.Dispose()
        }
        if (-not $hasSignatureBlock) {
            throw "AAB contains no JAR signature block: $artifact"
        }
    }

    $buildTools = Get-ChildItem -LiteralPath (Join-Path $resolvedAndroidSdkRoot "build-tools") -Directory |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "apksigner.bat") } |
        Sort-Object { [version]($_.Name -replace '-.*$', '') } -Descending |
        Select-Object -First 1
    if (-not $buildTools) { throw "apksigner.bat was not found in the Android SDK build-tools." }
    $apkSigner = Join-Path $buildTools.FullName "apksigner.bat"
    foreach ($artifact in $artifacts | Where-Object { $_.EndsWith(".apk", [System.StringComparison]::OrdinalIgnoreCase) }) {
        & $apkSigner verify --verbose --print-certs $artifact
        if ($LASTEXITCODE -ne 0) { throw "APK signature verification failed: $artifact" }
    }

    if (-not $SkipDist) {
        $cachePath = Join-Path $BuildDirectory "CMakeCache.txt"
        $versionEntry = Select-String -LiteralPath $cachePath `
            -Pattern '^CMAKE_PROJECT_VERSION:STATIC=(.+)$' | Select-Object -First 1
        if (-not $versionEntry) { throw "Unable to read project version from $cachePath" }
        $version = $versionEntry.Matches[0].Groups[1].Value
        $distDirectory = Join-Path $projectRoot "dist"
        New-Item -ItemType Directory -Path $distDirectory -Force | Out-Null
        foreach ($artifact in $artifacts) {
            $extension = [System.IO.Path]::GetExtension($artifact).ToLowerInvariant()
            $destination = Join-Path $distDirectory "LanDrop-$version-android-arm64-v8a$extension"
            Copy-Item -LiteralPath $artifact -Destination $destination -Force
            $hash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
            Set-Content -LiteralPath "$destination.sha256" -Encoding ascii `
                -Value "$hash  $([System.IO.Path]::GetFileName($destination))"
            Write-Host "Signed release package: $destination"
            Write-Host "SHA-256: $hash"
        }
    } else {
        foreach ($artifact in $artifacts) { Write-Host "Verified signed package: $artifact" }
    }
} else {
    foreach ($artifact in $artifacts) { Write-Host "Android package: $artifact" }
}

Write-Host "Android build completed: $BuildDirectory"
