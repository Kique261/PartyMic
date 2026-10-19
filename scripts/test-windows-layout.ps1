param([Parameter(Mandatory=$true)][string]$LogDirectory)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;using System.Runtime.InteropServices;using System.Text;
public class Pages {
 [DllImport("user32.dll")]public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr x);
 [DllImport("user32.dll")]public static extern IntPtr GetDlgItem(IntPtr h,int id);
 [DllImport("user32.dll",EntryPoint="SendMessageW")]public static extern IntPtr Send(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out R r);
 [DllImport("user32.dll")]public static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int w,int height,uint flags);
 [DllImport("user32.dll")]public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")]public static extern IntPtr SetFocus(IntPtr h);
 [DllImport("user32.dll")]public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
 [DllImport("user32.dll")]public static extern bool EnumWindows(C c,IntPtr p);
 [DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr h,out uint id);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 public delegate bool C(IntPtr h,IntPtr p);
 [StructLayout(LayoutKind.Sequential)]public struct R{public int L,T,Right,B;}
 public static IntPtr Find(uint pid){IntPtr v=IntPtr.Zero;EnumWindows((h,p)=>{uint n;GetWindowThreadProcessId(h,out n);var s=new StringBuilder(256);GetClassName(h,s,256);if(n==pid&&s.ToString()=="PartyMicWindow")v=h;return true;},IntPtr.Zero);return v;}
}
'@
[void][Pages]::SetThreadDpiAwarenessContext([IntPtr](-4))
$dir=$LogDirectory
New-Item -ItemType Directory -Force $dir | Out-Null
$instances=@(Get-Process PartyMic -ErrorAction Stop)
if($instances.Count -ne 1){throw 'Exactly one PartyMic GUI is required'}
$proc=$instances[0]
$deadline=[DateTime]::UtcNow.AddSeconds(8)
do{$main=[Pages]::Find($proc.Id);if($main -ne [IntPtr]::Zero){break};Start-Sleep -Milliseconds 100}while([DateTime]::UtcNow -lt $deadline)
if($main -eq [IntPtr]::Zero){throw 'PartyMic window missing'}
function Send($id,$message,$w=0,$l=0){[Pages]::Send([Pages]::GetDlgItem($main,$id),$message,[IntPtr]$w,[IntPtr]$l)}
function Require($ok,$why){if(!$ok){throw $why};$script:checks.Add('PASS '+$why)}
$checks=[Collections.Generic.List[string]]::new()
[void](Send 102 0x14f 1)
Require ((Send 102 0x157).ToInt32() -ne 0) 'Native dropdown opens'
[void](Send 102 0x14f 0)
# Native slider mouse coordinates, then arrow key. Restore gain after checking.
$slider=[Pages]::GetDlgItem($main,105);$r=[Pages+R]::new();[void][Pages]::GetWindowRect($slider,[ref]$r)
$x=[int](($r.Right-$r.L)*0.7);$y=[int](($r.B-$r.T)/2);$point=$x -bor ($y -shl 16)
[void](Send 105 0x201 1 $point);[void](Send 105 0x202 0 $point)
$gain=(Send 105 0x400).ToInt32()
Require ($gain -gt 25) 'Native mouse slider changes gain'
[void](Send 105 0x100 0x27);[void](Send 105 0x101 0x27)
Require ((Send 105 0x400).ToInt32() -gt $gain) 'Native right arrow changes slider'
[void](Send 105 0x405 1 25)
[void][Pages]::Send($main,0x114,[IntPtr]5,$slider)
foreach($entry in @(@(202,'recorder.png'),@(203,'guide.png'),@(201,'mixer.png'))){
 [void][Pages]::Send([Pages]::GetDlgItem($main,$entry[0]),0xf5,[IntPtr]::Zero,[IntPtr]::Zero)
 Require ([Pages]::IsWindowVisible([Pages]::GetDlgItem($main,$(if($entry[0] -eq 202){114}elseif($entry[0] -eq 201){110}else{203})))) ('Page navigation '+$entry[0])
 Start-Sleep -Milliseconds 250
 $r=[Pages+R]::new();[void][Pages]::GetWindowRect($main,[ref]$r)
 $b=[Drawing.Bitmap]::new($r.Right-$r.L,$r.B-$r.T);$g=[Drawing.Graphics]::FromImage($b);$dc=$g.GetHdc();[void][Pages]::PrintWindow($main,$dc,2);$g.ReleaseHdc($dc);$g.Dispose();$b.Save((Join-Path $dir $entry[1]));$b.Dispose()
}

$r=[Pages+R]::new();[void][Pages]::GetWindowRect($main,[ref]$r)
[void][Pages]::SetWindowPos($main,[IntPtr]::Zero,0,0,1250,813,6)
[void][Pages]::Send($main,0x115,[IntPtr]7,[IntPtr]::Zero)
Start-Sleep -Milliseconds 200
$small=[Pages+R]::new();[void][Pages]::GetWindowRect($main,[ref]$small)
$b=[Drawing.Bitmap]::new($small.Right-$small.L,$small.B-$small.T);$g=[Drawing.Graphics]::FromImage($b);$dc=$g.GetHdc();[void][Pages]::PrintWindow($main,$dc,2);$g.ReleaseHdc($dc);$g.Dispose();$b.Save((Join-Path $dir 'small-scrolled.png'));$b.Dispose()
Require ([Pages]::IsWindowVisible([Pages]::GetDlgItem($main,201))) 'Sidebar stays available while scrolled'
[void][Pages]::SetWindowPos($main,[IntPtr]::Zero,0,0,$r.Right-$r.L,$r.B-$r.T,6)
[void][Pages]::Send($main,0x115,[IntPtr]6,[IntPtr]::Zero)
$proc.Refresh();$before=$proc.CPU;Start-Sleep -Seconds 2;$proc.Refresh();$idleCPU=$proc.CPU-$before
Require ($idleCPU -lt 1.2) 'Idle UI does not continuously consume a CPU core'
[pscustomobject]@{ObservedAt=(Get-Date).ToString('o');ExeSHA256=(Get-FileHash $proc.Path).Hash;Passed=$true;IdleCPUSecondsOverTwoSeconds=$idleCPU} | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $dir 'layout.json')
$checks.Add('ALL LAYOUT CHECKS PASSED')
$checks | Set-Content (Join-Path $dir 'layout.log')
