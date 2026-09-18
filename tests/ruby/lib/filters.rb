class GarnetResizer
  def self.run(clip, width: 360, height: 240, enabled: true, amount: 1)
    raise 'false keyword lost' unless enabled == false
    raise 'zero keyword lost' unless amount == 0
    clip.BilinearResize(width, height)
  end
end
AVS.filter :GarnetResize,
  args: {clip: :clip},
  options: {width: :int, height: :int, enabled: :bool, amount: :int} do |clip, **options|
  GarnetResizer.run(clip, **options)
end
factor = 2
AVS.export(:GarnetTwice, 'i') { |x| AVS.Round(x * factor) }
AVS.filter(:GarnetCall, args: {callback: :func, value: :int}) { |callback, value| callback.call(value) }
