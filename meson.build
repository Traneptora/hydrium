project('hydrium', 'c', version : '0.1.0')

libhydrium_sources = files(
    'libhydrium/bitwriter.c',
    'libhydrium/encoder.c',
    'libhydrium/entropy.c',
    'libhydrium/libhydrium.c',
    'libhydrium/xyb.c',
)

libhydrium = library('hydrium', sources: libhydrium_sources, version : '0.1.0', soversion : '0')
executable('hydrium', 'hydrium.c', 'lodepng.c', link_with: libhydrium, c_args: ['-DLODEPNG_NO_COMPILE_ENCODER'])
executable('entropy-test', 'libhydrium/entropy-test.c', link_with: libhydrium)
