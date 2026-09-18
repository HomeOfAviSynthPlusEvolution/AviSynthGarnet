AVS.set_var(:pipeline_local, 17)
source = AVS.ColorBars(width: 720, height: 480, pixel_type: 'YV12').Trim(0, 19)
output = source.import_relative('transform.avs')
raise 'Wrong frame count' unless output.FrameCount == 12
raise 'Local variable leaked' unless AVS.get_var(:pipeline_local) == 17
second = source.Trim(0, 9).import_relative('transform.avs')
raise 'Pipeline result was cached' unless second.FrameCount == 6
output
