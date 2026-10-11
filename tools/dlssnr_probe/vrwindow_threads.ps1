# Which thread owns the renderer's video window, beside the thread of the player's
# own window. Read-only: it enumerates windows and prints.
#
# A player that opens the film on a worker thread (MPC-HC does) has the renderer
# create its window there. When the renderer makes that window again from the
# interface thread, MPC-HC's mouse tracking reports the pointer as gone while it
# is still over the picture, and the pointer is never hidden again.
#
#   powershell -File vrwindow_threads.ps1            one reading
#   powershell -File vrwindow_threads.ps1 -Watch 60  a reading whenever it changes, for 60 s
param([int]$Watch = 0)

Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class VRWin {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    public static List<string> Read() {
        var lines = new List<string>();
        EnumWindows((top, l) => {
            EnumChildWindows(top, (h, l2) => {
                var s = new StringBuilder(64);
                GetClassNameW(h, s, 64);
                if (s.ToString() == "VRWindow") {
                    uint pid, pidRoot;
                    uint tid = GetWindowThreadProcessId(h, out pid);
                    IntPtr root = GetAncestor(h, 2);
                    uint tidRoot = GetWindowThreadProcessId(root, out pidRoot);
                    lines.Add(String.Format("process {0}: video window 0x{1:X} on thread {2}, player window on thread {3} -> {4}{5}",
                        pid, h.ToInt64(), tid, tidRoot, tid == tidRoot ? "SAME thread" : "another thread",
                        IsWindowVisible(h) ? "" : " (hidden)"));
                }
                return true;
            }, IntPtr.Zero);
            return true;
        }, IntPtr.Zero);
        return lines;
    }
}
"@

$last = $null
$until = (Get-Date).AddSeconds($Watch)
do {
    $now = (([VRWin]::Read()) -join "`n")
    if ($now -ne $last) {
        if ($now) { "{0:HH:mm:ss}  {1}" -f (Get-Date), $now } else { "{0:HH:mm:ss}  no video window" -f (Get-Date) }
        $last = $now
    }
    if ($Watch -gt 0) { Start-Sleep -Milliseconds 300 }
} while ((Get-Date) -lt $until)
