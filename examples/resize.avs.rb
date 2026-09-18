# A Ruby main script needs neither a class nor AVS.filter registration.
# AVS.Name(...) calls a native filter; clip.Name(...) supplies its first clip.
# Use the native filter name (case-insensitive), not an invented snake_case name.
clip = AVS.ColorBars(width: 720, height: 480, pixel_type: 'YV12')

# The last expression becomes ImportRuby's result. This builds a native graph;
# pixel processing occurs later when the host requests frames.
# Ruby expressions do not implicitly replace AVS's `last`: chain or assign them.
clip.BilinearResize(360, 240)
