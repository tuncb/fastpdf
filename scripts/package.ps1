param([string]$BuildDirectory = 'build')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $repo $BuildDirectory
$stage = Join-Path $repo 'artifacts\release\FastPDF-x64'
New-Item -ItemType Directory -Force -Path $stage | Out-Null
foreach ($name in @('fastpdf.exe','pdfprobe.exe')) {
    $source = Join-Path $buildPath $name
    if (!(Test-Path -LiteralPath $source)) { throw "Build output is missing: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $stage $name) -Force
}
Copy-Item -LiteralPath (Join-Path $repo 'README.md') -Destination $stage -Force
$docs = Join-Path $stage 'docs'
New-Item -ItemType Directory -Force -Path $docs | Out-Null
foreach ($name in @('ARCHITECTURE.md','COMPATIBILITY.md','VALIDATION.md','validation-corpus.json','FONT-DATA-LICENSE.txt','UNICODE-LICENSE.txt')) {
    Copy-Item -LiteralPath (Join-Path $repo ('docs\'+$name)) -Destination $docs -Force
}
$zip = Join-Path $repo 'artifacts\release\FastPDF-x64.zip'
Compress-Archive -LiteralPath $stage -DestinationPath $zip -Force
$hash = Get-FileHash -LiteralPath $zip -Algorithm SHA256
($hash.Hash.ToLowerInvariant() + '  FastPDF-x64.zip') | Set-Content -LiteralPath (Join-Path $repo 'artifacts\release\SHA256SUMS.txt') -Encoding ascii
$hash
Write-Output "Portable application: $zip"
