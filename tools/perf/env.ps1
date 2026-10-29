# Shared settings for the scripts in this folder (dot-source it: . "$PSScriptRoot\env.ps1").
# Machine-specific paths are overridable from the environment.
#   TKDR_DOLPHIN       Dolphin.exe  (a build with the portable user dir support, run with -u)
#   TKDR_DOLPHIN_USER  Dolphin's user directory; it must hold Load\WiiSD.raw with the game ISO and image.bin on it
#   TKDR_OUT           where logs and screenshots go (default: %TEMP%\tkdr-perf)
$script:Root    = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script:Dolphin = if ($env:TKDR_DOLPHIN) { $env:TKDR_DOLPHIN } else { 'C:\Users\sherv\Desktop\Projects\Ballanced\build\tools\dolphin\Dolphin.exe' }
$script:UserDir = if ($env:TKDR_DOLPHIN_USER) { $env:TKDR_DOLPHIN_USER } else { 'C:\tkdr_raw_user' }
$script:OutDir  = if ($env:TKDR_OUT) { $env:TKDR_OUT } else { Join-Path $env:TEMP 'tkdr-perf' }
New-Item -ItemType Directory -Force -Path $script:OutDir | Out-Null

# devkitPPC for make (run make from PowerShell: the msys2 make cannot create temp files from Git Bash)
$env:DEVKITPRO = 'C:/devkitPro'
$env:DEVKITPPC = 'C:/devkitPro/devkitPPC'
if ($env:PATH -notlike '*devkitPPC\bin*') {
    $env:PATH = 'C:\devkitPro\devkitPPC\bin;C:\devkitPro\tools\bin;C:\devkitPro\msys2\usr\bin;' + $env:PATH
}
