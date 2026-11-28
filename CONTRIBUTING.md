# Contributing to cluau

Thanks for taking an interest. Bug reports, fixes and improvements are all welcome. This
page explains how to get a change in, and how the maintainers land changes and make
releases.

## Reporting a problem

Open an issue on GitHub. The most useful reports say which cluau you ran (`cluau
--version`) and on which system, the command, what you expected and what happened. A
short script that shows the problem is the best report of all: cluau's own tests are
made of scripts like that. For a type-checking problem, include what `cluau check`
printed; for an installer problem, the installer's output.

A security problem (a way past the sandbox, a release check that can be fooled, a crash
on input a script reads from the network) should not go in a public issue: report it
privately with **Report a vulnerability** on the repository's Security tab.

## Proposing a change

For anything bigger than a small fix, open an issue first and describe what you'd like
to change, so that we can agree on the approach before you spend time on it. Then:

1. Fork the repository and make a branch from `dev`. Pull requests go into `dev`, never
   into `main`.
2. Make the change, with tests (below).
3. Run the tests and the formatter locally.
4. Open the pull request. CI builds and tests it on Linux, macOS and Windows.

Pull requests are rebased rather than merged, so history stays linear. Keep a pull
request to one piece of work, and write commit messages that say what changed and why,
with no attribution lines. Say in the pull request what someone using cluau would
notice, so that it can go in the release's changelog entry.

## Building and testing

You'll need CMake 3.21 or later, a C++17 compiler (GCC, Clang or MSVC), Git and Python 3
(some tests are Python scripts). The build fetches its libraries itself, each at a
pinned release checked by its hash:

```sh
git clone https://github.com/odd-fish-society/cluau.git
cd cluau
cmake -S . -B build && cmake --build build --config Release
ctest --test-dir build -C Release
```

The tests are:

- `tests/*_tests.cpp`: the translator, the converter and the runtime's API.
- `tests/scripts/<name>.cluau`: each runs and its output is compared with
  `<name>.expected`, and `cluau check` passes on it (or reports what `<name>.checked`
  says). Most behaviour is tested this way, and a new feature or a fix usually adds a
  script or a few lines to one.
- `tests/check/<name>.cluau`: `cluau check` alone, against `<name>.checked`.
- `tests/http/run.py`, `tests/upgrade/run.py` and `tests/repl_terminal.py`: `@std/http`
  against a local server, `cluau upgrade` against releases signed for the test, and
  the prompt in a pseudo-terminal.

`ctest --test-dir build -R <name>` runs only the tests whose names match.

`.cluau` files are formatted with cluau itself; CI fails on one that isn't:

```sh
./build/cluau fmt --check src tests hamr.cluau
```

`hamr run` runs all of this as a [hamr](https://github.com/odd-fish-society/hamr) suite
(`hamr.cluau`), in a release and a debug build, with the guides' examples run through
the tool. `hamr run --only fuzz` builds the fuzz targets in `tests/fuzz` with clang and the
sanitizers and runs each for a while: run it after a change to anything that reads
input a script did not write (a parser, a decoder, an archive reader).

## What to keep in mind

The translator rewrites cluau into Luau without moving code to another line, so that an
error's line number is the line in the cluau file. A change to the translator must keep
that; the tests compare positions.

`docs/syntax.md` and `docs/scripting.md` describe how the language and the standard
library behave, and a change in behaviour updates them in the same pull request. The
examples in them are run by `hamr run`, so they have to work.

The standard modules written in cluau (`src/runtime/std/*.cluau`) pass `cluau check`
themselves, in strict mode; a test checks each one.

A new library is a bigger decision than a new module: cluau is one file with everything
in it, so each library is fetched at a pinned release by its hash, has an option that
leaves it out, and has its licence in `THIRD_PARTY_NOTICES.md` (a test keeps that file
in step). Open an issue before adding one.

cluau builds and passes its tests on Linux, macOS and Windows. A change that only works
on one of them isn't finished.

## Licensing of contributions

cluau is released under the Odd Fish Society License (`LICENSE`). Unless you say
otherwise in the pull request, a contribution you send is licensed under the same terms.

---

## For maintainers: branches and releases

cluau keeps three kinds of branch.

- **`main`** is the latest release. Every commit on it is a release and has an annotated
  tag, `v0.x.y`. Nothing is committed to `main` directly: it only moves forward to a
  `dev` commit that CI has passed.
- **`dev`** is where work gathers between releases.
- **Streams**, `stream/<name>`, are where a maintainer builds a feature or a fix. Each
  branches from `dev` and holds one piece of work.

CI (the Linux, macOS and Windows workflows) runs on every push to any branch and on
every pull request. History stays linear: `dev` and `main` only fast-forward.

### Landing a stream or a pull request

1. Rebase it onto the latest `dev`.
2. Check that CI passed on it.
3. Fast-forward `dev` to it and push `dev`.
4. Delete the stream, locally and on GitHub if it was pushed.

If CI fails on `dev`, fix it on `dev` (or on a new stream) before anything else lands.

### Making a release

When `dev` has gathered enough for a release:

1. On `dev`, make one commit named `cluau 0.x.y: <summary>` that sets the version
   (`Version()` in `src/translate/Translate.cpp`, `version` in
   `editors/vscode/package.json` and its `package-lock.json`, and the package names in
   the README's install commands) and adds the release's entry to the top of
   `CHANGELOG.md`. A release with new features raises the minor version; one with fixes
   only raises the patch.
2. Push `dev`, and wait for CI to pass on that commit.
3. Run the Release workflow by hand on `dev` as a dry run: `tag` the new version, `ref`
   `dev`, `publish` off. It builds and tests every release download, tries both
   installers and `cluau upgrade` on each platform, and signs with a test key.
4. Fast-forward `main` to the commit.
5. Tag it with an annotated `v0.x.y` whose message is the changelog entry, headed
   `cluau 0.x.y: <summary>`.
6. Push `main` and the tag. The tag starts the Release workflow, which builds and tests
   everything again, signs `SHA256SUMS` with the release key, checks the signature as
   `cluau upgrade` will, publishes the GitHub release with the tag's message as its
   notes, and publishes the VS Code extension to the Marketplace and Open VSX.
7. Delete the streams the release took in.

The release key is the `MINISIGN_SECRET_KEY` secret in the repository's `release`
environment, which only `v*` tags may use. Its public half, and a spare's that is kept
offline, are in `installer/release-keys.pub`. Should the release key ever have to be
replaced, the spare signs from then on: releases already installed trust it, because
they carry both keys.
