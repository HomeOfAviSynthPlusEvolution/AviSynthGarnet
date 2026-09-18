# Load Garnet.dll first; see resize.avs.
# Native anonymous functions are first-class values passed through AVS variables.
example_double = function(int value) { return value * 2 }
output = ImportRuby("function_values.avs.rb")

# The Ruby library exported a named filter accepting a function value, and placed
# a Ruby-created function in an AVS variable. Both can be called from native AVS.
Assert(ExampleApply(21, example_double) == 42)
Assert(example_add(40, offset=2) == 42)
return output
