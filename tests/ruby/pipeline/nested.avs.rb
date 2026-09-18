input = last
result = input.import_relative('subdirectory/bridge.avs')
raise 'Nested last leaked' unless last.FrameCount == input.FrameCount
raise 'Caller local changed' unless input.FrameCount == 20
result
