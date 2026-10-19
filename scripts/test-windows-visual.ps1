param([Parameter(Mandatory=$true)][string]$LogDirectory)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;using System.Runtime.InteropServices;using System.Text;
public class Visual {
 [DllImport("user32.dll")]public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr x);
 [DllImport("user32.dll")]public static extern IntPtr GetDlgItem(IntPtr h,int id);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern IntPtr FindWindowEx(IntPtr p,IntPtr after,string cls,string name);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern IntPtr GetProp(IntPtr h,string name);
 [DllImport("user32.dll",EntryPoint="SendMessageW")]public static extern IntPtr Send(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")]public static extern bool GetCursorPos(out P p);
 [DllImport("user32.dll")]public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")]public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")]public static extern int GetScrollPos(IntPtr h,int bar);
 [DllImport("user32.dll")]public static extern uint GetGuiResources(IntPtr p,uint flags);
 [DllImport("user32.dll")]public static extern bool SystemParametersInfo(uint action,uint p,out bool enabled,uint flags);
 [StructLayout(LayoutKind.Sequential)]public struct P{public int X,Y;}
 public static System.Drawing.Bitmap Capture(IntPtr h,bool screen=false){R r;GetWindowRect(h,out r);var b=new System.Drawing.Bitmap(r.Right-r.L,r.B-r.T);using(var g=System.Drawing.Graphics.FromImage(b)){if(screen){g.CopyFromScreen(r.L,r.T,0,0,b.Size);}else{var dc=g.GetHdc();PrintWindow(h,dc,2);g.ReleaseHdc(dc);}}return b;}
 // Read a visible control pixel directly. PrintWindow(RENDERFULLCONTENT) can
 // wait for DWM long enough to miss the entire 160 ms hover transition.
 [DllImport("user32.dll")]static extern IntPtr GetDC(IntPtr h);
 [DllImport("user32.dll")]static extern int ReleaseDC(IntPtr h,IntPtr dc);
 [DllImport("gdi32.dll")]static extern uint GetPixel(IntPtr dc,int x,int y);
 public static int Pixel(IntPtr h){R r;GetWindowRect(h,out r);var dc=GetDC(h);try{uint c=GetPixel(dc,r.Right-r.L-10,(r.B-r.T)/2);if(c==0xffffffff)throw new Exception("Visible control pixel unavailable");return unchecked((int)(0xff000000|((c&255)<<16)|(c&0xff00)|((c>>16)&255)));}finally{ReleaseDC(h,dc);}}
 public static double Compare(System.Drawing.Bitmap a,System.Drawing.Bitmap b){int bad=0,total=a.Width*a.Height;for(int y=0;y<a.Height;y++)for(int x=0;x<a.Width;x++){var p=a.GetPixel(x,y);var q=b.GetPixel(x,y);if(Math.Abs(p.R-q.R)+Math.Abs(p.G-q.G)+Math.Abs(p.B-q.B)>30)bad++;}return (double)bad/total;}
 [DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out R r);
 [DllImport("user32.dll")]public static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int w,int height,uint flags);
 [DllImport("user32.dll")]public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")]public static extern bool ShowWindow(IntPtr h,int n);
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

[void][Visual]::SetThreadDpiAwarenessContext([IntPtr](-4))
New-Item -ItemType Directory -Force $LogDirectory | Out-Null
$instances=@(Get-Process PartyMic -ErrorAction Stop)
if($instances.Count -ne 1){throw 'Exactly one PartyMic GUI is required'}
$proc=$instances[0];$main=[Visual]::Find($proc.Id)
$checks=[Collections.Generic.List[string]]::new()
function Require($ok,$why){if(!$ok){throw $why};$checks.Add('PASS '+$why)}
function Control($id){[Visual]::GetDlgItem($main,$id)}
function Send($id,$msg,$w=0,$l=0){[Visual]::Send((Control $id),$msg,[IntPtr]$w,[IntPtr]$l)}
function Click($id){[void](Send $id 0xf5)}
function Capture($name){$b=[Visual]::Capture($main);try{$b.Save((Join-Path $LogDirectory $name))}finally{$b.Dispose()}}
function Gain($id,$value){[void](Send $id 0x405 0 $value);[void][Visual]::Send($main,0x114,[IntPtr]5,(Control $id))}
$cursor=[Visual+P]::new();[void][Visual]::GetCursorPos([ref]$cursor)
$passed=$false;$screenDifference=$null;$motion=$true;$effects=$true
$cadence=[Collections.Generic.List[object]]::new()
try{
 [void][Visual]::SystemParametersInfo(0x1042,0,[ref]$motion,0)
 [void][Visual]::SystemParametersInfo(0x103e,0,[ref]$effects,0)
 $motion=$motion -and $effects
 Require ([Visual]::Send($main,0x7f,[IntPtr]0,[IntPtr]::Zero) -ne [IntPtr]::Zero) 'Explicit small window icon loaded'
 Require ([Visual]::Send($main,0x7f,[IntPtr]1,[IntPtr]::Zero) -ne [IntPtr]::Zero) 'Explicit large window icon loaded'
 $icon=[Drawing.Icon]::ExtractAssociatedIcon($proc.Path);try{$b=$icon.ToBitmap();try{$b.Save((Join-Path $LogDirectory 'exe-icon.png'))}finally{$b.Dispose()}}finally{$icon.Dispose()}
 Click 201;Start-Sleep -Milliseconds 350
 Click 202
 $r=[Visual+R]::new();[void][Visual]::GetWindowRect((Control 114),[ref]$r);$firstY=$r.T
 if($motion){Require ([Visual]::FindWindowEx($main,[IntPtr]::Zero,'PartyMicPageTransition',$null) -ne [IntPtr]::Zero) 'Content fade overlay created'}
 Capture 'motion-00.png';Start-Sleep -Milliseconds 45;Capture 'motion-01.png'
 Start-Sleep -Milliseconds 250
 [void][Visual]::GetWindowRect((Control 114),[ref]$r)
 Require ($firstY -eq $r.T) 'Native controls stay at final coordinates throughout page fade'
 Require ([Visual]::FindWindowEx($main,[IntPtr]::Zero,'PartyMicPageTransition',$null) -eq [IntPtr]::Zero) 'Content fade overlay released after completion'
 Capture 'motion-final.png'
 if($motion){
  foreach($page in @(201,202,201,202)){
   $watch=[Diagnostics.Stopwatch]::StartNew();Click $page;$commandMs=$watch.Elapsed.TotalMilliseconds
   Start-Sleep -Milliseconds 350
   $frames=[Visual]::GetProp($main,'PartyMicTransitionFrames').ToInt64()
   $gap=[Visual]::GetProp($main,'PartyMicTransitionMaxGapMs').ToInt64()
   $duration=[Visual]::GetProp($main,'PartyMicTransitionDurationMs').ToInt64()
   $cadence.Add([pscustomobject]@{Page=$page;CommandMs=$commandMs;PaintFrames=$frames;MaxPaintGapMs=$gap;DurationMs=$duration})
   Require ($frames -ge 5 -and $gap -le 65 -and $duration -ge 190 -and $duration -le 300) ('Page fade paint cadence '+$page)
  }
 }
 # Rapid navigation must retain the shield instead of exposing a live intermediate page.
 Click 201;Start-Sleep -Milliseconds 300
 Click 202
 $shield=[Visual]::FindWindowEx($main,[IntPtr]::Zero,'PartyMicPageTransition',$null)
 Click 203
 if($motion){Require ($shield -ne [IntPtr]::Zero -and [Visual]::FindWindowEx($main,[IntPtr]::Zero,'PartyMicPageTransition',$null) -eq $shield) 'Rapid navigation retains the same content shield'}
 Start-Sleep -Milliseconds 300
 Require ([Visual]::FindWindowEx($main,[IntPtr]::Zero,'PartyMicPageTransition',$null) -eq [IntPtr]::Zero) 'Rapid navigation settles on live destination'
 Click 202
 [void][Visual]::Send($main,0x115,[IntPtr]7,[IntPtr]::Zero)
 Require ([Visual]::FindWindowEx($main,[IntPtr]::Zero,'PartyMicPageTransition',$null) -eq [IntPtr]::Zero) 'Scrolling interrupts the fade'
 Click 201;Start-Sleep -Milliseconds 300
 Click 202
 $size=[Visual+R]::new();[void][Visual]::GetWindowRect($main,[ref]$size)
 [void][Visual]::SetWindowPos($main,[IntPtr]::Zero,0,0,($size.Right-$size.L-10),($size.B-$size.T),6)
 Require ([Visual]::FindWindowEx($main,[IntPtr]::Zero,'PartyMicPageTransition',$null) -eq [IntPtr]::Zero) 'Resizing interrupts the fade'
 [void][Visual]::SetWindowPos($main,[IntPtr]::Zero,0,0,($size.Right-$size.L),($size.B-$size.T),6)
 Click 203
 [void][Visual]::ShowWindow($main,6);Start-Sleep -Milliseconds 100
 Require ([Visual]::FindWindowEx($main,[IntPtr]::Zero,'PartyMicPageTransition',$null) -eq [IntPtr]::Zero) 'Minimizing releases the fade'
 [void][Visual]::ShowWindow($main,9);[void][Visual]::SetForegroundWindow($main)
 Click 201;Click 202;Gain 106 110
 Require ([Visual]::FindWindowEx($main,[IntPtr]::Zero,'PartyMicPageTransition',$null) -eq [IntPtr]::Zero) 'Gain input immediately reveals live controls during fade'
 Gain 106 100
 [void][Visual]::SetForegroundWindow($main)
 [void][Visual]::GetWindowRect((Control 114),[ref]$r)
 [void][Visual]::SetCursorPos($r.L-24,$r.T+20)
 [void](Send 114 0x2a3);Start-Sleep -Milliseconds 220
 $b=[Visual]::Capture((Control 114));$b.Save((Join-Path $LogDirectory 'hover-base.png'));$b.Dispose()
 $base=[Visual]::Pixel((Control 114));[void][Visual]::SetCursorPos($r.L+10,$r.T+20);[void](Send 114 0x200)
 Start-Sleep -Milliseconds 45;$middle=[Visual]::Pixel((Control 114))
 Start-Sleep -Milliseconds 230;$end=[Visual]::Pixel((Control 114))
 $b=[Visual]::Capture((Control 114));$b.Save((Join-Path $LogDirectory 'hover-end.png'));$b.Dispose()
 $checks.Add(('Hover pixels {0:X} {1:X} {2:X}' -f $base,$middle,$end))
 Require ($base -ne $end) 'Primary button responds to hover'
 if($motion){Require ($middle -ne $base -and $middle -ne $end) 'Hover includes an intermediate color'}
 Click 201;Start-Sleep -Milliseconds 250
 $gdiBefore=[Visual]::GetGuiResources($proc.Handle,0)
 foreach($id in @(105,106)){
  Gain $id 50;$scroll=[Visual]::GetScrollPos($main,1)
  for($i=0;$i -lt 40;$i++){[void](Send $id 0x20a (120 -shl 16))}
  for($i=0;$i -lt 20;$i++){[void](Send $id 0x20a ((-120) -shl 16))}
  Require ((Send $id 0x400).ToInt32() -eq 70) ('Rapid wheel gain '+$id)
  for($i=0;$i -lt 4;$i++){[void](Send $id 0x20a (30 -shl 16))}
  Require ((Send $id 0x400).ToInt32() -eq 71) ('High resolution wheel gain '+$id)
  Require ([Visual]::GetScrollPos($main,1) -eq $scroll) ('Wheel over slider does not scroll page '+$id)
  $r=[Visual+R]::new();[void][Visual]::GetWindowRect((Control $id),[ref]$r);$width=$r.Right-$r.L;$y=[int](($r.B-$r.T)/2)
  [void](Send $id 0x201 1 (($y -shl 16) -bor 20))
  for($i=20;$i -lt $width-20;$i+=3){[void](Send $id 0x200 1 (($y -shl 16) -bor $i))}
  [void](Send $id 0x202 0 (($y -shl 16) -bor ($width-20)))
  Require ((Send $id 0x400).ToInt32() -gt 150) ('Continuous drag updates native value '+$id)
 }
 Gain 105 25;Gain 106 100
 # Actual pixels on the visible slider are compared with a clean themed render.
 [void][Visual]::SetForegroundWindow($main);Start-Sleep -Milliseconds 300
 if([Visual]::GetForegroundWindow() -eq $main){
  $a=[Visual]::Capture((Control 105),$true);$b=[Visual]::Capture((Control 105))
  try{$screenDifference=[Visual]::Compare($a,$b);$a.Save((Join-Path $LogDirectory 'slider-screen.png'));$b.Save((Join-Path $LogDirectory 'slider-clean.png'))}finally{$a.Dispose();$b.Dispose()}
  Require ($screenDifference -lt 0.02) 'Slider screen pixels match clean render after rapid input'
 }
 Capture 'rapid-input.png'
 [void][Visual]::GetWindowRect($main,[ref]$r);$width=$r.Right-$r.L;$height=$r.B-$r.T
 [void][Visual]::SetWindowPos($main,[IntPtr]::Zero,0,0,1250,813,6)
 for($i=0;$i -lt 60;$i++){[void][Visual]::Send($main,0x20a,[IntPtr]((-120) -shl 16),[IntPtr]::Zero)}
 Capture 'wheel-bottom.png'
 for($i=0;$i -lt 60;$i++){[void][Visual]::Send($main,0x20a,[IntPtr](120 -shl 16),[IntPtr]::Zero)}
 Capture 'wheel-top.png'
 [void][Visual]::SetWindowPos($main,[IntPtr]::Zero,0,0,$width,$height,6)
 # Repeated transitions must release per-control GDI buffers and settle timers.
 for($i=0;$i -lt 20;$i++){Click 202;Click 201;Click 109;Click 109}
 Start-Sleep -Milliseconds 500
 $gdiAfter=[Visual]::GetGuiResources($proc.Handle,0)
 Require ($gdiAfter -le $gdiBefore+3) 'Repeated transitions do not leak GDI objects'
 $proc.Refresh();$cpuBefore=$proc.CPU;Start-Sleep -Seconds 2;$proc.Refresh();$idleCPU=$proc.CPU-$cpuBefore
 Require ($idleCPU -lt 1.2) 'Motion timers settle without a busy loop'
 Capture 'visual-final.png'
 $passed=$true;$checks.Add('ALL VISUAL CHECKS PASSED')
}finally{
 [void][Visual]::SetCursorPos($cursor.X,$cursor.Y)
 $checks | Set-Content -Encoding UTF8 (Join-Path $LogDirectory 'visual.log')
 [pscustomobject]@{ObservedAt=(Get-Date).ToString('o');ExeSHA256=(Get-FileHash $proc.Path).Hash;Passed=$passed;SystemAnimationsEnabled=$motion;PageFadeCadence=@($cadence.ToArray());ScreenDifferenceRatio=$screenDifference;GDIObjectsBefore=$gdiBefore;GDIObjectsAfter=$gdiAfter;IdleCPUSecondsOverTwoSeconds=$idleCPU} | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 (Join-Path $LogDirectory 'visual.json')
}
