$ErrorActionPreference = 'Stop'

# Downloads the current Wiener Linien OGD static data into wl_ogv_resources and
# regenerates the LittleFS files in data/. Upload them afterwards with
# `pio run -t uploadfs` (USB) or the /fsimage endpoint (OTA).

$root = Split-Path -Parent $PSScriptRoot
$resources = Join-Path $root 'wl_ogv_resources'
$baseUrl = 'https://www.wienerlinien.at/ogd_realtime/doku/ogd/'

# Expected first line per file. The site sits behind bot protection, so a
# mismatch means we got something other than the data and the old file is kept.
$files = [ordered]@{
  'wienerlinien-ogd-haltepunkte.csv'      = 'StopID;DIVA;StopText;Municipality;MunicipalityID;Longitude;Latitude'
  'wienerlinien-ogd-haltestellen.csv'     = 'DIVA;PlatformText;Municipality;MunicipalityID;Longitude;Latitude'
  'wienerlinien-ogd-linien.csv'           = 'LineID;LineText;SortingHelp;Realtime;MeansOfTransport'
  'wienerlinien-ogd-fahrwegverlaeufe.csv' = 'LineID;PatternID;StopSeqCount;StopID;Direction'
  'wienerlinien-ogd-steige.csv'           = 'StopID;Platform'
  'wienerlinien-ogd-verbindungen.csv'     = 'FromStartStop;ToEndStop;TransportMode;Length'
  'wienerlinien-echtzeitdaten-dokumentation.pdf' = '%PDF'
}

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
if (-not (Test-Path $resources)) {
  New-Item -ItemType Directory -Path $resources | Out-Null
}

foreach ($name in $files.Keys) {
  $target = Join-Path $resources $name
  $temp = "$target.download"
  Invoke-WebRequest -Uri ($baseUrl + $name) -OutFile $temp -UseBasicParsing -UserAgent 'Tramagotchi data updater'

  $firstLine = (Get-Content -Path $temp -TotalCount 1 -Encoding UTF8).TrimStart([char]0xFEFF).Trim()
  if (-not $firstLine.StartsWith($files[$name])) {
    Remove-Item -Path $temp -Force
    throw "Unexpected content for $name (first line: '$firstLine'). Kept the existing file."
  }
  Move-Item -Path $temp -Destination $target -Force
  Write-Output "Downloaded $name ($((Get-Item $target).Length) bytes)."
}

& (Join-Path $PSScriptRoot 'generate_direction_options.ps1')
& (Join-Path $PSScriptRoot 'generate_line_options.ps1')

# The device compares this with the published copy (DATA_UPDATE_URL) and only
# downloads a new image when they differ, so publish both files together.
$version = Get-Date -Format 'yyyy-MM-dd'
$versionPath = Join-Path $root 'data\version.txt'
[System.IO.File]::WriteAllText($versionPath, "$version`n", (New-Object System.Text.UTF8Encoding($false)))
Write-Output "Data version $version written to $versionPath."
