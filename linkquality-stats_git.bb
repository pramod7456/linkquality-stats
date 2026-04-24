SUMMARY = "WiFi Link Quality Stats standalone process"
HOMEPAGE = "https://github.com/nickel-xb/linkquality-stats"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://configure.ac;beginline=1;endline=17;md5=TODO_UPDATE_CHECKSUM"

DEPENDS = "rbus ccsp-one-wifi cjson libmemfnswrap"
RDEPENDS:${PN} = "rbus cjson ccsp-one-wifi libmemfnswrap"

SRC_URI = "${CMF_GITHUB_ROOT}/linkquality-stats;protocol=${CMF_GIT_PROTOCOL};branch=${CMF_GIT_BRANCH};name=linkquality-stats"
SRCREV_linkquality-stats = "${AUTOREV}"
PV = "${RDK_RELEASE}+git${SRCPV}"

S = "${WORKDIR}/git"

inherit autotools pkgconfig systemd

# Tell Makefile.am where ccsp-one-wifi headers live
EXTRA_OEMAKE += "ONE_WIFI_SRCDIR=${STAGING_DIR_TARGET}${includedir}/../src/ccsp-one-wifi"

CFLAGS:append = " \
    -I${STAGING_INCDIR}/rbus \
    -I${STAGING_INCDIR}/ccsp \
    -I${STAGING_INCDIR}/ccsp/quality_mgr \
"

LDFLAGS:append = " \
    -lrbus \
    -lwifi_quality_manager \
    -lwifi_webserver \
    -lwifi_math_utils \
    -lstdc++ \
    -lpthread \
    -lm \
"

ISSYSTEMD = "${@bb.utils.contains('DISTRO_FEATURES', 'systemd', 'true', 'false', d)}"

do_install:append() {
    if [ "${ISSYSTEMD}" = "true" ]; then
        install -d ${D}${systemd_unitdir}/system
        install -m 0644 ${S}/linkquality_stats.service ${D}${systemd_unitdir}/system/
    fi
}

FILES:${PN} += "${bindir}/linkquality_stats"
SYSTEMD_SERVICE:${PN} = "${@bb.utils.contains('DISTRO_FEATURES', 'systemd', 'linkquality_stats.service', '', d)}"
