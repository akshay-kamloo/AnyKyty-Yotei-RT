param(
    [switch]$Reconfigure,
    [ValidateSet("OFF", "GEN", "USE")]
    [string]$PgoMode = "OFF",
    [string]$PgoProfile = ""
)
$ErrorActionPreference = "Continue"
$root = $PSScriptRoot
$cmake = Join-Path $root "_tools\cmake\cmake-4.5.1-windows-x86_64\bin\cmake.exe"
$mingwBin = Join-Path $root "_tools\winlibs\mingw64\bin"
$llvmMingwBin = Join-Path $root "_tools\llvm-mingw\bin"
$logDir = Join-Path $root "build-logs"
$logFile = Join-Path $logDir "build-erlou-dll.txt"

New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$env:PATH = "$llvmMingwBin;$mingwBin;$(Split-Path $cmake);$env:PATH"

$buildDir = Join-Path $root "build-erlou-dll"
$pgoDir = Join-Path $buildDir "kyty-dll-pgo"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

$now = Get-Date
foreach ($dir in @("core\kyty", "core\libs\prx\libSceAgcDriver")) {
    $full = Join-Path $root $dir
    if (Test-Path $full) {
        Get-ChildItem -Path $full -Recurse -File | Where-Object { $_.LastWriteTime -gt $now } | ForEach-Object { $_.LastWriteTime = $now }
    }
}

function Log-Line([string]$msg) {
    $ts = (Get-Date).ToString("HH:mm:ss")
    $line = "[$ts] $msg"
    Write-Host $line
    [System.IO.File]::AppendAllText($logFile, "$line`n", $utf8NoBom)
}

[System.IO.File]::WriteAllText($logFile, "=== build-erlou-dll.ps1 started $(Get-Date -Format o) (PgoMode=$PgoMode) ===`n", $utf8NoBom)

if ($Reconfigure -or -not (Test-Path (Join-Path $buildDir "build.ninja"))) {
    Log-Line "=== Configuring build-erlou-dll (ANYPS5_KYTY_DLL=ON, ThinLTO=ON, PGO=$PgoMode) ==="
    $cfgArgs = @(
        "-S", $root,
        "-B", $buildDir,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_COMPILER=$($mingwBin.Replace('\','/'))/gcc.exe",
        "-DCMAKE_CXX_COMPILER=$($mingwBin.Replace('\','/'))/g++.exe",
        "-DCMAKE_MAKE_PROGRAM=$($mingwBin.Replace('\','/'))/ninja.exe",
        "-DANYPS5_GPU_BACKEND=kyty",
        "-DANYPS5_KYTY_DLL=ON",
        "-DANYPS5_KYTY_DLL_THIN_LTO=ON",
        "-DANYPS5_KYTY_DLL_PGO=$PgoMode",
        "-DANYPS5_KYTY_DLL_PGO_DIR=$($pgoDir.Replace('\','/'))",
        "-DANYPS5_KYTY_DLL_PGO_PROFILE=$($PgoProfile.Replace('\','/'))",
        "-DANYPS5_KYTY_DLL_C_COMPILER=$($llvmMingwBin.Replace('\','/'))/x86_64-w64-mingw32-clang.exe",
        "-DANYPS5_KYTY_DLL_CXX_COMPILER=$($llvmMingwBin.Replace('\','/'))/x86_64-w64-mingw32-clang++.exe",
        "-DANYPS5_KYTY_PGO=",
        "-DBUILD_TESTING=OFF"
    )
    & $cmake @cfgArgs 2>&1 | ForEach-Object {
        $s = $_.ToString()
        Write-Host $s
        [System.IO.File]::AppendAllText($logFile, "$s`n", $utf8NoBom)
    }
    $cfgCode = $LASTEXITCODE
    if ($cfgCode -ne 0) {
        Log-Line "=== Configure FAILED (exit $cfgCode) ==="
        exit $cfgCode
    }
}

Log-Line "=== Building libSceAgcDriver + kyty_gpu.prx ==="
& $cmake --build $buildDir --target libSceAgcDriver --parallel 2>&1 | ForEach-Object {
    $s = $_.ToString()
    Write-Host $s
    [System.IO.File]::AppendAllText($logFile, "$s`n", $utf8NoBom)
}
$bldCode = $LASTEXITCODE
if ($bldCode -ne 0) {
    Log-Line "=== libSceAgcDriver FAILED (exit $bldCode) ==="
    exit $bldCode
}

Log-Line "=== Building patched libs target ==="
& $cmake --build $buildDir --target libs --parallel 2>&1 | ForEach-Object {
    $s = $_.ToString()
    Write-Host $s
    [System.IO.File]::AppendAllText($logFile, "$s`n", $utf8NoBom)
}
$libsCode = $LASTEXITCODE
Log-Line "=== libs finished (exit $libsCode) ==="
exit $libsCode