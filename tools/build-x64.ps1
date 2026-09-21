# Builds the x64 solution (or one of its projects) and prints a digest of the errors:
# one line per distinct (file, line, code), capped, so a port iteration stays readable.
#
#   tools\build-x64.ps1                          the whole solution, Release
#   tools\build-x64.ps1 -Project idlib-x64       one project
#   tools\build-x64.ps1 -Config Debug -Max 80
param(
    [string]$Project = '',
    [string]$Config = 'Release',
    [int]$Max = 60
)
$root = Split-Path -Parent $PSScriptRoot
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$target = Join-Path $root 'neo\doom-x64.sln'
if ($Project -ne '') { $target = Join-Path $root "neo\$Project.vcxproj" }
$log = Join-Path $root 'build\x64-build.log'
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
& $msbuild $target /p:Configuration=$Config /p:Platform=x64 /m /nr:false /v:minimal "/flp:LogFile=$log;Verbosity=minimal" | Out-Null
$lines = Get-Content $log
$errors = $lines | Where-Object { $_ -match ': (fatal )?error ' } |
    ForEach-Object { ($_ -replace '\s*\[C:\\.*\]$', '') -replace [regex]::Escape((Join-Path $root 'neo') + '\'), '' } | Sort-Object -Unique
$files = $errors | ForEach-Object { ($_ -split '\(')[0] } | Sort-Object -Unique
Write-Output ("{0} distinct errors in {1} files" -f @($errors).Count, @($files).Count)
$errors | Select-Object -First $Max
