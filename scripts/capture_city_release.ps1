param(
    [string]$Seed = '20260316'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$shotDir = Join-Path $root 'App\Screenshot\city_generation'
if (Test-Path $shotDir) {
    Get-ChildItem $shotDir -Filter 'city_render_*.png' | Remove-Item -Force
}
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$seed = $Seed

Push-Location $root
try {
    & $msbuild City.sln -p:Configuration=Release -p:Platform=x64 -verbosity:minimal -noLogo
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $test = Start-Process -FilePath '.\Test.exe' `
        -WorkingDirectory (Join-Path $root 'Test\App') `
        -Wait -PassThru -WindowStyle Hidden
    if ($test.ExitCode -ne 0) { exit $test.ExitCode }

    $game = Start-Process -FilePath '.\City.exe' `
        -ArgumentList "--capture-city --seed $seed" `
        -WorkingDirectory (Join-Path $root 'App') `
        -Wait -PassThru
    $captured = @(Get-ChildItem -Force (Join-Path $root 'App\Screenshot\city_generation') -Filter 'city_render_*.png')
    if ($game.ExitCode -ne 0 -and $captured.Count -lt 6) { exit $game.ExitCode }
    if ($captured.Count -lt 6) { throw "Expected 6 city_render screenshots, got $($captured.Count)" }

    Get-ChildItem -Force (Join-Path $root 'App\Screenshot\city_generation') |
        Select-Object Name, Length, LastWriteTime |
        Format-Table -AutoSize
}
finally {
    Pop-Location
}
