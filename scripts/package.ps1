param([string]$BuildDirectory = 'build')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $repo $BuildDirectory
$stage = Join-Path $repo 'artifacts\release\FastPDF-x64'
$cmake = Get-Content -LiteralPath (Join-Path $repo 'CMakeLists.txt') -Raw
$expectedVersion = [regex]::Match($cmake, 'project\(FastPDF VERSION ([0-9.]+)').Groups[1].Value
python (Join-Path $repo '.github\scripts\release.py') validate-tag "v$expectedVersion"
if ($LASTEXITCODE) { throw 'Release version validation failed' }
# Remove only this fixed staging directory so old files cannot enter a release.
$stage = [IO.Path]::GetFullPath($stage)
$releaseRoot = [IO.Path]::GetFullPath((Join-Path $repo 'artifacts\release'))
if ([IO.Path]::GetDirectoryName($stage) -ne $releaseRoot) { throw 'Invalid package staging path' }
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
foreach ($name in @('fastpdf.exe','pdfprobe.exe')) {
    $source = Join-Path $buildPath $name
    if (!(Test-Path -LiteralPath $source)) { throw "Build output is missing: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $stage $name) -Force
}
Copy-Item -LiteralPath (Join-Path $repo 'README.md') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $repo 'CHANGELOG.md') -Destination $stage -Force
$docs = Join-Path $stage 'docs'
New-Item -ItemType Directory -Force -Path $docs | Out-Null
foreach ($name in @('ARCHITECTURE.md','COMPATIBILITY.md','VALIDATION.md','validation-corpus.json','FONT-DATA-LICENSE.txt','UNICODE-LICENSE.txt')) {
    Copy-Item -LiteralPath (Join-Path $repo ('docs\'+$name)) -Destination $docs -Force
}
$binaryVersion = (Get-Item -LiteralPath (Join-Path $stage 'fastpdf.exe')).VersionInfo.FileVersion
if ($binaryVersion -ne $expectedVersion) { throw "Binary version $binaryVersion does not match $expectedVersion; rebuild Release first." }
python (Join-Path $repo '.github\scripts\release.py') manifest $stage
if ($LASTEXITCODE) { throw 'Package manifest generation failed' }
$zip = Join-Path $repo 'artifacts\release\FastPDF-x64.zip'
Compress-Archive -LiteralPath $stage -DestinationPath $zip -Force
$hash = Get-FileHash -LiteralPath $zip -Algorithm SHA256
($hash.Hash.ToLowerInvariant() + '  FastPDF-x64.zip') | Set-Content -LiteralPath (Join-Path $repo 'artifacts\release\SHA256SUMS.txt') -Encoding ascii
$hash
Write-Output "Portable application: $zip"
