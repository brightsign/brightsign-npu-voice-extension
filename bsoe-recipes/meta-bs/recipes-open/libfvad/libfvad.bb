SUMMARY = "A simple voice activity detector (VAD) based on WebRTC VAD"
DESCRIPTION = "libfvad is a small library providing a simple API for voice activity detection based on the VAD from WebRTC."
HOMEPAGE = "https://github.com/dpirch/libfvad"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=af17ff429a56faf8ea04172e1aa14a6f"
SRC_URI = "git://github.com/dpirch/libfvad.git;branch=master"
SRCREV = "532ab666c20d3cfda38bca63abbb0f152706c369"

S = "${WORKDIR}/git"

inherit cmake

EXTRA_OECMAKE = "-DBUILD_SHARED_LIBS=OFF"

do_install:append() {
    install -d ${D}${libdir}
    install -m 0644 ${B}/src/libfvad.a ${D}${libdir}/
    install -d ${D}${includedir}
    install -m 0644 ${S}/include/fvad.h ${D}${includedir}/
}
ALLOW_EMPTY:${PN} = "1"
FILES:${PN} = ""
INSANE_SKIP:${PN}-dev = "staticdev"
# Only install static library and header in -dev
FILES:${PN}-dev = "${libdir}/libfvad.a ${includedir}/fvad.h"

