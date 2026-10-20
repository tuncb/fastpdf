param([ValidateSet('Debug','Release')][string]$Configuration = 'Release', [switch]$Sanitize)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Install Visual Studio with Desktop development with C++ and the Windows SDK.' }
$env:PATH = (Split-Path -Parent $vswhere) + ';' + $env:PATH
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$build = Join-Path $repo $(if ($Sanitize) { 'build-asan' } else { 'build' })
cmake -S $repo -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Configuration" "-DFASTPDF_SANITIZE=$($Sanitize.IsPresent.ToString().ToUpper())"
if ($LASTEXITCODE) { throw 'Configuration failed' }
cmake --build $build --parallel
if ($LASTEXITCODE) { throw 'Build failed' }
ctest --test-dir $build --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed' }
