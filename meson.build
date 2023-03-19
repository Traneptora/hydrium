project('hydrium', 'c', version : '0.0.1')

libhydrium_sources = files(
    'libhydrium/bitwriter.c',
    'libhydrium/encoder.c',
    'libhydrium/entropy.c',
    'libhydrium/libhydrium.c',
    'libhydrium/xyb.c',
)

libhydrium = library('hydrium', sources: libhydrium_sources, version : '0.0.1', soversion : '0')
executable('hydrium', 'hydrium.c', link_with: libhydrium)
executable('entropy-test', 'libhydrium/entropy-test.c', link_with: libhydrium)
