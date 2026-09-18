outer = 37
raise 'first load' unless require_relative 'lib/helper'
raise 'duplicate load' if require_relative './lib/helper.rb'
raise 'library ran twice' unless $garnet_load_count == 1
raise 'caller locals damaged' unless outer == 37
raise 'wrong library class scope' unless GarnetLibraryMarker.new.answer == 5
raise 'wrong library module scope' unless GarnetLibraryModule::Value == 11
outer + garnet_answer
