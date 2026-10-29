param(
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string[]]$Keys
)
# Writes the key list for a `-DAUTOPILOT_KEYS -I<dir of $Out>` build (runtime/wii/frontend.c includes it as ap_keys.inc).
# Each entry of -Keys is "ms name [length_ms]": the button is held from ms after the first frontend frame, 300 ms by
# default.  Names: Start Select Up Right Down Left Triangle Circle Cross Square, joined with + for a chord (Down+Cross).
# Example: -Keys '5000 Cross','30000 Start 5000','37000 Cross','60000 Down'
$mask = @{ select = 0x0001; start = 0x0008; up = 0x0010; right = 0x0020; down = 0x0040; left = 0x0080;
           triangle = 0x1000; circle = 0x2000; cross = 0x4000; square = 0x8000 }
$rows = foreach ($k in $Keys) {
    $f = $k -split '\s+'
    if ($f.Count -lt 2) { throw "bad entry '$k'" }
    $m = 0
    foreach ($n in ($f[1] -split '\+')) {
        if (-not $mask.ContainsKey($n.ToLower())) { throw "unknown button '$n' in '$k'" }
        $m = $m -bor $mask[$n.ToLower()]
    }
    $len = if ($f.Count -ge 3) { [int]$f[2] } else { 300 }
    '{{ {0}, {1}, 0x{2:X4} }},  /* {3} */' -f [int]$f[0], $len, $m, $f[1]
}
$dir = Split-Path -Parent $Out
if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
Set-Content -Path $Out -Value $rows -Encoding ascii
"wrote $Out ($($rows.Count) rows)"
