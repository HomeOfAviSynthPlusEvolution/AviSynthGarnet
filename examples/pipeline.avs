# Load Garnet.dll first; see resize.avs.
# ImportScript takes an input clip and RUNS the transformation on every call.
# Unlike ImportRuby/require_relative, it does not cache the returned graph.
source = ColorBars(width=720, height=480, pixel_type="YV12").Trim(0, 31)
ImportScript(source, "stages/window.avs.rb")
