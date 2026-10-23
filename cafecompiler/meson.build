cafecompiler_sources = files(
  'api.cpp',
  'cafe_compiler.cpp',
  'native_compute.cpp',
  'gfd_write.cpp',
)

# Bundle fallback libraries into the installed archive, just like Mesa's
# internal libraries. System dependencies remain consumer link dependencies.
cafeglsl_bundled_libs = []
cafeglsl_private_deps = []
cafeglsl_cmake_packages = []
if dep_zlib.found() and dep_zlib.type_name() == 'internal'
  cafeglsl_bundled_libs += subproject('zlib').get_variable('zlib')
elif dep_zlib.found()
  cafeglsl_private_deps += dep_zlib
  cafeglsl_cmake_packages += 'zlib'
endif
foreach entry : [[dep_zstd, 'libzstd'], [dep_unwind, 'libunwind'], [dep_libdrm, 'libdrm']]
  dep = entry[0]
  if dep.found()
    cafeglsl_private_deps += dep
    cafeglsl_cmake_packages += entry[1]
  endif
endforeach

cafeglsl_cmake_libs = []
foreach entry : [[dep_m, 'm'], [dep_clock, 'rt'], [dep_atomic, 'atomic'],
                 [dep_dl, 'dl'], [dep_ws2_32, 'ws2_32']]
  if entry[0].found()
    cafeglsl_private_deps += entry[0]
    cafeglsl_cmake_libs += entry[1]
  endif
endforeach

cafeglsl_private_libs = []
cafeglsl_cmake_dependencies = ''
cafeglsl_link_options = ld_args_gc_sections
if host_machine.system() == 'cafeos'
  cafeglsl_private_libs += '-lwut'
  cafeglsl_cmake_libs += 'wut'
elif host_machine.system() != 'windows'
  cafeglsl_private_deps += dep_thread
  cafeglsl_cmake_dependencies += 'find_dependency(Threads)\n'
  cafeglsl_cmake_libs += 'Threads::Threads'
endif
if host_machine.system() == 'darwin'
  cafeglsl_link_options = ['-Wl,-dead_strip']
  cafeglsl_private_libs += '-lc++'
else
  cafeglsl_private_libs += '-lstdc++'
endif
if cafeglsl_cmake_packages.length() != 0
  cafeglsl_cmake_dependencies += 'find_dependency(PkgConfig)\n'
  cafeglsl_cmake_dependencies += 'pkg_check_modules(_cafeglsl_system REQUIRED IMPORTED_TARGET ' + ' '.join(cafeglsl_cmake_packages) + ')\n'
  cafeglsl_cmake_libs += 'PkgConfig::_cafeglsl_system'
endif

libcafeglsl = static_library(
  'cafeglsl',
  cafecompiler_sources,
  include_directories : [
    inc_src,
    inc_include,
    inc_mesa,
    inc_compiler,
    inc_gallium,
    inc_gallium_aux,
    inc_gallium_drivers,
    inc_amd_common,
  ],
  link_whole : [libglsl_standalone, libr600, libgallium, cafeglsl_bundled_libs],
  dependencies : [idep_nir, idep_nir_headers, idep_mesautil],
  override_options : ['cpp_std=c++17'],
  gnu_symbol_visibility : 'hidden',
  install : true,
)

install_headers('CafeGLSLCompiler.h', 'cafe_gx2.h', 'gfd.h', subdir : 'cafeglsl')

# Version of the CafeGLSL API, as reported by GetGLSLCompilerVersion(). Separate from
# Mesa's own VERSION file.
cafeglsl_version = '0.3.0'

pkg.generate(
  name : 'cafeglsl',
  filebase : 'cafeglsl',
  description : 'GLSL to GX2 shader compiler for the Wii U (Latte) GPU, built from Mesa',
  version : cafeglsl_version,
  url : 'https://github.com/devkitPro/wut-packages',
  # Headers go to <includedir>/cafeglsl/, so consumers include <cafeglsl/CafeGLSLCompiler.h>
  # and need no extra -I beyond the portlibs include dir.
  libraries : ['-L${libdir}', '-lcafeglsl'],
  libraries_private : [cafeglsl_link_options, cafeglsl_private_libs, cafeglsl_private_deps],
)

_cafeglsl_cmake_conf = configuration_data()
_cafeglsl_cmake_conf.set('VERSION', cafeglsl_version)
_cafeglsl_cmake_conf.set('PREFIX_FROM_CONFIG', fs.relative_to(get_option('prefix'), get_option('prefix') / get_option('libdir') / 'cmake/cafeglsl'))
_cafeglsl_cmake_conf.set('INCLUDEDIR', fs.relative_to(get_option('prefix') / get_option('includedir'), get_option('prefix')))
_cafeglsl_cmake_conf.set('LIBDIR', fs.relative_to(get_option('prefix') / get_option('libdir'), get_option('prefix')))
_cafeglsl_cmake_conf.set('DEPENDENCIES', cafeglsl_cmake_dependencies)
_cafeglsl_cmake_conf.set('LIBRARIES', ';'.join(cafeglsl_cmake_libs))
_cafeglsl_cmake_conf.set('LINK_OPTIONS', ';'.join(cafeglsl_link_options))

configure_file(
  input : 'cafeglslConfig.cmake.in',
  output : 'cafeglslConfig.cmake',
  configuration : _cafeglsl_cmake_conf,
  install : true,
  install_dir : get_option('libdir') / 'cmake' / 'cafeglsl',
)

configure_file(
  input : 'cafeglslConfigVersion.cmake.in',
  output : 'cafeglslConfigVersion.cmake',
  configuration : _cafeglsl_cmake_conf,
  install : true,
  install_dir : get_option('libdir') / 'cmake' / 'cafeglsl',
)

if host_machine.system() != 'cafeos'
  cafeglsl_smoke = executable(
    'cafeglsl-smoke',
    'tests.cpp',
    include_directories : include_directories('.'),
    link_with : libcafeglsl,
    dependencies : cafeglsl_private_deps,
    link_args : cafeglsl_link_options,
    override_options : ['cpp_std=c++17'],
  )

  test('cafeglsl-smoke', cafeglsl_smoke)

  cafeglsl_cli = executable(
    'glslcompiler',
    ['main.cpp'],
    include_directories : include_directories('.'),
    link_with : libcafeglsl,
    dependencies : cafeglsl_private_deps,
    link_args : cafeglsl_link_options,
    override_options : ['cpp_std=c++17'],
    install : true,
  )

  # use CLI to test a tricky shader
  test(
    'cafeglsl-skinning-regression',
    cafeglsl_cli,
    args : ['-vs'] + files('testdata/skinning-regression.vert'),
  )

  # use CLI to test various shader setups that NSMBU also uses
  test(
    'cafeglsl-uber-regression',
    cafeglsl_cli,
    args : ['-vs'] + files('testdata/nsmbu-uber.vert'),
  )

  test(
    'cafeglsl-single-pixel-block',
    cafeglsl_cli,
    args : ['--uniform-mode', 'block', '-ps'] + files('testdata/basic.frag'),
  )

  test(
    'cafeglsl-pair-auto',
    cafeglsl_cli,
    args : ['--uniform-mode', 'auto', '-vs'] + files('testdata/basic.vert') +
           ['-ps'] + files('testdata/basic.frag'),
  )

  test(
    'cafeglsl-pair-block-reversed',
    cafeglsl_cli,
    args : ['--uniform-mode', 'block', '-ps'] + files('testdata/basic.frag') +
           ['-vs'] + files('testdata/basic.vert'),
  )

  test(
    'cafeglsl-pair-default',
    cafeglsl_cli,
    args : ['-vs'] + files('testdata/basic.vert') + ['-ps'] + files('testdata/basic.frag'),
  )

  test(
    'cafeglsl-single-vertex-block',
    cafeglsl_cli,
    args : ['--uniform-mode', 'block', '-vs'] + files('testdata/basic.vert'),
  )

  test(
    'cafeglsl-single-pixel-auto',
    cafeglsl_cli,
    args : ['--uniform-mode', 'auto', '-ps'] + files('testdata/basic.frag'),
  )

  test(
    'cafeglsl-pair-register-rejects-block',
    cafeglsl_cli,
    args : ['--uniform-mode', 'register', '-vs'] + files('testdata/basic.vert') +
           ['-ps'] + files('testdata/basic.frag'),
    should_fail : true,
  )

  test(
    'cafeglsl-invalid-uniform-policy',
    cafeglsl_cli,
    args : ['--uniform-mode', 'invalid', '-vs'] + files('testdata/basic.vert') +
           ['-ps'] + files('testdata/basic.frag'),
    should_fail : true,
  )
endif
