# cluau

[![Linux](https://github.com/Aosiee/cluau/actions/workflows/linux.yml/badge.svg?branch=main)](https://github.com/Aosiee/cluau/actions/workflows/linux.yml)
[![macOS](https://github.com/Aosiee/cluau/actions/workflows/macos.yml/badge.svg?branch=main)](https://github.com/Aosiee/cluau/actions/workflows/macos.yml)
[![Windows](https://github.com/Aosiee/cluau/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/Aosiee/cluau/actions/workflows/windows.yml)
[![Release](https://img.shields.io/github/v/release/Aosiee/cluau)](https://github.com/Aosiee/cluau/releases/latest)

**C-syntax-flavoured Luau.** cluau is a Luau dialect: braces, C operators,
`let` / `const` and classes, translated to standard Luau before Luau's type
checker and compiler run. Luau's types, semantics, VM and libraries are
unchanged, and every translated line stays on its line, so Luau's error
messages and line-based debuggers point at the `.cluau` source.

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

- [`docs/syntax.md`](docs/syntax.md) is the language reference.
- `cluau` (the translator, `include/cluau/cluau.h`, and the Luau-to-cluau
  converter, `include/cluau/convert.h`) has no dependencies.
- `cluau_check` (the strict-typing rules: strict mode, no `any` without a
  line exemption, required annotations) runs over Luau's analysis and builds
  against the host's Luau.

## Releases

Each version is a tag (`vX.Y.Z`), and each tag is a
[release](https://github.com/Aosiee/cluau/releases) with the `cluau` tool
built for Linux (x86-64), macOS (arm64) and Windows (x86-64), its tests run
on each, and a `SHA256SUMS` file. A host that embeds the translator pins a
tag and builds from source.

## Building

```
cmake -S . -B build && cmake --build build && ./build/cluau_tests
./build/cluau translate file.cluau
./build/cluau convert --verify file.luau
```

`hamr run` runs the same checks as a [hamr](https://github.com/Aosiee/hamr)
suite (`hamr.cluau`): the tests in a release and a debug build and the
syntax guide's examples through the tool; `hamr run --tier nightly --set
luauDir=<a Luau checkout>` also builds the strict checker. Each build lives
in its own `build-hamr-<name>` directory.

A host embeds the translator with `add_subdirectory` (CMake) or its own build
manifest over `src/translate/*.cpp` (plus `src/convert/*.cpp` for the
converter), and supplies a `ClassProfile` for what
its classes lower to.

## Hosts

cluau was written for [TinyEngine](https://github.com/Aosiee) (its gameplay
scripts) and its build tool Kiln (build manifests), and has no dependency on
either.

## Licence

The cluau License: MIT plus a credit line in the credits of every product that
includes cluau or is made with it. No splash screen or logo is required. See
[`LICENSE`](LICENSE).
