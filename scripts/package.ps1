param([string]$BuildDirectory = 'build')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $repo $BuildDirectory
$cmake = Get-Content -LiteralPath (Join-Path $repo 'CMakeLists.txt') -Raw
$expectedVersion = [regex]::Match($cmake, 'project\(FastPDF VERSION ([0-9.]+)').Groups[1].Value
python (Join-Path $repo '.github\scripts\release.py') validate-tag "v$expectedVersion"
if ($LASTEXITCODE) { throw 'Release version validation failed' }
$source = Join-Path $buildPath 'fastpdf.exe'
if (!(Test-Path -LiteralPath $source)) { throw "Build output is missing: $source" }
$binaryVersion = (Get-Item -LiteralPath $source).VersionInfo.FileVersion
if ($binaryVersion -ne $expectedVersion) { throw "Binary version $binaryVersion does not match $expectedVersion; rebuild Release first." }
# Clear only the fixed release output directory, including legacy ZIP packages.
$artifactsRoot = [IO.Path]::GetFullPath((Join-Path $repo 'artifacts'))
$releaseRoot = [IO.Path]::GetFullPath((Join-Path $artifactsRoot 'release'))
if ([IO.Path]::GetDirectoryName($releaseRoot) -ne $artifactsRoot) { throw 'Invalid release output path' }
if (Test-Path -LiteralPath $releaseRoot) { Remove-Item -LiteralPath $releaseRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $releaseRoot | Out-Null
$executable = Join-Path $releaseRoot 'fastpdf.exe'
Copy-Item -LiteralPath $source -Destination $executable -Force
$hash = Get-FileHash -LiteralPath $executable -Algorithm SHA256
($hash.Hash.ToLowerInvariant() + '  fastpdf.exe') | Set-Content -LiteralPath (Join-Path $releaseRoot 'SHA256SUMS.txt') -Encoding ascii
python (Join-Path $repo '.github\scripts\release.py') verify $releaseRoot
if ($LASTEXITCODE) { throw 'Standalone executable verification failed; rebuild Release first.' }
$hash
Write-Output "Standalone application: $executable"
