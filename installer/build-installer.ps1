# Stages the installer payload from the current build and compiles dist\FM2-Setup-<version>.exe.
param([string]$Version = "0.1.0")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$payload = Join-Path $PSScriptRoot "payload"
Remove-Item -Recurse -Force $payload -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$payload\build\tu", "$payload\scripts", "$payload\py" | Out-Null

Copy-Item "$root\build\tu\*" "$payload\build\tu" -Recurse -Force
Copy-Item "$root\play.cmd", "$root\play-cold.cmd", "$root\benchmark.cmd", "$root\install-content.cmd", "$root\TESTING.md" "$payload\scripts" -Force
Copy-Item "$root\out\release\FM2\changelog.txt" "$payload\scripts" -Force
Copy-Item "$PSScriptRoot\setup_game.py" "$payload\py" -Force
Copy-Item "$root\tools\xiso.py", "$root\tools\stfs.py" "$payload\py" -Force
Copy-Item "$root\out\pyembed\py\*" "$payload\py" -Recurse -Force

$iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
& $iscc "/DAppVersion=$Version" (Join-Path $PSScriptRoot "fm2.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with $LASTEXITCODE" }
Write-Host "Built: $PSScriptRoot\dist\FM2-Setup-$Version.exe"
