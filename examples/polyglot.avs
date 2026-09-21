ruby __END__
# This is a single-file polyglot AviSynth+ / Ruby script for Garnet.
#
# Line 1:
#   - In AviSynth+: Evaluates `ruby()`, which executes this file in Ruby,
#     and halts AviSynth parsing at `__END__`.
#   - In Ruby: Evaluates `ruby(__END__())` as a harmless no-op.
#
# Starting from line 2, write standard Ruby code.
# In production, drop Garnet.dll into plugins64/ and AviSynth+ autoloads it
# automatically upon seeing `ruby`.

source = AVS.ColorBars(width: 720, height: 480, pixel_type: 'YV12').Trim(0, 31)
source.BilinearResize(360, 240)
