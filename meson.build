project('hydrium', 'c',
    license: ['BSD-2-Clause'],
    version : '0.4.2',
    default_options: [
        'debug=false',
        'optimization=s',
        'c_std=c99',
        'warning_level=1',
    ],
)

cflags = [
    '-DHYDRIUM_INTERNAL_BUILD',
]

wanted_cflags = [
    '-Werror=implicit-function-declaration',
    '-fPIC',
    '-fno-plt',
    '-Wformat',
    '-Werror=format-security',
    '-fvisibility=hidden',
    '-ffp-contract=fast',
    '-fallow-store-data-races',
]

ldflags = []

wanted_ldflags = [

]

cc = meson.get_compiler('c')

cflags += cc.get_supported_arguments(wanted_cflags)
ldflags += cc.get_supported_link_arguments(wanted_ldflags)

libhydrium_sources = files(
    'src/libhydrium/bitwriter.c',
    'src/libhydrium/cluster-map.c',
    'src/libhydrium/encoder.c',
    'src/libhydrium/entropy.c',
    'src/libhydrium/libhydrium.c',
    'src/libhydrium/memory.c',
)

hydrium_sources = files(
    'src/hydrium.c'
)

libhydrium_includes = include_directories('src/include')

libhydrium = library('hydrium',
    sources: libhydrium_sources,
    soversion : '0',
    c_args: cflags,
    link_args: ldflags,
    install: true,
    include_directories: libhydrium_includes,
)

libhydrium_dep = declare_dependency(include_directories: libhydrium_includes, link_with: libhydrium)
libspng_dep = dependency('spng', fallback : ['spng', 'spng_dep'])

hydrium = executable('hydrium',
    sources: [hydrium_sources],
    link_with: libhydrium,
    dependencies: [libhydrium_dep, libspng_dep],
    c_args: cflags,
    link_args: ldflags,
    install: true,
    include_directories: libhydrium_includes,
)

install_headers('src/include/libhydrium/libhydrium.h', subdir: 'libhydrium')
