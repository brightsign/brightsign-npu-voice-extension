SUMMARY = "C library for reading and writing files containing sampled sound"
DESCRIPTION = "libsndfile is a library of C routines for reading and writing files containing sampled sound (such as MS Windows WAV and the Apple/SGI AIFF format) through one standard library interface."
HOMEPAGE = "http://libsndfile.github.io/libsndfile/"
LICENSE = "LGPL-2.1"
LIC_FILES_CHKSUM = "file://COPYING;md5=2d5025d4aa3495befef8f17206a5b0a1"

SRC_URI = "git://github.com/libsndfile/libsndfile.git;branch=master;protocol=https"
SRCREV = "52b803f57a1f4d23471f5c5f77e1a21e0721ea0e"

S = "${WORKDIR}/git"

inherit cmake pkgconfig

DEPENDS = "flac libogg libvorbis"

EXTRA_OECMAKE = "\
    -DENABLE_EXTERNAL_LIBS=OFF \
    -DBUILD_PROGRAMS=OFF \
    -DBUILD_TESTING=OFF \
    -DENABLE_EXPERIMENTAL=OFF \
    -DENABLE_EXAMPLES=OFF \
    -DBUILD_SHARED_LIBS=OFF \
"

do_install:append() {
    install -d ${D}${libdir}
    install -m 0644 ${B}/libsndfile.a ${D}${libdir}/
    install -d ${D}${includedir}
    install -m 0644 ${S}/include/sndfile.h ${D}${includedir}/
}
ALLOW_EMPTY:${PN} = "1"
FILES:${PN} = ""
INSANE_SKIP:${PN}-dev = "staticdev"
# Only install static library and header in -dev
FILES:${PN}-dev = "${libdir}/libsndfile.a ${includedir}/sndfile.h"

