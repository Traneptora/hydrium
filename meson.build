project('hydrium', 'c',
    license: ['BSD-2-Clause'],
    version : '0.2.1',
    default_options: [
        'buildtype=debugoptimized',
        'c_std=c99',
        'warning_level=1',
    ],
)

cflags = [
    '-DLODEPNG_NO_COMPILE_ENCODER',
]

wanted_flags = [
    '-Werror=implicit-function-declaration',
    '-fPIC',
    '-fno-plt',
    '-Wformat',
    '-Werror=format-security',
]

cc = meson.get_compiler('c')

cflags += cc.get_supported_arguments(wanted_flags)

libhydrium_sources = files(
    'src/libhydrium/bitwriter.c',
    'src/libhydrium/encoder.c',
    'src/libhydrium/entropy.c',
    'src/libhydrium/libhydrium.c',
    'src/libhydrium/xyb.c',
)

lodepng_sources = files(
    'src/lodepng/lodepng.c',
)

hydrium_sources = files(
    'src/hydrium.c'
)

libhydrium = library('hydrium', sources: libhydrium_sources, version : '0.2.1', soversion : '0')

executable('hydrium', sources: [hydrium_sources, lodepng_sources], link_with: libhydrium, c_args: cflags)
