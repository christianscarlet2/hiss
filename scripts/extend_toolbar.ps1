# Appends three 16x16 toolbar glyphs (AUTO toggle, back arrow, next arrow) to
# Vision's res\Toolbar.bmp, matching the existing transparent background colour.
# Regenerates from a clean 144-wide base each run (idempotent) when possible.
Add-Type -AssemblyName System.Drawing
$path = "C:\www\openholdembot_old\Vision\res\Toolbar.bmp"
$old = New-Object System.Drawing.Bitmap($path)
$cell = 16
# Use only the first 144px (9 original glyphs) so re-running doesn't keep growing.
$baseW = 144
$h = $old.Height
$bg = $old.GetPixel(0, 0)
$newW = $baseW + $cell * 3
$bmp = New-Object System.Drawing.Bitmap($newW, $h, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'
$g.Clear($bg)
$src = New-Object System.Drawing.Rectangle(0, 0, $baseW, $h)
$g.DrawImage($old, $src, $src, [System.Drawing.GraphicsUnit]::Pixel)

$greenB = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,40,170,70))
$blueB  = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,30,60,150))
$fnt    = New-Object System.Drawing.Font("Arial", 9, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
$sf     = New-Object System.Drawing.StringFormat
$sf.Alignment = [System.Drawing.StringAlignment]::Center
$sf.LineAlignment = [System.Drawing.StringAlignment]::Center

# AUTO: green square + white "A"
[int]$x = $baseW + 0 * $cell
$g.FillRectangle($greenB, ([int]($x+2)), 2, 12, 12)
$rect = New-Object System.Drawing.RectangleF([single]($x+2), [single]1, [single]12, [single]14)
$g.DrawString("A", $fnt, [System.Drawing.Brushes]::White, $rect, $sf)

# BACK: thick left triangle
[int]$x = $baseW + 1 * $cell
[System.Drawing.Point[]]$back = @(
  (New-Object System.Drawing.Point ([int]($x+4)),  ([int]8)),
  (New-Object System.Drawing.Point ([int]($x+11)), ([int]3)),
  (New-Object System.Drawing.Point ([int]($x+11)), ([int]13)))
$g.FillPolygon($blueB, $back)

# NEXT: thick right triangle
[int]$x = $baseW + 2 * $cell
[System.Drawing.Point[]]$next = @(
  (New-Object System.Drawing.Point ([int]($x+12)), ([int]8)),
  (New-Object System.Drawing.Point ([int]($x+5)),  ([int]3)),
  (New-Object System.Drawing.Point ([int]($x+5)),  ([int]13)))
$g.FillPolygon($blueB, $next)

$g.Dispose(); $old.Dispose()
$bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Bmp)
$bmp.Dispose()
Write-Output ("Toolbar.bmp now {0}x{1}" -f $newW, $h)
