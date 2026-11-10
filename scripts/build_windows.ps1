param([string]$Python = 'python')

$ErrorActionPreference = 'Stop'
$Python = (Get-Command $Python -CommandType Application).Source
$previousConfigDir = $env:PYINSTALLER_CONFIG_DIR
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    $version = & $Python -c 'from convertor import VERSION; print(VERSION)'
    if ($LASTEXITCODE -ne 0) { throw 'Cannot determine Convertor version.' }
    & $Python -c 'import sysconfig; assert sysconfig.get_platform() == "win-amd64"'
    if ($LASTEXITCODE -ne 0) { throw 'Use Windows x64 Python.' }

    New-Item -ItemType Directory -Force build, dist | Out-Null
    $env:PYINSTALLER_CONFIG_DIR = Join-Path (Get-Location) 'build/pyinstaller-cache'
    & $Python -m PyInstaller --clean --noconfirm --windowed --onedir --name Convertor `
        --distpath build/windows --workpath build/pyinstaller --specpath build main.py
    if ($LASTEXITCODE -ne 0) { throw 'PyInstaller build failed.' }

    $bundle = 'build/windows/Convertor'
    $notices = Join-Path $bundle 'licenses'
    New-Item -ItemType Directory -Force $notices | Out-Null
    $pythonBase = & $Python -c 'import sys; print(sys.base_prefix)'
    $openSslVersion = & $Python -c 'import ssl; print(ssl.OPENSSL_VERSION.split()[1])'
    $tclVersion = & $Python -c 'import tkinter; print(tkinter.Tcl().eval("info patchlevel"))'
    $tclTag = 'core-' + $tclVersion.Replace('.', '-')
    Copy-Item -LiteralPath (Join-Path $pythonBase 'LICENSE.txt') -Destination "$notices/Python.txt"
    Copy-Item -LiteralPath (Join-Path $pythonBase 'tcl/tk8.6/license.terms') -Destination "$notices/Tk.txt"
    Invoke-WebRequest -UseBasicParsing `
        -Uri "https://raw.githubusercontent.com/tcltk/tcl/$tclTag/license.terms" `
        -OutFile "$notices/Tcl.txt"
    Invoke-WebRequest -UseBasicParsing `
        -Uri "https://raw.githubusercontent.com/openssl/openssl/openssl-$openSslVersion/LICENSE.txt" `
        -OutFile "$notices/OpenSSL.txt"
    Copy-Item -LiteralPath 'README.md' -Destination $bundle

    $archive = "dist/Convertor-v$version-windows-x64.zip"
    Compress-Archive -LiteralPath $bundle -DestinationPath $archive -Force
    Get-FileHash -LiteralPath $archive -Algorithm SHA256
} finally {
    $env:PYINSTALLER_CONFIG_DIR = $previousConfigDir
    Pop-Location
}
