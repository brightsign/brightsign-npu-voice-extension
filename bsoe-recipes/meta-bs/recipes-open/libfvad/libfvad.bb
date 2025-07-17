SUMMARY = "A simple voice activity detector (VAD) based on WebRTC VAD"
DESCRIPTION = "libfvad is a small library providing a simple API for voice activity detection based on the VAD from WebRTC."
HOMEPAGE = "https://github.com/dpirch/libfvad"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=38c1c1f8ecdbde8e86a2eb9f697b5f06"

SRC_URI = "git://github.com/dpirch/libfvad.git;branch=master"

SRCREV = "532ab666c20d3cfda38bca63abbb0f152706c369"

S = "${WORKDIR}/git"

inherit cmake

# If you want only static, disable shared
EXTRA_OECMAKE = "-DBUILD_SHARED_LIBS=OFF"

do_install:append() {
    install -d ${D}${libdir}
    install -d ${D}${includedir}

}
# Install static library and header in the -dev package (for SDK!)
FILES_${PN}-dev += "${libdir}/libfvad.a ${includedir}/fvad.h"
