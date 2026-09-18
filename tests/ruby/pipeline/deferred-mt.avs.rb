source = AVS.ColorBars(width: 720, height: 480, pixel_type: 'YV12').Trim(0, 63)
inner = AVS.function(args: {clip: :clip}, returns: :clip) { |clip| clip }
source = source.ScriptClip(inner)
# This independent branch is first requested from INSIDE the outer callback.
# Its Prefetch worker must enter the same Ruby VM while that callback waits.
probe = AVS.ColorBars(width: 720, height: 480, pixel_type: 'YV12').Trim(0, 63)
probe = probe.ScriptClip(inner).Prefetch(2)
kept = []
callback = AVS.function(args: {clip: :clip}, returns: :clip) do |clip|
  n = AVS[:current_frame]
  # Exercise registry growth while other workers look up existing callbacks.
  identity = AVS.function(args: {value: :int}, returns: :int) { |value| value }
  raise 'Dynamic function dispatch failed' unless identity.call(n) == n
  # Force a same-thread nested Ruby callback while the outer VM entry is held.
  raise 'Nested frame request failed' unless clip.AverageLuma > 0
  raise 'Cross-worker frame request failed' unless probe.AverageLuma > 0
  result = clip.BilinearResize(360, 240).PointResize(720, 480).propSet('garnet_n', n)
  # Keep native filters across worker invocations and until VM destruction.
  kept[n % 8] = result
  GC.start if n % 7 == 0
  result
end
source.ScriptClip(callback).PointResize(360, 240).Prefetch(2)
