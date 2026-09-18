pipeline_local = 17
ColorBars(width=720, height=480, pixel_type="YV12").Trim(0, 19)
ImportScript("transform.avs.rb")
Assert(FrameCount() == 12)
Assert(pipeline_local == 17)
return last
