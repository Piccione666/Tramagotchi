$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$resources = Join-Path $root 'wl_ogv_resources'
$linesPath = Join-Path $resources 'wienerlinien-ogd-linien.csv'
$stopsPath = Join-Path $resources 'wienerlinien-ogd-haltepunkte.csv'
$pathsPath = Join-Path $resources 'wienerlinien-ogd-fahrwegverlaeufe.csv'
$dataDir = Join-Path $root 'data'
$linesOutPath = Join-Path $dataDir 'line_options.csv'
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)  # no BOM: keeps the first row clean for the firmware scan
$directionsPath = Join-Path $resources 'line_stop_directions.csv'

# Names stay UTF-8: the config page shows them as-is and its clean() turns
# umlauts into ASCII only when a row is added to the station list.
function Get-CleanText([string]$text) {
  if ($null -eq $text) { return '' }
  return $text.Trim()
}

$lineStopDirections = @{}
if (Test-Path $directionsPath) {
  Get-Content -Path $directionsPath -Encoding UTF8 | ForEach-Object {
    $parts = $_ -split '\|', 4
    if ($parts.Count -lt 4) { return }
    $lineId = $parts[0].Trim()
    $stopId = $parts[1].Trim()
    $terminal = Get-CleanText $parts[3]
    if (-not $lineId -or -not $stopId -or -not $terminal) { return }

    $key = "$lineId|$stopId"
    if (-not $lineStopDirections.ContainsKey($key)) {
      $lineStopDirections[$key] = New-Object System.Collections.Generic.List[string]
    }
    if (-not $lineStopDirections[$key].Contains($terminal)) {
      $lineStopDirections[$key].Add($terminal)
    }
  }
}

$stopNames = @{}
Import-Csv -Path $stopsPath -Delimiter ';' | ForEach-Object {
  $stopId = ($_.StopID | Out-String).Trim()
  $name = Get-CleanText $_.StopText
  $diva = ($_.DIVA | Out-String).Trim()
  if ($stopId -and $name -and $diva) {
    $stopNames[$stopId] = [PSCustomObject]@{
      Name = $name
      Diva = $diva
    }
  }
}

$lineRows = @{}
Import-Csv -Path $linesPath -Delimiter ';' | ForEach-Object {
  $lineId = ($_.LineID | Out-String).Trim()
  $lineText = Get-CleanText $_.LineText
  $sortKey = ($_.SortingHelp | Out-String).Trim()
  $realtime = ($_.Realtime | Out-String).Trim()
  if (-not $lineId -or -not $lineText) { return }
  if ($realtime -and $realtime -ne '1') { return }
  $sortValue = 99999
  if ($sortKey -match '^\d+$') {
    $sortValue = [int]$sortKey
  }
  $lineRows[$lineId] = [PSCustomObject]@{
    Id = $lineId
    Text = $lineText
    SortKey = $sortValue
  }
}

$lineStops = @{}
Import-Csv -Path $pathsPath -Delimiter ';' | ForEach-Object {
  $lineId = ($_.LineID | Out-String).Trim()
  $stopId = ($_.StopID | Out-String).Trim()
  if (-not $lineRows.ContainsKey($lineId)) { return }
  if (-not $stopNames.ContainsKey($stopId)) { return }
  if (-not $lineStops.ContainsKey($lineId)) {
    $lineStops[$lineId] = @{}
  }
  $lineStops[$lineId][$stopId] = $stopNames[$stopId]
}

$sortedLines = $lineRows.Values |
  Where-Object { $lineStops.ContainsKey($_.Id) -and $lineStops[$_.Id].Count -gt 0 } |
  Sort-Object SortKey, Text, Id

if (-not (Test-Path $dataDir)) {
  New-Item -ItemType Directory -Path $dataDir | Out-Null
}

$lineSb = New-Object System.Text.StringBuilder
foreach ($line in $sortedLines) {
  [void]$lineSb.AppendLine("$($line.Id)|$($line.Text)")
}

# Per-line stop files let the firmware load only the selected line.
$stopsDir = Join-Path $dataDir 'stops'
if (-not (Test-Path $stopsDir)) {
  New-Item -ItemType Directory -Path $stopsDir | Out-Null
} else {
  Get-ChildItem -Path $stopsDir -Filter '*.csv' | Remove-Item -Force
}

foreach ($line in $sortedLines) {
  $entries = $lineStops[$line.Id].GetEnumerator() |
    Sort-Object { $_.Value.Name }, Name
  $perLineSb = New-Object System.Text.StringBuilder
  foreach ($entry in $entries) {
    $key = "$($line.Id)|$($entry.Name)"
    $directions = ''
    if ($lineStopDirections.ContainsKey($key)) {
      $directions = ($lineStopDirections[$key] | Sort-Object) -join '~'
    }
    $row = "$($entry.Name)|$($entry.Value.Name)|$directions|$($entry.Value.Diva)"
    [void]$perLineSb.AppendLine($row)
  }
  $perLinePath = Join-Path $stopsDir "$($line.Id).csv"
  [System.IO.File]::WriteAllText($perLinePath, $perLineSb.ToString(), $utf8NoBom)
}

[System.IO.File]::WriteAllText($linesOutPath, $lineSb.ToString(), $utf8NoBom)
Write-Output "Generated $linesOutPath and $($sortedLines.Count) per-line files in $stopsDir."
