require_relative 'lib/preview.avs.rb'

source = AVS.ColorBars(width: 720, height: 480, pixel_type: 'YV12').Trim(0, 31)

# A preset is just a collection of defaults. Explicit keywords override it.
# Zero and false are real values, not "missing"; nil optional keywords are
# omitted by the bridge. Set show_settings: true to inspect the resolved settings.
source.ExamplePreview(preset: 'smooth', softness: 0.0, show_settings: false)
