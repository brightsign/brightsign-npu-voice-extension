SUMMARY = "FFTW3 (Fastest Fourier Transform in the West), single-precision (float) library"
HOMEPAGE = "http://www.fftw.org/"
LICENSE = "GPL-2.0"
LIC_FILES_CHKSUM = "file://COPYING;md5=751419260aa954499f7abaabaa882bbe"

PV = "3.3.10"

SRC_URI = "http://www.fftw.org/fftw-${PV}.tar.gz"
SRC_URI[sha256sum] = "56c932549852cddcfafdab3820b0200c7742675be92179e59e6215b340e26467"

# Only build single-precision (float), static only, no tests/programs
EXTRA_OECONF = "--enable-float --enable-static --disable-shared --disable-fortran --disable-mpi --disable-threads --disable-openmp --disable-doc"

# Do not build shared libraries
PACKAGECONFIG = ""

# Only install .a and headers, no shared objects
FILES:${PN}-dev += "${libdir}/libfftw3f.a ${includedir}"

ALLOW_EMPTY:${PN} = "1"

inherit autotools pkgconfig

do_install:append() {
    install -d ${D}${includedir}
    install -m 0644 ${S}/api/fftw3.h ${D}${includedir}/
}

