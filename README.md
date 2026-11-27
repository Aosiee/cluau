# cluau

[![Linux](https://github.com/odd-fish-society/cluau/actions/workflows/linux.yml/badge.svg?branch=main)](https://github.com/odd-fish-society/cluau/actions/workflows/linux.yml)
[![macOS](https://github.com/odd-fish-society/cluau/actions/workflows/macos.yml/badge.svg?branch=main)](https://github.com/odd-fish-society/cluau/actions/workflows/macos.yml)
[![Windows](https://github.com/odd-fish-society/cluau/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/odd-fish-society/cluau/actions/workflows/windows.yml)
[![Release](https://img.shields.io/github/v/release/odd-fish-society/cluau)](https://github.com/odd-fish-society/cluau/releases/latest)

**C-syntax-flavoured Luau.** cluau is a Luau dialect: braces, C operators,
`let` / `const`, classes, `try` / `catch`, enums, `switch`, generators,
`??` and optional chains, destructuring, spreads, `async` / `await`,
translated to standard
Luau before Luau's type checker and compiler run. Luau's types, semantics,
VM and libraries are unchanged, and every translated line stays on its
line, so Luau's error messages and line-based debuggers point at the
`.cluau` source.

```
class Player {
	@export Speed: number = 4
	VelocityY: number = 0

	function OnFixedUpdate(dt: number) {
		if (self.VelocityY != 0 && !self:Grounded()) {
			self.VelocityY -= 9.81 * dt
		}
	}

	function Grounded(): boolean {
		return self.Entity.Transform.Position.y <= 0
	}
}
```

It is also a scripting language on its own: `cluau script.cluau` runs a
script the way `python script.py` does, with a REPL, `require` for other
files and a standard library for files, paths, processes, JSON, times,
HTTP (a client and a server), WebSockets, TCP and TLS sockets, SQLite
databases, hashing, compression, zip and tar archives, URLs, terminal
colours, regular expressions, tasks that run side by side, command-line
arguments, CSV, TOML and YAML, collections, random numbers, logging,
UUIDs, semantic versions, file watching and workers that run in parallel, and `cluau check script.cluau` type-checks it (and the
modules it requires) without running it, `cluau test` runs its test files
(with `--coverage`, the lines they missed), `cluau profile` shows where
its time goes, `cluau fmt` formats it, and `cluau add` brings in packages
from Git repositories. Scripts run as native code on x64 and arm64, and
`cluau --sandbox` runs one without file, network or process access. `cluau
lsp` is a language server for editors, `cluau debug` a debugger for them,
and the VS Code extension ([`editors/vscode`](editors/vscode/README.md))
brings both, and highlighting, to VS Code.

```
#!/usr/bin/env cluau
const fs = require("@std/fs")
const process = require("@std/process")

for (_, file in fs.walk(process.args[1] or ".")) {
	if (string.sub(file, -6) == ".cluau") {
		print(file, #string.split(fs.read(file), "\n"))
	}
}
```

- [`docs/syntax.md`](docs/syntax.md) is the language reference.
- [`docs/scripting.md`](docs/scripting.md) is the scripting guide: running
  and checking scripts, the REPL, classes in scripts, modules and the
  standard library.
- `cluau_translator` (the translator, `include/cluau/cluau.h`, and the
  Luau-to-cluau converter, `include/cluau/convert.h`) has no dependencies.
- `cluau_runtime` (`include/cluau/runtime.h`, the script runner and checker
  behind the `cluau` tool) runs on Luau's VM, compiler and analysis, which
  CMake fetches (or `-DCLUAU_LUAU_DIR=<a Luau checkout>`), and on the
  libraries behind its optional modules, fetched and linked in too (see
  [Building](#building) for the options that leave them out).
- `cluau_check` (the strict-typing rules: strict mode, no `any` without a
  line exemption, required annotations) runs over Luau's analysis and builds
  against the host's Luau.

## Installing

On Linux or macOS:

```sh
curl -fsSL https://github.com/odd-fish-society/cluau/releases/latest/download/install.sh | sh
```

On Windows (PowerShell):

```powershell
irm https://github.com/odd-fish-society/cluau/releases/latest/download/install.ps1 | iex
```

Each installer downloads the latest release, checks it against the
release's `SHA256SUMS`, installs `cluau` for the current user and puts it on
`PATH`, so `cluau` and `#!/usr/bin/env cluau` scripts work from any
directory. Neither needs administrator rights.

- Linux and macOS: `~/.local/bin/cluau`, with a `PATH` line added to the
  shell's startup file when that directory is not on `PATH` yet. Options
  (after `sh -s --`): `--version vX.Y.Z`, `--prefix DIR` (for example
  `--prefix /usr/local`, run with `sudo`), `--no-modify-path`,
  `--uninstall`.
- Windows: `%LOCALAPPDATA%\Programs\cluau\bin\cluau.exe`, added to the
  user's `PATH`. Options: `-Version vX.Y.Z`, `-InstallDir`,
  `-NoModifyPath`, `-AssociateFiles` (open `.cluau` files with cluau),
  `-AddToPathExt` (also run a script on `PATH` by its name alone: `deploy`
  for `deploy.cluau`), `-Uninstall`. Windows' default execution policy
  does not run a downloaded script file, so pass the options to the script
  as a script block, or run the downloaded `install.ps1` with the policy
  bypassed for that run:

  ```powershell
  & ([scriptblock]::Create((irm https://github.com/odd-fish-society/cluau/releases/latest/download/install.ps1))) -Uninstall
  powershell -ExecutionPolicy Bypass -File .\install.ps1 -Version vX.Y.Z
  ```

  The environment variables `CLUAU_VERSION` and `CLUAU_INSTALL_DIR` stand
  in for `-Version` and `-InstallDir` with the `irm ... | iex` form (and
  `CLUAU_VERSION` and `CLUAU_PREFIX` for `install.sh`'s options).

Run an installer again to update: a new release whose `cluau --version`
does not run on the system leaves the installed one as it was. The installers are
[`installer/install.sh`](installer/install.sh) and
[`installer/install.ps1`](installer/install.ps1). There are release builds
for Linux (x86_64 and arm64; any distribution with glibc 2.28 or later:
RHEL 8, Debian 10, Ubuntu 20.04 and newer), macOS (11 Big Sur or later,
Apple silicon and Intel) and Windows (x86_64, which Windows on Arm runs
too; no Visual C++ redistributable needed). `CLUAU_REPOSITORY` points either
installer at another copy of the repository's releases (a Forgejo or Gitea
server's, which may live under a path: `https://forge.example.org/owner/cluau`,
`https://example.org/git/owner/cluau`).

Each release also has Debian and RPM packages for Linux, which install
`/usr/bin/cluau` for every user:

```sh
sudo apt install ./cluau_0.33.1_amd64.deb                   # Debian, Ubuntu (arm64: _arm64.deb)
sudo dnf install --nogpgcheck ./cluau-0.33.1-1.x86_64.rpm   # Fedora, RHEL (arm64: .aarch64.rpm)
```

The packages are not signed yet (`--nogpgcheck` lets dnf install one); the
release's `SHA256SUMS` lists their checksums.

## Releases

Each version is a tag (`vX.Y.Z`), and each tag is a
[release](https://github.com/odd-fish-society/cluau/releases) with the `cluau` tool
(scripts, the REPL, check, translate, convert) built for Linux (x86-64 and arm64),
macOS (arm64 and x86-64) and Windows (x86-64), its tests run on each, both installers
tried against the release's own packages on each, and a `SHA256SUMS` file.
A host that embeds the translator pins a tag and builds from source.

## Building

```sh
cmake -S . -B build && cmake --build build --config Release && ctest --test-dir build -C Release
./build/cluau script.cluau args...
./build/cluau check script.cluau
./build/cluau test
./build/cluau fmt --check
./build/cluau
./build/cluau translate file.cluau
./build/cluau convert --verify file.luau
```

The build is a release (optimised) build unless `-DCMAKE_BUILD_TYPE` names
another. With a multi-configuration generator (Visual Studio, Xcode), the
configuration is chosen when building and testing: `--config Release` and
`-C Release` above (with Visual Studio the executable is
`build/Release/cluau.exe`). With MSVC, the C and C++ runtime is linked into
`cluau.exe` (`/MT`); `-DCMAKE_MSVC_RUNTIME_LIBRARY=...` chooses another.
CMake 3.21 or later builds it.

`cmake --install build` puts `cluau` on the install prefix's `bin` (add
`--prefix ~/.local` for a user install) with the guides and the licences
beside it; a release's archive holds the same tool, to put on `PATH` by
hand.

The build fetches Luau 0.748 for the runtime and the checker (`-DCLUAU_LUAU_VERSION=<tag>` picks another release);
`-DCLUAU_LUAU_DIR=<dir>` uses a local checkout instead (Luau 0.748 or later), and
`-DCLUAU_BUILD_RUNTIME=OFF` builds the translator and the tool's translate,
convert and fmt commands with no Luau at all.

Each library the standard modules use is fetched at a pinned release (by
its hash) and linked in, so the tool stays one file. An option leaves one
out, with the modules on it:

| option | builds | on |
|---|---|---|
| `CLUAU_HTTP` | `@std/http` | libcurl, mbedTLS |
| `CLUAU_CRYPTO` | `@std/crypto` (and the WebSocket handshake in `@std/websocket` and `@std/server`) | mbedTLS |
| `CLUAU_SQLITE` | `@std/sqlite` | SQLite |
| `CLUAU_REGEX` | `@std/regex` | PCRE2 |
| `CLUAU_COMPRESS` | `@std/compress`, `@std/zip` (and gzip in `@std/tar`) | miniz |

Each is on by default; `-DCLUAU_HTTP=OFF` (and so on) turns one off. TLS in
`@std/net` is built when either `CLUAU_HTTP` or `CLUAU_CRYPTO` is on; with
both off, mbedTLS is not fetched at all. `cluau --help` lists the modules a
build has, and `cluau check` reports requiring one it leaves out.
`-DCLUAU_FUZZ=ON` (with clang, and `CLUAU_BUILD_TESTS`) builds the fuzz
targets in `tests/fuzz`.

`ctest` runs the translator's
tests, the script tests (`tests/scripts/*.cluau` against their `.expected`
output, and `cluau check` on each) and the checker's tests
(`tests/check/*.cluau` against their `.checked` output), `@std/http`
against a local HTTP and HTTPS server (`tests/http/run.py`), and on Linux
and macOS the prompt driven in a pseudo-terminal (`tests/repl_terminal.py`).

`hamr run` runs the same checks as a [hamr](https://github.com/odd-fish-society/hamr)
suite (`hamr.cluau`): the tests, the script tests and the checker's tests in
a release and a debug build, the guides' and the README's examples through
the tool (and the scripts that show their output, run), and cluau in a
host's CMake build (`cmake/host`). Each build lives in its own
`build-hamr-<name>` directory.

A host embeds the translator with `add_subdirectory` (CMake, linking
`cluau_translator`) or its own build manifest over `src/translate/*.cpp`
(plus `src/convert/*.cpp` for the converter), and supplies a
`ClassProfile` for what its classes lower to. Under `add_subdirectory` the runtime is off (no Luau
is fetched); `-DCLUAU_BUILD_RUNTIME=ON` adds it, against the host's own
Luau targets when they exist. Adding cluau leaves the host's own settings
(`BUILD_SHARED_LIBS`, `BUILD_TESTING`, its build type and MSVC runtime)
as they were, and `cluau_translator` is position-independent, so it links
into a host's shared library (a plugin, an editor module).

## Hosts

cluau was written for [TinyEngine](https://github.com/Aosiee) (its gameplay
scripts) and its build tool Kiln (build manifests), and has no dependency on
either.

## Licence

cluau is released under the Odd Fish Society License ([`LICENSE`](LICENSE)), which is
MIT with one added condition. A product that includes cluau, is written in cluau or is
built with it, credits it in its credits, about box or documentation, as "Uses cluau -
Copyright (c) 2026 Odd Fish Society / Felipe Lara". A product that uses several Odd Fish
Society libraries can credit them together, on one line or several, such as "Uses kiln,
cluau and wisp - Copyright (c) 2026 Odd Fish Society / Felipe Lara". No logo or splash
screen is required.

The `cluau` tool contains Luau, isocline, libcurl, Mbed TLS, PCRE2, SQLite
and miniz, each under its own licence (MIT, the curl licence, Apache 2.0,
BSD, public domain): [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)
holds their notices, and the release downloads, the packages and the
installers ship it beside the tool. The translator library alone
(`cluau_translator`) contains none of them.
