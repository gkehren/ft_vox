# Windows build helper for ft_vox (vcpkg + CMake + MSVC/Ninja).
# Usage:
#   .\build.ps1
#   .\build.ps1 -Config Debug
#   .\build.ps1 -VcpkgRoot D:\vcpkg
#   .\build.ps1 -Test

param(
    [string]$Config = "Release",
    [string]$BuildDir = "build",
    [string]$VcpkgRoot = "",
    [switch]$Test,
    [switch]$Run,
    [string]$Args = ""
)

$ErrorActionPreference = "Stop"

function Find-VcpkgRoot {
    if ($env:VCPKG_ROOT -and (Test-Path "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake")) {
        return $env:VCPKG_ROOT
    }
    $candidates = @(
        "C:\vcpkg",
        "D:\vcpkg",
        "D:\Projects\vcpkg",
        "$env:USERPROFILE\vcpkg",
        "$env:USERPROFILE\source\vcpkg"
    )
    foreach ($c in $candidates) {
        if (Test-Path "$c\scripts\buildsystems\vcpkg.cmake") {
            return $c
        }
    }
    return $null
}

if (-not $VcpkgRoot) {
    $VcpkgRoot = Find-VcpkgRoot
}

if (-not $VcpkgRoot) {
    Write-Host @"
vcpkg not found. On Windows, ft_vox expects vcpkg for SDL3, Boost, volk, VMA, glslang.

  git clone https://github.com/microsoft/vcpkg C:\vcpkg
  C:\vcpkg\bootstrap-vcpkg.bat
  `$env:VCPKG_ROOT = 'C:\vcpkg'
  .\build.ps1

Or pass -VcpkgRoot path\to\vcpkg
"@
    exit 1
}

$toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
Write-Host "==> VCPKG_ROOT=$VcpkgRoot"
Write-Host "==> Config=$Config BuildDir=$BuildDir"

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Error "cmake not found in PATH. Install CMake or Visual Studio C++ tools."
}

cmake -B $BuildDir `
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" `
    -DCMAKE_BUILD_TYPE=$Config `
    -DFT_VOX_DEP_MODE=vcpkg

if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $BuildDir --config $Config -j
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Test) {
    Push-Location $BuildDir
    ctest -C $Config --output-on-failure --timeout 120
    $code = $LASTEXITCODE
    Pop-Location
    if ($code -ne 0) { exit $code }
}

if ($Run) {
    $exe = Join-Path $BuildDir "$Config\ft_vox.exe"
    if (-not (Test-Path $exe)) {
        $exe = Join-Path $BuildDir "ft_vox.exe"
    }
    if (-not (Test-Path $exe)) {
        Write-Error "Executable not found under $BuildDir"
    }
    & $exe $Args.Split(' ', [System.StringSplitOptions]::RemoveEmptyEntries)
}
