# Run via function_values.avs, which supplies the native function variable.
native_double = AVS[:example_double]
raise 'native function call failed' unless native_double.call(21) == 42

# A plain Ruby Proc/lambda is NOT implicitly an AVS function. AVS.function gives
# it an explicit argument schema and return contract for the language boundary.
# Omitted optional arguments use the block's ordinary Ruby defaults.
def make_adder(default_offset)
  AVS.function(args: {value: :int}, options: {offset: :int}, returns: :int) do |value, offset: default_offset|
    value + offset
  end
end
AVS[:example_add] = make_adder(2)

# AVS variable access copies arrays; arbitrary Ruby Hashes/objects cannot cross
# as-is. Function values remain callable, but a native roundtrip need not
# return the identical Ruby wrapper. Do not use object identity as a cache key.
raise 'roundtrip function call failed' unless AVS[:example_add].call(40) == 42

AVS.filter :ExampleApply, args: {value: :int, callback: :func} do |value, callback|
  callback.call(value)
end

AVS.ColorBars(width: 720, height: 480, pixel_type: 'YV12')
  .Trim(0, 31)
  .BilinearResize(360, 240)
