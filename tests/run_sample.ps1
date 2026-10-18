param(
    [string]$Bag = (Join-Path $PSScriptRoot "..\20251111_143747_001_cam1_CL8R35300SH_gait.bag"),
    [string]$Out = (Join-Path $PSScriptRoot "..\out\sample.mkv"),
    [ValidateSet("bgra", "mjpg")][string]$Color = "bgra",
    [switch]$Keep
)

$ErrorActionPreference = "Stop"
$exe = Join-Path $PSScriptRoot "..\build\Release\bag2mkv.exe"
if (-not (Test-Path $exe)) {
    throw "build first: cmake --build build --config Release"
}
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
& $exe convert $Bag -o $Out --force --verify --color $Color
if ($LASTEXITCODE -ne 0) {
    throw "convert/verify failed with exit code $LASTEXITCODE"
}
if (-not $Keep) {
    Remove-Item $Out -Force
    Write-Host "removed $Out (use -Keep to keep it)"
}
