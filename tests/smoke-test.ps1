# Automated smoke test for AutoKeyDown.
# Starts the app with --accept-injected (so SendInput events count as physical)
# and drives it with the F24 key, which no normal application reacts to.
# Note: briefly toggles Scroll Lock; the original state is restored at the end.
param([string]$Exe = "$PSScriptRoot\..\build\Release\autokeydown.exe")
$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class K {
    [StructLayout(LayoutKind.Sequential)] struct KI { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] struct IN { public uint type; public KI ki; public long pad1; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, IN[] i, int cb);
    [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
    [DllImport("user32.dll")] public static extern short GetKeyState(int vk);
    public static void Send(ushort vk, bool up) {
        var i = new IN[1]; i[0].type = 1; i[0].ki.vk = vk; i[0].ki.flags = up ? 2u : 0u;
        SendInput(1, i, Marshal.SizeOf(typeof(IN)));
    }
    public static bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
}
"@

$F24 = 0x87; $F23 = 0x86; $SCROLL = 0x91
function Tap($vk) { [K]::Send($vk, $false); Start-Sleep -Milliseconds 60; [K]::Send($vk, $true); Start-Sleep -Milliseconds 150 }
function Press($vk) { [K]::Send($vk, $false); Start-Sleep -Milliseconds 150 }
function Release($vk) { [K]::Send($vk, $true); Start-Sleep -Milliseconds 150 }
function ScrollOn { ([K]::GetKeyState($SCROLL) -band 1) -ne 0 }
$fail = 0
function Check($name, $cond) { if ($cond) { "PASS  $name" } else { "FAIL  $name"; $script:fail++ } }

$origScroll = ScrollOn
if ($origScroll) { Tap $SCROLL }   # start with Scroll Lock off

$p = Start-Process $Exe -ArgumentList '--accept-injected' -PassThru
Start-Sleep -Milliseconds 700
try {
    # 1. Baseline: no Scroll Lock, key releases normally.
    Press $F24; Release $F24
    Check 'key releases normally without Scroll Lock' (-not [K]::Down($F24))

    # 2. Hold F24 + F23, Scroll Lock on, release both -> both stay down.
    Press $F24; Press $F23; Tap $SCROLL; Release $F24; Release $F23
    Check 'F24 preserved after physical release' ([K]::Down($F24))
    Check 'F23 preserved after physical release' ([K]::Down($F23))

    # 3. Press F24 again -> all preserved keys released, Scroll Lock auto-off.
    Press $F24
    Check 'F23 released when a preserved key is pressed' (-not [K]::Down($F23))
    Release $F24
    Check 'F24 released after its press/release' (-not [K]::Down($F24))
    Check 'Scroll Lock turned off automatically' (-not (ScrollOn))

    # 4. Hold F24, Scroll Lock on, release, Scroll Lock off -> released.
    Press $F24; Tap $SCROLL; Release $F24
    Check 'F24 preserved (second round)' ([K]::Down($F24))
    Tap $SCROLL
    Check 'F24 released when Scroll Lock turned off' (-not [K]::Down($F24))

    # 5. Key pressed only after Scroll Lock is on is NOT preserved.
    Tap $SCROLL; Press $F23; Release $F23
    Check 'key pressed after activation is not preserved' (-not [K]::Down($F23))
    Tap $SCROLL

    # 6. Key still physically held when Scroll Lock goes off stays down until released.
    Press $F24; Tap $SCROLL; Tap $SCROLL
    Check 'still-held key stays down after Scroll Lock off' ([K]::Down($F24))
    Release $F24
    Check 'still-held key released by its physical key-up' (-not [K]::Down($F24))
}
finally {
    Stop-Process -Id $p.Id -ErrorAction SilentlyContinue
    [K]::Send($F24, $true); [K]::Send($F23, $true)
    if ((ScrollOn) -ne $origScroll) { Tap $SCROLL }
}
if ($fail) { "$fail check(s) failed"; exit 1 } else { 'All checks passed' }
