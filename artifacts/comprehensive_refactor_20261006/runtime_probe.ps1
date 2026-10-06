$ErrorActionPreference = 'Stop'
$probeRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$probeApp = Join-Path $probeRoot 'App'
$probeOutput = $PSScriptRoot
$probeCommand = Join-Path $probeOutput 'runtime_command.json'
$probeState = Join-Path $probeApp 'playtest_state.json'
$probeLog = Join-Path $probeApp 'debug.log'
$probeAlias = Join-Path $probeApp 'saves\railway_review'
$probeFinal = Join-Path $probeApp 'saves\refactoring_20261006'
if (Get-Process -Name City,Test -ErrorAction SilentlyContinue) { throw 'City/Test is already running' }
if ((Test-Path -LiteralPath $probeAlias) -or (Test-Path -LiteralPath $probeFinal)) { throw 'Probe save names already exist; refusing to overwrite' }
$probeResults = [System.Collections.Generic.List[object]]::new()
$probeProcess = $null
function Set-ProbeCommand([int]$id, [string]$action) {
    @{id=$id;action=$action;speed=0} | ConvertTo-Json -Compress | Set-Content -LiteralPath $probeCommand -Encoding utf8
}
function Start-ProbeGame([string]$save) {
    $script:probeStarted = Get-Date
    $launchArgs = @('--seed','42','--playtest-commands',('"'+$probeCommand+'"'))
    if ($save) { $launchArgs += @('--load',$save) } else { $launchArgs += '--new' }
    $script:probeProcess = Start-Process -FilePath (Join-Path $probeApp 'City.exe') -WorkingDirectory $probeApp -ArgumentList $launchArgs -WindowStyle Hidden -PassThru
}
function Wait-ProbeState([int]$id) {
    $limit = (Get-Date).AddSeconds(240)
    while ((Get-Date) -lt $limit) {
        $probeProcess.Refresh()
        if ($probeProcess.HasExited) { throw "Game exited before command $id" }
        if ((Test-Path -LiteralPath $probeState) -and (Get-Item -LiteralPath $probeState).LastWriteTime -gt $probeStarted) {
            try { $state=Get-Content -LiteralPath $probeState -Raw | ConvertFrom-Json } catch { $state=$null }
            if ($state -and $state.commandId -ge $id) { return $state }
        }
        if ((Test-Path -LiteralPath $probeLog) -and (Get-Item -LiteralPath $probeLog).LastWriteTime -gt $probeStarted -and (Select-String -LiteralPath $probeLog -Pattern '\[Load\] Failed|\[Loading\] (Exception|Unknown exception)' -Quiet)) { throw 'Loading failed; see debug.log' }
        Start-Sleep -Milliseconds 200
    }
    throw "Timeout waiting for command $id"
}
function Stop-ProbeGame([int]$id, [string]$label) {
    Set-ProbeCommand $id 'exit'
    if (-not $probeProcess.WaitForExit(120000)) { throw 'Game shutdown timed out' }
    if ($probeProcess.ExitCode -ne 0) { throw "Game failed: $($probeProcess.ExitCode)" }
    $log = Get-Content -LiteralPath $probeLog -Raw
    if ($log -notmatch '\[GameApp\] stopped' -or $log -notmatch 'DebugLog closed') { throw 'Missing completed log session' }
    Copy-Item -LiteralPath $probeLog -Destination (Join-Path $probeOutput ($label+'.log'))
}
try {
    Set-ProbeCommand 1 'benchmark_view'
    Start-ProbeGame ''
    $loaded = Wait-ProbeState 1
    $generatedLog = Get-Content -LiteralPath $probeLog -Raw
    $generatedMatch = [regex]::Matches($generatedLog,'\[CityConstraintValidation\] buildings=(\d+)[^\r\n]*passed=true')
    if ($generatedMatch.Count -eq 0) { throw 'Generated city constraint validation failed' }
    $generatedBuildings = [int]$generatedMatch[$generatedMatch.Count-1].Groups[1].Value
    if ($loaded.population -le 0) { throw 'Current generated city has no population' }
    Set-ProbeCommand 2 'rail_save_review'
    $saved = Wait-ProbeState 2
    if (-not (Select-String -LiteralPath $probeLog -Pattern '\[Save\] Saved atomically' -Quiet)) { throw 'Real save transaction did not complete' }
    $savedEconomy=Get-Content (Join-Path $probeAlias 'global\economy.json') -Raw | ConvertFrom-Json
    $savedMeta=Get-Content (Join-Path $probeAlias 'meta.json') -Raw | ConvertFrom-Json
    $developmentHashes = @{}
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $probeAlias 'chunks') -Recurse -Filter 'development.bin' -File) { $developmentHashes[$file.Directory.Name] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
    if ($developmentHashes.Count -ne 4096) { throw 'Incomplete saved layout' }
    Stop-ProbeGame 3 'save'
    $probeResults.Add(@{stage='new-and-save';status='passed';buildings=$generatedBuildings;population=$savedEconomy.population;funds=$savedEconomy.funds;railEdges=$saved.railway.edgeCount;railLines=$saved.railway.lines.Count})
    Set-ProbeCommand 4 'benchmark_view'
    Start-ProbeGame 'railway_review'
    $reloaded = Wait-ProbeState 4
    if ($reloaded.population -ne $savedEconomy.population -or $reloaded.funds -ne $savedEconomy.funds -or $reloaded.railway.edgeCount -ne $savedMeta.railway.edges.Count -or $reloaded.railway.lines.Count -ne $savedMeta.railway.schedules.Count) { throw 'Saved economy or railway did not round-trip' }
    $restoredLog = Get-Content -LiteralPath $probeLog -Raw
    $restoredMatch = [regex]::Matches($restoredLog,'\[CityConstraintValidation\] buildings=(\d+)[^\r\n]*passed=true')
    if ($restoredMatch.Count -eq 0 -or [int]$restoredMatch[$restoredMatch.Count-1].Groups[1].Value -ne $generatedBuildings) { throw 'Building layout or parcel integrity changed on reload' }
    Set-ProbeCommand 5 'rail_save_review'
    $resaved = Wait-ProbeState 5
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $probeAlias 'chunks') -Recurse -Filter 'development.bin' -File) { if ($developmentHashes[$file.Directory.Name] -ne (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash) { throw ('Resaved layout differs: '+$file.Directory.Name) } }
    Stop-ProbeGame 6 'reload'
    $probeResults.Add(@{stage='reload-and-resave';status='passed';identicalChunkSnapshots=$developmentHashes.Count;buildings=$generatedBuildings;population=$reloaded.population;railEdges=$reloaded.railway.edgeCount;railLines=$reloaded.railway.lines.Count})
    $resolvedAlias = (Resolve-Path -LiteralPath $probeAlias).Path
    $resolvedParent = (Resolve-Path -LiteralPath (Split-Path -Parent $probeFinal)).Path
    if (-not $resolvedAlias.StartsWith($probeRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or -not $resolvedParent.StartsWith($probeRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Save move escaped the workspace' }
    Move-Item -LiteralPath $resolvedAlias -Destination $probeFinal
    Set-ProbeCommand 7 'benchmark_view'
    Start-ProbeGame 'refactoring_20261006'
    $limit=(Get-Date).AddSeconds(30)
    $closeAccepted=$false
    while ((Get-Date) -lt $limit) {
        if ((Test-Path -LiteralPath $probeLog) -and (Get-Item -LiteralPath $probeLog).LastWriteTime -gt $probeStarted -and (Select-String -LiteralPath $probeLog -Pattern '\[Load\] meta\+init' -Quiet)) {
            if (Select-String -LiteralPath $probeLog -Pattern '\[Load\] TOTAL' -Quiet) { throw 'Load finished before the early-close probe' }
            $probeProcess.Refresh()
            if ($probeProcess.CloseMainWindow()) { $closeAccepted=$true; break }
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $closeAccepted) { throw 'Could not request early shutdown' }
    if (-not $probeProcess.WaitForExit(240000)) { throw 'Early load shutdown timed out' }
    if ($probeProcess.ExitCode -ne 0) { throw 'Early shutdown failed' }
    $closingLog=Get-Content -LiteralPath $probeLog -Raw
    if ($closingLog -notmatch '\[Load\] TOTAL[\s\S]*\[GameApp\] stopped[\s\S]*DebugLog closed') { throw 'Workers did not finish before log shutdown' }
    Copy-Item -LiteralPath $probeLog -Destination (Join-Path $probeOutput 'early_shutdown.log')
    $probeResults.Add(@{stage='close-during-loading';status='passed';exitCode=$probeProcess.ExitCode})
    @{status='passed';save=$probeFinal;stages=$probeResults.ToArray()} | ConvertTo-Json -Depth 8 | Tee-Object -FilePath (Join-Path $probeOutput 'runtime_summary.json')
} finally {
    if ($probeProcess) {
        $probeProcess.Refresh()
        if (-not $probeProcess.HasExited) {
            [void]$probeProcess.CloseMainWindow()
            if (-not $probeProcess.WaitForExit(120000)) { Stop-Process -Id $probeProcess.Id -Force }
        }
    }
}
