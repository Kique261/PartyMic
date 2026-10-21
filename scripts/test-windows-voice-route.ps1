# Opt-in integration model of the observed selected/effective input mismatch.
# Runs PartyMic and our own tone/capture helpers; never opens or controls LOL.
param(
    [Parameter(Mandatory=$true)][string]$Application,
    [Parameter(Mandatory=$true)][string]$Probe,
    [Parameter(Mandatory=$true)][string]$AudioTestExecutable,
    [Parameter(Mandatory=$true)][string]$LogDirectory
)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$toneExe=(Resolve-Path -LiteralPath $AudioTestExecutable).Path
$handoff=Join-Path $PSScriptRoot 'test-windows-mic-handoff.ps1'
$eventName='Local\PartyMicRouteTone-'+[guid]::NewGuid().ToString('N')
$stopTone=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$eventName)
$tone=$null
try {
    $tone=Start-Process -FilePath $toneExe -ArgumentList @('--tone','997',$eventName) -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 1200
    $tone.Refresh()
    if($tone.HasExited){throw ('Tone source exited early: '+$tone.ExitCode)}
    & $handoff -Application $Application -Probe $Probe -Music ([IO.Path]::GetFileName($toneExe)) `
        -Methods @('route') -LogDirectory $LogDirectory
} finally {
    [void]$stopTone.Set()
    if($tone){
        if(!$tone.WaitForExit(5000)){$tone.Kill();[void]$tone.WaitForExit(5000)}
        $tone.Dispose()
    }
    $stopTone.Dispose()
}
