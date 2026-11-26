# Changes

Each release's notes. A release's tag message is its section here.

## 0.28.0: a full review, fixes and hardening

A review of the whole of cluau, with each fix covered by a test. Some
behaviour changes on purpose; those are marked.

Language and tools:
- A statement after a line ending in `x++`, `x--`, an optional type's `?`,
  a generic's `>` or `...` was read as part of an expression (an `if` on
  the next line became an if-expression); the converter had the same
  rule.
- Class field defaults keep their text: spaces inside strings, multi-line
  values (and so the lines after them), comments, and function values.
- `return` and `throw` inside `try` take a value that goes on to the next
  line; `...` works inside `try` and `catch`; `static Name = function`
  works; a `try` inside a `finally` cannot leave it; `throw` in a host
  file no longer calls a local named `error`.
- Comments: a line comment starting `//[[` no longer swallows the code
  after it, and a block comment ending in `]` translates; comments inside
  a method's parameter list are allowed.
- Nested interpolated strings translate their operators; a string
  continued with a backslash at a CRLF line end lexes; `try` and its `{`
  may be on separate lines; `for (local i = ...)` works.
- Very deep nesting is an error, not a crash.
- `cluau fmt` keeps each line's own line ending and does not indent after
  a generic's `>`.
- `cluau convert`: a call whose table starts on the next line stays a call;
  `function() do ... end end`, `until` before another statement, and
  `throw`/`try` used as ordinary names convert correctly.
- `cluau check` reports requiring a module the build leaves out, and
  `cluau --help` lists the modules a build has.
- A UTF-8 byte order mark at the start of a file is skipped.

Language server, debugger and profiler:
- The language server no longer crashes after a file whose class extends
  another module's is closed; a "module not found" clears when the file
  appears; built-in globals are not renamed; references without the
  declaration drop the declaration, not a use; semantic tokens cover each
  use of a local; position lookups no longer slow large files down; bad
  messages get the protocol's error answers.
- The debugger steps into and out of coroutines, answers the editor while
  the script waits (breakpoints, pause, terminate), stops on each call of
  a callback a C function makes, never stops past a file's end, pauses on
  errors `coroutine.resume` catches, and `test` defaults to on for a
  `*_test.cluau` program. Terminating is a clean exit.
- `cluau profile` counts a wait in a standard module under the script's
  line, names a module's top level, and refuses an unwritable `--folded`
  file before running.

Runtime and standard library:
- `process.run` with a large `input` no longer deadlocks, and its timeout
  holds while it writes; a Ctrl-C ends the program it runs. A program
  reading the terminal stays in the terminal's process group.
- SIGPIPE is ignored once for the process (and restored in each program
  it starts) instead of around each call, which could end cluau when two
  calls overlapped.
- `child:write` waits in its own task, so a task reading the child's
  output keeps running.
- `@std/task`: a task whose blocking call finished at the wrong moment
  could be dropped; a wait cut short by another task's error left later
  waits returning at once; `task.wait` where a task cannot yield (a sort's
  comparison, `db:transaction`) waits in place; many tasks no longer slow
  each other down.
- `@std/net`: sockets no longer block a write past its timeout; two
  accepts waiting for one client no longer hang; a TLS server's handshake
  happens on the connection's first read or write, so a slow client holds
  up only itself (a failed one reads as a connection that ended);
  `listen` checks the key file; `socket:readLine(limit)` refuses a line
  longer than `limit` bytes.
- `@std/server`: request smuggling closed off (Transfer-Encoding with
  Content-Length, codings other than chunked, malformed lengths and chunk
  sizes are 400s); a body that ends early never reaches the handler;
  header lines and heads are limited (431); `Expect: 100-continue` is
  answered; response headers with CR or LF are refused; a failed accept
  is retried instead of ending the server; `close()` closes idle
  keep-alive connections; dotfiles and paths with NUL are not served;
  `/users/a%2Fb` matches `/users/:id`.
- `@std/websocket`: a frame cut short ends the connection instead of
  arriving empty; the protocol's rules are checked (masking, reserved bits
  and opcodes, control frames, UTF-8 text, the message size); IPv6 hosts
  keep their brackets; masking is about four times faster.
- `@std/tar`: names over 100 bytes that cannot be split are written as PAX
  records (this crashed); GNU long link names are read; devices, FIFOs and
  other special entries are never extracted; input that is not a tar
  archive is an error. `@std/tar` and `@std/websocket` load in builds
  without `@std/compress` or `@std/crypto`.
- `@std/url`: a `Url` keeps its path as written (`rawPath`), so `%2F`
  survives a round trip; `resolve` keeps empty path segments and an empty
  query.
- Packages: a repository or ref starting with `-` is refused and git is
  always given `--` before addresses (a crafted dependency could run a
  command); `cluau add` handles `user@` in addresses; package names are
  checked; a failed `add` leaves `cluau.toml` as it was; manifests may hold
  arrays; short commit ids resolve.
- Behaviour changes: `db:run`, `query`, `first`, `rows` and `prepare`
  refuse SQL with more than one statement (`db:exec` runs several);
  `require` loads only `.cluau` and `.luau` files; `json.decode` refuses
  numbers JSON does not allow (`01`, `1.`); zip entry names match case.
- Smaller fixes: `json.encode(v, 4)` indents by four spaces; a lone
  surrogate in JSON keeps the character after it; `time.parse` refuses
  days a month does not have; `file:read(count)` reads in pieces;
  `fs.walk` passes over a directory it cannot read; a command argument
  holding NUL is refused; an uncaught error object's `__tostring` is used;
  nested `@std/term` styles keep the outer one and `term.strip` removes
  links; `regex.escape` escapes white space; `split` needs a limit of 1 or
  more; gunzip passes over zero padding; SQLite stores whole numbers in its
  integer range as integers and refuses NaN; `csv.encodeRecords` writes
  `false`; `test.near` fails on NaN; `@std/args` reads `-inf` as flags
  when they are declared.
- mbedTLS starts when a script first uses it (`bench/compare.py`
  compares two builds' start-up time).

Build and installation:
- Luau is fetched by its release archive's hash, like the other
  libraries. Each library's option (`CLUAU_HTTP`, `CLUAU_CRYPTO`,
  `CLUAU_SQLITE`, `CLUAU_REGEX`, `CLUAU_COMPRESS`) is documented, and a
  build with any of them off passes its own tests and builds only the
  fuzz targets it can run.
- The exit-time race between the job threads and the job table (found by
  ThreadSanitizer) is fixed.
- `install.ps1 -Uninstall` removes only what it installed (it removed the
  whole install directory), and the user's `PATH` keeps its `%VARIABLE%`
  entries. `install.sh` adds and removes the `PATH` line of its own prefix
  only, and makes a relative `--prefix` absolute.
- The release workflow runs with read-only access except for the step
  that publishes, and the marketplace tokens reach only the steps that
  use them.

## 0.27.0: @std/term, @std/url and @std/tar

- `@std/term`: whether a stream is a terminal, the terminal's size, and
  colours and styles (left out when the output is not a terminal or
  `NO_COLOR` is set; Windows consoles switched to escape sequences).
- `@std/url`: parsing and formatting addresses, resolving one against a
  base (RFC 3986), percent-encoding and query strings.
- `@std/tar`: tar archives, plain or gzip-compressed, read and made as
  `@std/zip` does; `extract` refuses an entry that would land outside its
  directory.

## 0.26.0: @std/server and @std/websocket

- `@std/server`: an HTTP/1.1 server (`serve`, keep-alive, chunked
  request bodies, HTTPS), a router (`/users/:id`, `*`), files served from
  a directory, and WebSocket upgrades. It runs in tasks, beside the rest
  of the script.
- `@std/websocket`: WebSocket connections (`ws://`, `wss://`): text and
  binary messages, fragments joined, pings answered, the close handshake.
- `install.ps1 -AddToPathExt` adds `.CLUAU` to the user's `PATHEXT`, so a
  script on `PATH` runs by its name alone in cmd.exe and PowerShell
  (`-Uninstall` takes it out again).
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
