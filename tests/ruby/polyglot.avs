ruby __END__
# This is pure Ruby after line 1.
clip = AVS.ColorBars(width: 720, height: 480, pixel_type: 'YV12')
clip.BilinearResize(360, 240)
