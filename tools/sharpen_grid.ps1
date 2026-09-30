# Builds the side-by-side grid from the crops dlssnr_harness --tsharpen writes.
#
# The harness writes one PNG per method, 320x180 of the reference's most
# detailed window. This puts them next to each other, blown up twice by pixel
# repetition so that edges, halos and amplified grain show as they are, each
# panel named -- which is the one thing the bench's own board builder cannot do.

param(
  [string]$Dir  = "C:\Users\Bruno\Desktop\MPCVR-DLSS5\tools\dlssnr_probe",
  [string]$Out  = "C:\Users\Bruno\Desktop\MPCVR-DLSS5\tools\dlssnr_probe",
  [int]$Zoom    = 2,
  [int]$Columns = 4
)

Add-Type -AssemblyName System.Drawing

foreach ($what in "sharpen", "sharpendiff") {
foreach ($set in 0, 1) {
  $files = Get-ChildItem -Path $Dir -Filter "${what}_${set}_*.png" | Sort-Object Name
  if (-not $files) { continue }

  $images = @()
  foreach ($f in $files) {
    # The label is what is left of the name once the set and the order are off.
    $label = $f.BaseName -replace "^${what}_${set}_\d+_", ""
    $images += [pscustomobject]@{ Label = $label; Bitmap = [System.Drawing.Image]::FromFile($f.FullName) }
  }

  $pw = $images[0].Bitmap.Width * $Zoom
  $ph = $images[0].Bitmap.Height * $Zoom
  $bar = 26
  $pad = 8
  $cols = [Math]::Min($Columns, $images.Count)
  $rows = [Math]::Ceiling($images.Count / $cols)
  $title = 34
  $W = $cols * ($pw + $pad) + $pad
  $H = $title + $rows * ($ph + $bar + $pad) + $pad

  $canvas = New-Object System.Drawing.Bitmap($W, $H)
  $g = [System.Drawing.Graphics]::FromImage($canvas)
  $g.Clear([System.Drawing.Color]::FromArgb(24, 24, 24))
  # Pixel repetition, not a smooth enlargement: the point is to see the pixels.
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
  $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half

  $fontTitle = New-Object System.Drawing.Font("Segoe UI", 13, [System.Drawing.FontStyle]::Bold)
  $fontLabel = New-Object System.Drawing.Font("Consolas", 12)
  $white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
  $grey  = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(180, 180, 180))

  $heading = if ($what -eq "sharpendiff") {
    "Sharpening, set $set -- what each one CHANGED, 24 times the difference, enlarged $($Zoom)x"
  } else {
    "Sharpening, set $set -- the same window of the same frame, enlarged $($Zoom)x by pixel repetition"
  }
  $g.DrawString($heading, $fontTitle, $white, 8, 7)

  for ($i = 0; $i -lt $images.Count; $i++) {
    $cx = $pad + ($i % $cols) * ($pw + $pad)
    $cy = $title + [Math]::Floor($i / $cols) * ($ph + $bar + $pad)
    # The reference and the unsharpened picture are the anchors: named in white,
    # the methods in grey, so the eye finds them first.
    $brush = if ($i -lt 2) { $white } else { $grey }
    $g.DrawString($images[$i].Label, $fontLabel, $brush, $cx, $cy + 4)
    $rect = New-Object System.Drawing.Rectangle($cx, ($cy + $bar), $pw, $ph)
    $g.DrawImage($images[$i].Bitmap, $rect)
  }

  $g.Dispose()
  $suffix = if ($what -eq "sharpendiff") { "_diff" } else { "" }
  $path = Join-Path $Out "sharpen_grid_$set$suffix.png"
  $canvas.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $canvas.Dispose()
  foreach ($im in $images) { $im.Bitmap.Dispose() }
  Write-Output "$path  ($W x $H, $($images.Count) panels)"
}
}
