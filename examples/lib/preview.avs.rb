# A miniature example of large-library organization: preset resolution, input
# checks, a native filter pipeline, and readable settings output. This is NOT
# QTGMC, LSFmod, or an approximation of their video-processing algorithms.
module GarnetExamples
  class Preview
    PRESETS = {
      'draft' => {width: 360, height: 240, softness: 0.0},
      'smooth' => {width: 360, height: 240, softness: 0.3}
    }

    def initialize(clip, preset: 'draft', show_settings: false, **overrides)
      defaults = PRESETS[preset]
      raise ArgumentError, "unknown preview preset: #{preset}" unless defaults

      # merge returns a new Hash: never mutate shared preset defaults per call.
      @settings = defaults.merge(overrides)
      @clip = clip
      @show_settings = show_settings
      width, height = @settings[:width], @settings[:height]
      unless width > 0 && height > 0 && width % 2 == 0 && height % 2 == 0
        raise ArgumentError, 'this YUV420 example needs positive even dimensions'
      end
      unless @settings[:softness] >= 0.0 && @settings[:softness] <= 1.0
        raise ArgumentError, 'softness must be between 0.0 and 1.0'
      end
    end

    def render
      # These calls create native nodes ONCE during script loading.
      output = @clip.BilinearResize(@settings[:width], @settings[:height])
      output = output.Blur(@settings[:softness]) if @settings[:softness] > 0.0
      if @show_settings
        # Ordinary interpolation replaces Eval/string-built script source.
        text = "Preview #{@settings[:width]}x#{@settings[:height]}, softness=#{@settings[:softness]}"
        output = output.Subtitle(text)
      end
      output
    end
  end
end

# Keep the language boundary thin. Both AVS and Ruby callers can use this filter;
# Ruby-only callers may also instantiate Preview directly without registration.
# Optional keys arrive using the DECLARED spelling, so lowercase Ruby keywords
# are convenient even when the AVS caller spells them differently.
AVS.filter :ExamplePreview,
  args: {clip: :clip},
  options: {preset: :string, width: :int, height: :int, softness: :float, show_settings: :bool} do |clip, **options|
  GarnetExamples::Preview.new(clip, **options).render
end
nil
