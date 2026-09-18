# Exercise the optional blur/settings branches without changing example defaults.
ImportRuby("../../examples/lib/preview.avs.rb")
ColorBars(width=720, height=480, pixel_type="YV12").Trim(0, 31)
ExamplePreview(preset="smooth", show_settings=true)
