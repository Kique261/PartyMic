# Opt-in: run on the signed-in Windows desktop. No recordings or game access.
param(
    [Parameter(Mandatory=$true)][string]$Application,
    [Parameter(Mandatory=$true)][string]$Probe,
    [string]$Music='cloudmusic.exe',
    [ValidateSet('wavein','directsound','other','communications')]
    [string[]]$Methods=@('wavein','directsound','other','communications'),
    [Parameter(Mandatory=$true)][string]$LogDirectory
)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$exe=(Resolve-Path -LiteralPath $Application).Path
$probeExe=(Resolve-Path -LiteralPath $Probe).Path
New-Item -ItemType Directory -Path $LogDirectory -Force|Out-Null
$root=(Resolve-Path -LiteralPath $LogDirectory).Path
if(@(Get-Process PartyMic -ErrorAction SilentlyContinue).Count){throw 'Exit PartyMic before this opt-in test; existing processes are left untouched'}
Add-Type @'
using System;using System.Text;using System.Runtime.InteropServices;
public static class HandoffUi {
 [DllImport("kernel32.dll")]public static extern ulong GetTickCount64();
 [DllImport("user32.dll")]public static extern IntPtr GetDlgItem(IntPtr h,int id);
 [DllImport("user32.dll")]public static extern bool IsWindowEnabled(IntPtr h);
 [DllImport("user32.dll")]public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,EntryPoint="SendMessageTimeoutW")]
 static extern IntPtr SendValue(IntPtr h,uint m,IntPtr w,IntPtr l,uint flags,uint timeout,out IntPtr result);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,EntryPoint="SendMessageTimeoutW")]
 static extern IntPtr SendText(IntPtr h,uint m,IntPtr w,StringBuilder l,uint flags,uint timeout,out IntPtr result);
 public static int Value(IntPtr h,uint m,int w){IntPtr r;if(SendValue(h,m,new IntPtr(w),IntPtr.Zero,2,2000,out r)==IntPtr.Zero)throw new Exception("PartyMic control timeout");return r.ToInt32();}
 public static string Item(IntPtr h,int n){var b=new StringBuilder(4096);IntPtr r;if(SendText(h,0x148,new IntPtr(n),b,2,2000,out r)==IntPtr.Zero)throw new Exception("PartyMic combo timeout");return b.ToString();}
}
'@
$script:app=$null
$script:mic=$null
$script:window=[IntPtr]::Zero
$script:appLog=$null
$results=[Collections.Generic.List[object]]::new()
function SelectItem([int]$id,[string]$match){
    $combo=[HandoffUi]::GetDlgItem($script:window,$id)
    $count=[HandoffUi]::Value($combo,0x146,0)
    for($i=0;$i -lt $count;$i++){
        if([HandoffUi]::Item($combo,$i).Contains($match)){
            if([HandoffUi]::Value($combo,0x14e,$i) -lt 0){throw 'Selection failed'}
            return
        }
    }
    throw ('Missing PartyMic choice: '+$match)
}
function OpenApp {
    $script:app=[Diagnostics.Process]::Start($exe)
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    do{
        Start-Sleep -Milliseconds 80
        $script:app.Refresh()
        if($script:app.HasExited){throw 'PartyMic exited during startup'}
        $script:window=$script:app.MainWindowHandle
        $ready=$script:window -ne [IntPtr]::Zero -and [HandoffUi]::IsWindowEnabled([HandoffUi]::GetDlgItem($script:window,110))
    }while(!$ready -and [DateTime]::UtcNow -lt $deadline)
    if(!$ready){throw 'PartyMic not ready'}
    SelectItem 102 $Music
    SelectItem 103 'Brio 90'
    SelectItem 104 'CABLE Input'
    $script:appLog=Get-ChildItem (Join-Path $env:LOCALAPPDATA 'PartyMic\Logs') -Filter ('partymic-*-'+$script:app.Id+'-*.log')|Sort-Object Name|Select-Object -Last 1
    if(!$script:appLog){throw 'PartyMic log not found'}
}
function Click([int]$id){
    $control=[HandoffUi]::GetDlgItem($script:window,$id)
    if(![HandoffUi]::IsWindowEnabled($control)){throw ('PartyMic control disabled: '+$id)}
    if(![HandoffUi]::PostMessage($control,0xf5,[IntPtr]::Zero,[IntPtr]::Zero)){throw 'PartyMic click failed'}
}
function ReadAppLog {
    $file=[IO.FileStream]::new($script:appLog.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    $reader=[IO.StreamReader]::new($file,[Text.Encoding]::UTF8)
    try{$reader.ReadToEnd()}finally{$reader.Dispose()}
}
function StartMix {
    $previous=([regex]::Matches((ReadAppLog),' engine.running ')).Count
    Click 110
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    do{
        Start-Sleep -Milliseconds 80
        $text=(ReadAppLog)
        $ready=([regex]::Matches($text,' engine.running ')).Count -gt $previous
        if($text.Contains('engine.error')){throw 'Mixer failed; preserve application log'}
    }while(!$ready -and [DateTime]::UtcNow -lt $deadline)
    if(!$ready){throw 'Mixer startup deadline'}
}
function StopMix {
    Click 111
    $deadline=[DateTime]::UtcNow.AddSeconds(6)
    while(![HandoffUi]::IsWindowEnabled([HandoffUi]::GetDlgItem($script:window,110))){
        if([DateTime]::UtcNow -ge $deadline){throw 'Cooperative stop deadline'}
        Start-Sleep -Milliseconds 40
    }
}
function CloseApp {
    if(!$script:app.CloseMainWindow()){throw 'PartyMic close request failed'}
    if(!$script:app.WaitForExit(20000)){throw 'PartyMic close deadline'}
    $text=(ReadAppLog)
    if(!$text.Contains('app.exit') -or $text.Contains('ui.close.timeout') -or $text.Contains('engine.error')){throw 'Unclean PartyMic lifecycle'}
    Copy-Item $script:appLog.FullName $root -Force
    $script:app.Dispose();$script:app=$null
}
function HoldPhase([string]$name){
    $start=[HandoffUi]::GetTickCount64()
    Start-Sleep -Milliseconds 2800
    @{name=$name;start=$start;end=[HandoffUi]::GetTickCount64()}
}
function Evaluate($phases,[string]$text){
    $open=[regex]::Match($text,'OPEN api=\S+ tick=(\d+)')
    if(!$open.Success){throw 'Probe never opened'}
    $previous=[uint64]$open.Groups[1].Value
    $windows=@(foreach($line in ($text -split '\r?\n')){
        $match=[regex]::Match($line,'MIC api=\S+ second=\d+ tick=(\d+) frames=(\d+) nonzero=(\d+) rms=([0-9.eE+-]+) peak=([0-9.eE+-]+)')
        if($match.Success){
            $tick=[uint64]$match.Groups[1].Value
            @{start=$previous;end=$tick;frames=[uint64]$match.Groups[2].Value;nonzero=[uint64]$match.Groups[3].Value;rms=[double]::Parse($match.Groups[4].Value,[Globalization.CultureInfo]::InvariantCulture)}
            $previous=$tick
        }
    })
    $reports=@(foreach($phase in $phases){
        # Ignore windows that overlap a transition or its first 150 ms.
        $samples=@($windows|Where-Object {$_.start -ge $phase.start+150 -and $_.end -le $phase.end -and $_.end-$_.start -ge 700})
        $verdict='pass'
        if(!$samples.Count){$verdict='inconclusive'}
        elseif(@($samples|Where-Object {$_.frames -lt 8000 -or $_.nonzero -eq 0}).Count){$verdict='fail'}
        @{phase=$phase.name;verdict=$verdict;windows=$samples}
    })
    $baseline=$reports[0]
    if($baseline.verdict -ne 'pass'){throw 'No valid microphone signal baseline while mixing; not a handoff verdict'}
    $reports
}
try{
    foreach($method in $Methods){
        OpenApp
        StartMix
        $script:mic=[Diagnostics.Process]::new()
        $script:mic.StartInfo.FileName=$probeExe
        $script:mic.StartInfo.Arguments=$method+' 24'
        $script:mic.StartInfo.UseShellExecute=$false
        $script:mic.StartInfo.CreateNoWindow=$true
        $script:mic.StartInfo.RedirectStandardOutput=$true
        $script:mic.StartInfo.RedirectStandardError=$true
        $script:mic.StartInfo.StandardOutputEncoding=[Text.UTF8Encoding]::new($false)
        [void]$script:mic.Start()
        $stdout=$script:mic.StandardOutput.ReadToEndAsync()
        $stderr=$script:mic.StandardError.ReadToEndAsync()
        Start-Sleep -Milliseconds 500
        $phases=@(HoldPhase 'mixing')
        StopMix
        $phases+=HoldPhase 'stopped'
        StartMix
        $phases+=HoldPhase 'restarted'
        CloseApp
        $phases+=HoldPhase 'exited'
        OpenApp
        StartMix
        $phases+=HoldPhase 'reopened'
        CloseApp
        $phases+=HoldPhase 'exited_again'
        if(!$script:mic.WaitForExit(8000)){throw 'Probe deadline'}
        $text=$stdout.Result
        [IO.File]::WriteAllText((Join-Path $root ($method+'.log')),$text,[Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText((Join-Path $root ($method+'.err')),$stderr.Result,[Text.UTF8Encoding]::new($false))
        if($script:mic.ExitCode -ne 0){throw ('Probe failure: '+$method)}
        [IO.File]::WriteAllText((Join-Path $root ($method+'-phases.json')),($phases|ConvertTo-Json -Depth 4),[Text.UTF8Encoding]::new($false))
        $reports=@(Evaluate $phases $text)
        $results.Add(@{method=$method;phases=$phases;reports=$reports;scope='Independent client signal activity only; LOL and speech quality unverified'})
        [IO.File]::WriteAllText((Join-Path $root 'results.json'),($results|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
        $script:mic.Dispose();$script:mic=$null
        if(@($reports|Where-Object {$_.verdict -ne 'pass'}).Count){throw ('Handoff signal check failed or inconclusive: '+$method)}
    }
    'PASS independent microphone activity across GUI stop, restart and full process exit'|Set-Content -Encoding UTF8 (Join-Path $root 'result.txt')
}catch{
    ($_|Out-String)|Set-Content -Encoding UTF8 (Join-Path $root 'result.txt')
    throw
}finally{
    if($script:app -and !$script:app.HasExited){[void]$script:app.CloseMainWindow();[void]$script:app.WaitForExit(20000)}
    if($script:mic -and !$script:mic.HasExited){$script:mic.Kill();$script:mic.WaitForExit()}
}
