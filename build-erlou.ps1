param([switch]$Reconfigure)
$ErrorActionPreference = "Continue"
$root = $PSScriptRoot
$cmake = Join-Path $root "_tools\cmake\cmake-4.5.1-windows-x86_64\bin\cmake.exe"
$mingwBin = Join-Path $root "_tools\winlibs\mingw64\bin"
$mingwRoot = Join-Path $root "_tools\winlibs\mingw64"
$llvmBin = Join-Path $root "_tools\llvm\bin"
$logDir = Join-Path $root "build-logs"
$logFile = Join-Path $logDir "build-erlou.txt"

New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$env:PATH = "$llvmBin;$mingwBin;$(Split-Path $cmake);$env:PATH"

$buildDir = Join-Path $root "build-erlou"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# Fix any future timestamps from the extracted overlay archive (prevents Ninja manifest dirty loop)
$now = Get-Date
foreach ($dir in @("core\kyty", "core\libs\prx\libSceAgcDriver")) {
    $full = Join-Path $root $dir
    if (Test-Path $full) {
        Get-ChildItem -Path $full -Recurse -File | Where-Object { $_.LastWriteTime -gt $now } | ForEach-Object { $_.LastWriteTime = $now }
    }
}

if ($Reconfigure -or -not (Test-Path (Join-Path $buildDir "build.ninja"))) {
    Write-Host "=== Configuring build-erlou ==="
    $cfgOut = & $cmake -S $root -B $buildDir -G Ninja `
        -DCMAKE_BUILD_TYPE=Release `
        "-DCMAKE_C_COMPILER=$($mingwBin.Replace('\','/'))/gcc.exe" `
        "-DCMAKE_CXX_COMPILER=$($mingwBin.Replace('\','/'))/g++.exe" `
        "-DCMAKE_MAKE_PROGRAM=$($mingwBin.Replace('\','/'))/ninja.exe" `
        -DANYPS5_GPU_BACKEND=kyty `
        "-DANYPS5_KYTY_C_COMPILER=$($llvmBin.Replace('\','/'))/clang.exe" `
        "-DANYPS5_KYTY_CXX_COMPILER=$($llvmBin.Replace('\','/'))/clang++.exe" `
        "-DANYPS5_KYTY_SYSROOT=$($mingwRoot.Replace('\','/'))" `
        -DANYPS5_KYTY_THIN_LTO=OFF `
        "-DANYPS5_KYTY_PGO=" `
        -DBUILD_TESTING=OFF 2>&1
    $cfgCode = $LASTEXITCODE
    $cfgOut | ForEach-Object { Write-Host $_ }
    if ($cfgCode -ne 0) {
        $tail = ($cfgOut | Select-Object -Last 500 | Out-String)
        [System.IO.File]::WriteAllText($logFile, ($tail + "`n=== Configure FAILED (exit $cfgCode) ===`n"), $utf8NoBom)
        exit $cfgCode
    }
}

Write-Host "=== Building libSceAgcDriver (Erlou kyty_gpu + KytyShim) ==="
$bldOut = & $cmake --build $buildDir --target libSceAgcDriver --parallel 2>&1
$bldCode = $LASTEXITCODE
$bldOut | ForEach-Object { Write-Host $_ }
if ($bldCode -ne 0) {
    $tail = ($bldOut | Select-Object -Last 500 | Out-String)
    [System.IO.File]::WriteAllText($logFile, ($tail + "`n=== libSceAgcDriver FAILED (exit $bldCode) ===`n"), $utf8NoBom)
    exit $bldCode
}

Write-Host "=== Building patched libs target ==="
$libsOut = & $cmake --build $buildDir --target libs --parallel 2>&1
$libsCode = $LASTEXITCODE
$libsOut | ForEach-Object { Write-Host $_ }
$tail = ($libsOut | Select-Object -Last 500 | Out-String)
[System.IO.File]::WriteAllText($logFile, ($tail + "`n=== libs finished (exit $libsCode) ===`n"), $utf8NoBom)
exit $libsCode
