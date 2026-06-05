# Extends Vision's 4-bit (16-colour) res\Toolbar.bmp from 144x16 (9 glyphs) to
# 192x16 (12 glyphs), appending AUTO ("A"), back (left triangle) and next (right
# triangle) glyphs in the palette's black on the standard gray (192,192,192)
# background. Keeps the original 4bppIndexed format so MFC renders it correctly.
$path = "C:\www\openholdembot_old\Vision\res\Toolbar.bmp"
$b = [System.IO.File]::ReadAllBytes($path)
$off    = [BitConverter]::ToInt32($b, 10)
$width  = [BitConverter]::ToInt32($b, 18)
$height = [BitConverter]::ToInt32($b, 22)
$bits   = [BitConverter]::ToInt16($b, 28)
if ($bits -ne 4 -or $width -ne 144) { throw "expected 4bpp 144-wide, got ${bits}bpp ${width}px" }

# Palette: 16 RGBQUAD (B,G,R,0) at offset 54.
$palOff = 54
function PalIndex([int]$r,[int]$g,[int]$bl) {
  for ($i = 0; $i -lt 16; $i++) {
    $o = $palOff + $i*4
    if ($b[$o] -eq $bl -and $b[$o+1] -eq $g -and $b[$o+2] -eq $r) { return $i }
  }
  return -1
}
$bg = PalIndex 192 192 192          # standard toolbar transparent gray
$fg = PalIndex 0 0 0                # black glyph colour
if ($bg -lt 0) { $bg = 7 }
if ($fg -lt 0) { $fg = 0 }

$oldRow = ((($width*4 + 31) -band -32) / 8)      # 72
$newW = 208                                       # 13 glyphs (9 orig + AUTO/back/next/clear)
$newRow = ((($newW*4 + 31) -band -32) / 8)

# Decode original into a [y][x] index grid (file rows are bottom-up).
$grid = New-Object 'int[,]' $height, $newW
for ($r = 0; $r -lt $height; $r++) {
  $rowStart = $off + $r*$oldRow
  for ($x = 0; $x -lt $width; $x++) {
    $byte = $b[$rowStart + [int]($x/2)]
    $idx = if (($x -band 1) -eq 0) { ($byte -shr 4) -band 0xF } else { $byte -band 0xF }
    $grid[$r, $x] = $idx
  }
  for ($x = $width; $x -lt $newW; $x++) { $grid[$r, $x] = $bg }   # new cells -> bg
}

function SetPx([int]$ix,[int]$iy,[int]$idx) {       # image coords (y top-down)
  if ($iy -lt 0 -or $iy -ge $height -or $ix -lt 0 -or $ix -ge $newW) { return }
  $fileRow = $height - 1 - $iy
  $script:grid[$fileRow, $ix] = $idx
}

# AUTO "A" in cell at x=144.
$A = @(
  '.......XX.......',
  '......XXXX......',
  '......X..X......',
  '.....X....X.....',
  '.....X....X.....',
  '.....XXXXXX.....',
  '....X......X....',
  '....X......X....',
  '...X........X...',
  '...X........X...')
for ($i = 0; $i -lt $A.Count; $i++) {
  $row = $A[$i]; $iy = 3 + $i
  for ($c = 0; $c -lt $row.Length; $c++) { if ($row[$c] -eq 'X') { SetPx (144 + $c) $iy $fg } }
}

# Back (left triangle) in cell x=160, Next (right triangle) in cell x=176.
for ($iy = 3; $iy -le 13; $iy++) {
  $d = [math]::Abs($iy - 8)
  $xL = 4 + $d
  for ($rx = $xL; $rx -le 12; $rx++) { SetPx (160 + $rx) $iy $fg }   # back  (tip left)
  $xR = 12 - $d
  for ($rx = 4; $rx -le $xR; $rx++) { SetPx (176 + $rx) $iy $fg }    # next  (tip right)
}

# Clear (X) in cell x=192 -- two diagonals, 2px thick.
for ($i = 0; $i -le 8; $i++) {
  SetPx (192 + 4 + $i) (4 + $i) $fg
  SetPx (192 + 5 + $i) (4 + $i) $fg
  SetPx (192 + 12 - $i) (4 + $i) $fg
  SetPx (192 + 11 - $i) (4 + $i) $fg
}

# Re-encode pixel rows (bottom-up, 4bpp packed, padded to newRow).
$pixels = New-Object 'byte[]' ($newRow * $height)
for ($r = 0; $r -lt $height; $r++) {
  for ($x = 0; $x -lt $newW; $x += 2) {
    $x1 = $x + 1
    $hi = $grid[$r, $x] -band 0xF
    $lo = 0
    if ($x1 -lt $newW) { $lo = $grid[$r, $x1] -band 0xF }
    $pixels[$r*$newRow + [int]($x/2)] = [byte](($hi -shl 4) -bor $lo)
  }
}

# Rebuild the file: headers + palette (unchanged) then new pixel data; patch fields.
$out = New-Object 'byte[]' ($off + $pixels.Length)
[Array]::Copy($b, 0, $out, 0, $off)
[Array]::Copy($pixels, 0, $out, $off, $pixels.Length)
[Array]::Copy([BitConverter]::GetBytes([int]$out.Length), 0, $out, 2, 4)    # bfSize
[Array]::Copy([BitConverter]::GetBytes([int]$newW), 0, $out, 18, 4)         # biWidth
[Array]::Copy([BitConverter]::GetBytes([int]$pixels.Length), 0, $out, 34, 4) # biSizeImage
[System.IO.File]::WriteAllBytes($path, $out)
Write-Output ("Toolbar.bmp -> {0}x{1} 4bpp (bg idx {2}, fg idx {3})" -f $newW, $height, $bg, $fg)
