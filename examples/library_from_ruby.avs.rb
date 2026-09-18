# Resolve relative to THIS Ruby file, not the process working directory.
# require_relative and ImportRuby share the library's per-environment load cache:
# importing this file's dependency from AVS as well does not export it twice.
require_relative 'lib/resizers.avs.rb'

source = AVS.ColorBars(width: 720, height: 480, pixel_type: 'YV12').Trim(0, 31)

# Pure Ruby-to-Ruby code can call the method directly without the export bridge:
# Calling the registered filter instead gives the same API as the AVS caller.
# Equivalent: source.ExampleBicubicResize3(360, 240)
GarnetExamples.bicubic_resize3(source, 360, 240)
