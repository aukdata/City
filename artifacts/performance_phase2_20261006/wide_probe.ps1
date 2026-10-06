param([string]$Label = 'baseline', [string]$SaveName = 'refactoring_20261006')
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$app=Join-Path $root 'App'
$out=Join-Path $PSScriptRoot $Label
New-Item -ItemType Directory -Path $out -Force | Out-Null
if (Get-Process City,Test -ErrorAction SilentlyContinue) { throw 'City/Test already running' }
$command=Join-Path $out 'command.json'
$statePath=Join-Path $app 'playtest_state.json'
function Send-View([int]$id,[int]$distance) { @{id=$id;action='benchmark_view';performanceOnly=$true;distance=$distance;yaw=-.7;pitch=.72;speed=0;hour=13} | ConvertTo-Json -Compress | Set-Content -LiteralPath $command -Encoding utf8 }
function Wait-State([int]$id,[int]$frame) {
 $limit=(Get-Date).AddSeconds(300)
 while ((Get-Date) -lt $limit) {
  $game.Refresh();if ($game.HasExited) { throw 'Game exited early' }
  if ((Test-Path -LiteralPath $statePath) -and (Get-Item -LiteralPath $statePath).LastWriteTime -gt $started) {
   try { $state=Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json } catch { $state=$null }
   if ($state -and $state.commandId -ge $id -and $state.frame -ge $frame) { return $state }
  }
  Start-Sleep -Milliseconds 200
 }
 throw 'Game state timed out'
}
if ($SaveName -and -not (Test-Path -LiteralPath (Join-Path $app ('saves\'+$SaveName+'\meta.json')))) { throw 'Requested current-format benchmark save is missing' }
Send-View 1 650
$started=Get-Date
$launchArgs=@('--window-size','1920','1080','--seed','42','--playtest-commands',('"'+$command+'"'))
if ($SaveName) { $launchArgs += @('--load',$SaveName) } else { $launchArgs += '--new' }
$game=Start-Process -FilePath (Join-Path $app 'City.exe') -WorkingDirectory $app -ArgumentList $launchArgs -WindowStyle Hidden -PassThru
foreach ($phase in @(@{id=1;distance=650},@{id=2;distance=3000},@{id=3;distance=6000},@{id=4;distance=12000})) {
 if ($phase.id -gt 1) { Send-View $phase.id $phase.distance }
 $begin=Wait-State $phase.id 0
 $end=Wait-State $phase.id ($begin.frame+480)
 Write-Output "$Label view=$($phase.distance) firstFrame=$($begin.frame) lastFrame=$($end.frame)"
}
@{id=5;action='exit'} | ConvertTo-Json -Compress | Set-Content -LiteralPath $command -Encoding utf8
if (-not $game.WaitForExit(120000)) { throw 'Shutdown timed out' }
if ($game.ExitCode -ne 0) { throw "Game failed: $($game.ExitCode)" }
foreach ($name in @('playtest_frames.csv','debug.log','perf.log')) { Copy-Item -LiteralPath (Join-Path $app $name) -Destination (Join-Path $out $name) }
Write-Output "$Label complete"
