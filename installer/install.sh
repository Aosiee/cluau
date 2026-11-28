#!/bin/sh
# cluau's installer for Linux and macOS: downloads a release, checks it
# against the release's SHA256SUMS (and that file's signature), installs the
# `cluau` command and puts it on PATH.
#
#   curl -fsSL https://github.com/odd-fish-society/cluau/releases/latest/download/install.sh | sh
#   curl -fsSL .../install.sh | sh -s -- --version v0.7.0
#   sh install.sh [--version vX.Y.Z] [--prefix DIR] [--no-modify-path] [--uninstall]
#
# The command goes to PREFIX/bin (PREFIX is ~/.local unless --prefix names
# another), the guides and the licences to PREFIX/share/doc/cluau. When PREFIX/bin is not on
# PATH, a line adding it goes in the shell's startup file, marked so
# --uninstall can take it out again; --no-modify-path leaves the startup
# files alone.
#
# The environment can stand in for the options: CLUAU_VERSION,
# CLUAU_PREFIX. CLUAU_REPOSITORY is the repository the releases come from
# (its web address: https://github.com/odd-fish-society/cluau unless it names
# another, such as a Forgejo or Gitea server's), whose API gives the latest
# release. CLUAU_DOWNLOAD_URL replaces a release's download address (a URL
# or a local directory holding the release's files), for mirrors and for
# testing a release before it is published.
#
# Releases from 0.34.0 on are signed: SHA256SUMS.minisig is a minisign
# signature of SHA256SUMS by one of the release keys below. The signature
# is checked with minisign when it is installed, or else with OpenSSL 3;
# with neither, the installer says so and goes by the checksum alone.
# CLUAU_RELEASE_KEYS (minisign public keys, separated by spaces) are
# trusted in place of these.

set -eu

repository="${CLUAU_REPOSITORY:-https://github.com/odd-fish-society/cluau}"
repository="${repository%/}"
# owner/name are the address's last two parts; the server is what comes
# before them (a Forgejo or Gitea server may live under a path of its own).
repo_name="${repository##*/}"
repo_rest="${repository%/*}"
repo="${repo_rest##*/}/$repo_name" # owner/name
server="${repo_rest%/*}"           # https://host, or https://host/path
version="${CLUAU_VERSION:-latest}"
prefix="${CLUAU_PREFIX:-$HOME/.local}"
download_url="${CLUAU_DOWNLOAD_URL:-}"
modify_path=1
uninstall=0
marker="# added by the cluau installer"
# The release keys, as installer/release-keys.pub has them (a test checks
# that they match).
release_keys="${CLUAU_RELEASE_KEYS-
}"
# The first release that is signed: an earlier one installs on its
# checksum alone.
signed_since="0.34.0"

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

# A relative prefix is made absolute: the PATH line must not depend on the
# directory a shell starts in.
case "$prefix" in
/*) ;;
*) prefix="$(pwd)/$prefix" ;;
esac
prefix="${prefix%/}"
bin_dir="$prefix/bin"
doc_dir="$prefix/share/doc/cluau"

# Whether a line is the one this installer added for this bin_dir (not
# another prefix's).
is_our_line() {
	case "$1" in
	*"$marker"*) ;;
	*) return 1 ;;
	esac
	case "$1" in
	*"\"$bin_dir\""* | *"\"$bin_dir:"*) return 0 ;;
	esac
	return 1
}

# Whether a file has this bin_dir's PATH line.
has_our_line() {
	[ -f "$1" ] || return 1
	while IFS= read -r line || [ -n "$line" ]; do
		if is_our_line "$line"; then
			return 0
		fi
	done <"$1"
	return 1
}

# The documents a release holds, installed beside the command.
docs="README.md LICENSE THIRD_PARTY_NOTICES.md syntax.md scripting.md"

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

# Removes the line this installer added for bin_dir from a file (another
# prefix's line stays).
remove_marked_lines() {
	file="$1"
	has_our_line "$file" || return 0
	tmp="$file.cluau-tmp"
	: >"$tmp"
	while IFS= read -r line || [ -n "$line" ]; do
		if ! is_our_line "$line"; then
			printf '%s\n' "$line" >>"$tmp"
		fi
	done <"$file"
	cat "$tmp" >"$file"
	rm -f "$tmp"
	say "removed the PATH line from $file"
	# A file that held nothing else (one this installer made) goes too.
	if ! grep -q '[^[:space:]]' "$file"; then
		rm -f "$file"
	fi
}

if [ "$uninstall" = 1 ]; then
	removed=0
	if [ -e "$bin_dir/cluau" ]; then
		rm -f "$bin_dir/cluau"
		removed=1
	fi
	rm -f "$bin_dir/.cluau.new" "$bin_dir"/.cluau.new.*
	if [ -d "$doc_dir" ]; then
		rm -rf "$doc_dir"
		removed=1
	fi
	# The directories the install made, when nothing else is in them.
	for dir in "$bin_dir" "$prefix/share/doc" "$prefix/share"; do
		rmdir "$dir" 2>/dev/null || true
	done
	for file in "$HOME/.profile" "$HOME/.bashrc" "$HOME/.bash_profile" "${ZDOTDIR:-$HOME}/.zshrc"; do
		remove_marked_lines "$file"
	done
	fish_file="${XDG_CONFIG_HOME:-$HOME/.config}/fish/conf.d/cluau.fish"
	if has_our_line "$fish_file" && command -v fish >/dev/null 2>&1; then
		# Earlier installers added the directory to fish's universal
		# fish_user_paths, which outlives the file.
		fish -c 'if set -l i (contains -i -- $argv[1] $fish_user_paths); set -U -e fish_user_paths[$i]; end' "$bin_dir" 2>/dev/null || true
	fi
	remove_marked_lines "$fish_file"
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
Linux-aarch64 | Linux-arm64) platform="linux-arm64" ;;
Darwin-arm64 | Darwin-aarch64) platform="macos-arm64" ;;
Darwin-x86_64) platform="macos-x86_64" ;;
*)
	fail "there is no release build for $os $arch yet: build cluau from source ($repository#building)"
	;;
esac

# A download runs in the background and is waited for, so that a signal
# stops the install at once (a shell runs its traps only between commands,
# and `wait` is one it interrupts).
fetch_pid=""
if command -v curl >/dev/null 2>&1; then
	fetch() {
		curl -fsSL "$1" -o "$2" &
		fetch_pid=$!
		wait "$fetch_pid"
	}
	fetch_text() { curl -fsSL "$1"; }
elif command -v wget >/dev/null 2>&1; then
	fetch() {
		wget -q "$1" -O "$2" &
		fetch_pid=$!
		wait "$fetch_pid"
	}
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

# The same, for a file a release may not have: false when it has not.
get_if_there() {
	case "$base" in
	http://* | https://* | file://*) fetch "$base/$1" "$2" 2>/dev/null ;;
	*) cp "$base/$1" "$2" 2>/dev/null ;;
	esac
}

if [ "$version" = "latest" ]; then
	[ -z "$download_url" ] || fail "CLUAU_DOWNLOAD_URL needs a version (--version vX.Y.Z)"
	# GitHub's API, or a Forgejo or Gitea server's (both answer tag_name).
	case "$server" in
	https://github.com) api="https://api.github.com/repos/$repo/releases/latest" ;;
	*) api="$server/api/v1/repos/$repo/releases/latest" ;;
	esac
	version="$(fetch_text "$api" |
		sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1)"
	[ -n "$version" ] || fail "cannot find the latest release ($api did not answer)"
fi
case "$version" in
v*) ;;
*) version="v$version" ;;
esac
base="${download_url:-$repository/releases/download/$version}"
archive="cluau-$version-$platform.tar.gz"

if command -v sha256sum >/dev/null 2>&1; then
	sha256() { sha256sum "$1" | cut -d ' ' -f 1; }
elif command -v shasum >/dev/null 2>&1; then
	sha256() { shasum -a 256 "$1" | cut -d ' ' -f 1; }
else
	fail "checking the download needs sha256sum or shasum"
fi

# Whether version $1 (X.Y.Z) comes before version $2.
version_before() {
	[ "$(printf '%s\n%s\n' "$1" "$2" | awk -F. '{ printf "%09d%09d%09d\n", $1, $2, $3 }' | sort | head -n 1)" != \
		"$(printf '%s\n' "$2" | awk -F. '{ printf "%09d%09d%09d\n", $1, $2, $3 }')" ]
}

# The bytes of standard base64 text (its own command: sizes differ between
# systems' base64 tools).
from_base64() { openssl base64 -d -A; }

# Whether SHA256SUMS ($1) is signed by $2 (a minisign public key) in $3
# (the .minisig), with OpenSSL 3: the signature is Ed25519, of the file's
# BLAKE2b-512 hash ("ED") or of the file itself ("Ed"); the trusted comment
# is signed with it. Prints the trusted comment.
verify_openssl() {
	v="$work/verify"
	rm -rf "$v"
	mkdir "$v"
	tr -d '\r' <"$3" >"$v/sig"
	sed -n 2p "$v/sig" | tr -d '\n' | from_base64 >"$v/sig.bin" 2>/dev/null || return 1
	sed -n 4p "$v/sig" | tr -d '\n' | from_base64 >"$v/global.bin" 2>/dev/null || return 1
	comment="$(sed -n 3p "$v/sig")"
	case "$comment" in
	"trusted comment: "*) comment="${comment#trusted comment: }" ;;
	*) return 1 ;;
	esac
	printf '%s' "$2" | from_base64 >"$v/key.bin" 2>/dev/null || return 1
	[ "$(wc -c <"$v/sig.bin" | tr -d ' ')" = 74 ] && [ "$(wc -c <"$v/key.bin" | tr -d ' ')" = 42 ] || return 1
	dd if="$v/sig.bin" of="$v/algorithm" bs=1 count=2 2>/dev/null
	dd if="$v/sig.bin" of="$v/sig.id" bs=1 skip=2 count=8 2>/dev/null
	dd if="$v/sig.bin" of="$v/ed25519" bs=1 skip=10 count=64 2>/dev/null
	dd if="$v/key.bin" of="$v/key.id" bs=1 skip=2 count=8 2>/dev/null
	cmp -s "$v/sig.id" "$v/key.id" || return 1
	# The public key as OpenSSL reads it: DER's Ed25519 prefix, then the key.
	printf '\060\052\060\005\006\003\053\145\160\003\041\000' >"$v/key.der"
	dd if="$v/key.bin" bs=1 skip=10 count=32 2>/dev/null >>"$v/key.der"
	openssl pkey -pubin -inform DER -in "$v/key.der" -out "$v/key.pem" 2>/dev/null || return 1
	case "$(cat "$v/algorithm")" in
	ED) openssl dgst -blake2b512 -binary "$1" >"$v/message" 2>/dev/null || return 1 ;;
	Ed) cp "$1" "$v/message" ;;
	*) return 1 ;;
	esac
	openssl pkeyutl -verify -pubin -inkey "$v/key.pem" -rawin -in "$v/message" -sigfile "$v/ed25519" >/dev/null 2>&1 || return 1
	{
		cat "$v/ed25519"
		printf '%s' "$comment"
	} >"$v/signed-comment"
	openssl pkeyutl -verify -pubin -inkey "$v/key.pem" -rawin -in "$v/signed-comment" -sigfile "$v/global.bin" >/dev/null 2>&1 || return 1
	printf '%s\n' "$comment"
}

# Checks the release's SHA256SUMS against its SHA256SUMS.minisig, or says
# why it cannot (and stops when the release should be signed and is not).
check_signature() {
	number="${version#v}"
	signed=1
	if ! get_if_there "SHA256SUMS.minisig" "$work/SHA256SUMS.minisig"; then
		if version_before "$number" "$signed_since"; then
			say "cluau $version is from before releases were signed: going by its checksum alone"
			return 0
		fi
		fail "cluau $version has no SHA256SUMS.minisig, its signature: not installing it"
	fi
	if [ -z "$(printf '%s' "$release_keys" | tr -d ' \n\t\r')" ]; then
		if version_before "$number" "$signed_since"; then
			say "this installer has no release key to check the signature with: going by the checksum alone"
			return 0
		fi
		fail "this installer carries no release key, so it cannot check cluau $version's signature: use the installer from a release"
	fi
	comment=""
	tool=""
	if command -v minisign >/dev/null 2>&1; then
		tool="minisign"
		for key in $release_keys; do
			if comment="$(minisign -V -Q -m "$work/SHA256SUMS" -x "$work/SHA256SUMS.minisig" -P "$key" 2>/dev/null)"; then
				break
			fi
			comment=""
		done
	else
		case "$(openssl version 2>/dev/null)" in
		"OpenSSL "[3-9]*)
			tool="OpenSSL"
			for key in $release_keys; do
				if comment="$(verify_openssl "$work/SHA256SUMS" "$key" "$work/SHA256SUMS.minisig")"; then
					break
				fi
				comment=""
			done
			;;
		esac
	fi
	if [ -z "$tool" ]; then
		say "the signature was not checked (that needs minisign, or OpenSSL 3): going by the checksum alone"
		return 0
	fi
	[ -n "$comment" ] || fail "SHA256SUMS is not signed by a release key ($tool): not installing cluau $version"
	[ "$comment" = "cluau $version" ] || fail "SHA256SUMS is signed as \"$comment\", not \"cluau $version\": not installing it"
	say "the release's signature checks out ($tool)"
}

work="$(mktemp -d)"
# Whatever happens, nothing half-made is left: a download still running,
# the downloads, and a new command not yet in place.
cleanup() {
	if [ -n "$fetch_pid" ]; then
		kill "$fetch_pid" 2>/dev/null || true
	fi
	rm -rf "$work" "$bin_dir/.cluau.new"
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

say "downloading cluau $version ($platform)"
get "$archive" "$work/$archive"
get "SHA256SUMS" "$work/SHA256SUMS"
check_signature
expected="$(grep "[[:space:]]\*\{0,1\}$archive\$" "$work/SHA256SUMS" | cut -d ' ' -f 1 | head -n 1)"
[ -n "$expected" ] || fail "SHA256SUMS has no line for $archive"
actual="$(sha256 "$work/$archive")"
[ "$expected" = "$actual" ] || fail "$archive does not match its checksum (expected $expected, got $actual): not installing it"

tar -xzf "$work/$archive" -C "$work"
unpacked="$work/cluau-$version-$platform"
[ -f "$unpacked/cluau" ] || fail "$archive holds no cluau command"

mkdir -p "$bin_dir" "$doc_dir"
# The new command runs here before it replaces anything (from the directory
# it goes to: a temporary directory may not allow running programs). Then
# into place by a rename, so a cluau that is running is not overwritten.
cp "$unpacked/cluau" "$bin_dir/.cluau.new"
chmod 755 "$bin_dir/.cluau.new"
if ! installed="$("$bin_dir/.cluau.new" --version 2>"$work/version-error")"; then
	rm -f "$bin_dir/.cluau.new"
	if [ -s "$work/version-error" ]; then
		sed 's/^/  /' "$work/version-error" >&2
	fi
	fail "cluau $version does not run on this system: nothing was changed"
fi
mv -f "$bin_dir/.cluau.new" "$bin_dir/cluau"
for doc in $docs; do
	if [ -f "$unpacked/$doc" ]; then
		cp "$unpacked/$doc" "$doc_dir/"
	fi
done
say "installed $installed to $bin_dir/cluau"

case ":$PATH:" in
*":$bin_dir:"*) say "$bin_dir is on PATH: run \`cluau\` to start" ;;
*)
	if [ "$modify_path" = 1 ]; then
		file="$(startup_files)"
		mkdir -p "$(dirname "$file")"
		if has_our_line "$file"; then
			say "$file already adds $bin_dir to PATH: open a new shell to use \`cluau\`"
		else
			if [ "${file##*.}" = "fish" ]; then
				# --path: PATH itself, which lasts as long as this file
				# (not the universal fish_user_paths, which outlives it).
				printf 'fish_add_path --path "%s" %s\n' "$bin_dir" "$marker" >>"$file"
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
