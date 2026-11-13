param(
    [Parameter(Mandatory)][string]$Archive
)

$ErrorActionPreference = 'Stop'
$expectedSha256 = 'F99B331541EDCCD99079FAAFB2416BACB820D6890CCC50EB7346C6F0BA1A71A5'
if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $expectedSha256) {
    throw 'The downloaded archive does not match the published v2.0.0 ZIP.'
}

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class ConvertorWindow {
    [StructLayout(LayoutKind.Sequential)]
    private struct Rect { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string className, string title);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")]
    private static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wParam, IntPtr lParam,
                                                    uint flags, uint timeout, out IntPtr result);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GlobalAlloc(uint flags, UIntPtr bytes);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GlobalLock(IntPtr memory);
    [DllImport("kernel32.dll")]
    private static extern bool GlobalUnlock(IntPtr memory);
    [DllImport("kernel32.dll")]
    private static extern IntPtr GlobalFree(IntPtr memory);

    public static IntPtr Find(int pid) {
        var window = IntPtr.Zero;
        while ((window = FindWindowEx(IntPtr.Zero, window, "Convertor.Desktop", "Convertor")) != IntPtr.Zero) {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner == pid) return window;
        }
        return IntPtr.Zero;
    }

    private static void Send(IntPtr window, uint message, IntPtr wParam, IntPtr lParam) {
        IntPtr result;
        if (SendMessageTimeout(window, message, wParam, lParam, 2, 3000, out result) == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Convertor did not handle message " + message);
    }

    public static void Click(IntPtr window, int x, int y) {
        // Rebuild the custom-painted controls before sending a click, including on a hidden test window.
        Send(window, 0x000f, IntPtr.Zero, IntPtr.Zero); // WM_PAINT
        Rect bounds;
        var dpi = GetDpiForWindow(window);
        if (!GetClientRect(window, out bounds) || dpi == 0) throw new InvalidOperationException("No client area");
        int pixelX = x < 0 ? bounds.Right + x * (int)dpi / 96 : x * (int)dpi / 96;
        int pixelY = y < 0 ? bounds.Bottom + y * (int)dpi / 96 : y * (int)dpi / 96;
        Send(window, 0x0202, IntPtr.Zero, new IntPtr((pixelY << 16) | pixelX)); // WM_LBUTTONUP
    }

    public static void Drop(IntPtr window, string file) {
        const int headerSize = 20;
        var names = Encoding.Unicode.GetBytes(file + "\0\0");
        var handle = GlobalAlloc(0x42, new UIntPtr((uint)(headerSize + names.Length)));
        if (handle == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        var buffer = GlobalLock(handle);
        if (buffer == IntPtr.Zero) {
            GlobalFree(handle);
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        Marshal.WriteInt32(buffer, 0, headerSize);
        Marshal.WriteInt32(buffer, 16, 1); // DROPFILES.fWide
        Marshal.Copy(names, 0, IntPtr.Add(buffer, headerSize), names.Length);
        GlobalUnlock(handle);
        Send(window, 0x0233, handle, IntPtr.Zero); // WM_DROPFILES, ownership passes to Convertor
    }
}
'@

function Wait-For([scriptblock]$Condition, [string]$Description, [int]$Seconds = 20) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        $process.Refresh()
        if ($process.HasExited) { throw "Convertor exited with $($process.ExitCode) while waiting for $Description." }
        $result = & $Condition
        if ($result) { return $result }
        Start-Sleep -Milliseconds 250
    } while ($timer.Elapsed.TotalSeconds -lt $Seconds)
    throw "Timed out waiting for $Description."
}

$directory = Join-Path ([IO.Path]::GetTempPath()) ('convertor-release-' + [guid]::NewGuid().ToString('N'))
Expand-Archive -LiteralPath $Archive -DestinationPath $directory
$app = Join-Path $directory 'Convertor/Convertor.exe'
if ((Get-Item -LiteralPath $app).VersionInfo.ProductVersion -ne '2.0.0') { throw 'Unexpected executable version.' }

$originalPath = $env:PATH
$originalLocal = $env:LOCALAPPDATA
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
$env:LOCALAPPDATA = Join-Path $directory 'profile'
$cache = Join-Path $env:LOCALAPPDATA 'Convertor/ffmpeg-9.0.2'
if ((Get-Command ffmpeg.exe -CommandType Application -ErrorAction SilentlyContinue) -or (Test-Path $cache)) {
    throw 'The test must start without FFmpeg on PATH or in the application cache.'
}

# A half-second PCM WAV generated by .NET avoids depending on any preinstalled media tool.
$inputFile = Join-Path $directory 'sample.wav'
$outputFile = Join-Path $directory 'sample.mp3'
$samples = 4000
$sampleRate = 8000
$writer = [IO.BinaryWriter]::new([IO.File]::Create($inputFile))
try {
    $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
    $writer.Write([int](36 + 2 * $samples))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
    $writer.Write([int]16)
    $writer.Write([int16]1)
    $writer.Write([int16]1)
    $writer.Write([int]$sampleRate)
    $writer.Write([int](2 * $sampleRate))
    $writer.Write([int16]2)
    $writer.Write([int16]16)
    $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
    $writer.Write([int](2 * $samples))
    for ($index = 0; $index -lt $samples; $index++) {
        $writer.Write([int16](12000 * [math]::Sin(2 * [math]::PI * 440 * $index / $sampleRate)))
    }
} finally {
    $writer.Dispose()
}

$originalHash = (Get-FileHash -LiteralPath $inputFile).Hash
$process = $null
try {
    $process = Start-Process -FilePath $app -WorkingDirectory (Split-Path $app) -WindowStyle Hidden -PassThru
    if (-not $process.WaitForInputIdle(15000)) { throw 'Convertor did not start its Windows message loop.' }
    $window = Wait-For { $handle = [ConvertorWindow]::Find($process.Id); if ($handle -ne [IntPtr]::Zero) { $handle } } 'the Convertor window'
    Write-Host 'PASS: released Convertor.exe opened with only Windows system directories on PATH and no FFmpeg cache.'

    [ConvertorWindow]::Click($window, 433, 32) # Audio tab
    [ConvertorWindow]::Drop($window, $inputFile)
    [ConvertorWindow]::Click($window, -94, -34) # Convert
    if (Test-Path -LiteralPath $outputFile) { throw 'Conversion succeeded unexpectedly before FFmpeg was available.' }

    [ConvertorWindow]::Click($window, -173, 32) # Download FFmpeg
    $null = Wait-For { (Test-Path "$cache/ffmpeg.exe") -and (Test-Path "$cache/ffprobe.exe") } 'Convertor to download FFmpeg' 180
    Write-Host 'PASS: Convertor downloaded and extracted FFmpeg into its own profile.'

    $null = Wait-For {
        if (Test-Path -LiteralPath $outputFile) { return $true }
        [ConvertorWindow]::Drop($window, $inputFile)
        [ConvertorWindow]::Click($window, -94, -34)
        return $false
    } 'WAV-to-MP3 conversion through the Convertor window' 30

    $codec = & "$cache/ffprobe.exe" -v error -show_entries stream=codec_name -of csv=p=0 $outputFile
    if ($LASTEXITCODE -ne 0 -or $codec -ne 'mp3') { throw "Output is not MP3 ($codec)." }
    & "$cache/ffmpeg.exe" -v error -xerror -i $outputFile -f null -
    if ($LASTEXITCODE -ne 0) { throw 'MP3 output could not be decoded.' }
    if ((Get-FileHash -LiteralPath $inputFile).Hash -ne $originalHash) { throw 'The original WAV was modified.' }
    Write-Host 'PASS: Convertor converted WAV to a decodable MP3 and preserved the original.'
} finally {
    if ($process -and -not $process.HasExited) { Stop-Process -InputObject $process }
    $env:PATH = $originalPath
    $env:LOCALAPPDATA = $originalLocal
}
