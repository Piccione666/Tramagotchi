$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$resources = Join-Path $root 'wl_ogv_resources'
$linesPath = Join-Path $resources 'wienerlinien-ogd-linien.csv'
$stopsPath = Join-Path $resources 'wienerlinien-ogd-haltepunkte.csv'
$pathsPath = Join-Path $resources 'wienerlinien-ogd-fahrwegverlaeufe.csv'
$dataDir = Join-Path $root 'data'
$outPath = Join-Path $resources 'line_stop_directions.csv'

# Names stay UTF-8 (the config page converts umlauts itself); only the field
# separator and runs of whitespace are normalised.
function Get-DisplayText([string]$text) {
  if ($null -eq $text) { return '' }
  $text = $text -replace '\|', ' '
  $text = $text -replace '\s+', ' '
  return $text.Trim()
}

if (-not (Test-Path $dataDir)) {
  New-Item -ItemType Directory -Path $dataDir | Out-Null
}

$stopNames = @{}
Import-Csv -Path $stopsPath -Delimiter ';' -Encoding UTF8 | ForEach-Object {
  $stopId = ($_.StopID | Out-String).Trim()
  $name = Get-DisplayText $_.StopText
  if ($stopId -and $name) {
    $stopNames[$stopId] = $name
  }
}

$lineNames = @{}
$lineSort = @{}
Import-Csv -Path $linesPath -Delimiter ';' -Encoding UTF8 | ForEach-Object {
  $lineId = ($_.LineID | Out-String).Trim()
  $lineText = Get-DisplayText $_.LineText
  $sortKey = ($_.SortingHelp | Out-String).Trim()
  $realtime = ($_.Realtime | Out-String).Trim()
  if (-not $lineId -or -not $lineText) { return }
  if ($realtime -and $realtime -ne '1') { return }
  $lineNames[$lineId] = $lineText
  $lineSort[$lineId] = if ($sortKey -match '^\d+$') { [int]$sortKey } else { 99999 }
}

$patterns = @{}      # "lineId|patternId" -> list of { StopId, Seq }
$patternMeta = @{}   # "lineId|patternId" -> { LineId, Direction }
Import-Csv -Path $pathsPath -Delimiter ';' -Encoding UTF8 | ForEach-Object {
  $lineId = ($_.LineID | Out-String).Trim()
  $patternId = ($_.PatternID | Out-String).Trim()
  $stopId = ($_.StopID | Out-String).Trim()
  $seq = ($_.StopSeqCount | Out-String).Trim()
  $direction = ($_.Direction | Out-String).Trim()
  if (-not $lineNames.ContainsKey($lineId)) { return }
  if (-not $stopNames.ContainsKey($stopId)) { return }
  if (-not ($seq -match '^\d+$')) { return }

  $key = "$lineId|$patternId"
  if (-not $patterns.ContainsKey($key)) {
    $patterns[$key] = New-Object System.Collections.Generic.List[object]
    $patternMeta[$key] = [PSCustomObject]@{ LineId = $lineId; Direction = $direction }
  }
  $patterns[$key].Add([PSCustomObject]@{
    StopId = $stopId
    Seq = [int]$seq
  })
}

# Resolve each pattern's terminus (its last stop) and length, grouped by line+direction.
$dirTerminals = @{}     # "lineId|direction" -> list of terminalStopId (one per pattern)
$dirTermMaxLen = @{}    # "lineId|direction|terminalStopId" -> longest pattern reaching it
$patternTerminal = @{}  # patternKey -> terminalStopId
foreach ($patternKey in $patterns.Keys) {
  $stops = $patterns[$patternKey] | Sort-Object Seq
  if (-not $stops -or $stops.Count -eq 0) { continue }
  $terminalStopId = $stops[-1].StopId
  if (-not $stopNames.ContainsKey($terminalStopId)) { continue }
  $patternTerminal[$patternKey] = $terminalStopId

  $meta = $patternMeta[$patternKey]
  $dirKey = "$($meta.LineId)|$($meta.Direction)"
  if (-not $dirTerminals.ContainsKey($dirKey)) {
    $dirTerminals[$dirKey] = New-Object System.Collections.Generic.List[string]
  }
  $dirTerminals[$dirKey].Add($terminalStopId)
  $lenKey = "$dirKey|$terminalStopId"
  if (-not $dirTermMaxLen.ContainsKey($lenKey) -or $stops.Count -gt $dirTermMaxLen[$lenKey]) {
    $dirTermMaxLen[$lenKey] = $stops.Count
  }
}

# The canonical terminus per direction is the end of the longest route variant; ties go
# to the most frequent terminus (the dominant destination). This collapses the many
# evening/event short-turn termini into the one real end stop per direction.
$dominant = @{}
foreach ($dirKey in $dirTerminals.Keys) {
  $freq = @{}
  foreach ($t in $dirTerminals[$dirKey]) {
    if (-not $freq.ContainsKey($t)) { $freq[$t] = 0 }
    $freq[$t]++
  }
  $best = $null; $bestLen = -1; $bestFreq = -1
  foreach ($t in $freq.Keys) {
    $len = $dirTermMaxLen["$dirKey|$t"]
    $f = $freq[$t]
    if ($len -gt $bestLen -or ($len -eq $bestLen -and $f -gt $bestFreq)) {
      $best = $t; $bestLen = $len; $bestFreq = $f
    }
  }
  $dominant[$dirKey] = $best
}

# Emit one row per (line, stop, served-direction) using that direction's dominant
# terminus, deduped — so each stop carries at most one label per direction.
$rows = @{}
foreach ($patternKey in $patterns.Keys) {
  if (-not $patternTerminal.ContainsKey($patternKey)) { continue }
  $meta = $patternMeta[$patternKey]
  $lineId = $meta.LineId
  $dirKey = "$lineId|$($meta.Direction)"
  if (-not $dominant.ContainsKey($dirKey)) { continue }
  $terminalStopId = $dominant[$dirKey]
  $lineText = $lineNames[$lineId]
  $terminal = $stopNames[$terminalStopId]
  foreach ($stop in $patterns[$patternKey]) {
    $row = "$lineId|$($stop.StopId)|$lineText|$terminal"
    if (-not $rows.ContainsKey($row)) {
      $rows[$row] = [PSCustomObject]@{
        Row = $row
        Sort = $lineSort[$lineId]
        Line = $lineText
        Stop = $stopNames[$stop.StopId]
        Terminal = $terminal
      }
    }
  }
}

$content = $rows.Values |
  Sort-Object Sort, Line, Stop, Terminal, Row |
  ForEach-Object { $_.Row }

Set-Content -Path $outPath -Encoding utf8 -Value $content
Write-Output "Generated $outPath with $($content.Count) line-stop direction rows."
