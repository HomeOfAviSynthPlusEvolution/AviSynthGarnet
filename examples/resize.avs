# START HERE: open this .avs file in your AviSynth+ application.
# Load Garnet.dll first, or put it in the host's plugin autoload directory:
# LoadPlugin("C:/path/to/Garnet.dll")
# Match the DLL architecture to the host. No separate Ruby installation is needed.
# These examples use synthetic video and built-in filters, not external plugins.
#
# Other entry points, in suggested reading order:
#   library_from_avs.avs  - a small Ruby library exported as an AVS filter
#   library_from_ruby.avs - the SAME library used from a Ruby main script
#   pipeline.avs          - AVS -> Ruby -> AVS clip transformations
#   preset_filter.avs     - presets, keyword overrides and class-based organization
#   function_values.avs   - callable values crossing the language boundary
#   frame_callback.avs    - deferred Ruby callbacks with prebuilt native branches
#
# .avs.rb is a naming convention, not a new extension your application must know.
# This one-line .avs wrapper loads a top-level Ruby script and returns its result.
ImportRuby("resize.avs.rb")
