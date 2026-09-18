require_relative 'lib/filters'
[:function, :Function].each do |name|
  rejected = false
  begin
    AVS.function(options: {name => :int}) { |**values| values[name] }
  rescue RuntimeError => error
    raise unless error.to_s.include?('Reserved AVS function parameter')
    rejected = true
  end
  raise 'Reserved function parameter accepted' unless rejected
end
factor = 2
rubyfn = AVS.function(args: {x: :int}, options: {offset: :int}, returns: :int) do |x, offset: 0|
  x * factor + offset
end
raise 'Ruby-created function' unless rubyfn.call(20, OFFSET: 2) == 42
AVS[:ruby_created_lambda] = rubyfn
AVS[:ruby_zero_lambda] = AVS.function(returns: :int) { 42 }
echo = AVS.function(options: {value: :any}) { |value: nil| value }
raise 'nested function argument array' unless echo.call(value: [[1, 2], [3, 4]]) == [[1, 2], [3, 4]]
raise 'undefined optional argument' unless echo.call.nil?
raise 'false optional argument' unless echo.call(value: false) == false
GC.start
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
