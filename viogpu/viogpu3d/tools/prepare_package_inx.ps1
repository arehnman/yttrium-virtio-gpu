param(
  [Parameter(Mandatory = $true)]
  [string]$SourcePath,

  [Parameter(Mandatory = $true)]
  [string]$DestinationPath,

  [string]$NativeLvpPath = '',
  [string]$Wow64LvpPath = '',
  [string]$Yttrium12Path = '',
  [string]$On12Path = ''
)

$ErrorActionPreference = 'Stop'

function Read-NativeDll([string]$Path) {
  $payload = [System.IO.File]::ReadAllBytes($Path)
  if ($payload.Length -lt 64 -or [BitConverter]::ToUInt16($payload, 0) -ne 0x5a4d) {
    throw "Invalid PE image: $Path"
  }
  $peOffset = [BitConverter]::ToInt32($payload, 60)
  if ($peOffset -lt 64 -or $peOffset -gt $payload.Length - 264 -or
      [BitConverter]::ToUInt32($payload, $peOffset) -ne 0x4550 -or
      [BitConverter]::ToUInt16($payload, $peOffset + 4) -ne 0x8664 -or
      [BitConverter]::ToUInt16($payload, $peOffset + 24) -ne 0x20b -or
      ([BitConverter]::ToUInt16($payload, $peOffset + 22) -band 0x2000) -eq 0) {
    throw "Payload must be an AMD64 PE32+ DLL: $Path"
  }
  return ,$payload
}

function Get-ImportNames([byte[]]$Image) {
  $pe = [BitConverter]::ToInt32($Image, 60)
  $sectionCount = [BitConverter]::ToUInt16($Image, $pe + 6)
  $sections = $pe + 24 + [BitConverter]::ToUInt16($Image, $pe + 20)
  function Convert-Rva([uint32]$Rva) {
    for ($i = 0; $i -lt $sectionCount; $i++) {
      $s = $sections + 40 * $i
      $start = [BitConverter]::ToUInt32($Image, $s + 12)
      $size = [BitConverter]::ToUInt32($Image, $s + 16)
      if ($Rva -ge $start -and ([uint64]$Rva - $start) -lt $size) {
        $offset = [BitConverter]::ToUInt32($Image, $s + 20) + ([uint64]$Rva - $start)
        if ($offset -ge $Image.Length) { throw 'Import RVA is outside the DLL' }
        return [int]$offset
      }
    }
    throw 'Import RVA has no file backing'
  }
  # Only normal imports preload a dependency before the UMD entry point.
  $rva = [BitConverter]::ToUInt32($Image, $pe + 24 + 120)
  if (-not $rva) { return }
  $entry = Convert-Rva $rva
  while ($true) {
    $nameRva = [BitConverter]::ToUInt32($Image, $entry + 12)
    if (-not $nameRva) { break }
    $name = Convert-Rva $nameRva
    $end = $name
    while ($end -lt $Image.Length -and $Image[$end]) { $end++ }
    if ($end -eq $Image.Length) { throw 'Unterminated DLL import name' }
    [Text.Encoding]::ASCII.GetString($Image, $name, $end - $name)
    $entry += 20
  }
}

if (-not (Test-Path -LiteralPath $SourcePath)) {
  throw "Missing package INX template: $SourcePath"
}

$sourceName = [System.IO.Path]::GetFileName($SourcePath)
$isX64Package = $sourceName -like 'viogpu3d_x64*'
$yttrium12Present = $isX64Package -and
  -not [string]::IsNullOrWhiteSpace($Yttrium12Path)
$on12Present = $isX64Package -and
  -not [string]::IsNullOrWhiteSpace($On12Path)
if ($on12Present -and -not $yttrium12Present) {
  throw 'A packaged On12 translator requires the x64 Yttrium12 UMD.'
}
if ($on12Present -and -not (Test-Path -LiteralPath $On12Path -PathType Leaf)) {
  throw "Missing On12 translator payload: $On12Path"
}
if ($yttrium12Present) {
  if (-not (Test-Path -LiteralPath $Yttrium12Path -PathType Leaf)) {
    throw "Missing x64 Yttrium12 payload: $Yttrium12Path. Build yttrium12 first and set YTTRIUM12_DLL_x64."
  }

  # Do not register an x86 build in the native DX12 slot. Its remaining DDI
  # stubs are currently only usable with the x64 calling convention.
  $payload = Read-NativeDll $Yttrium12Path
  $importsOn12 = @(Get-ImportNames $payload) -contains 'd3d11on12.dll'
  if ($importsOn12 -ne $on12Present) {
    throw 'Yttrium12 on12_preload and YTTRIUM12_ON12_DLL_x64 must be enabled together. Rebuild the UMD or supply its patched translator.'
  }
}
if ($on12Present) { $null = Read-NativeDll $On12Path }

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
  $lines = $lines -replace ',%13%\\x64\\yttrium12\.dll\s*$', ''
  $lines = $lines -replace ',yttrium12\s*$', ''
  $lines = $lines -replace ', VioGpu3D_Files\.Yttrium12', ''
  $removeTokens += @('yttrium12.dll', 'VioGpu3D_Files.Yttrium12')
}
if (-not $on12Present) {
  $removeTokens += @('d3d11on12.dll')
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

Write-Host "Prepared $DestinationPath (native Lavapipe=$nativeLvpPresent, wow64 Lavapipe=$wow64LvpPresent, Yttrium12=$yttrium12Present, On12=$on12Present)"
