source = AVS.ColorBars(width: 720, height: 480, pixel_type: 'YV12').Trim(0, 7)
expected_width = source.Width
# Exercise deferred entry after import returns, including GC and native calls.
callback = AVS.function(args: {clip: :clip}, returns: :clip) do |clip|
  n = AVS.get_var(:current_frame)
  raise 'Missing runtime frame number' unless n.is_a?(Integer) && n >= 0 && n < 8
  raise 'Captured state lost' unless clip.Width == expected_width
  GC.start
  clip
end
result = source.ScriptClip(callback).BilinearResize(360, 240)
callback = nil
GC.start
result
