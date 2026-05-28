$buildDir = Join-Path $PSScriptRoot 'build'
if (-not (Test-Path $buildDir)) {
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

Write-Host "Building into $buildDir"
$mainSrc       = Join-Path $PSScriptRoot 'src\main.cpp'
$captureSrc    = Join-Path $PSScriptRoot 'src\capture\wasapi_capture.cpp'
$suppressorSrc = Join-Path $PSScriptRoot 'src\suppressor\suppressor.cpp'
$processingSrc = Join-Path $PSScriptRoot 'src\processing\processing_stage.cpp'
$testSrc       = Join-Path $PSScriptRoot 'tests\test_ring_buffer.cpp'
$captureTestSrc = Join-Path $PSScriptRoot 'tests\test_wasapi_capture.cpp'
$processingTestSrc = Join-Path $PSScriptRoot 'tests\test_processing_stage.cpp'
$mainExe    = Join-Path $buildDir 'main.exe'
$testExe    = Join-Path $buildDir 'test_ring_buffer.exe'
$captureTestExe = Join-Path $buildDir 'test_wasapi_capture.exe'
$processingTestExe = Join-Path $buildDir 'test_processing_stage.exe'

# Windows audio link libs (only needed by main, not the test).
# -lole32  : CoInitializeEx, CoCreateInstance, CoTaskMemFree
# -lwinmm  : timing helpers / general MM linkage (paranoid include)
# -lksuser : KSDATAFORMAT_SUBTYPE_IEEE_FLOAT (mmreg/ksmedia GUIDs INITGUID doesn't emit)
$winAudioLibs = @('-lole32','-lwinmm','-lksuser')

$gxx = 'C:\msys64\mingw64\bin\g++.exe'
if (-not (Test-Path $gxx)) {
    $gxx = (Get-Command g++ -ErrorAction Stop).Source
}
Write-Host "Using compiler: $gxx"

# g++ spawns cc1plus, as, ld from its own bin directory at compile time.
# If C:\MinGW\bin (or any other toolchain bin) shadows them on PATH, the
# subprocess lookup picks up a mismatched binary and the build dies silently.
$gxxDir = Split-Path -Parent $gxx
if (($env:PATH -split ';')[0] -ne $gxxDir) {
    $env:PATH = "$gxxDir;$env:PATH"
}

$commonArgs = @('-std=c++17', '-pthread',
                '-static', '-static-libgcc', '-static-libstdc++')

& $gxx @commonArgs $mainSrc $captureSrc $suppressorSrc $processingSrc -o $mainExe @winAudioLibs
if ($LASTEXITCODE -ne 0) { throw "Failed to compile main + capture + suppressor + processing" }

& $gxx @commonArgs $testSrc -o $testExe
if ($LASTEXITCODE -ne 0) { throw "Failed to compile test_ring_buffer.cpp" }

& $gxx @commonArgs $captureTestSrc $captureSrc -o $captureTestExe @winAudioLibs
if ($LASTEXITCODE -ne 0) { throw "Failed to compile test_wasapi_capture" }

& $gxx @commonArgs $processingTestSrc $processingSrc $suppressorSrc -o $processingTestExe
if ($LASTEXITCODE -ne 0) { throw "Failed to compile test_processing_stage" }

Write-Host "Build complete. Executables:"
Write-Host "  $mainExe"
Write-Host "  $testExe"
Write-Host "  $captureTestExe"
Write-Host "  $processingTestExe"
