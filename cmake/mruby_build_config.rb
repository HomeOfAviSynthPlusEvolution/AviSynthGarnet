# Invoked by CMake; compiler/configuration policy lives in Mruby.cmake.
require 'json'
settings = JSON.parse(File.read(ENV.fetch('GARNET_MRUBY_SETTINGS')))
MRuby::Build.new('garnet', settings.fetch('build_root')) do |conf|
  conf.toolchain(settings.fetch('msvc') ? :visualcpp : :gcc)
  # mruby interpolates commands into shell strings; paths may contain spaces.
  command = ->(key) { '"' + settings.fetch(key) + '"' }
  conf.cc.command = command.call('cc')
  conf.cxx.command = command.call('cxx')
  conf.cc.flags = [settings.fetch('cflags')]
  conf.cxx.flags = [settings.fetch('cxxflags')]
  conf.linker.command = command.call('ld')
  conf.linker.flags = [settings.fetch('ldflags')]
  conf.archiver.command = command.call('ar')
  if settings.fetch('msvc')
    # Do not force debug information in every configuration, or put PDBs into
    # the source checkout. The CMake flags select debug information and CRT.
    [conf.cc, conf.cxx].each do |compiler|
      compiler.compile_options = compiler.compile_options.sub('/Zi ', '')
      compiler.flags << '/nologo'
      compiler.flags << '/Fd"' + conf.build_dir + '/mruby.pdb"'
    end
    conf.linker.flags << '/NOLOGO'
  end
  conf.enable_cxx_exception
  conf.cc.defines << 'MRB_INT64'
  conf.cc.defines << 'MRB_WORDBOX_NO_INLINE_FLOAT'
  conf.cxx.defines << 'MRB_INT64'
  conf.cxx.defines << 'MRB_WORDBOX_NO_INLINE_FLOAT'
  if settings.fetch('pointer_size') == 4
    conf.cc.defines << 'MRB_NO_BOXING'
    conf.cxx.defines << 'MRB_NO_BOXING'
  end
  conf.gem core: 'mruby-compiler'
  # mruby 4 parses literals outside int32 as bigint, even with MRB_INT64.
  conf.gem core: 'mruby-bigint'
  conf.gem core: 'mruby-metaprog'
  conf.gem core: 'mruby-method'
  conf.gem core: 'mruby-enum-ext'
  conf.gem core: 'mruby-array-ext'
  conf.gem core: 'mruby-hash-ext'
  conf.gem core: 'mruby-bin-mrbc'
end
