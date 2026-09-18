last = ColorBars(width=720, height=480, pixel_type="YV12").Trim(0, 19)
pipeline_local = 17
try {
    ImportScript("native-throw.avs")
    Assert(false, "Expected native pipeline failure")
} catch (message) {
    Assert(FindStr(message, "deliberate native failure") > 0, message)
}
Assert(Width(last) == 720 && FrameCount(last) == 20, "Native failure leaked last")
Assert(pipeline_local == 17, "Native failure leaked local variables")
try {
    ImportScript("native-value.avs")
    Assert(false, "Expected non-clip native result")
} catch (message) {
    Assert(FindStr(message, "Pipeline script must return a clip") > 0, message)
}
Assert(Width(last) == 720 && pipeline_local == 17, "Invalid result leaked context")
# Native failures must not poison an otherwise untouched Ruby session.
ImportScript("transform.avs.rb")
Assert(FrameCount() == 12)
Assert(pipeline_local == 17)
return last
