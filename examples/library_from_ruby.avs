# Load Garnet.dll first; see resize.avs. The main script is ordinary Ruby.
output = ImportRuby("library_from_ruby.avs.rb")
# Its nested library's exports are also visible back in the outer AVS script.
Assert(FunctionExists("ExampleBicubicResize3"))
return output
