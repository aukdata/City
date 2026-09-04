param(
	[string]$AssetRoot = "App\assets\buildings"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$script:VertexIndex = 1

function Write-TextFile([string]$Path, [string]$Content) {
	$full = Join-Path (Get-Location) $Path
	[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($full)) | Out-Null
	[System.IO.File]::WriteAllText($full, $Content, $Utf8NoBom)
}

function Add-Vertex([System.Collections.Generic.List[string]]$Lines, [double]$X, [double]$Y, [double]$Z) {
	$Lines.Add(("v {0:0.###} {1:0.###} {2:0.###}" -f $X, $Y, $Z))
}

function Add-Face([System.Collections.Generic.List[string]]$Lines, [int[]]$Ids) {
	$Lines.Add("f " + ($Ids -join " "))
}

function Add-Box([System.Collections.Generic.List[string]]$Lines, [string]$Material, [double]$X0, [double]$Y0, [double]$Z0, [double]$X1, [double]$Y1, [double]$Z1) {
	$i = $script:VertexIndex
	@(@($X0,$Y0,$Z0),@($X1,$Y0,$Z0),@($X1,$Y0,$Z1),@($X0,$Y0,$Z1),@($X0,$Y1,$Z0),@($X1,$Y1,$Z0),@($X1,$Y1,$Z1),@($X0,$Y1,$Z1)) | ForEach-Object { Add-Vertex $Lines $_[0] $_[1] $_[2] }
	$Lines.Add("usemtl $Material")
	$faces = @(@(0,2,1),@(0,3,2),@(4,5,6),@(4,6,7),@(0,1,5),@(0,5,4),@(3,7,6),@(3,6,2),@(0,4,7),@(0,7,3),@(1,2,6),@(1,6,5))
	foreach ($face in $faces) { Add-Face $Lines @(($i + $face[0]), ($i + $face[1]), ($i + $face[2])) }
	$script:VertexIndex += 8
}

function Add-GableRoof([System.Collections.Generic.List[string]]$Lines, [string]$Material, [double]$X0, [double]$Y0, [double]$Z0, [double]$X1, [double]$Y1, [double]$Z1) {
	$i = $script:VertexIndex
	$xm = ($X0 + $X1) * 0.5
	@(@($X0,$Y0,$Z0),@($X1,$Y0,$Z0),@($X1,$Y0,$Z1),@($X0,$Y0,$Z1),@($xm,$Y1,$Z0),@($xm,$Y1,$Z1)) | ForEach-Object { Add-Vertex $Lines $_[0] $_[1] $_[2] }
	$Lines.Add("usemtl $Material")
	$faces = @(@(0,1,4),@(3,5,2),@(0,4,5),@(0,5,3),@(1,2,5),@(1,5,4))
	foreach ($face in $faces) { Add-Face $Lines @(($i + $face[0]), ($i + $face[1]), ($i + $face[2])) }
	$script:VertexIndex += 6
}

function New-Mtl([hashtable]$Materials) {
	$lines = [System.Collections.Generic.List[string]]::new()
	foreach ($name in ($Materials.Keys | Sort-Object)) {
		$c = $Materials[$name]
		$lines.Add("newmtl $name")
		$lines.Add("Ka 0.14 0.14 0.14")
		$lines.Add(("Kd {0:0.###} {1:0.###} {2:0.###}" -f $c[0], $c[1], $c[2]))
		$lines.Add(("Ks {0:0.###} {1:0.###} {2:0.###}" -f $c[3], $c[3], $c[3]))
		$lines.Add(("Ns {0:0.#}" -f $c[4]))
		$lines.Add("")
	}
	return ($lines -join "`r`n")
}

function New-Obj([string]$Stem, [scriptblock]$Build) {
	$lines = [System.Collections.Generic.List[string]]::new()
	$lines.Add("mtllib $Stem.mtl")
	$lines.Add("o $Stem")
	$script:VertexIndex = 1
	& $Build $lines
	return ($lines -join "`r`n") + "`r`n"
}

$materials = @{
	"WallA"=@(0.70,0.68,0.61,0.04,14); "WallB"=@(0.60,0.63,0.62,0.04,14); "WallC"=@(0.50,0.45,0.39,0.04,14); "WallD"=@(0.76,0.75,0.70,0.04,14)
	"RoofDark"=@(0.16,0.17,0.18,0.06,18); "RoofTile"=@(0.36,0.18,0.14,0.05,14); "RoofBlue"=@(0.19,0.27,0.32,0.06,18); "RoofMetal"=@(0.48,0.49,0.46,0.10,24)
	"Glass"=@(0.18,0.25,0.28,0.18,36); "Concrete"=@(0.50,0.49,0.46,0.05,14); "Awning"=@(0.36,0.14,0.12,0.04,12); "Green"=@(0.22,0.34,0.20,0.03,8); "Soil"=@(0.42,0.34,0.24,0.02,8); "CarLight"=@(0.66,0.67,0.65,0.16,32); "CarDark"=@(0.18,0.20,0.23,0.16,32)
}

$profiles = @(
	@{ w=5.6; d=6.0; h=3.7; roof=1.35; setback=0.7; kind="detached" },
	@{ w=6.2; d=5.6; h=4.2; roof=1.45; setback=0.8; kind="detached" },
	@{ w=7.0; d=6.4; h=6.2; roof=1.35; setback=0.9; kind="detached" },
	@{ w=6.5; d=7.2; h=5.4; roof=1.60; setback=0.9; kind="detached" },
	@{ w=8.0; d=7.2; h=8.0; roof=0.45; setback=1.2; kind="low" },
	@{ w=8.6; d=7.6; h=10.0; roof=0.45; setback=1.3; kind="low" },
	@{ w=9.0; d=8.0; h=16.0; roof=0.50; setback=1.7; kind="mid" },
	@{ w=9.4; d=8.2; h=21.0; roof=0.50; setback=1.8; kind="mid" },
	@{ w=9.8; d=8.8; h=31.0; roof=0.55; setback=2.0; kind="high" },
	@{ w=10.2; d=9.0; h=40.0; roof=0.55; setback=2.2; kind="high" }
)

for ($n=1; $n -le 10; $n++) {
	$stem = "residential_{0:000}" -f $n
	$profile = $profiles[$n - 1]
	$wall = @("WallA","WallB","WallC","WallD")[$n % 4]
	$roof = @("RoofDark","RoofTile","RoofBlue","RoofMetal")[$n % 4]
	$w = [double]$profile.w; $d = [double]$profile.d; $h = [double]$profile.h; $roofH = [double]$profile.roof
	$obj = New-Obj $stem {
		param($L)
		Add-Box $L $wall (-$w/2) 0 (-$d/2) ($w/2) $h ($d/2)
		if($profile.kind -eq "detached"){
			Add-GableRoof $L $roof (-$w/2-0.35) $h (-$d/2-0.30) ($w/2+0.35) ($h+$roofH) ($d/2+0.30)
			Add-Box $L "Concrete" (-$w/2-1.55) 0.0 ($d*0.05) (-$w/2-0.18) 2.05 ($d/2+0.55)
			Add-Box $L "CarDark" ($w*0.08) 0.05 ($d/2+0.48) ($w*0.38) 0.52 ($d/2+3.15)
			Add-Box $L "Soil" (-$w/2-1.7) 0.02 (-$d/2-0.7) ($w/2+1.7) 0.05 (-$d/2-0.18)
		} else {
			Add-Box $L $roof (-$w/2-0.25) $h (-$d/2-0.25) ($w/2+0.25) ($h+$roofH) ($d/2+0.25)
		}
		$floors = [Math]::Max(1, [Math]::Min(10, [Math]::Floor($h / 3.0)))
		for($f=0; $f -lt $floors; $f++){
			$y=1.05+$f*2.85
			if($y -gt $h-0.55){ break }
			Add-Box $L "Glass" (-$w*0.32) $y (-$d/2-0.06) (-$w*0.12) ($y+0.72) (-$d/2+0.08)
			Add-Box $L "Glass" ($w*0.08) $y (-$d/2-0.06) ($w*0.30) ($y+0.72) (-$d/2+0.08)
			if($profile.kind -ne "detached"){
				Add-Box $L "Concrete" (-$w*0.42) ($y-0.13) (-$d/2-0.28) ($w*0.42) ($y+0.04) (-$d/2-0.06)
				Add-Box $L "Glass" (-$w/2-0.07) $y (-$d*0.22) (-$w/2+0.07) ($y+0.72) ($d*0.22)
			}
		}
		Add-Box $L "Green" ($w/2+0.2) 0.0 (-$d/2+0.5) ($w/2+1.2) 1.0 ($d/2-0.7)
	}
	Write-TextFile (Join-Path $AssetRoot "residential\$stem.mtl") (New-Mtl $materials)
	Write-TextFile (Join-Path $AssetRoot "residential\$stem.obj") $obj
	$setback = [double]$profile.setback
	Write-TextFile (Join-Path $AssetRoot "residential\$stem.toml") "# Procedural low-poly residential model generated by scripts/generate_procedural_building_assets.ps1`r`n# Uses only programmatically generated geometry; no external asset license required.`r`nsetback_from_road_m = $setback`r`nscale = 1.0`r`n"
}

. "$PSScriptRoot\generate_commercial_building_assets.ps1" -AssetDir (Join-Path $AssetRoot "commercial")
Write-Host "Generated procedural residential and commercial building assets under $AssetRoot"