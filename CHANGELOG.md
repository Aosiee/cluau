# Changes

Each release's notes. A release's tag message is its section here.

## 0.26.0: @std/server and @std/websocket

- `@std/server`: an HTTP/1.1 server (`serve`, keep-alive, chunked
  request bodies, HTTPS), a router (`/users/:id`, `*`), files served from
  a directory, and WebSocket upgrades. It runs in tasks, beside the rest
  of the script.
- `@std/websocket`: WebSocket connections (`ws://`, `wss://`): text and
  binary messages, fragments joined, pings answered, the close handshake.
- Fixes found by running the tests under the sanitizers and by fuzzing
  (the fuzz targets are in `tests/fuzz`):
  - `zip.create(...):finish()` leaked the archive's buffer.
  - Two first TLS handshakes at once could build mbedTLS's list of
    cipher suites together.
  - Some damaged deflate data decoded into output without end; inflate and
    gunzip now refuse it.
  - A zip entry could claim any size and have it allocated; one whose
    sizes cannot fit together is refused as damaged.

## 0.25.0: quick fixes, semantic highlighting, the call hierarchy

- `cluau lsp` offers quick fixes: the likely names for a misspelt member
  or global (`p:Lenght()` to `p:Length()`), the `require` of a standard
  module used without one (`json.encode` adds
  `const json = require("@std/json")`), and a line's `allow-any`
  exemption.
- Semantic highlighting: each name by what the checker knows it is (a
  class, a method, a property, a parameter, a local, a function, the
  standard library's), declarations marked.
- The call hierarchy: what calls a function (across the workspace) and
  what it calls.

## 0.24.0: @std/compress and @std/zip

- `@std/compress`: gzip and gunzip (gzip files, joined ones too), deflate
  and inflate (zlib's format, or raw deflate), with a level from 0 to 9.
- `@std/zip`: zip archives, read from a file or a string (`open`,
  `fromString`: `entries`, `read`, `extract`) and made (`create`: `add`,
  `addFile`, `addDirectory`, `finish`). `extract` refuses an entry that
  would land outside its directory.
- Both are on miniz 3.1.2, built in; `-DCLUAU_COMPRESS=OFF` builds without
  them. The sandbox leaves `@std/zip` out.

## 0.23.0: @std/crypto, @std/encoding, TLS connections, more platforms

- `@std/crypto`: hashes (MD5, SHA-1, SHA-224 to SHA-512), HMACs, a hasher
  fed in parts, random bytes from the operating system, random UUIDs, and
  a comparison of secrets in constant time.
- `@std/encoding`: base64 (standard and URL-safe, padded or not) and hex.
- TLS in `@std/net`: `net.connect(host, port, { tls = true })` checks the
  server's certificate against the system's (or `caFile`'s) and its name;
  `net.listen(host, port, { tls = { certFile, keyFile } })` serves TLS. A
  read and a write in two tasks go ahead together on one connection.
- HTTPS transfers in parallel tasks are safe: mbedTLS is built with its
  threading.
- Release builds for Linux on arm64 and macOS on Intel, beside Linux on
  x86_64, macOS on Apple silicon and Windows on x86_64. The Linux builds run
  on any distribution with glibc 2.28 or later (RHEL 8, Debian 10, Ubuntu
  20.04 and newer).
- Debian (`.deb`) and RPM (`.rpm`) packages for Linux on x86_64 and arm64,
  installed and run on Debian 11 and 12, Ubuntu 22.04 and 26.04, Rocky
  Linux 8 and 9, and Fedora before each release is published. They are not
  signed yet: `dnf install --nogpgcheck` installs one.
- `CLUAU_REPOSITORY` points the installers at another copy of the releases,
  such as a Forgejo or Gitea server's.

## 0.22.0: @std/sqlite

- `@std/sqlite`: SQLite databases, with SQLite 3.54.1 built in (full-text
  search and the math functions included). `sqlite.open` opens a file or
  `":memory:"`; a database has `exec`, `query`, `first`, `rows`, `run`,
  `prepare`, `transaction` and `close`.
- Parameters fill `?` in order, or named ones (`:name`, `@name`, `$name`)
  from a table. A row is a table of its columns: `NULL` is `nil`, a `BLOB`
  a `buffer`. A row's type is what the variable it goes to says it is:
  `const users: { User } = db:query(...)`.
- `db:transaction(fn)` commits when `fn` returns and rolls back when it
  raises; nested transactions are savepoints.
- The sandbox leaves `@std/sqlite` out. `-DCLUAU_SQLITE=OFF` builds without
  it.

## 0.21.0: @std/net

- `@std/net`: TCP connections and servers. `net.connect` and `net.listen`;
  a server's `accept` and `close`; a socket's `read`, `readLine`, `lines`,
  `readAll`, `write`, `setTimeout` and `close`, and its local and remote
  addresses.
- Each wait (connecting, accepting, reading, writing) waits on its own in a
  task, so one script serves many connections, or talks to many servers, at
  once. A timeout, a `close` from another task or Ctrl-C ends a wait.
- The sandbox leaves `@std/net` out.

## 0.20.0: blocking calls wait on their own in tasks

- The standard library's blocking calls let the other `@std/task` tasks
  run while they wait: `@std/http`'s requests and downloads, `process.run`
  and `process.exec`, a child's `read`, `readLine`, `lines` and `wait`,
  `io.readLine`, `io.readAll`, `input` and `process.sleep`. Three downloads
  in three tasks take as long as the slowest. In the script's own code the
  tasks run until the call is done; without `@std/task` the calls block as
  before.
- Starting a program is safe beside other threads: the child's
  environment is made before it starts, and programs are started one at a
  time, so no child holds another's pipes open.

## 0.19.0: @std/regex and @std/task

- `@std/regex`: regular expressions as Perl and Python write them, on
  PCRE2 10.49. `compile`, and a pattern's `test`, `match`, `matches`,
  `replace` (with `$1`, `${name}` and `$&`, or a function) and `split`;
  Unicode-aware over UTF-8 text, with 1-based byte positions. A pattern
  that backtracks too long stops with an error. `-DCLUAU_REGEX=OFF` builds
  without it.
- `@std/task`: tasks that run side by side on the script's thread:
  `spawn`, `delay`, `wait`, `join` (typed by the task's function), `cancel`
  and `running`. The script's own waits run the other tasks; the tasks left
  when the script's last line has run go on to their end; an error in a
  task is reported with the task's own stack.
- The translator reads generic type packs in types (`R...`), as in a return
  type `: R...`.

## 0.18.0: test coverage and cluau profile

- `cluau test --coverage` counts the lines the tests ran in every file they
  loaded (not the test files, nor packages) and reports each file's lines
  run and the lines that never ran; `--lcov <file>` writes the counts as
  LCOV for editors and CI services.
- `cluau profile script.cluau` runs a script and reports where its time
  went: each function's own and total time, and the busiest lines, from
  samples taken about once a millisecond. `--folded <file>` writes the
  samples as folded stacks for flame graph tools.

## 0.17.0: references across files, workspace symbols, inlay hints

- `cluau lsp` finds a class member's or a module's exported name's
  references in every file of the workspace that uses it, open or not, and
  renames it in all of them; a local's stay within its file, and a standard
  module's member is not renamed.
- The workspace's classes, members, functions and variables are listed
  for the editor's symbol search, and the types of names declared without
  one show beside them (inlay hints).
- `cluau debug` runs a test file's `@std/test` cases (the launch's `test`
  and `filter`; the default in VS Code for a `*_test.cluau` file).
- A step inside a function called from C (a sort comparator, a metamethod)
  steps. A module whose top level fails can be required again under the
  debugger.

## 0.16.0: cluau debug

- `cluau debug`: a debugger for editors (the Debug Adapter Protocol, on
  standard input and output). Breakpoints with conditions, hit counts and
  log messages; pausing on uncaught errors, or on every error; stepping
  over, into and out of calls, into a module's top level and through
  coroutines; pausing a running script; the paused frames' locals and
  upvalues, tables and class instances field by field, and cluau
  expressions evaluated in a frame. The script's output arrives in the
  debug console.
- The VS Code extension registers the debugger: F5 debugs the current
  file, and *cluau: Debug File* joins *cluau: Run File*. Neovim (nvim-dap)
  and Helix settings are in the scripting guide.

## 0.15.0: cluau lsp and the VS Code extension

- `cluau lsp`: a language server, on the same checker as `cluau check`:
  the checker's findings as you type, completion with types, hover with the
  comment above a declaration, signature help, go to definition,
  references and renaming of locals, the outline, and formatting.
- The VS Code extension (`editors/vscode`): highlighting from a TextMate
  grammar, the language server, and *cluau: Run File*. Each release
  attaches it as a `.vsix`; it is published to the VS Code Marketplace and
  Open VSX. Neovim and Helix settings are in the scripting guide.

## Earlier releases

- 0.14.0: native code, host modules, the sandbox.
- 0.13.0: packages from Git repositories (`cluau.toml`, `cluau.lock`,
  `cluau add`, `install`, `update`, `remove`).
- 0.12.0: `try` / `catch` / `finally`, `throw`, enums, abstract classes.
- 0.11.0: `cluau test` and `cluau fmt`.
- 0.10.0: `@std/http`.
- 0.9.0: the standard library, part one (`fs.glob`, files, `@std/time`,
  `@std/args`, `@std/csv`, `process.spawn`).
- 0.8.0: the prompt (Ctrl-C, pasting, completion) and `cluau check`'s
  options.
- 0.7.1: Luau 0.748.
- 0.7.0: subclasses, commands from argument lists, the installers.
- 0.6.0: scripts (`cluau script.cluau`, the REPL, `require`, `@std`) and
  `cluau check`.
- 0.5.0 and earlier: the translator for hosts (classes, signals, the
  profile's hooks).

The tag of each release holds its full notes.
