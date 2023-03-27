project('hydrium', 'c',
    license: ['BSD-2-Clause'],
    version : '0.2.2',
    default_options: [
        'buildtype=minsize',
        'c_std=c99',
        'warning_level=1',
    ],
)

cflags = [
    '-DLODEPNG_NO_COMPILE_ENCODER',
    '-DHYDRIUM_INTERNAL_BUILD',
]

wanted_cflags = [
    '-Werror=implicit-function-declaration',
    '-fPIC',
    '-fno-plt',
    '-Wformat',
    '-Werror=format-security',
    '-fvisibility=hidden',
]

ldflags = []

wanted_ldflags = [
    '-Wl,-s',
]

cc = meson.get_compiler('c')

cflags += cc.get_supported_arguments(wanted_cflags)
ldflags += cc.get_supported_link_arguments(wanted_ldflags)

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

libhydrium = library('hydrium',
    sources: libhydrium_sources,
    soversion : '0',
    c_args: cflags,
    link_args: ldflags,
    install: true,
)

hydrium = executable('hydrium',
    sources: [hydrium_sources, lodepng_sources],
    link_with: libhydrium,
    c_args: cflags,
    link_args: ldflags,
    install: true,
)

install_headers('src/libhydrium/libhydrium.h', subdir: 'libhydrium')
