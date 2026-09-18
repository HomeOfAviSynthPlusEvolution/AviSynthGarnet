# Build alternative native graphs once, then select one at frame-request time.
# This demonstrates the structure used by conditional/adaptive script helpers,
# not a useful image-processing algorithm by itself.
def make_preview_selector(smooth, sharp)
  # Isolating the closure factory also avoids accidentally capturing a variable
  # that later stores the output ScriptClip graph (a cross-language reference cycle).
  AVS.function(args: {clip: :clip}, returns: :clip) do |_clip|
    # ScriptClip supplies the input clip and AVS current_frame at callback time.
    # Return a CLIP; AviSynth obtains the requested frame from that graph.
    n = AVS[:current_frame]
    n % 2 == 0 ? smooth : sharp
  end
end

source = AVS.ColorBars(width: 720, height: 480, pixel_type: 'YV12').Trim(0, 31)
smooth = source.BilinearResize(360, 240)
sharp = source.PointResize(360, 240)

# No new filters or registrations are created inside the callback. It may run
# after this file returns, on frame workers, more than once or out of order.
# Derive decisions from current_frame, not a shared incrementing Ruby counter.
# Both alternatives have the same format, dimensions and length.
smooth.ScriptClip(make_preview_selector(smooth, sharp)).Prefetch(2)
