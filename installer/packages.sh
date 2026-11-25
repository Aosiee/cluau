#!/bin/sh
# Builds cluau's Debian (.deb) and RPM (.rpm) packages from a release build,
# for the machine's architecture (x86_64 or arm64):
#
#   sh installer/packages.sh <version> <unpacked release dir> <out dir>
#
# The release directory is an unpacked cluau-<version>-linux-<arch>.tar.gz
# (cluau, LICENSE, README.md, syntax.md, scripting.md). Needs dpkg-deb and
# rpmbuild. The packages install /usr/bin/cluau and the documents under
# /usr/share/doc/cluau.

set -eu

[ $# -eq 3 ] || {
	echo "usage: sh installer/packages.sh <version> <unpacked release dir> <out dir>" >&2
	exit 2
}
version="${1#v}"
dir="$(cd "$2" && pwd)"
mkdir -p "$3"
out="$(cd "$3" && pwd)"

case "$(uname -m)" in
x86_64 | amd64) deb_arch=amd64 rpm_arch=x86_64 ;;
aarch64 | arm64) deb_arch=arm64 rpm_arch=aarch64 ;;
*)
	echo "packages.sh: no packages for $(uname -m)" >&2
	exit 1
	;;
esac
for file in cluau LICENSE README.md syntax.md scripting.md; do
	[ -f "$dir/$file" ] || {
		echo "packages.sh: $dir has no $file" >&2
		exit 1
	}
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

summary="C-syntax-flavoured Luau, as a scripting language"
description="cluau runs scripts written with braces, C operators, let and const,
classes, try and catch, and enums, on Luau's VM: with a REPL, a type
checker, a test runner, a formatter, a profiler, a language server and a
debugger, and a standard library for files, processes, HTTP, sockets,
SQLite, regular expressions and tasks."

# ---- .deb ----------------------------------------------------------------

root="$work/deb"
mkdir -p "$root/DEBIAN" "$root/usr/bin" "$root/usr/share/doc/cluau"
install -m 755 "$dir/cluau" "$root/usr/bin/cluau"
install -m 644 "$dir/README.md" "$dir/syntax.md" "$dir/scripting.md" "$root/usr/share/doc/cluau/"
install -m 644 "$dir/LICENSE" "$root/usr/share/doc/cluau/copyright"
size="$(du -sk "$root/usr" | cut -f 1)"
{
	echo "Package: cluau"
	echo "Version: $version"
	echo "Architecture: $deb_arch"
	echo "Maintainer: cluau <https://github.com/Aosiee/cluau/issues>"
	echo "Installed-Size: $size"
	echo "Depends: libc6"
	echo "Section: interpreters"
	echo "Priority: optional"
	echo "Homepage: https://github.com/Aosiee/cluau"
	echo "Description: $summary"
	echo "$description" | sed 's/^/ /'
} > "$root/DEBIAN/control"
# xz: what every dpkg reads (newer ones default to zstd, which Debian 11's cannot).
dpkg-deb --root-owner-group -Zxz --build "$root" "$out/cluau_${version}_${deb_arch}.deb"

# ---- .rpm ----------------------------------------------------------------

mkdir -p "$work/rpm"
cat > "$work/cluau.spec" <<EOF
Name: cluau
Version: $version
Release: 1
Summary: $summary
License: LicenseRef-cluau
URL: https://github.com/Aosiee/cluau
# A release build, packaged as it is: no debug package, nothing stripped.
%global debug_package %{nil}
%global __strip /bin/true
%global _build_id_links none

%description
$description

%install
install -D -m 755 "$dir/cluau" %{buildroot}%{_bindir}/cluau
install -D -m 644 "$dir/README.md" %{buildroot}%{_docdir}/cluau/README.md
install -D -m 644 "$dir/syntax.md" %{buildroot}%{_docdir}/cluau/syntax.md
install -D -m 644 "$dir/scripting.md" %{buildroot}%{_docdir}/cluau/scripting.md
install -D -m 644 "$dir/LICENSE" %{buildroot}%{_licensedir}/cluau/LICENSE

%files
%{_bindir}/cluau
%{_docdir}/cluau
%license %{_licensedir}/cluau/LICENSE
EOF
# xz too, for the older RPM systems (RHEL 8).
rpmbuild -bb --quiet --target "$rpm_arch" --define "_topdir $work/rpm" --define "_binary_payload w9.xzdio" "$work/cluau.spec"
cp "$work/rpm/RPMS/$rpm_arch/"*.rpm "$out/"

ls -1 "$out"/*.deb "$out"/*.rpm
