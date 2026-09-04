param(
	[string]$AssetDir = "App\assets\buildings\commercial"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-TextFile {
	param([string]$Path, [string]$Content)
	$full = Join-Path (Get-Location) $Path
	[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($full)) | Out-Null
	[System.IO.File]::WriteAllText($full, $Content, $Utf8NoBom)
}

function Add-Box {
	param(
		[System.Collections.Generic.List[string]]$Lines,
		[string]$Material,
		[double]$X0, [double]$Y0, [double]$Z0,
		[double]$X1, [double]$Y1, [double]$Z1
	)
	$i = $script:VertexIndex
	$vertices = @(
		@($X0, $Y0, $Z0), @($X1, $Y0, $Z0), @($X1, $Y0, $Z1), @($X0, $Y0, $Z1),
		@($X0, $Y1, $Z0), @($X1, $Y1, $Z0), @($X1, $Y1, $Z1), @($X0, $Y1, $Z1)
	)
	foreach ($v in $vertices) {
		$Lines.Add(("v {0:0.###} {1:0.###} {2:0.###}" -f $v[0], $v[1], $v[2]))
	}
	$Lines.Add("usemtl $Material")
	$faces = @(
		@($i, ($i + 2), ($i + 1)), @($i, ($i + 3), ($i + 2)),
		@(($i + 4), ($i + 5), ($i + 6)), @(($i + 4), ($i + 6), ($i + 7)),
		@($i, ($i + 1), ($i + 5)), @($i, ($i + 5), ($i + 4)),
		@(($i + 3), ($i + 7), ($i + 6)), @(($i + 3), ($i + 6), ($i + 2)),
		@($i, ($i + 4), ($i + 7)), @($i, ($i + 7), ($i + 3)),
		@(($i + 1), ($i + 2), ($i + 6)), @(($i + 1), ($i + 6), ($i + 5))
	)
	foreach ($f in $faces) {
		$Lines.Add(("f {0} {1} {2}" -f $f[0], $f[1], $f[2]))
	}
	$script:VertexIndex += 8
}

function New-Obj {
	param([string]$Stem, [array]$Boxes)
	$lines = [System.Collections.Generic.List[string]]::new()
	$lines.Add("mtllib $Stem.mtl")
	$lines.Add("o $Stem")
	$script:VertexIndex = 1
	foreach ($box in $Boxes) {
		Add-Box $lines $box.Material $box.X0 $box.Y0 $box.Z0 $box.X1 $box.Y1 $box.Z1
	}
	return ($lines -join "`r`n") + "`r`n"
}

function New-Mtl {
	param([hashtable]$Materials)
	$lines = [System.Collections.Generic.List[string]]::new()
	foreach ($name in ($Materials.Keys | Sort-Object)) {
		$c = $Materials[$name]
		$lines.Add("newmtl $name")
		$lines.Add("Ka 0.18 0.18 0.18")
		$lines.Add(("Kd {0:0.###} {1:0.###} {2:0.###}" -f $c[0], $c[1], $c[2]))
		$lines.Add(("Ks {0:0.###} {1:0.###} {2:0.###}" -f $c[3], $c[3], $c[3]))
		$lines.Add(("Ns {0:0.#}" -f $c[4]))
		$lines.Add("")
	}
	return ($lines -join "`r`n")
}

$shopMaterials = @{
	"ShopWall"    = @(0.62, 0.58, 0.50, 0.08, 18)
	"ShopGlass"   = @(0.26, 0.36, 0.39, 0.22, 48)
	"ShopAwning"  = @(0.46, 0.17, 0.13, 0.06, 14)
	"ShopRoof"    = @(0.38, 0.37, 0.35, 0.05, 10)
	"ShopShutter" = @(0.50, 0.51, 0.49, 0.12, 24)
}
$officeMaterials = @{
	"OfficeWall"  = @(0.55, 0.57, 0.56, 0.12, 28)
	"OfficeGlass" = @(0.28, 0.36, 0.40, 0.28, 64)
	"OfficeCore"  = @(0.43, 0.44, 0.42, 0.08, 20)
	"OfficeRoof"  = @(0.34, 0.35, 0.34, 0.06, 16)
}

$shopVariants = @(
	@{ Stem = "shop_001"; Boxes = @(
		@{ Material = "ShopWall"; X0 = -3.8; Y0 = 0.0; Z0 = -3.4; X1 = 3.8; Y1 = 3.8; Z1 = 3.4 },
		@{ Material = "ShopGlass"; X0 = -3.2; Y0 = 0.5; Z0 = -3.55; X1 = 1.4; Y1 = 2.3; Z1 = -3.35 },
		@{ Material = "ShopShutter"; X0 = 1.8; Y0 = 0.5; Z0 = -3.56; X1 = 3.2; Y1 = 2.5; Z1 = -3.34 },
		@{ Material = "ShopAwning"; X0 = -3.5; Y0 = 2.45; Z0 = -3.95; X1 = 3.5; Y1 = 2.85; Z1 = -3.35 },
		@{ Material = "ShopRoof"; X0 = -4.0; Y0 = 3.8; Z0 = -3.6; X1 = 4.0; Y1 = 4.15; Z1 = 3.6 }
	) },
	@{ Stem = "shop_002"; Boxes = @(
		@{ Material = "ShopWall"; X0 = -3.3; Y0 = 0.0; Z0 = -4.0; X1 = 3.3; Y1 = 4.2; Z1 = 4.0 },
		@{ Material = "ShopGlass"; X0 = -2.7; Y0 = 0.45; Z0 = -4.18; X1 = -0.2; Y1 = 2.4; Z1 = -3.92 },
		@{ Material = "ShopGlass"; X0 = 0.5; Y0 = 0.45; Z0 = -4.18; X1 = 2.7; Y1 = 2.4; Z1 = -3.92 },
		@{ Material = "ShopAwning"; X0 = -3.1; Y0 = 2.7; Z0 = -4.45; X1 = 3.1; Y1 = 3.05; Z1 = -3.95 },
		@{ Material = "ShopRoof"; X0 = -3.5; Y0 = 4.2; Z0 = -4.2; X1 = 3.5; Y1 = 4.55; Z1 = 4.2 }
	) },
	@{ Stem = "shop_003"; Boxes = @(
		@{ Material = "ShopWall"; X0 = -4.0; Y0 = 0.0; Z0 = -3.0; X1 = 4.0; Y1 = 3.6; Z1 = 3.0 },
		@{ Material = "ShopGlass"; X0 = -3.4; Y0 = 0.45; Z0 = -3.22; X1 = 3.4; Y1 = 2.2; Z1 = -2.94 },
		@{ Material = "ShopAwning"; X0 = -3.8; Y0 = 2.45; Z0 = -3.55; X1 = 3.8; Y1 = 2.8; Z1 = -2.95 },
		@{ Material = "ShopShutter"; X0 = -4.12; Y0 = 0.7; Z0 = -1.9; X1 = -3.88; Y1 = 2.3; Z1 = 1.7 },
		@{ Material = "ShopRoof"; X0 = -4.2; Y0 = 3.6; Z0 = -3.2; X1 = 4.2; Y1 = 3.95; Z1 = 3.2 }
	) }
)

$officeVariants = @(
	@{ Stem = "office_001"; Boxes = @(
		@{ Material = "OfficeWall"; X0 = -4.8; Y0 = 0.0; Z0 = -4.6; X1 = 4.8; Y1 = 15.5; Z1 = 4.6 },
		@{ Material = "OfficeGlass"; X0 = -4.95; Y0 = 2.0; Z0 = -4.78; X1 = 4.95; Y1 = 3.1; Z1 = -4.48 },
		@{ Material = "OfficeGlass"; X0 = -4.95; Y0 = 5.2; Z0 = -4.78; X1 = 4.95; Y1 = 6.3; Z1 = -4.48 },
		@{ Material = "OfficeGlass"; X0 = -4.95; Y0 = 8.4; Z0 = -4.78; X1 = 4.95; Y1 = 9.5; Z1 = -4.48 },
		@{ Material = "OfficeGlass"; X0 = -4.95; Y0 = 11.6; Z0 = -4.78; X1 = 4.95; Y1 = 12.7; Z1 = -4.48 },
		@{ Material = "OfficeRoof"; X0 = -5.1; Y0 = 15.5; Z0 = -4.9; X1 = 5.1; Y1 = 16.0; Z1 = 4.9 }
	) },
	@{ Stem = "office_002"; Boxes = @(
		@{ Material = "OfficeCore"; X0 = -4.2; Y0 = 0.0; Z0 = -5.0; X1 = 4.2; Y1 = 18.0; Z1 = 5.0 },
		@{ Material = "OfficeWall"; X0 = -5.2; Y0 = 0.0; Z0 = -3.8; X1 = -4.2; Y1 = 14.5; Z1 = 3.8 },
		@{ Material = "OfficeGlass"; X0 = -3.4; Y0 = 2.0; Z0 = -5.18; X1 = 3.4; Y1 = 3.0; Z1 = -4.88 },
		@{ Material = "OfficeGlass"; X0 = -3.4; Y0 = 5.0; Z0 = -5.18; X1 = 3.4; Y1 = 6.0; Z1 = -4.88 },
		@{ Material = "OfficeGlass"; X0 = -3.4; Y0 = 8.0; Z0 = -5.18; X1 = 3.4; Y1 = 9.0; Z1 = -4.88 },
		@{ Material = "OfficeGlass"; X0 = -3.4; Y0 = 11.0; Z0 = -5.18; X1 = 3.4; Y1 = 12.0; Z1 = -4.88 },
		@{ Material = "OfficeRoof"; X0 = -5.4; Y0 = 18.0; Z0 = -5.2; X1 = 4.4; Y1 = 18.5; Z1 = 5.2 }
	) }
)

foreach ($variant in $shopVariants) {
	Write-TextFile (Join-Path $AssetDir "$($variant.Stem).mtl") (New-Mtl $shopMaterials)
	Write-TextFile (Join-Path $AssetDir "$($variant.Stem).obj") (New-Obj $variant.Stem $variant.Boxes)
	Write-TextFile (Join-Path $AssetDir "$($variant.Stem).toml") "# Road setback distance in meters for this model`r`nsetback_from_road_m = 1.8`r`n# Uniform render scale for OBJ model`r`nscale = 1.0`r`n"
}
foreach ($variant in $officeVariants) {
	Write-TextFile (Join-Path $AssetDir "$($variant.Stem).mtl") (New-Mtl $officeMaterials)
	Write-TextFile (Join-Path $AssetDir "$($variant.Stem).obj") (New-Obj $variant.Stem $variant.Boxes)
	Write-TextFile (Join-Path $AssetDir "$($variant.Stem).toml") "# Road setback distance in meters for this model`r`nsetback_from_road_m = 2.4`r`n# Uniform render scale for OBJ model`r`nscale = 1.0`r`n"
}

Write-Host "Generated commercial building OBJ/MTL/TOML assets in $AssetDir"
