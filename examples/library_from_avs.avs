# Prerequisite for every entry point: load Garnet.dll (see resize.avs).
# ImportRuby executes this library once per environment. Its registrations
# remain available after import; its nil result is not an output clip.
ImportRuby("lib/resizers.avs.rb")

ColorBars(width=720, height=480, pixel_type="YV12").Trim(0, 31)
ExampleBicubicResize3(360, 240)
