# Locate an Unreal Engine 5.8 install: $env:UE_ROOT, the launcher registry entry, then <drive>:\UE_5.8 and
# <drive>:\Program Files\Epic Games\UE_5.8 on every local drive. Prints the engine root (the folder containing Engine\).
$cands = @()
if ($env:UE_ROOT) { $cands += $env:UE_ROOT }
try { $cands += (Get-ItemProperty 'HKLM:\SOFTWARE\EpicGames\Unreal Engine\5.8' -ErrorAction Stop).InstalledDirectory } catch {}
foreach ($d in (Get-PSDrive -PSProvider FileSystem).Root) {
    $cands += (Join-Path $d 'UE_5.8')
    $cands += (Join-Path $d 'Program Files\Epic Games\UE_5.8')
}
foreach ($c in $cands) {
    if ($c -and (Test-Path (Join-Path $c 'Engine\Binaries\Win64\UnrealEditor.exe'))) { Write-Output $c; exit 0 }
}
Write-Error "Unreal Engine 5.8 not found. Set the UE_ROOT environment variable to the engine folder."
exit 1
