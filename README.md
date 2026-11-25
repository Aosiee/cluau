# cluau

[![Linux](https://github.com/Aosiee/cluau/actions/workflows/linux.yml/badge.svg?branch=main)](https://github.com/Aosiee/cluau/actions/workflows/linux.yml)
[![macOS](https://github.com/Aosiee/cluau/actions/workflows/macos.yml/badge.svg?branch=main)](https://github.com/Aosiee/cluau/actions/workflows/macos.yml)
[![Windows](https://github.com/Aosiee/cluau/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/Aosiee/cluau/actions/workflows/windows.yml)
[![Release](https://img.shields.io/github/v/release/Aosiee/cluau)](https://github.com/Aosiee/cluau/releases/latest)

**C-syntax-flavoured Luau.** cluau is a Luau dialect: braces, C operators,
`let` / `const`, classes, `try` / `catch` and enums, translated to standard
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
HTTP (a client and a server), WebSockets, TCP and TLS sockets, SQLite databases, hashing, compression and zip
archives, regular expressions, tasks that run side by side, command-line
arguments and CSV, and
`cluau check script.cluau` type-checks it (and the modules it requires)
without running it, `cluau test` runs its test files (with
`--coverage`, the lines they missed), `cluau profile` shows where its time
goes, `cluau fmt` formats it, and `cluau add` brings in packages from Git repositories. Scripts run
as native code on x64 and arm64, and `cluau --sandbox` runs one without
file, network or process access. `cluau lsp` is a language server for
editors, `cluau debug` a debugger for them, and the VS Code extension
([`editors/vscode`](editors/vscode/README.md)) brings both, and
highlighting, to VS Code.

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
  CMake fetches (or `-DCLUAU_LUAU_DIR=<a Luau checkout>`), and `@std/http` on
  libcurl and mbedTLS, fetched and linked in too (`-DCLUAU_HTTP=OFF` leaves
  them out).
- `cluau_check` (the strict-typing rules: strict mode, no `any` without a
  line exemption, required annotations) runs over Luau's analysis and builds
  against the host's Luau.

## Installing

On Linux or macOS:

```
curl -fsSL https://github.com/Aosiee/cluau/releases/latest/download/install.sh | sh
```

On Windows (PowerShell):

```
irm https://github.com/Aosiee/cluau/releases/latest/download/install.ps1 | iex
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
  user's `PATH`. Run the script itself (`.\install.ps1`, downloaded from the
  release) for its options: `-Version vX.Y.Z`, `-InstallDir`,
  `-NoModifyPath`, `-AssociateFiles` (open `.cluau` files with cluau),
  `-AddToPathExt` (also run a script on `PATH` by its name alone: `deploy`
  for `deploy.cluau`), `-Uninstall`.

Run an installer again to update. The installers are
[`installer/install.sh`](installer/install.sh) and
[`installer/install.ps1`](installer/install.ps1). There are release builds
for Linux (x86_64 and arm64; any distribution with glibc 2.28 or later:
RHEL 8, Debian 10, Ubuntu 20.04 and newer), macOS (Apple silicon and
Intel) and Windows (x86_64, which Windows on Arm runs too). `CLUAU_REPOSITORY` points either
installer at another copy of the repository's releases (a Forgejo or Gitea
server's: `https://forge.example.org/owner/cluau`).

Each release also has Debian and RPM packages for Linux, which install
`/usr/bin/cluau` for every user:

```
sudo apt install ./cluau_0.26.0_amd64.deb                   # Debian, Ubuntu (arm64: _arm64.deb)
sudo dnf install --nogpgcheck ./cluau-0.26.0-1.x86_64.rpm   # Fedora, RHEL (arm64: .aarch64.rpm)
```

The packages are not signed yet (`--nogpgcheck` lets dnf install one); the
release's `SHA256SUMS` lists their checksums.

## Releases

Each version is a tag (`vX.Y.Z`), and each tag is a
[release](https://github.com/Aosiee/cluau/releases) with the `cluau` tool
(scripts, the REPL, check, translate, convert) built for Linux (x86-64),
macOS (arm64) and Windows (x86-64), its tests run on each, both installers
tried against the release's own packages on each, and a `SHA256SUMS` file.
A host that embeds the translator pins a tag and builds from source.

## Building

```
cmake -S . -B build && cmake --build build && ctest --test-dir build
./build/cluau script.cluau args...
./build/cluau check script.cluau
./build/cluau test
./build/cluau fmt --check
./build/cluau
./build/cluau translate file.cluau
./build/cluau convert --verify file.luau
```

`cmake --install build` puts `cluau` on the install prefix's `bin` (add
`--prefix ~/.local` for a user install) with the guides beside it; a
release's archive holds the same tool, to put on `PATH` by hand.

The build fetches Luau 0.748 for the runtime and the checker (`-DCLUAU_LUAU_VERSION=<tag>` picks another release);
`-DCLUAU_LUAU_DIR=<dir>` uses a local checkout instead (Luau 0.748 or later), and
`-DCLUAU_BUILD_RUNTIME=OFF` builds the translator and the tool's translate
and convert commands with no Luau at all. `ctest` runs the translator's
tests, the script tests (`tests/scripts/*.cluau` against their `.expected`
output, and `cluau check` on each) and the checker's tests
(`tests/check/*.cluau` against their `.checked` output), `@std/http`
against a local HTTP and HTTPS server (`tests/http/run.py`), and on Linux
and macOS the prompt driven in a pseudo-terminal (`tests/repl_terminal.py`).

`hamr run` runs the same checks as a [hamr](https://github.com/Aosiee/hamr)
suite (`hamr.cluau`): the tests, the script tests and the checker's tests in
a release and a debug build, and the guides' examples through the tool.
Each build lives in its own `build-hamr-<name>` directory.

A host embeds the translator with `add_subdirectory` (CMake, linking
`cluau_translator`) or its own build manifest over `src/translate/*.cpp`
(plus `src/convert/*.cpp` for the converter), and supplies a
`ClassProfile` for what its classes lower to. Under `add_subdirectory` the runtime is off (no Luau
is fetched); `-DCLUAU_BUILD_RUNTIME=ON` adds it, against the host's own
Luau targets when they exist.

## Hosts

cluau was written for [TinyEngine](https://github.com/Aosiee) (its gameplay
scripts) and its build tool Kiln (build manifests), and has no dependency on
either.

## Licence

The cluau License: MIT plus a credit line in the credits of every product that
includes cluau or is made with it. No splash screen or logo is required. See
[`LICENSE`](LICENSE).
