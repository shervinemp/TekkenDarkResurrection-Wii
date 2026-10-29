param(
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string[]]$Files,
    [int]$Cols = 4,
    [int]$W = 380
)
# Contact sheet of screenshots (from shots.ps1), each labelled with its folder and name; keeps the 3D area
# (crops the window's title bar).
Add-Type -AssemblyName System.Drawing
$imgs = @()
foreach ($f in $Files) {
    if (-not (Test-Path $f)) { continue }
    $b = New-Object System.Drawing.Bitmap $f
    $imgs += [pscustomobject]@{ Name = ((Split-Path (Split-Path $f) -Leaf) + '/' + (Split-Path $f -Leaf)); Bmp = $b }
}
if (-not $imgs.Count) { throw 'no images' }
$h = [int]($W * 0.62)
$rows = [math]::Ceiling($imgs.Count / $Cols)
$sheet = New-Object System.Drawing.Bitmap ($Cols * $W), ($rows * ($h + 16))
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.Clear([System.Drawing.Color]::Black)
$font = New-Object System.Drawing.Font('Consolas', 9)
for ($i = 0; $i -lt $imgs.Count; $i++) {
    $x = ($i % $Cols) * $W; $y = [math]::Floor($i / $Cols) * ($h + 16)
    $src = $imgs[$i].Bmp
    $crop = New-Object System.Drawing.Rectangle 0, 40, $src.Width, ($src.Height - 40)
    $g.DrawImage($src, (New-Object System.Drawing.Rectangle $x, ($y + 16), $W, $h), $crop, [System.Drawing.GraphicsUnit]::Pixel)
    $g.DrawString($imgs[$i].Name, $font, [System.Drawing.Brushes]::Yellow, $x + 2, $y + 1)
}
$g.Dispose()
$sheet.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
foreach ($i in $imgs) { $i.Bmp.Dispose() }
$sheet.Dispose()
"saved $Out ($($imgs.Count) images)"
