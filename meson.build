project('hydrium', 'c')

libhydrium_sources = files(
    'libhydrium/libhydrium.c',
    'libhydrium/encoder.c',
    'libhydrium/writer/bitwriter.c',
    'libhydrium/color/xyb.c',
)

libhydrium = library('hydrium', sources: libhydrium_sources, version : '0.0.1', soversion : '0')
executable('hydrium', 'hydrium.c', link_with: libhydrium)
