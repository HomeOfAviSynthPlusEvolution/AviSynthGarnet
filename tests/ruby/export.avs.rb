require_relative 'lib/filters'
fn = AVS[:avs_lambda]
raise 'function call' unless fn.call(21) == 42
raise 'named function call' unless AVS[:avs_named_lambda].call(VaLuE: 21) == 42
AVS[:ruby_returned_lambda] = fn
raise 'function array' unless AVS.GarnetCall(fn, 21) == 42
raise 'AVS input missing' unless AVS[:avs_input] == 17
AVS[:ruby_output] = 42
AVS.set_global_var(:garnet_global, false)
raise 'global false lost' unless AVS.get_var(:garnet_global, 123) == false
raise 'missing fallback' unless AVS.get_var(:not_a_variable, 123) == 123
raise 'function query' unless AVS.function_exists?(:GarnetResize)
array = [1, 2]
AVS[:garnet_array] = array
array[0] = 7
raise 'array not copied' unless AVS[:garnet_array][0] == 1
raise 'nested export call failed' unless AVS.GarnetTwice(21) == 42
GC.start
AVS.ColorBars(width: 720, height: 480, pixel_type: 'YV12')
  .GarnetResize(wIDTH: 720, hEIGHT: 480, Enabled: false, Amount: 0)
  .GarnetResize(width: nil, height: nil, enabled: false, amount: 0)
