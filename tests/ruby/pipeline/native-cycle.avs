last = ColorBars(width=720, height=480, pixel_type="YV12")
function GarnetNativeLoop(clip c) {
    return ImportScript(c, "native-loop.avs")
}
try {
    ImportScript("native-loop.avs")
    Assert(false, "Expected native recursion failure")
} catch (message) {
    Assert(FindStr(message, "Circular pipeline import") > 0, message)
}
Assert(Width(last) == 720, "Native recursion leaked last")
# The native depth counter must unwind, allowing a subsequent import.
return ImportScript(last, "transform.avs")
