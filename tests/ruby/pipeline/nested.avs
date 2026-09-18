ColorBars(width=720, height=480, pixel_type="YV12").Trim(0, 19)
ImportScript("nested.avs.rb")
Assert(FrameCount() == 12)
return last
