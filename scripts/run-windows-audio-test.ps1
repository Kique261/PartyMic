# Run on the signed-in Windows desktop. Opens Brio 90, saves scalar logs only.
param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [ValidateSet('Smoke','Player','Soak','Fidelity','Diagnostic')][string]$Mode='Smoke',
    [string]$Player='cloudmusic.exe',
    [ValidateRange(1,3600)][int]$Seconds=60,
    [string]$LogDirectory=(Join-Path $PWD 'audio-test-results')
)
$ErrorActionPreference='Stop'
$OutputEncoding=[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$exe=(Resolve-Path -LiteralPath $Executable).Path
New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
$logRoot=(Resolve-Path -LiteralPath $LogDirectory).Path
if($Mode -in @('Soak','Diagnostic') -and $Seconds -gt 90){throw 'Synthetic soak/diagnostic duration must be at most 90 seconds'}
[string[]]$argsList=if($Mode -eq 'Player'){@('--player',$Player,[string]$Seconds)}elseif($Mode -eq 'Soak'){@('--soak',[string]$Seconds)}elseif($Mode -eq 'Diagnostic'){@('--diagnose',[string]$Seconds)}elseif($Mode -eq 'Fidelity'){@('--fidelity')}else{@('--run')}
$lines=[Collections.Generic.List[string]]::new()
& $exe @argsList 2>&1 | ForEach-Object {$line=$_.ToString();$lines.Add($line);Write-Output $line}
$code=$LASTEXITCODE
$expected=if($Mode -eq 'Smoke'){'ALL WINDOWS AUDIO SMOKE CHECKS PASSED'}elseif($Mode -eq 'Soak'){'ALL SYNTHETIC SOAK CHECKS PASSED'}elseif($Mode -eq 'Fidelity'){'ALL WINDOWS AUDIO FIDELITY CHECKS PASSED'}elseif($Mode -eq 'Diagnostic'){'ALL CONTINUITY DIAGNOSTICS COMPLETED'}else{'ALL ACTUAL PLAYER CHECKS PASSED'}
if($code -eq 0 -and !$lines.Contains($expected)){$code=1;$lines.Add('FAIL Missing completion marker')}
[IO.File]::WriteAllLines((Join-Path $logRoot ($Mode.ToLower()+'.log')),$lines,[Text.UTF8Encoding]::new($false))
$result=[pscustomobject]@{
    ObservedAt=(Get-Date).ToString('o');Mode=$Mode;ExitCode=$code
    TestExecutableSHA256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    Player=if($Mode -eq 'Player'){$Player}else{$null}
    RequestedSeconds=if($Mode -in @('Player','Soak','Diagnostic')){$Seconds}else{$null}
    Scope=if($Mode -eq 'Diagnostic'){'Observation only; completion does not assert gap-free audio'}else{'Acceptance assertions enforced'}
}
[IO.File]::WriteAllText((Join-Path $logRoot ($Mode.ToLower()+'.json')),($result | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
exit $code
