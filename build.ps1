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
$wavIoTestSrc = Join-Path $PSScriptRoot 'tests\test_wav_io.cpp'
$fftSrc        = Join-Path $PSScriptRoot 'src\suppressor\fft.cpp'
$kissSrc       = Join-Path $PSScriptRoot 'src\suppressor\kiss\kiss_fft.c'
$fftTestSrc    = Join-Path $PSScriptRoot 'tests\test_fft.cpp'
$mainExe    = Join-Path $buildDir 'main.exe'
$testExe    = Join-Path $buildDir 'test_ring_buffer.exe'
$captureTestExe = Join-Path $buildDir 'test_wasapi_capture.exe'
$processingTestExe = Join-Path $buildDir 'test_processing_stage.exe'
$wavIoTestExe = Join-Path $buildDir 'test_wav_io.exe'
$kissObj    = Join-Path $buildDir 'kiss_fft.o'
$fftTestExe = Join-Path $buildDir 'test_fft.exe'

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

# Vendored KISS FFT (src/suppressor/kiss/) is C, not C++ — it assigns malloc's void* to typed
# pointers, which g++ rejects. Compile it with gcc; fft.cpp calls into it via kiss_fft.h's
# extern "C" declarations, so the C-linkage object links cleanly against the C++ program.
$gcc = 'C:\msys64\mingw64\bin\gcc.exe'
if (-not (Test-Path $gcc)) {
    $gcc = (Get-Command gcc -ErrorAction Stop).Source
}

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

# Header-only WAV/raw-PCM helper (tests/wav_io.h) — offline test support, no extra .cpp.
& $gxx @commonArgs $wavIoTestSrc -o $wavIoTestExe
if ($LASTEXITCODE -ne 0) { throw "Failed to compile test_wav_io" }

# Vendored KISS FFT -> C object, then the fft.cpp wrapper + its test linked against it.
& $gcc -O2 -c $kissSrc -o $kissObj
if ($LASTEXITCODE -ne 0) { throw "Failed to compile kiss_fft.c" }

& $gxx @commonArgs $fftTestSrc $fftSrc $kissObj -o $fftTestExe
if ($LASTEXITCODE -ne 0) { throw "Failed to compile test_fft" }

Write-Host "Build complete. Executables:"
Write-Host "  $mainExe"
Write-Host "  $testExe"
Write-Host "  $captureTestExe"
Write-Host "  $processingTestExe"
Write-Host "  $wavIoTestExe"
Write-Host "  $fftTestExe"
