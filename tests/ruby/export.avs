avs_input = 17
avs_lambda = function(int value) { return value * 2 }
avs_named_lambda = function(int "value") { return Default(value, 21) * 2 }
ImportRuby("export.avs.rb")
Assert(ruby_output == 42)
Assert(GarnetTwice(21) == 42)
Assert(GarnetCall(avs_lambda, 21) == 42)
Assert(ruby_returned_lambda(21) == 42)
Assert(ruby_created_lambda(21) == 42)
Assert(ruby_created_lambda(20, Offset=2) == 42)
Assert(ruby_zero_lambda() == 42)
GarnetResize(last, width=360, HEIGHT=240, enabled=false, amount=0)
