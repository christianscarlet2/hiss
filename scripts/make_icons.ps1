# Generates custom .ico files (snake / barbell / illuminati-eye / rubber-duck) for
# Hiss, trainer, Vision and the DeveloperToolbar. Programmer-art, drawn with GDI+.
Add-Type -AssemblyName System.Drawing

function New-Canvas([int]$size) {
  $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = 'AntiAlias'
  $g.Clear([System.Drawing.Color]::Transparent)
  # Caller draws in a 0..256 coordinate space; scale to actual size.
  $g.ScaleTransform($size / 256.0, $size / 256.0)
  return @{ bmp = $bmp; g = $g }
}

function Draw-Snake($g) {
  $green     = [System.Drawing.Color]::FromArgb(255, 46, 196, 64)
  $darkgreen = [System.Drawing.Color]::FromArgb(255, 22, 110, 40)
  $pts = @(
    (New-Object System.Drawing.PointF(64,214)),
    (New-Object System.Drawing.PointF(150,176)),
    (New-Object System.Drawing.PointF(78,120)),
    (New-Object System.Drawing.PointF(150,78)),
    (New-Object System.Drawing.PointF(120,40)))
  $penOut = New-Object System.Drawing.Pen($darkgreen, 46); $penOut.StartCap='Round'; $penOut.EndCap='Round'; $penOut.LineJoin='Round'
  $penIn  = New-Object System.Drawing.Pen($green, 34);     $penIn.StartCap='Round';  $penIn.EndCap='Round';  $penIn.LineJoin='Round'
  $g.DrawCurve($penOut, $pts, 0.6)
  $g.DrawCurve($penIn,  $pts, 0.6)
  # Head
  $headBrush = New-Object System.Drawing.SolidBrush($green)
  $g.FillEllipse($headBrush, 96, 18, 56, 46)
  # Eye
  $g.FillEllipse((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)), 132, 28, 14, 14)
  $g.FillEllipse((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::Black)), 137, 32, 6, 6)
  # Forked red tongue
  $tongue = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255,220,30,30), 4); $tongue.StartCap='Round'; $tongue.EndCap='Round'
  $g.DrawLine($tongue, 120, 22, 120, 4)
  $g.DrawLine($tongue, 120, 8, 114, 0)
  $g.DrawLine($tongue, 120, 8, 126, 0)
}

function Draw-Barbell($g) {
  $bar   = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,205,208,212))
  $plate = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,40,42,48))
  $grip  = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,150,153,158))
  # Bar
  $g.FillRectangle($bar, 44, 116, 168, 24)
  # Inner plates (tall)
  $g.FillRectangle($plate, 56, 74, 30, 108)
  $g.FillRectangle($plate, 170, 74, 30, 108)
  # Outer plates (shorter)
  $g.FillRectangle($plate, 30, 92, 24, 72)
  $g.FillRectangle($plate, 202, 92, 24, 72)
  # End caps
  $g.FillRectangle($grip, 20, 110, 12, 36)
  $g.FillRectangle($grip, 224, 110, 12, 36)
}

function Draw-Eye($g) {
  $gold = [System.Drawing.Color]::FromArgb(255,214,175,55)
  $pen  = New-Object System.Drawing.Pen($gold, 12); $pen.LineJoin='Round'
  # Triangle
  $tri = @(
    (New-Object System.Drawing.PointF(128,34)),
    (New-Object System.Drawing.PointF(30,214)),
    (New-Object System.Drawing.PointF(226,214)))
  $g.FillPolygon((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(235,18,20,28))), $tri)
  $g.DrawPolygon($pen, $tri)
  # All-seeing eye
  $g.FillEllipse((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)), 86, 132, 84, 48)
  $g.FillEllipse((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,40,110,200))), 112, 134, 32, 32)
  $g.FillEllipse((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::Black)), 122, 144, 14, 14)
  # Rays from apex
  $ray = New-Object System.Drawing.Pen($gold, 4)
  for ($a = 0; $a -lt 7; $a++) {
    $ang = (-90 + ($a - 3) * 16) * [Math]::PI / 180.0
    $g.DrawLine($ray, 128, 86, [float](128 + 40*[Math]::Cos($ang)), [float](120 + 40*[Math]::Sin($ang)))
  }
}

function Draw-Duck($g) {
  $yellow = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,250,205,30))
  $orange = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,242,140,20))
  # Body
  $g.FillEllipse($yellow, 48, 138, 150, 84)
  # Tail
  $g.FillPolygon($yellow, @(
    (New-Object System.Drawing.PointF(56,150)),
    (New-Object System.Drawing.PointF(20,128)),
    (New-Object System.Drawing.PointF(64,182))))
  # Head
  $g.FillEllipse($yellow, 130, 70, 86, 86)
  # Beak
  $g.FillPolygon($orange, @(
    (New-Object System.Drawing.PointF(204,104)),
    (New-Object System.Drawing.PointF(250,116)),
    (New-Object System.Drawing.PointF(204,130))))
  # Eye
  $g.FillEllipse((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::Black)), 178, 96, 16, 16)
  $g.FillEllipse((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)), 182, 99, 6, 6)
  # Water ripple
  $rip = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255,90,170,225), 6); $rip.StartCap='Round'; $rip.EndCap='Round'
  $g.DrawLine($rip, 40, 224, 210, 224)
}

function Save-Ico($drawFn, $path) {
  $sizes = @(256, 48, 32, 16)
  $pngs = @()
  foreach ($s in $sizes) {
    $c = New-Canvas $s
    & $drawFn $c.g
    $c.g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $c.bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $c.bmp.Dispose()
    $pngs += ,($ms.ToArray())
  }
  $out = New-Object System.IO.MemoryStream
  $bw = New-Object System.IO.BinaryWriter($out)
  $bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)  # ICONDIR
  $offset = 6 + 16 * $sizes.Count
  for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $pngs[$i].Length
    $bw.Write([Byte]([Math]::Min($s,256) -band 0xFF))   # width (0 => 256)
    $bw.Write([Byte]([Math]::Min($s,256) -band 0xFF))   # height
    $bw.Write([Byte]0); $bw.Write([Byte]0)              # colors, reserved
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)         # planes, bitcount
    $bw.Write([UInt32]$len); $bw.Write([UInt32]$offset) # size, offset
    $offset += $len
  }
  foreach ($p in $pngs) { $bw.Write($p) }
  $bw.Flush()
  [System.IO.File]::WriteAllBytes($path, $out.ToArray())
  Write-Output ("wrote {0} ({1} bytes)" -f $path, $out.Length)
}

$root = "C:\www\openholdembot_old"
Save-Ico ${function:Draw-Snake}   "$root\Hiss\res\OpenHoldem.ico"
Save-Ico ${function:Draw-Barbell} "$root\trainer\res\trainer.ico"
Save-Ico ${function:Draw-Eye}     "$root\Vision\res\OpenScrape.ico"
if (-not (Test-Path "$root\DeveloperToolbar\res")) { New-Item -ItemType Directory "$root\DeveloperToolbar\res" | Out-Null }
Save-Ico ${function:Draw-Duck}    "$root\DeveloperToolbar\res\devtoolbar.ico"
