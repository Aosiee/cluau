#!/bin/sh
# cluau's installer for Linux and macOS: downloads a release, checks it
# against the release's SHA256SUMS, installs the `cluau` command and puts it
# on PATH.
#
#   curl -fsSL https://github.com/Aosiee/cluau/releases/latest/download/install.sh | sh
#   curl -fsSL .../install.sh | sh -s -- --version v0.7.0
#   sh install.sh [--version vX.Y.Z] [--prefix DIR] [--no-modify-path] [--uninstall]
#
# The command goes to PREFIX/bin (PREFIX is ~/.local unless --prefix names
# another), the guides to PREFIX/share/doc/cluau. When PREFIX/bin is not on
# PATH, a line adding it goes in the shell's startup file, marked so
# --uninstall can take it out again; --no-modify-path leaves the startup
# files alone.
#
# The environment can stand in for the options: CLUAU_VERSION,
# CLUAU_PREFIX. CLUAU_DOWNLOAD_URL replaces the release's download address
# (a URL or a local directory holding the release's files), for mirrors and
# for testing a release before it is published.

set -eu

repo="Aosiee/cluau"
version="${CLUAU_VERSION:-latest}"
prefix="${CLUAU_PREFIX:-$HOME/.local}"
download_url="${CLUAU_DOWNLOAD_URL:-}"
modify_path=1
uninstall=0
marker="# added by the cluau installer"

say() { printf 'cluau: %s\n' "$*"; }
fail() {
	printf 'cluau: %s\n' "$*" >&2
	exit 1
}

usage() {
	cat <<'EOF'
usage: install.sh [--version vX.Y.Z] [--prefix DIR] [--no-modify-path] [--uninstall]

  --version vX.Y.Z   the release to install (default: the latest)
  --prefix DIR       install into DIR/bin and DIR/share/doc/cluau (default: ~/.local)
  --no-modify-path   do not add DIR/bin to PATH in the shell's startup file
  --uninstall        remove cluau (and the PATH line this installer added)
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
	--version)
		[ $# -ge 2 ] || fail "--version needs a release (vX.Y.Z)"
		version="$2"
		shift 2
		;;
	--prefix)
		[ $# -ge 2 ] || fail "--prefix needs a directory"
		prefix="$2"
		shift 2
		;;
	--no-modify-path)
		modify_path=0
		shift
		;;
	--uninstall)
		uninstall=1
		shift
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		usage >&2
		exit 2
		;;
	esac
done

bin_dir="$prefix/bin"
doc_dir="$prefix/share/doc/cluau"

# The startup files a PATH line may be in (the shell's own first).
startup_files() {
	case "$(basename "${SHELL:-sh}")" in
	zsh) echo "${ZDOTDIR:-$HOME}/.zshrc" ;;
	bash)
		if [ "$(uname -s)" = "Darwin" ]; then
			echo "$HOME/.bash_profile"
		else
			echo "$HOME/.bashrc"
		fi
		;;
	fish) echo "${XDG_CONFIG_HOME:-$HOME/.config}/fish/conf.d/cluau.fish" ;;
	*) echo "$HOME/.profile" ;;
	esac
}

# Removes the lines this installer added from a file.
remove_marked_lines() {
	file="$1"
	[ -f "$file" ] || return 0
	grep -qF "$marker" "$file" || return 0
	tmp="$file.cluau-tmp"
	grep -vF "$marker" "$file" >"$tmp" || true
	cat "$tmp" >"$file"
	rm -f "$tmp"
	say "removed the PATH line from $file"
}

if [ "$uninstall" = 1 ]; then
	removed=0
	if [ -e "$bin_dir/cluau" ]; then
		rm -f "$bin_dir/cluau"
		removed=1
	fi
	if [ -d "$doc_dir" ]; then
		rm -rf "$doc_dir"
		removed=1
	fi
	for file in "$HOME/.profile" "$HOME/.bashrc" "$HOME/.bash_profile" "${ZDOTDIR:-$HOME}/.zshrc"; do
		remove_marked_lines "$file"
	done
	fish_file="${XDG_CONFIG_HOME:-$HOME/.config}/fish/conf.d/cluau.fish"
	if [ -f "$fish_file" ] && grep -qF "$marker" "$fish_file"; then
		rm -f "$fish_file"
		say "removed $fish_file"
	fi
	if [ "$removed" = 1 ]; then
		say "removed cluau from $prefix"
	else
		say "cluau is not installed in $prefix"
	fi
	exit 0
fi

# The release for this system.
os="$(uname -s)"
arch="$(uname -m)"
case "$os-$arch" in
Linux-x86_64 | Linux-amd64) platform="linux-x86_64" ;;
Darwin-arm64 | Darwin-aarch64) platform="macos-arm64" ;;
*)
	fail "there is no release build for $os $arch yet: build cluau from source (https://github.com/$repo#building)"
	;;
esac

if command -v curl >/dev/null 2>&1; then
	fetch() { curl -fsSL "$1" -o "$2"; }
	fetch_text() { curl -fsSL "$1"; }
elif command -v wget >/dev/null 2>&1; then
	fetch() { wget -q "$1" -O "$2"; }
	fetch_text() { wget -qO- "$1"; }
else
	fail "downloading needs curl or wget"
fi

# A file of the release: from the download address, or a local directory.
get() {
	case "$base" in
	http://* | https://* | file://*) fetch "$base/$1" "$2" || fail "cannot download $base/$1" ;;
	*) cp "$base/$1" "$2" 2>/dev/null || fail "no $1 in $base" ;;
	esac
}

if [ "$version" = "latest" ]; then
	[ -z "$download_url" ] || fail "CLUAU_DOWNLOAD_URL needs a version (--version vX.Y.Z)"
	version="$(fetch_text "https://api.github.com/repos/$repo/releases/latest" |
		sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1)"
	[ -n "$version" ] || fail "cannot find the latest release (GitHub's API did not answer)"
fi
case "$version" in
v*) ;;
*) version="v$version" ;;
esac
base="${download_url:-https://github.com/$repo/releases/download/$version}"
archive="cluau-$version-$platform.tar.gz"

if command -v sha256sum >/dev/null 2>&1; then
	sha256() { sha256sum "$1" | cut -d ' ' -f 1; }
elif command -v shasum >/dev/null 2>&1; then
	sha256() { shasum -a 256 "$1" | cut -d ' ' -f 1; }
else
	fail "checking the download needs sha256sum or shasum"
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

say "downloading cluau $version ($platform)"
get "$archive" "$work/$archive"
get "SHA256SUMS" "$work/SHA256SUMS"
expected="$(grep "[[:space:]]\*\{0,1\}$archive\$" "$work/SHA256SUMS" | cut -d ' ' -f 1 | head -n 1)"
[ -n "$expected" ] || fail "SHA256SUMS has no line for $archive"
actual="$(sha256 "$work/$archive")"
[ "$expected" = "$actual" ] || fail "$archive does not match its checksum (expected $expected, got $actual): not installing it"

tar -xzf "$work/$archive" -C "$work"
unpacked="$work/cluau-$version-$platform"
[ -f "$unpacked/cluau" ] || fail "$archive holds no cluau command"

mkdir -p "$bin_dir" "$doc_dir"
# Into place by a rename, so a cluau that is running is not overwritten.
cp "$unpacked/cluau" "$bin_dir/.cluau.new"
chmod 755 "$bin_dir/.cluau.new"
mv -f "$bin_dir/.cluau.new" "$bin_dir/cluau"
for doc in README.md LICENSE syntax.md scripting.md; do
	[ -f "$unpacked/$doc" ] && cp "$unpacked/$doc" "$doc_dir/"
done
installed="$("$bin_dir/cluau" --version 2>/dev/null)" || fail "the installed $bin_dir/cluau does not run on this system"
say "installed $installed to $bin_dir/cluau"

case ":$PATH:" in
*":$bin_dir:"*) say "$bin_dir is on PATH: run \`cluau\` to start" ;;
*)
	if [ "$modify_path" = 1 ]; then
		file="$(startup_files)"
		mkdir -p "$(dirname "$file")"
		if [ -f "$file" ] && grep -qF "$marker" "$file"; then
			say "$file already adds $bin_dir to PATH: open a new shell to use \`cluau\`"
		else
			if [ "${file##*.}" = "fish" ]; then
				printf 'fish_add_path "%s" %s\n' "$bin_dir" "$marker" >>"$file"
			else
				printf 'export PATH="%s:$PATH" %s\n' "$bin_dir" "$marker" >>"$file"
			fi
			say "added $bin_dir to PATH in $file: open a new shell (or run: export PATH=\"$bin_dir:\$PATH\") to use \`cluau\`"
		fi
	else
		say "$bin_dir is not on PATH: add it to use \`cluau\` by name (export PATH=\"$bin_dir:\$PATH\")"
	fi
	;;
esac
