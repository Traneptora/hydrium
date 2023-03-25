project('hydrium', 'c', version : '0.2.1')

libhydrium_sources = files(
    'libhydrium/bitwriter.c',
    'libhydrium/encoder.c',
    'libhydrium/entropy.c',
    'libhydrium/libhydrium.c',
    'libhydrium/xyb.c',
)

libhydrium = library('hydrium', sources: libhydrium_sources, version : '0.2.1', soversion : '0')
executable('hydrium', 'hydrium.c', 'lodepng.c', link_with: libhydrium, c_args: ['-DLODEPNG_NO_COMPILE_ENCODER'])
