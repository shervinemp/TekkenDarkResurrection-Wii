param([string]$Tag = 'bs', [int[]]$Shifts = @(2500, 18000), [string]$Extra = '')
# Builds the autopilot demo once, then one DOL per shift (a tiny rebuild of frontend.c each), named
# tekken6wii-<Tag><shift>.dol in the repository root (git-ignored; delete them when done).
#   -Shifts  AUTOPILOT_SHIFT_MS values: the autopilot script starts that much later, so a build plays another fight
#            (Dolphin is deterministic: one binary always plays the same fight).  2500 and 18000 are the heavy ones.
#   -Extra   more compiler flags, e.g. '-DTKDR_FINEPROF' or '-DEXP_NOCULL'.  A flag-only change does not rebuild
#            objects: delete the .o of the sources that use it first (build-audit-demo\obj\...).
. "$PSScriptRoot\env.ps1"
Set-Location $script:Root
$flags = "-DWII_OSREPORT -DTKDR_AUTOPILOT $Extra"
make TARGET=tekken6wii-audit-demo BUILD=build-audit-demo "EXTRA_CFLAGS=$flags" -j16 2>&1 | Select-String -Pattern "error|Error 1" | Select-Object -First 5
foreach ($s in $Shifts) {
    $b = "build-$Tag$s"
    if (Test-Path $b) { Remove-Item -Recurse -Force $b }
    Copy-Item build-audit-demo $b -Recurse
    Remove-Item -Force "$b/obj/runtime/wii/frontend.o" -ErrorAction SilentlyContinue
    make TARGET="tekken6wii-$Tag$s" BUILD=$b "EXTRA_CFLAGS=$flags -DAUTOPILOT_SHIFT_MS=$s" -j16 2>&1 | Select-String -Pattern "error|Error 1" | Select-Object -First 5
    Remove-Item -Recurse -Force $b
}
Get-ChildItem "tekken6wii-$Tag*.dol" | ForEach-Object { "$($_.Name) $($_.Length)" }
