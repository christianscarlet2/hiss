# Renders the dark "satanic" card-back (inverted pentagram + two intertwined green
# snakes) as a PNG, so the React table view doesn't depend on inline-SVG support.
Add-Type -AssemblyName System.Drawing

$W = 156; $H = 312                      # 1:2, 6x the on-screen 26x52 card
$bmp = New-Object System.Drawing.Bitmap($W, $H, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'
$g.Clear([System.Drawing.Color]::Transparent)
$g.ScaleTransform($W / 40.0, $H / 80.0)   # draw in a 40x80 logical space

function RoundRectPath($x, $y, $w, $h, $r) {
  $p = New-Object System.Drawing.Drawing2D.GraphicsPath
  $d = $r * 2
  $p.AddArc($x, $y, $d, $d, 180, 90)
  $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
  $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
  $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
  $p.CloseFigure()
  return $p
}

# Classic red playing-card back: white card edge, red field, white border frame.
$card = RoundRectPath 0.6 0.6 38.8 78.8 3
$g.FillPath((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)), $card)   # white edge
$panel = RoundRectPath 3 3 34 74 2.5
$g.FillPath((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,192,20,43))), $panel)  # red field
$whiteFrame = New-Object System.Drawing.Pen([System.Drawing.Color]::White, 1.3)              # white border line
$whiteFrame.LineJoin = 'Round'
$g.DrawPath($whiteFrame, $panel)

# Two intertwined green snakes.
$snake = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255,53,196,58), 2.6)
$snake.StartCap = 'Round'; $snake.EndCap = 'Round'; $snake.LineJoin = 'Round'
$p1 = New-Object System.Drawing.Drawing2D.GraphicsPath
$p1.AddBezier(13,11, 27,20, 5,33, 20,42); $p1.AddBezier(20,42, 33,50, 11,63, 22,71)
$p2 = New-Object System.Drawing.Drawing2D.GraphicsPath
$p2.AddBezier(27,11, 13,20, 35,33, 20,42); $p2.AddBezier(20,42, 7,50, 29,63, 18,71)
$g.DrawPath($snake, $p1)
$g.DrawPath($snake, $p2)

# Heads + red eyes + tongues.
$greenB = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,53,196,58))
$redB   = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,255,32,32))
$g.FillEllipse($greenB, 9.8, 6.8, 6.4, 6.4)
$g.FillEllipse($greenB, 23.8, 6.8, 6.4, 6.4)
$g.FillEllipse($redB, 11.1, 8.4, 1.8, 1.8)
$g.FillEllipse($redB, 27.1, 8.4, 1.8, 1.8)
$tongue = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255,255,32,32), 1.0)
$tongue.StartCap = 'Round'; $tongue.EndCap = 'Round'
$g.DrawLine($tongue, 13, 7, 13, 3)
$g.DrawLine($tongue, 27, 7, 27, 3)

$g.Dispose()
$out = "C:\www\openholdembot_old\laravel-react-table-display\public\assets\cardback.png"
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output ("wrote {0}" -f $out)
