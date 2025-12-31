## Star map for the northern celestial hemisphere, magnitude 1–6
- approximately ~4,000 stars
- azimuthal equidistant, North Celestial Pole centered projection
- Symbols:
  * Mag ≤ 2: 4 px square
  * Mag 3–4: 3 px square
  * Mag 5: 2 px square
  * Mag 6: single-pixel dot

You can use https://splitter.imageonline.co/ to split images if you do not have [imagemagick](https://imagemagick.org) installed where you could most likely do just this:

```
convert starmap_input_file.png \
  -resize 640x640! \
  -threshold 50% \
  -crop 128x64 \
  +repage \
  -set filename:tile "%02d" \
  +adjoin \
  "%[filename:tile].png"
```

The following PowerShell-script convert all PNGs into BMPs:
```
Add-Type -AssemblyName System.Drawing; Get-ChildItem *.png | ForEach-Object { $b=[System.Drawing.Bitmap]::FromFile($_); $m=New-Object System.Drawing.Bitmap $b.Width,$b.Height,[System.Drawing.Imaging.PixelFormat]::Format1bppIndexed; $g=[System.Drawing.Graphics]::FromImage($m); $g.DrawImage($b,0,0); $m.Save("$($_.BaseName).bmp",[System.Drawing.Imaging.ImageFormat]::Bmp); $g.Dispose(); $b.Dispose(); $m.Dispose() }
```

