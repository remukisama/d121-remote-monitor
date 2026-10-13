param([switch]$ShowViewMenu,[int]$ClickFromRight=-1,[int]$ClickY=-1,[string]$Output='ide-render.png')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class D121IDE {
 [StructLayout(LayoutKind.Sequential)]public struct RECT{public int L,T,R,B;}
 [DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")]public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
 [DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")]public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")]public static extern void mouse_event(uint f,uint x,uint y,uint d,UIntPtr e);
 [DllImport("user32.dll")]public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
 [DllImport("user32.dll")]public static extern bool IsIconic(IntPtr h);
 [DllImport("user32.dll")]public static extern bool ShowWindow(IntPtr h,int n);
 [DllImport("user32.dll")]public static extern bool SetPhysicalCursorPos(int x,int y);
 public delegate bool EnumFn(IntPtr h,IntPtr p);
 [DllImport("user32.dll")]public static extern bool EnumWindows(EnumFn f,IntPtr p);
 [DllImport("user32.dll")]public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
}
'@
[void][D121IDE]::SetThreadDpiAwarenessContext([IntPtr]::new(-4))
$windows=[Collections.Generic.List[object]]::new()
$callback=[D121IDE+EnumFn]{param($h,$unused)
 if([D121IDE]::IsWindowVisible($h)){
  $title=[Text.StringBuilder]::new(512);[void][D121IDE]::GetWindowText($h,$title,512)
  if($title.ToString() -like 'D121远程监控*'){
   $bounds=[D121IDE+RECT]::new();[void][D121IDE]::GetWindowRect($h,[ref]$bounds)
   $windows.Add([PSCustomObject]@{Handle=$h;Area=($bounds.R-$bounds.L)*($bounds.B-$bounds.T)})
  }
 };return $true
}
[void][D121IDE]::EnumWindows($callback,[IntPtr]::Zero)
$window=$windows | Sort-Object Area -Descending | Select-Object -First 1
if(!$window){throw 'D121 project window missing'}
if([D121IDE]::IsIconic($window.Handle)){[void][D121IDE]::ShowWindow($window.Handle,9);Start-Sleep -Milliseconds 500}
$r=[D121IDE+RECT]::new();[void][D121IDE]::GetWindowRect($window.Handle,[ref]$r)
if($ShowViewMenu -or $ClickFromRight -ge 0){
 [void][D121IDE]::SetForegroundWindow($window.Handle)
 Start-Sleep -Milliseconds 300
 [void][D121IDE]::GetWindowRect($window.Handle,[ref]$r)
 $x=365; $y=26
 if($ClickFromRight -ge 0){$x=$r.R-$r.L-$ClickFromRight;$y=$ClickY}
 if($x -lt 0 -or $x -ge $r.R-$r.L -or $y -lt 0 -or $y -ge $r.B-$r.T){throw 'Click outside project window'}
 [void][D121IDE]::SetPhysicalCursorPos($r.L+$x,$r.T+$y)
 [D121IDE]::mouse_event(2,0,0,0,[UIntPtr]::Zero);Start-Sleep -Milliseconds 100;[D121IDE]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
 Start-Sleep -Milliseconds 2500
}
$bitmap=[Drawing.Bitmap]::new($r.R-$r.L,$r.B-$r.T);$g=[Drawing.Graphics]::FromImage($bitmap);$dc=$g.GetHdc()
try{[void][D121IDE]::PrintWindow($window.Handle,$dc,2)}finally{$g.ReleaseHdc($dc)}
$dest=Join-Path (Join-Path $PSScriptRoot '../output/native') $Output
try{$bitmap.Save($dest)}finally{$g.Dispose();$bitmap.Dispose()}
Write-Output $dest
Write-Output "Window pixels: $($r.R-$r.L)x$($r.B-$r.T)"
