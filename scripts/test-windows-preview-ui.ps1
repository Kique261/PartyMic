# Run explicitly on the signed-in desktop. Saves short local recordings; never packaged.
param([Parameter(Mandatory=$true)][string]$Executable,[Parameter(Mandatory=$true)][string]$LogDirectory,
      [Parameter(Mandatory=$true)][string]$AudioTestExecutable)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;using System.Text;using System.Runtime.InteropServices;using System.Collections.Generic;using System.IO;
public class PreviewProbe {
 [DllImport("user32.dll",EntryPoint="SendMessageW")] public static extern IntPtr Send(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",EntryPoint="SendMessageW",CharSet=CharSet.Unicode)] public static extern IntPtr Read(IntPtr h,uint m,IntPtr w,StringBuilder l);
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
 [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr value);
 [DllImport("user32.dll")] public static extern bool EnumWindows(Callback c,IntPtr p);
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h,Callback c,IntPtr p);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out Rect r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint flags);
 public delegate bool Callback(IntPtr h,IntPtr p);
 [StructLayout(LayoutKind.Sequential)] public struct Rect {public int Left,Top,Right,Bottom;}
 public static string Text(IntPtr h){var s=new StringBuilder(4096);Read(h,0xD,(IntPtr)4096,s);return s.ToString();}
 public static string Class(IntPtr h){var s=new StringBuilder(256);GetClassName(h,s,256);return s.ToString();}
 public static IntPtr Window(uint pid){IntPtr result=IntPtr.Zero;EnumWindows((h,p)=>{uint id;GetWindowThreadProcessId(h,out id);if(id==pid&&Class(h)=="PartyMicWindow")result=h;return true;},IntPtr.Zero);return result;}
 public static string AllText(IntPtr h){var s=new StringBuilder();EnumChildWindows(h,(c,p)=>{s.AppendLine(Text(c));return true;},IntPtr.Zero);return s.ToString();}
 public static double[] Wav(string path){var b=File.ReadAllBytes(path);if(b.Length<44||Encoding.ASCII.GetString(b,0,4)!="RIFF"||BitConverter.ToUInt16(b,22)!=2||BitConverter.ToUInt32(b,24)!=48000||BitConverter.ToUInt16(b,34)!=16)throw new Exception("WAV format invalid");double sum=0,peak=0;int clipped=0;for(int i=44;i+1<b.Length;i+=2){double x=BitConverter.ToInt16(b,i)/32768.0;sum+=x*x;peak=Math.Max(peak,Math.Abs(x));if(Math.Abs(x)>=32767.0/32768)clipped++;}return new double[]{(b.Length-44)/4.0/48000,Math.Sqrt(sum/((b.Length-44)/2)),peak,clipped};}
}
'@
[void][PreviewProbe]::SetThreadDpiAwarenessContext([IntPtr](-4))
New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
$log=[Collections.Generic.List[string]]::new()
$results=[Collections.Generic.List[object]]::new()
$recordFolder=Join-Path ([Environment]::GetFolderPath('Desktop')) 'PartyMic录音'
function Require($ok,[string]$why){if(!$ok){$log.Add([PreviewProbe]::AllText($script:main));throw $why};$log.Add('PASS '+$why)}
function Control([int]$id){[PreviewProbe]::GetDlgItem($script:main,$id)}
function Click([int]$id){[void][PreviewProbe]::Send((Control $id),0xf5,[IntPtr]::Zero,[IntPtr]::Zero)}
function SetGain([int]$id,[int]$value){[void][PreviewProbe]::Send((Control $id),0x405,[IntPtr]1,[IntPtr]$value);[void][PreviewProbe]::Send($script:main,0x114,[IntPtr]5,(Control $id))}
function SelectItem([int]$id,[int]$value){[void][PreviewProbe]::Send((Control $id),0x14e,[IntPtr]$value,[IntPtr]::Zero)}
function WaitState([scriptblock]$predicate,[int]$seconds,[string]$why){$deadline=[DateTime]::UtcNow.AddSeconds($seconds);do{if(& $predicate){return};Start-Sleep -Milliseconds 100}while([DateTime]::UtcNow -lt $deadline);$log.Add([PreviewProbe]::AllText($script:main));throw $why}
function NewRecording([string]$label,[scriptblock]$action){
 $before=@(Get-ChildItem -LiteralPath $recordFolder -Filter '*.wav' -ErrorAction SilentlyContinue | ForEach-Object {$_.FullName})
 & $action
 WaitState { [PreviewProbe]::IsWindowEnabled((Control 116)) } 8 'Recording did not complete'
 $new=@(Get-ChildItem -LiteralPath $recordFolder -Filter '*.wav' | Where-Object {$_.FullName -notin $before})
 Require ($new.Count -eq 1) ('Exactly one saved WAV for '+$label)
 $m=[PreviewProbe]::Wav($new[0].FullName)
 $results.Add([pscustomobject]@{Label=$label;File=$new[0].FullName;Seconds=$m[0];RMS=$m[1];Peak=$m[2];ClippedSamples=$m[3]})
 return ,$m
}
$proc=$null;$tone=$null;$toneEvent=$null
try {
 Require (!(Get-Process PartyMic -ErrorAction SilentlyContinue)) 'No concurrent PartyMic GUI'
 $proc=Start-Process -FilePath $Executable -PassThru
 WaitState {$script:main=[PreviewProbe]::Window($proc.Id);$script:main -ne [IntPtr]::Zero} 10 'GUI missing'
 Start-Sleep -Milliseconds 400
 Require ([PreviewProbe]::Send((Control 109),0xf0,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -eq 0) 'Ducking is off by default'
 Require (![PreviewProbe]::IsWindowEnabled((Control 112))) 'Duck depth disabled while ducking off'
 Click 109;Start-Sleep -Milliseconds 150
 Require ([PreviewProbe]::IsWindowEnabled((Control 112))) 'Duck depth enabled with ducking'
 SetGain 112 75
 Click 109
 Require ([PreviewProbe]::Text((Control 117)).Contains('Mi Monitor')) 'HDMI selected as preview output'
 Require ([PreviewProbe]::Text((Control 103)).Contains('Brio 90')) 'Brio selected for microphone recording'
 Click 202
 SetGain 106 0
 $silent=NewRecording 'mic_gain_zero' {Click 114;Start-Sleep -Seconds 2;Click 115}
 Require ($silent[0] -ge 1 -and $silent[1] -eq 0) 'Microphone recording honors zero gain'
 SetGain 106 100
 $mic=NewRecording 'microphone_auto_stop' {Click 114;Start-Sleep -Seconds 16}
 Require ($mic[0] -eq 15 -and $mic[3] -eq 0) 'Microphone auto-stops at 15 seconds without clipping'
 SetGain 106 0
 # Generate our own source so a paused music player cannot invalidate this test.
 $eventName='Local\PartyMicPreviewTone-'+$PID
 $toneEvent=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$eventName)
 $tone=Start-Process -FilePath $AudioTestExecutable -ArgumentList @('--tone','997',$eventName) -WindowStyle Hidden -PassThru
 Start-Sleep -Seconds 1;Click 201;Click 101
 $combo=Control 102;$count=[PreviewProbe]::Send($combo,0x146,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32();$source=-1
 for($i=0;$i -lt $count;$i++){$text=[Text.StringBuilder]::new(4096);[void][PreviewProbe]::Read($combo,0x148,[IntPtr]$i,$text);if($text.ToString().Contains('windows_audio_smoke.exe')){$source=$i;break}}
 Require ($source -ge 0) 'Own synthetic audio source enumerated'
 SelectItem 102 $source
 Click 110
 WaitState {([PreviewProbe]::AllText($script:main)).Contains('正在混音')} 12 'Mixer failed to start'
 Click 202
 SelectItem 113 1
 $mix=NewRecording 'music_mix' {Click 114;Start-Sleep -Seconds 3;Click 115}
 Require ($mix[0] -ge 2 -and $mix[1] -gt 0.00001) 'Selected music reaches mix recording'
 Click 201;Click 111;Click 202
 [void]$toneEvent.Set();Require ($tone.WaitForExit(5000)) 'Own tone source stopped before preview'
 SetGain 118 60
 Click 116
 WaitState {([PreviewProbe]::AllText($script:main)).Contains('正在试听：')} 6 'Preview failed to start'
 Require (![PreviewProbe]::IsWindowEnabled((Control 111))) 'Preview stops mixing before speaker playback'
 Require (![PreviewProbe]::IsWindowEnabled((Control 114))) 'Cannot record while playing back'
 Start-Sleep -Milliseconds 400;Click 115
 Require ([PreviewProbe]::IsWindowEnabled((Control 116))) 'Preview can be stopped and replayed'
 Click 116
 WaitState {([PreviewProbe]::AllText($script:main)).Contains('试听结束。')} 10 'Preview did not drain and complete'
 for($i=0;$i -lt 4;$i++){[void][PreviewProbe]::Send($script:main,0x115,[IntPtr]3,[IntPtr]::Zero)}
 Start-Sleep -Milliseconds 200
 $rect=[PreviewProbe+Rect]::new();[void][PreviewProbe]::GetWindowRect($script:main,[ref]$rect)
 $bitmap=[Drawing.Bitmap]::new($rect.Right-$rect.Left,$rect.Bottom-$rect.Top);$graphics=[Drawing.Graphics]::FromImage($bitmap);$dc=$graphics.GetHdc();[void][PreviewProbe]::PrintWindow($script:main,$dc,2);$graphics.ReleaseHdc($dc);$graphics.Dispose();$bitmap.Save((Join-Path $LogDirectory 'preview-ui.png'));$bitmap.Dispose()
 SelectItem 113 0;SetGain 106 100;SetGain 118 70
 Click 114;Start-Sleep -Seconds 1
 [void][PreviewProbe]::PostMessage($script:main,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 Require ($proc.WaitForExit(8000)) 'Closing window stops and saves recording cleanly'
 Require ($proc.ExitCode -eq 0) 'GUI exits successfully'
 $log.Add('ALL PREVIEW UI CHECKS PASSED')
 [pscustomobject]@{ObservedAt=(Get-Date).ToString('o');ExeSHA256=(Get-FileHash $Executable).Hash;Passed=$true;Recordings=$results} | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 (Join-Path $LogDirectory 'preview-ui.json')
} catch {$log.Add('FAIL '+$_);throw}
finally {
 if($proc -and !$proc.HasExited){$h=[PreviewProbe]::Window($proc.Id);if($h -ne [IntPtr]::Zero){[void][PreviewProbe]::PostMessage($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero)};[void]$proc.WaitForExit(8000)}
 if($toneEvent){[void]$toneEvent.Set()};if($tone){[void]$tone.WaitForExit(5000)};if($toneEvent){$toneEvent.Dispose()}
 [pscustomobject]@{ObservedAt=(Get-Date).ToString('o');ExeSHA256=(Get-FileHash $Executable).Hash;Passed=$log.Contains('ALL PREVIEW UI CHECKS PASSED');Recordings=$results} | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 (Join-Path $LogDirectory 'preview-ui.json')
 [IO.File]::WriteAllLines((Join-Path $LogDirectory 'preview-ui.log'),$log,[Text.UTF8Encoding]::new($false))
}
