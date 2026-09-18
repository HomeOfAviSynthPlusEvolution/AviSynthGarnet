# One library, two callers: library_from_avs.avs and library_from_ruby.avs.rb.
# A plain Ruby method is useful inside Ruby, but is not automatically an AVS filter.
module GarnetExamples
  def self.bicubic_resize3(clip, width, height)
    # Floating-point division matters: 1 / 3 would be zero in Ruby.
    clip.BicubicResize(width, height, 1.0 / 3.0, 1.0 / 3.0)
  end
end

# Registration is only needed at the AVS boundary. Required parameters are
# positional, in schema order; the first clip also enables clip.Filter(...).
# This name is global in AVS, not scoped by the Ruby module. Use a library prefix
# to avoid collisions. Do not register the same name again during frame rendering.
AVS.filter :ExampleBicubicResize3,
  args: {clip: :clip, width: :int, height: :int} do |clip, width, height|
  GarnetExamples.bicubic_resize3(clip, width, height)
end

# A library registers functions; it does not have to return a video clip.
nil
