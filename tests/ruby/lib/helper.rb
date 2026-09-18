$garnet_load_count = ($garnet_load_count || 0) + 1
def garnet_answer
  5
end
$garnet_late = -> do
  require_relative 'value'
  GarnetValue
end
class GarnetLibraryMarker
  def answer
    5
  end
end
module GarnetLibraryModule
  Value = 11
end
