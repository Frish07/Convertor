param(
    [Parameter(Mandatory)][string]$Toolchain
)

$ErrorActionPreference = 'Stop'
$Toolchain = (Resolve-Path -LiteralPath $Toolchain).Path
$compiler = Join-Path $Toolchain 'bin/x86_64-w64-mingw32-clang++.exe'
$resources = Join-Path $Toolchain 'bin/x86_64-w64-mingw32-windres.exe'
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    New-Item -ItemType Directory -Force build/v2, dist | Out-Null
    Push-Location assets
    try {
        & $resources -O coff convertor.rc ../build/v2/convertor.o
        if ($LASTEXITCODE -ne 0) { throw 'Windows resource compilation failed.' }
    } finally {
        Pop-Location
    }

    $exe = 'build/v2/Convertor.exe'
    & $compiler -std=c++20 -O2 -Wall -Wextra -Wpedantic -static -mwindows -municode `
        -DUNICODE -D_UNICODE -DNOMINMAX -D_WIN32_WINNT=0x0A00 `
        src/app.cpp src/canvas.cpp src/media.cpp build/v2/convertor.o -o $exe `
        -lgdi32 -lmsimg32 -ldwmapi -lcomctl32 -lole32 -luuid -lshell32 -lwinhttp -lbcrypt
    if ($LASTEXITCODE -ne 0) { throw 'C++ build failed.' }

    $bundle = 'build/v2/package/Convertor'
    New-Item -ItemType Directory -Force $bundle | Out-Null
    Copy-Item -LiteralPath $exe, 'README.md' -Destination $bundle
    Copy-Item -LiteralPath (Join-Path $Toolchain 'LICENSE.TXT') -Destination "$bundle/LLVM-MinGW.txt"
    $version = (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion
    if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Missing executable version.' }
    $archive = "dist/Convertor-v$version-windows-x64.zip"
    Compress-Archive -LiteralPath $bundle -DestinationPath $archive -Force
    Get-FileHash -LiteralPath $archive -Algorithm SHA256
} finally {
    Pop-Location
}
