source = ColorBars(width=720, height=480, pixel_type="YV12").Trim(0, 31)
smooth = source.BilinearResize(360, 240)
sharp = source.PointResize(360, 240)
selector = function [smooth, sharp](clip input) {
    return current_frame % 2 == 0 ? smooth : sharp
}
smooth.ScriptClip(selector)
