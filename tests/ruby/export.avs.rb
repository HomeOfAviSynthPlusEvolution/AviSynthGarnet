require_relative 'lib/filters'
raise 'nested export call failed' unless AVS.GarnetTwice(21) == 42
GC.start
AVS.ColorBars(width: 720, height: 480, pixel_type: 'YV12')
  .GarnetResize(wIDTH: 720, hEIGHT: 480, Enabled: false, Amount: 0)
  .GarnetResize(width: nil, height: nil, enabled: false, amount: 0)
