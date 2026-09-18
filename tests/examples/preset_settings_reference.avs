ColorBars(width=720, height=480, pixel_type="YV12").Trim(0, 31)
BilinearResize(360, 240)
Blur(0.3)
Subtitle("Preview 360x240, softness=0.3")
