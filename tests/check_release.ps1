param(
    [Parameter(Mandatory)][string]$Archive
)

$ErrorActionPreference = 'Stop'
$expectedSha256 = 'F99B331541EDCCD99079FAAFB2416BACB820D6890CCC50EB7346C6F0BA1A71A5'
if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $expectedSha256) {
    throw 'The archive does not match the published v2.0.0 ZIP.'
}

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

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

    public static IntPtr Find(int pid) {
        var window = IntPtr.Zero;
        while ((window = FindWindowEx(IntPtr.Zero, window, "Convertor.Desktop", "Convertor")) != IntPtr.Zero) {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner == pid) return window;
        }
        return IntPtr.Zero;
    }

    private static void Send(IntPtr window, uint message, IntPtr argument) {
        IntPtr result;
        if (SendMessageTimeout(window, message, IntPtr.Zero, argument, 2, 3000, out result) == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Convertor did not handle message " + message);
    }

    public static void ClickDownload(IntPtr window) {
        // The hidden test window still builds the custom-painted controls on WM_PAINT.
        Send(window, 0x000f, IntPtr.Zero);
        Rect bounds;
        var dpi = GetDpiForWindow(window);
        if (!GetClientRect(window, out bounds) || dpi == 0) throw new InvalidOperationException("No client area");
        int x = bounds.Right - 173 * (int)dpi / 96;
        int y = 32 * (int)dpi / 96;
        Send(window, 0x0202, new IntPtr((y << 16) | x)); // WM_LBUTTONUP
    }
}
'@

function Wait-For([scriptblock]$Condition, [string]$Description, [int]$Seconds) {
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
$process = $null
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:LOCALAPPDATA = Join-Path $directory 'profile'
    $cache = Join-Path $env:LOCALAPPDATA 'Convertor/ffmpeg-9.0.2'
    if ((Get-Command ffmpeg.exe -CommandType Application -ErrorAction SilentlyContinue) -or
        (Get-Command ffprobe.exe -CommandType Application -ErrorAction SilentlyContinue) -or
        (Test-Path -LiteralPath $cache)) {
        throw 'The test must start without FFmpeg on PATH or in the application cache.'
    }

    $process = Start-Process -FilePath $app -WorkingDirectory (Split-Path $app) -WindowStyle Hidden -PassThru
    if (-not $process.WaitForInputIdle(15000)) { throw 'Convertor did not start its Windows message loop.' }
    $window = Wait-For {
        $handle = [ConvertorWindow]::Find($process.Id)
        if ($handle -ne [IntPtr]::Zero) { $handle }
    } 'the Convertor window' 20
    Write-Host 'PASS: the published EXE opened with only Windows system directories on PATH and no FFmpeg cache.'

    [ConvertorWindow]::ClickDownload($window)
    $null = Wait-For {
        (Test-Path -LiteralPath (Join-Path $cache 'ffmpeg.exe')) -and
        (Test-Path -LiteralPath (Join-Path $cache 'ffprobe.exe'))
    } 'Convertor to download FFmpeg' 180
    Write-Host 'PASS: the published EXE downloaded and extracted FFmpeg into its own profile.'
    $cache
} finally {
    if ($process -and -not $process.HasExited) { Stop-Process -InputObject $process }
    $env:PATH = $originalPath
    $env:LOCALAPPDATA = $originalLocal
}
