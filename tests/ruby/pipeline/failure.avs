last = ColorBars(width=720, height=480, pixel_type="YV12")
try {
    ImportScript("nonclip.avs.rb")
    Assert(false, "Expected invalid pipeline result")
} catch (message) {
    Assert(FindStr(message, "Pipeline script must return a clip") > 0, message)
}
Assert(Width(last) == 720, "Failed import leaked last")
return last.BilinearResize(360, 240)
