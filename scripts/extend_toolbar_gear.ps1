# Appends ONE 16x16 gear glyph to Hiss's res\Toolbar.bmp for ID_MAIN_TOOLBAR_AUTOMATION.
#
# MFC maps toolbar buttons to glyphs POSITIONALLY: the Nth BUTTON in the TOOLBAR resource takes
# the Nth 16px cell (SEPARATORs consume no cell). So the new glyph must be appended at the END,
# matching the button being appended at the end of the resource -- inserting anywhere else would
# silently shift every existing icon by one.
#
# Idempotent: always rebuilds from the first 224px (the 14 original glyphs), so re-running does
# not keep growing the strip.
Add-Type -AssemblyName System.Drawing

$path  = "C:\www\openholdembot_old\Hiss\res\Toolbar.bmp"
$cell  = 16
$baseW = 224          # 14 existing buttons
$old   = New-Object System.Drawing.Bitmap($path)
$h     = $old.Height
$bg    = $old.GetPixel(0, 0)   # MFC treats this corner colour as transparent

$bmp = New-Object System.Drawing.Bitmap(($baseW + $cell), $h, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g   = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear($bg)
$src = New-Object System.Drawing.Rectangle(0, 0, $baseW, $h)
$g.DrawImage($old, $src, $src, [System.Drawing.GraphicsUnit]::Pixel)
$old.Dispose()

# --- gear ---------------------------------------------------------------
# Drawn with AntiAlias OFF: at 16x16 antialiasing blends glyph edges into the background
# colour, and MFC keys transparency off that exact colour -- blended pixels would survive as a
# grey halo around the icon.
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
$x  = $baseW
$cx = $x + 8.0
$cy = 8.0
$steel = [System.Drawing.Color]::FromArgb(255, 70, 78, 92)
$pen   = New-Object System.Drawing.Pen($steel, 2.0)
$brush = New-Object System.Drawing.SolidBrush($steel)

# Eight teeth as short spokes around the hub.
for ($i = 0; $i -lt 8; $i++) {
    $a  = [math]::PI * 2 * $i / 8.0
    $x1 = $cx + [math]::Cos($a) * 4.2
    $y1 = $cy + [math]::Sin($a) * 4.2
    $x2 = $cx + [math]::Cos($a) * 7.0
    $y2 = $cy + [math]::Sin($a) * 7.0
    $g.DrawLine($pen, [single]$x1, [single]$y1, [single]$x2, [single]$y2)
}
# Body, then punch the centre back to the transparent colour for the bore.
$g.FillEllipse($brush, [single]($cx - 5), [single]($cy - 5), 10, 10)
$bgBrush = New-Object System.Drawing.SolidBrush($bg)
$g.FillEllipse($bgBrush, [single]($cx - 2), [single]($cy - 2), 4, 4)

$g.Dispose()
$bmp.Save("$path.tmp", [System.Drawing.Imaging.ImageFormat]::Bmp)
$bmp.Dispose()
Move-Item "$path.tmp" $path -Force

$chk = New-Object System.Drawing.Bitmap($path)
Write-Output ("Toolbar.bmp now {0}x{1} = {2} glyph cells" -f $chk.Width, $chk.Height, [math]::Round($chk.Width / $cell))
$chk.Dispose()
