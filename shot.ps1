# Captures the ft_vox game window to docs/screenshots/issue135/<name>.png
# Usage: powershell -File shot.ps1 noon
param([Parameter(Mandatory = $true)][string]$Name)

Add-Type -AssemblyName System.Drawing
Add-Type '
using System;
using System.Runtime.InteropServices;
public class Win32 {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int L, T, R, B; }
}'

[Win32]::SetProcessDPIAware() | Out-Null
$p = Get-Process ft_vox | Select-Object -First 1
if (-not $p) { Write-Error "ft_vox is not running"; exit 1 }

$r = New-Object Win32+RECT
[Win32]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
$w = $r.R - $r.L; $h = $r.B - $r.T

$dir = Join-Path $PSScriptRoot "docs\screenshots\issue135"
New-Item -ItemType Directory -Force -Path $dir | Out-Null

$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$bmp.Save((Join-Path $dir "$Name.png"))
Write-Output "Saved $dir\$Name.png ($w x $h)"
