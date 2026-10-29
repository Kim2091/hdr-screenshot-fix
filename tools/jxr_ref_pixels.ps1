# Reads a JPEG XR with WPF, converts to Rgba128Float and writes the scRGB values of
# rows 0..($Rows-1) at a few columns to JSON: the brightest pixel, a mid pixel and fixed columns.
param([string]$Jxr, [string]$Json, [int]$Rows = 6)
Add-Type -AssemblyName PresentationCore
$fs = [IO.File]::OpenRead($Jxr)
try {
  $dec = [Windows.Media.Imaging.BitmapDecoder]::Create($fs, 'PreservePixelFormat', 'OnLoad')
  $frame = $dec.Frames[0]
  Write-Host "source format: $($frame.Format)  size: $($frame.PixelWidth)x$($frame.PixelHeight)"
  $conv = New-Object Windows.Media.Imaging.FormatConvertedBitmap($frame, [Windows.Media.PixelFormats]::Rgba128Float, $null, 0)
  $w = $conv.PixelWidth
  $buf = New-Object float[] ($w * 4 * $Rows)
  $conv.CopyPixels((New-Object Windows.Int32Rect(0, 0, $w, $Rows)), $buf, $w * 16, 0)
} finally { $fs.Close() }
$best = 0; $bestV = -1
for ($i = 0; $i -lt $w * $Rows; $i++) {
  $v = [Math]::Max($buf[$i*4], [Math]::Max($buf[$i*4+1], $buf[$i*4+2]))
  if ($v -gt $bestV) { $bestV = $v; $best = $i }
}
$picks = @($best, ($w * 2 + [int]($w / 2)), 0, ($w * ($Rows - 1) + $w - 1), ($w + 123))
$out = foreach ($i in $picks) {
  [ordered]@{ x = $i % $w; y = [int][Math]::Floor($i / $w); rgb = @([double]$buf[$i*4], [double]$buf[$i*4+1], [double]$buf[$i*4+2]) }
}
$out | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 $Json
Write-Host "brightest in first $Rows rows: $bestV ($([Math]::Round($bestV*80)) nits)"
