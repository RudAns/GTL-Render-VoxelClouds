# Build -> (optionally) rebuild assets -> run the scripted self-test. Usage:
#   powershell -File Tools/iterate.ps1 [-Assets] [-NoBuild] [-NoRun] [-ResX 1600] [-ResY 900]
param([switch]$Assets, [switch]$NoBuild, [switch]$NoRun, [int]$ResX = 1600, [int]$ResY = 900, [string]$Extra = "")
$ErrorActionPreference = "Continue"
$p = Split-Path -Parent $PSScriptRoot
$ue = Join-Path (& "$PSScriptRoot\find_ue.ps1") "Engine"
$proj = "$p\VoxelClouds.uproject"
$editor = "$ue\Binaries\Win64\UnrealEditor.exe"

if (-not $NoBuild) {
    $build = & "$ue\Build\BatchFiles\Build.bat" VoxelCloudsEditor Win64 Development -Project="$proj" -WaitMutex 2>&1
    $build | Select-String -Pattern "error C|error LNK|error :|Result:" | ForEach-Object { $_.Line }
    if (-not ($build | Select-String -Pattern "Result: Succeeded")) { "BUILD FAILED - stopping"; exit 1 }
}
if ($Assets) {
    Get-ChildItem "$p\Content" -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.IsReadOnly } | ForEach-Object { $_.IsReadOnly = $false }
    & "$ue\Binaries\Win64\UnrealEditor-Cmd.exe" "$proj" -run=pythonscript -script="$p\Tools\build_assets.py" -unattended -nop4 -nosplash -stdout 2>&1 |
        Select-String -Pattern "build_assets|LogPython: Error|Error:|error X|Failed to compile|Traceback" | ForEach-Object { $_.Line }
}
if ($NoRun) { exit 0 }
$out = "$p\Saved\Capture"
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem $out -Filter "*.png" | Remove-Item -Force
$argList = @("`"$proj`"", "/Game/Maps/Skies", "-game", "-windowed", "-ResX=$ResX", "-ResY=$ResY", "-CLCapture=$out", "-RenderOffScreen", "-ForceRes",
          "-ExecCmds=`"r.SetRes ${ResX}x${ResY}w`"", "-unattended", "-nosound", "-nosplash", "-log=capture.log")
if ($Extra) { $argList += $Extra.Split(" ") }
$proc = Start-Process -FilePath $editor -ArgumentList $argList -PassThru
if (-not $proc.WaitForExit(900000)) { $proc.Kill(); "capture timeout" }
Select-String -Path "$p\Saved\Logs\capture.log" -Pattern "CLTEST|Error: |error X|Failed to compile|Ensure condition|Fatal|Assertion" | Select-Object -First 80 |
    ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '' }
Get-ChildItem $out -Filter *.png | ForEach-Object { $_.Name }
