param(
    [Parameter(Mandatory)][string]$Toolchain,
    [Parameter(Mandatory)][string]$FFmpegDirectory,
    [switch]$CheckDownload
)

$ErrorActionPreference = 'Stop'
$compiler = Join-Path (Resolve-Path -LiteralPath $Toolchain).Path 'bin/x86_64-w64-mingw32-clang++.exe'
$ffmpegDir = (Resolve-Path -LiteralPath $FFmpegDirectory).Path
$ffmpeg = Join-Path $ffmpegDir 'ffmpeg.exe'
$ffprobe = Join-Path $ffmpegDir 'ffprobe.exe'
$oldLocal = $env:LOCALAPPDATA
$oldPath = $env:PATH
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    $root = Join-Path (Get-Location) ('build/media-' + [guid]::NewGuid().ToString('N'))
    $samples = Join-Path $root ('Samples ' + [char]0x410)
    $outputs = Join-Path $root ('Outputs ' + [char]0x416)
    New-Item -ItemType Directory -Force $samples, $outputs | Out-Null
    $runner = Join-Path $root 'MediaTests.exe'
    & $compiler -std=c++20 -O2 -Wall -Wextra -Wpedantic -static -municode -DNOMINMAX `
        -D_WIN32_WINNT=0x0A00 -I src tests/media_tests.cpp src/media.cpp -o $runner `
        -lole32 -lwinhttp -lbcrypt
    if ($LASTEXITCODE -ne 0) { throw 'Test compilation failed.' }

    $env:LOCALAPPDATA = Join-Path $root 'appdata'
    $env:PATH = "$ffmpegDir;$oldPath"
    if ($CheckDownload) {
        & $runner --install
        if ($LASTEXITCODE -ne 0) { throw 'FFmpeg download test failed.' }
    }

    function Invoke-FFmpeg([string[]]$Options) {
        & $ffmpeg -hide_banner -v error -nostdin -xerror @Options
        if ($LASTEXITCODE -ne 0) { throw "FFmpeg failed: $Options" }
    }
    Invoke-FFmpeg @('-f','lavfi','-i','testsrc2=size=96x64:rate=24','-f','lavfi','-i','sine=frequency=440',
        '-t','0.7','-c:v','libx264','-pix_fmt','yuv420p','-c:a','aac',(Join-Path $samples 'clip.mkv'))
    Invoke-FFmpeg @('-f','lavfi','-i','testsrc2=size=96x64:rate=24','-t','0.5','-c:v','libx264',
        (Join-Path $samples 'silent.mkv'))
    Invoke-FFmpeg @('-f','lavfi','-i','sine=frequency=880','-t','0.5','-c:a','libvorbis',
        (Join-Path $samples 'audio sample.ogg'))
    Invoke-FFmpeg @('-f','lavfi','-i','color=c=red@0.5:s=66x54,format=rgba','-frames:v','1','-c:v','libwebp',
        '-lossless','1',(Join-Path $samples 'image source.webp'))
    Set-Content -LiteralPath (Join-Path $samples 'damaged.mkv') -Value 'invalid media'

    & $runner $samples $outputs
    if ($LASTEXITCODE -ne 0) { throw 'Conversion tests failed.' }
    $codecs = @{
        'clip.mp4' = 'h264,aac'; 'clip.mkv' = 'h264,aac'; 'clip.webm' = 'vp9,opus'; 'clip.mov' = 'h264,aac';
        'image source.png' = 'png'; 'image source.jpg' = 'mjpeg'; 'image source.webp' = 'webp';
        'image source.bmp' = 'bmp'; 'image source.tiff' = 'tiff'; 'audio sample.mp3' = 'mp3';
        'audio sample (2).mp3' = 'mp3'; 'audio sample.wav' = 'pcm_s16le'; 'audio sample.flac' = 'flac';
        'audio sample.m4a' = 'aac'; 'audio sample.ogg' = 'vorbis'; 'audio sample.opus' = 'opus';
        'audio sample.aac' = 'aac'; 'clip.mp3' = 'mp3';
    }
    foreach ($item in $codecs.GetEnumerator()) {
        $file = Join-Path $outputs $item.Key
        $actual = (& $ffprobe -v error -show_entries stream=codec_name -of csv=p=0 $file) -join ','
        if ($LASTEXITCODE -ne 0 -or $actual -ne $item.Value) { throw "Unexpected codecs in $file ($actual)" }
        Invoke-FFmpeg @('-i', $file, '-f', 'null', '-')
    }
    $alpha = & $ffprobe -v error -show_entries stream=pix_fmt -of csv=p=0 (Join-Path $outputs 'image source.png')
    if ($LASTEXITCODE -ne 0 -or $alpha -ne 'rgba') { throw 'PNG transparency was lost.' }
    $before = Invoke-FFmpeg @('-i', (Join-Path $samples 'clip.mkv'), '-map', '0:v:0', '-c', 'copy', '-f', 'hash', '-')
    $after = Invoke-FFmpeg @('-i', (Join-Path $outputs 'clip.mp4'), '-map', '0:v:0', '-c', 'copy', '-f', 'hash', '-')
    if (-not $before -or $before -ne $after) { throw 'Compatible H.264 stream was re-encoded.' }
    "All outputs decode. Test files are in $root"
} finally {
    $env:LOCALAPPDATA = $oldLocal
    $env:PATH = $oldPath
    Pop-Location
}
