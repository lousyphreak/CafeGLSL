#!/usr/bin/env bash

set -euo pipefail

usage()
{
	echo "usage: ${0##*/} <host|wiiu> <builddir> <stagedir>" 1>&2
	exit 1
}

[ $# -eq 3 ] || usage

MODE="$1"
BUILDDIR="$2"
STAGEDIR="$3"

SRCDIR="$(cd "${BASH_SOURCE%/*}/../.." && pwd)"
CAFEDIR="${SRCDIR}/cafecompiler"

if [ ! -d "${BUILDDIR}" ]; then
	echo "no such build directory: ${BUILDDIR}" 1>&2
	exit 1
fi
BUILDDIR="$(cd "${BUILDDIR}" && pwd)"

VERSION="$("${CAFEDIR}/ci/version.sh")"

# Match Meson's installation layout, including lib64/multiarch and custom headers.
INSTALL_DIRS=()
while IFS= read -r directory; do
  INSTALL_DIRS+=("${directory}")
done < <(meson introspect --buildoptions "${BUILDDIR}" | python3 -c '
import json, os, sys
options = {option["name"]: option["value"] for option in json.load(sys.stdin)}
for key in ("libdir", "includedir", "bindir"):
    path = options[key]
    print(os.path.relpath(path, options["prefix"]) if os.path.isabs(path) else path)
')
LIBDIR="${INSTALL_DIRS[0]}"
INCLUDEDIR="${INSTALL_DIRS[1]}"
BINDIR="${INSTALL_DIRS[2]}"

rm -rf "${STAGEDIR}"
mkdir -p "${STAGEDIR}/${INCLUDEDIR}/cafeglsl" "${STAGEDIR}/${LIBDIR}"

install -m 644 "${CAFEDIR}/CafeGLSLCompiler.h" "${STAGEDIR}/${INCLUDEDIR}/cafeglsl/"
install -m 644 "${CAFEDIR}/cafe_gx2.h" "${STAGEDIR}/${INCLUDEDIR}/cafeglsl/"
install -m 644 "${CAFEDIR}/gfd.h" "${STAGEDIR}/${INCLUDEDIR}/cafeglsl/"
install -m 644 "${BUILDDIR}/cafecompiler/libcafeglsl.a" "${STAGEDIR}/${LIBDIR}/"
install -m 644 "${SRCDIR}/licenses/MIT" "${STAGEDIR}/LICENSE"

case "${MODE}" in
host)
	mkdir -p "${STAGEDIR}/${BINDIR}"
	if [ -f "${BUILDDIR}/cafecompiler/glslcompiler.exe" ]; then
		install -m 755 "${BUILDDIR}/cafecompiler/glslcompiler.exe" \
			"${STAGEDIR}/${BINDIR}/glslcompiler.exe"
		BINARY="${BINDIR}/glslcompiler.exe"
	else
		install -m 755 "${BUILDDIR}/cafecompiler/glslcompiler" \
			"${STAGEDIR}/${BINDIR}/glslcompiler"
		BINARY="${BINDIR}/glslcompiler"
	fi
	;;
wiiu)
	BINARY=""
	;;
*)
	usage
	;;
esac

mkdir -p "${STAGEDIR}/${LIBDIR}/pkgconfig" "${STAGEDIR}/${LIBDIR}/cmake/cafeglsl"
install -m 644 "${BUILDDIR}/meson-private/cafeglsl.pc" \
  "${STAGEDIR}/${LIBDIR}/pkgconfig/cafeglsl.pc"
install -m 644 "${BUILDDIR}/cafecompiler/cafeglslConfig.cmake" \
  "${STAGEDIR}/${LIBDIR}/cmake/cafeglsl/"
install -m 644 "${BUILDDIR}/cafecompiler/cafeglslConfigVersion.cmake" \
  "${STAGEDIR}/${LIBDIR}/cmake/cafeglsl/"

sed -e "s|@VERSION@|${VERSION}|g" -e "s|@BINARY@|${BINARY}|g" \
	"${CAFEDIR}/ci/README-${MODE}.md" > "${STAGEDIR}/README.md"

echo "staged ${MODE} tree for ${VERSION} in ${STAGEDIR}"
