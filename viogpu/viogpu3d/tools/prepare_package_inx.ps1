param(
  [Parameter(Mandatory = $true)]
  [string]$SourcePath,

  [Parameter(Mandatory = $true)]
  [string]$DestinationPath,

  [string]$NativeLvpPath = '',
  [string]$Wow64LvpPath = '',
  [string]$Yttrium12Path = ''
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $SourcePath)) {
  throw "Missing package INX template: $SourcePath"
}

$sourceName = [System.IO.Path]::GetFileName($SourcePath)
$yttrium12Present = $sourceName -like 'viogpu3d_x64*' -and
  -not [string]::IsNullOrWhiteSpace($Yttrium12Path)
if ($yttrium12Present) {
  if (-not (Test-Path -LiteralPath $Yttrium12Path -PathType Leaf)) {
    throw "Missing x64 Yttrium12 payload: $Yttrium12Path. Build yttrium12 first and set YTTRIUM12_DLL_x64."
  }

  # Do not register an x86 build in the native DX12 slot. Its remaining DDI
  # stubs are currently only usable with the x64 calling convention.
  $payload = [System.IO.File]::ReadAllBytes($Yttrium12Path)
  if ($payload.Length -lt 64 -or [BitConverter]::ToUInt16($payload, 0) -ne 0x5a4d) {
    throw "Invalid Yttrium12 PE image: $Yttrium12Path"
  }
  $peOffset = [BitConverter]::ToInt32($payload, 60)
  if ($peOffset -lt 64 -or $peOffset -gt $payload.Length - 26 -or
      [BitConverter]::ToUInt32($payload, $peOffset) -ne 0x4550 -or
      [BitConverter]::ToUInt16($payload, $peOffset + 4) -ne 0x8664 -or
      [BitConverter]::ToUInt16($payload, $peOffset + 24) -ne 0x20b -or
      ([BitConverter]::ToUInt16($payload, $peOffset + 22) -band 0x2000) -eq 0) {
    throw "Yttrium12 payload must be an AMD64 PE32+ DLL: $Yttrium12Path"
  }
}

$nativeTokens = switch -Wildcard ($sourceName) {
  'viogpu3d_x64*'   { @('vulkan_lvp_x64.dll', 'lvp_icd.x86_64.json'); break }
  'viogpu3d_arm64*' { @('vulkan_lvp_arm64.dll', 'lvp_icd.aarch64.json'); break }
  'viogpu3d_x86*'   { @('vulkan_lvp_x86.dll', 'lvp_icd.x86.json'); break }
  default           { throw "Unknown package INX template: $sourceName" }
}

$removeTokens = @()
$nativeLvpPresent = -not [string]::IsNullOrWhiteSpace($NativeLvpPath) -and
  (Test-Path -LiteralPath $NativeLvpPath)
if (-not $nativeLvpPresent) {
  $removeTokens += $nativeTokens
}

$hasWow64Payload = $sourceName -like '*_wow64.inx'
$wow64LvpPresent = $hasWow64Payload -and
  -not [string]::IsNullOrWhiteSpace($Wow64LvpPath) -and
  (Test-Path -LiteralPath $Wow64LvpPath)
if ($hasWow64Payload -and -not $wow64LvpPresent) {
  $removeTokens += @('vulkan_lvp_x86.dll', 'lvp_icd.x86.json')
}

$lines = [System.IO.File]::ReadAllLines($SourcePath)
if (-not $yttrium12Present) {
  # Drop the optional DX12 slot without removing the DX9/DX10/DX11 entries
  # on the same registry lines. The remaining matches are file entries.
  $lines = $lines -replace ',%11%\\yttrium12\.dll\s*$', ''
  $lines = $lines -replace ',yttrium12\s*$', ''
  $removeTokens += @('yttrium12_x64.dll')
}
if ($removeTokens.Count -ne 0) {
  $pattern = ($removeTokens | ForEach-Object { [regex]::Escape($_) }) -join '|'
  $lines = @($lines | Where-Object { $_ -notmatch $pattern })
}

$destinationDir = Split-Path -Parent $DestinationPath
if (-not (Test-Path -LiteralPath $destinationDir)) {
  New-Item -ItemType Directory -Path $destinationDir | Out-Null
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllLines($DestinationPath, $lines, $utf8NoBom)

Write-Host "Prepared $DestinationPath (native Lavapipe=$nativeLvpPresent, wow64 Lavapipe=$wow64LvpPresent, Yttrium12=$yttrium12Present)"
