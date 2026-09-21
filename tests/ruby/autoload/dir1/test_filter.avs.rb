AVS.filter :AutoloadTestFilter, args: {clip: :clip} do |clip|
  clip.BilinearResize(360, 240)
end
