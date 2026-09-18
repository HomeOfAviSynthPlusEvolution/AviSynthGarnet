last = ColorBars(width=720, height=480, pixel_type="YV12")
try {
    ImportScript("cycle.avs.rb")
    Assert(false, "Expected circular pipeline rejection")
} catch (message) {
    Assert(FindStr(message, "Circular pipeline import") > 0, message)
}
Assert(Width(last) == 720, "Failed import leaked last")
return last.BilinearResize(360, 240)
