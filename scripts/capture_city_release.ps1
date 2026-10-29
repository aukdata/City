param(
    [uint64]$Seed = 20260316
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$shotDir = Join-Path $root 'App\Screenshot\city_generation'
Push-Location $root
try {
    & python chore/run_check.py build --target City
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & python chore/run_check.py build --target Test
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & python chore/run_check.py test
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    if (Test-Path $shotDir) {
        Get-ChildItem $shotDir -Filter 'city_render_*.png' | Remove-Item -Force
        $constraintMarker = Join-Path $shotDir 'constraint_validation_failed.txt'
        if (Test-Path $constraintMarker) { Remove-Item $constraintMarker -Force }
    }

    $game = Start-Process -FilePath (Join-Path $root 'App\City.exe') `
        -ArgumentList "--capture-city --seed $Seed" `
        -WorkingDirectory (Join-Path $root 'App') `
        -Wait -PassThru -WindowStyle Hidden
    $captured = @(Get-ChildItem -Force (Join-Path $root 'App\Screenshot\city_generation') -Filter 'city_render_*.png')
    if ($game.ExitCode -ne 0) { exit $game.ExitCode }
    if ($captured.Count -lt 6) { throw "Expected 6 city_render screenshots, got $($captured.Count)" }
    $constraintFailure = Join-Path $root 'App\Screenshot\city_generation\constraint_validation_failed.txt'
    if (Test-Path $constraintFailure) { throw "City constraint validation failed; see $constraintFailure" }

    Get-ChildItem -Force (Join-Path $root 'App\Screenshot\city_generation') |
        Select-Object Name, Length, LastWriteTime |
        Format-Table -AutoSize
}
finally {
    Pop-Location
}
