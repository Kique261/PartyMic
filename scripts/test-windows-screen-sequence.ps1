param([string]$LogDirectory)
$ErrorActionPreference='Stop'
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;using System.Collections.Generic;using System.Drawing;using System.Drawing.Imaging;using System.Runtime.InteropServices;using System.Diagnostics;using System.Threading;using System.Threading.Tasks;
public class ScreenSequence {
 [DllImport("user32.dll")]public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr x);
 [DllImport("user32.dll")]public static extern bool EnumWindows(C callback,IntPtr p);
 [DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetClassName(IntPtr h,System.Text.StringBuilder name,int n);
 public delegate bool C(IntPtr h,IntPtr p);
 public static IntPtr Find(uint pid){IntPtr found=IntPtr.Zero;EnumWindows((h,p)=>{uint id;GetWindowThreadProcessId(h,out id);var name=new System.Text.StringBuilder(256);GetClassName(h,name,256);if(id==pid&&name.ToString()=="PartyMicWindow")found=h;return true;},IntPtr.Zero);return found;}
 [DllImport("user32.dll")]public static extern IntPtr GetDlgItem(IntPtr h,int id);
 [DllImport("user32.dll",EntryPoint="SendMessageW")]public static extern IntPtr Send(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")]public static extern bool ShowWindow(IntPtr h,int n);
 [DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")]public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")]public static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int w,int height,uint flags);
 [DllImport("user32.dll")]public static extern bool GetClientRect(IntPtr h,out R r);
 [DllImport("user32.dll")]public static extern bool ClientToScreen(IntPtr h,ref P p);
 [DllImport("user32.dll")]public static extern uint GetDpiForWindow(IntPtr h);
 [StructLayout(LayoutKind.Sequential)]public struct R{public int L,T,Right,B;}
 [StructLayout(LayoutKind.Sequential)]public struct P{public int X,Y;}
 public class Frame:IDisposable {public double Ms;public Bitmap Image;public float[] Pixels;public void Dispose(){Image.Dispose();}}
 public class Result {public int Destination;public double CommandMs;public int Frames;public int Backwards;public int OffPath;public double WorstBacktrack;public double MaxResidual;public double MaxCaptureGapMs;public double[] Progress;}
 [DllImport("user32.dll")]static extern IntPtr GetDC(IntPtr h);
 [DllImport("user32.dll")]static extern int ReleaseDC(IntPtr h,IntPtr dc);
 [DllImport("gdi32.dll")]static extern bool StretchBlt(IntPtr dest,int x,int y,int w,int h,IntPtr source,int sx,int sy,int sw,int sh,uint rop);
 public static Bitmap Capture(int x,int y,int w,int h){var small=new Bitmap(320,240,PixelFormat.Format32bppArgb);using(var g=Graphics.FromImage(small)){var dest=g.GetHdc();var screen=GetDC(IntPtr.Zero);try{if(!StretchBlt(dest,0,0,320,240,screen,x,y,w,h,0xcc0020))throw new Exception("Screen capture failed");}finally{ReleaseDC(IntPtr.Zero,screen);g.ReleaseHdc(dest);}}return small;}
 public static float[] Pixels(Bitmap b){var p=new float[b.Width*b.Height*3];var d=b.LockBits(new Rectangle(0,0,b.Width,b.Height),ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);try{byte[] bytes=new byte[d.Stride*b.Height];Marshal.Copy(d.Scan0,bytes,0,bytes.Length);int n=0;for(int y=0;y<b.Height;y++)for(int x=0;x<b.Width;x++)for(int c=0;c<3;c++)p[n++]=bytes[y*d.Stride+x*4+c]/255f;}finally{b.UnlockBits(d);}return p;}
 public static Result Run(IntPtr main,int source,int target,string folder){
  SetThreadDpiAwarenessContext((IntPtr)(-4));ShowWindow(main,9);SetForegroundWindow(main);
  Send(GetDlgItem(main,source),0xf5,IntPtr.Zero,IntPtr.Zero);Thread.Sleep(400);
  if(GetForegroundWindow()!=main)throw new Exception("Own window must be foreground for screen-pixel testing");
  R r;GetClientRect(main,out r);uint dpi=GetDpiForWindow(main);var origin=new P{X=(int)(224*dpi/96),Y=(int)(112*dpi/96)};int w=r.Right-origin.X-2,h=r.B-origin.Y-2;ClientToScreen(main,ref origin);if(w<=0||h<=0)throw new Exception("Invalid content extent "+w+"x"+h+" dpi="+dpi+" rect="+r.Right+"x"+r.B);
  var frames=new List<Frame>();var clock=Stopwatch.StartNew();double commandMs=0;
  var task=Task.Run(()=>{SetThreadDpiAwarenessContext((IntPtr)(-4));Thread.Sleep(120);var t=Stopwatch.StartNew();Send(GetDlgItem(main,target),0xf5,IntPtr.Zero,IntPtr.Zero);commandMs=t.Elapsed.TotalMilliseconds;});
  try {
   while(clock.ElapsedMilliseconds<800){if(GetForegroundWindow()!=main)throw new Exception("Own window lost foreground during screen capture");var b=Capture(origin.X,origin.Y,w,h);frames.Add(new Frame{Ms=clock.Elapsed.TotalMilliseconds,Image=b,Pixels=Pixels(b)});Thread.Sleep(4);}
   task.Wait();var first=frames[0].Pixels;var last=frames[frames.Count-1].Pixels;double energy=0;for(int j=0;j<first.Length;j++){double d=last[j]-first[j];energy+=d*d;}
   if(energy<1)throw new Exception("Pages must have distinct endpoint images");
   var result=new Result{Destination=target,CommandMs=commandMs,Frames=frames.Count,Progress=new double[frames.Count]};double previous=0;
   var rows=new List<string>{"elapsed_ms,progress,residual"};
   for(int i=0;i<frames.Count;i++){var f=frames[i];double dot=0,error=0;for(int j=0;j<first.Length;j++)dot+=(f.Pixels[j]-first[j])*(last[j]-first[j]);double alpha=dot/energy;for(int j=0;j<first.Length;j++){double e=f.Pixels[j]-first[j]-alpha*(last[j]-first[j]);error+=e*e;}double residual=Math.Sqrt(error/energy);
    result.Progress[i]=alpha;result.MaxResidual=Math.Max(result.MaxResidual,residual);result.WorstBacktrack=Math.Max(result.WorstBacktrack,previous-alpha);
    if(previous-alpha>0.18)result.Backwards++;
    if(residual>0.3)result.OffPath++;
    if(i>0)result.MaxCaptureGapMs=Math.Max(result.MaxCaptureGapMs,f.Ms-frames[i-1].Ms);
    previous=alpha;rows.Add(f.Ms.ToString("F3",System.Globalization.CultureInfo.InvariantCulture)+","+alpha.ToString("F5",System.Globalization.CultureInfo.InvariantCulture)+","+residual.ToString("F5",System.Globalization.CultureInfo.InvariantCulture));
    f.Image.Save(System.IO.Path.Combine(folder,"frame-"+i.ToString("D3")+".png"));
   }
   System.IO.File.WriteAllLines(System.IO.Path.Combine(folder,"frames.csv"),rows);return result;
  }finally{foreach(var f in frames)f.Dispose();}
 }
}
'@
[void][ScreenSequence]::SetThreadDpiAwarenessContext([IntPtr](-4))
New-Item -ItemType Directory -Force $LogDirectory|Out-Null
$instances=@(Get-Process PartyMic)
if($instances.Count -ne 1){throw 'Exactly one PartyMic GUI is required'}
$p=$instances[0]
$main=[ScreenSequence]::Find($p.Id)
if($main -eq [IntPtr]::Zero){throw 'Main window not available'}
[void][ScreenSequence]::SetWindowPos($main,[IntPtr]::Zero,0,0,0,0,0x15)
$results=@()
foreach($entry in @(@(201,202),@(202,201),@(201,203),@(203,202))){
 $folder=Join-Path $LogDirectory ([string]$entry[0]+'-'+$entry[1]);New-Item -ItemType Directory -Force $folder|Out-Null
 $results += [ScreenSequence]::Run($main,$entry[0],$entry[1],$folder)
}
$passed=@($results|Where-Object {$_.Backwards -gt 0 -or $_.OffPath -gt 0}).Count -eq 0
[pscustomobject]@{ObservedAt=(Get-Date).ToString('o');ExeSHA256=(Get-FileHash $p.Path).Hash;Passed=$passed;Measurement='Physical screen pixels from own content region; 320x240 samples';Sequences=$results}|ConvertTo-Json -Depth 6|Set-Content -Encoding UTF8 (Join-Path $LogDirectory 'screen-sequence.json')
if(!$passed){throw 'Screen sequence contains backwards/blank/partial-page frames'}
