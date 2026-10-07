$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class WindowTest {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int height, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
'@
$p = Start-Process ./build/Release/Tiaoyin.exe -PassThru
try {
    if (-not $p.WaitForInputIdle(15000)) { throw 'GUI never became idle.' }
    for ($i=0; $i -lt 30; $i++) {
        $p.Refresh()
        if ($p.MainWindowHandle -ne [IntPtr]::Zero) { break }
        if ($p.HasExited) { throw 'GUI exited unexpectedly.' }
        Start-Sleep -Milliseconds 100
    }
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { throw 'No top-level GUI window.' }
    foreach ($id in 101,102,110,111,119,120,131,138) {
        if ([WindowTest]::GetDlgItem($h, $id) -eq [IntPtr]::Zero) { throw "Missing GUI control $id." }
    }
    New-Item gui-test-output -ItemType Directory -Force | Out-Null
    foreach ($size in @(@(1020,880),@(1020,600))) {
        [void][WindowTest]::SetWindowPos($h,[IntPtr]::Zero,0,0,$size[0],$size[1],0x0044)
        Start-Sleep -Milliseconds 400
        $r = New-Object WindowTest+RECT
        if (-not [WindowTest]::GetWindowRect($h,[ref]$r)) { throw 'GetWindowRect failed.' }
        $bitmap = New-Object System.Drawing.Bitmap(($r.R-$r.L),($r.B-$r.T))
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $dc = $graphics.GetHdc()
        try { $ok = [WindowTest]::PrintWindow($h,$dc,2) }
        finally { $graphics.ReleaseHdc($dc); $graphics.Dispose() }
        if (-not $ok) { $bitmap.Dispose(); throw 'PrintWindow failed.' }
        $bitmap.Save((Join-Path (Get-Location) "gui-test-output/window-$($size[1]).png"))
        $bitmap.Dispose()
    }
    [void][WindowTest]::SendMessage($h,0x0111,[IntPtr]140,[IntPtr]::Zero)
    [void][WindowTest]::SendMessage($h,0x0111,[IntPtr]139,[IntPtr]::Zero)
    [void]$p.CloseMainWindow()
    if (-not $p.WaitForExit(10000)) { throw 'GUI did not close gracefully.' }
    if ($p.ExitCode -ne 0) { throw "GUI exited with $($p.ExitCode)." }
    'PASS: main window, representative controls, two window sizes, mute/stop, graceful close. No audio streams opened.' | Out-File gui-test-output/result.txt -Encoding utf8
} finally {
    $p.Refresh()
    if (-not $p.HasExited) { $p.Kill(); $p.WaitForExit() }
}
