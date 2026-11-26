# Changes

Each release's notes. A release's tag message is its section here.

## 0.32.0: patterns, spreads, rest parameters and async functions

Language:
- Destructuring patterns rename (`const { name: who } = person`), nest
  (`{ pos: { x, y } }`, `[[a, b], c]`), give defaults to fields and items
  that are nil (`{ port = 8080 }`, evaluated only then and able to use the
  names before them), and take a list's rest (`[head, ...tail]`).
- A loop takes its variables apart the same way: `for (_, { name, size }
  in ipairs(files))`.
- Rest parameters: `function sum(...numbers: number)` gives the body a
  list of the arguments from there on, typed `{ number }`.
- Spreads: `f(a, ...items)` passes a list's items as the last arguments,
  and `{ 0, ...items, ...defaults, port = 8080 }` adds a list's items in
  place and copies a table's other fields, later fields replacing earlier
  ones. The new table is typed as all of its parts together.
- Async functions (scripts): `async function f(): string` runs its body as
  an @std/task task and returns the task, typed `Task<string>`. `await t`
  waits for a task and gives back what it returned, anywhere in a script.
  An error in an async function's task is raised where it is awaited, so a
  `try` around the `await` catches it, with the task's own stack in the
  report; one that nothing awaits is raised once the script's own code has
  finished. Methods can be async.
- Luau's explicit type arguments, `f<<T>>(x)`, are documented and tested
  through cluau.

Standard library:
- `task.async(fn, ...)` starts a task as `task.spawn` does, but keeps an
  error in it for `task.join` to raise where the task is joined.

Fixes:
- A parameter default containing `??` or `?.` translated into broken
  Luau.

Tooling: highlighting, completion and the converter know `async` and
`await` (the converter reports a Luau name `await` used as a call, which a
script would read as the keyword).

## 0.31.0: TOML, collections, random numbers and logging

Four new standard modules, each available in the sandbox:
- @std/toml: `toml.decode` reads all of TOML 1.0.0 (every key, string,
  number and date form, inline tables, arrays of tables, and every rule
  against defining something twice, with the line and column of what is
  wrong); `toml.encode` writes a table back, keys sorted, floats so they
  read back exactly. Dates and times are `DateTime` values, and an integer
  beyond what a number holds exactly (2^53) is an error rather than a
  rounded value. Compared with Python's tomllib on about 240,000
  generated documents, with no disagreement.
- @std/collections: stable sorting (`sorted`, `sortBy`, `minBy`,
  `maxBy`), `counter` and `mostCommon`, `groupBy`, `unique`, `chunk`,
  `zip`, `flatten`, `range`, `slice`, `map`, `filter`, `reduce`, `some`,
  `every`, `sum`, `find`, `indexOf`, sorted `keys` and `values`, an
  insertion-ordered `Set<T>` and a `Deque<T>`, all typed generically.
- @std/random: reproducible random numbers from a seed (xoshiro256**),
  the same sequence on every platform: `random`, `integer` (unbiased),
  `uniform`, `normal`, `choice`, `shuffle`, `sample`, `weighted`, on a
  shared generator or one of a script's own (`random.new(seed)`). Not for
  secrets: @std/crypto's `randomBytes` is.
- @std/log: levels, named loggers, fields bound with `with`, text or JSON
  lines to standard error, standard output or a function, and the level
  from `CLUAU_LOG` unless the script sets one. A log call never raises
  because of what it is given to log.

## 0.30.0: defaults, nil operators, destructuring, switch and generators

Language:
- Parameters take defaults: `function greet(name: string, greeting:
  string = "Hello")`. A left-out or nil argument takes the default,
  evaluated at that call; to callers the parameter is optional, in the
  body it has its type. Methods take them too.
- `a ?? b` is `a` unless it is nil, then `b` (evaluated only then), and
  `x ??= value` assigns only a nil. `a?.b`, `a?[k]` and `a?:m()` are nil
  when `a` is, without reading further; as a statement, `a?:m()` calls
  only when there is something to call. Both are typed: `a ?? b` has
  `a`'s type without nil, or `b`'s.
- `const { name, age } = person` and `let [first, second] = list` declare
  a local for each field or item, the value evaluated once.
- `switch (value) { case a, b: ... default: ... }` compares with `==`,
  runs the first case that matches and no other, and narrows a name's
  type as an `if` does. A `break` in a case is an error, since there is
  no fall-through for it to stop.
- `function*` declares a generator: its body runs as a loop asks for
  values, up to each `yield`. Its declared type is what it yields, and
  each `yield` is checked against it.
- `cluau fmt` indents a switch's cases and the statements under them; the
  VS Code grammar knows the new keywords and operators; the converter
  reports a Luau call that would read as a switch statement.

## 0.29.1: what the second review left

- On Windows, a program the script starts is given its standard input,
  output and error and nothing else, as on Linux and macOS. It used to
  inherit every inheritable handle in the script, sockets and the files
  @std/fs had open among them, and held them for as long as it ran: a
  file the script had closed could not be removed.
- @std/regex: `replace`, `split` and `matches` through a long text that
  is not valid UTF-8 take time in proportion to its length, as they do
  for valid text (120 KB with one invalid byte took 3 to 18 seconds, now
  milliseconds). The text is searched piece by piece between its invalid
  sequences, as PCRE2 itself does, and a pattern that starts with `.*`
  now finds its matches after an invalid byte too.
- `cluau check` accepts `getmetatable` on any value, as the runtime does
  (it took only tables with a metatable); a table's metatable keeps its
  type.
- `server.Answer` includes what `server.upgrade` answers with, so a
  handler that upgrades is typed as it is. The standard modules have no
  `any` left in them.
- A task that cancels itself where it cannot yield ends with the error
  "task.cancel: the task was cancelled", which a pcall of its own sees.
- Release workflow: actions are pinned to commits, and the VS Code
  download for the editor test is checked against its SHA-256.

## 0.29.0: a second, deeper review

A second review of the whole of cluau, area by area, with each fix covered
by a test. Beyond reading every line, it tested against references:
generated programs against hand-written Luau, the converter round-tripping
Luau's own sources, JSON, CSV, tar, URLs, compression and hashes against
Python's libraries and published test vectors, and the servers against
the HTTP and WebSocket RFCs with clients made by hand. Coverage, the
sanitizers and longer fuzzing pointed it at code the tests never ran.
Some behaviour changes on purpose; those are listed under "Behaviour
changes".

Security:
- @std/server: no client can end a server any more. A WebSocket upgrade
  whose client has gone, a handler answering with the wrong kind of value,
  and any other error on one connection end only that connection (a 500
  where an answer is still possible). A new `headTimeout` option (30
  seconds) bounds how long a request's head may take, so a client sending
  a byte now and then cannot hold a connection; large bodies and served
  files go out in pieces, so a slow reader holds little memory; a stream
  of empty WebSocket fragments is bounded.
- `server.files` follows a symbolic link only to a file inside its
  directory (a new `fs.realpath` resolves links).
- @std/http and `websocket.connect` refuse header names, values, methods,
  protocols and URLs that could inject into a request; a redirect to
  another host drops the request's own headers but a few harmless ones;
  error messages no longer show a URL's password.
- `tar.extract` refuses names holding a NUL byte (which could write outside
  its directory), and every @std/fs and @std/path function refuses a path
  holding one instead of cutting it short.
- Packages: install never writes or deletes through a symbolic link: a
  `cluau_modules`, `cluau.toml` or `cluau.lock` that is a link is refused,
  and a package's own `.cluau-commit` link is not followed. A commit
  pinned in `cluau.lock` is checked against `cluau.toml` (a tag's must be
  the tag's, a branch's must be on the branch); git's `ext::` transport
  is always refused.
- `fs.remove(dir, true)` no longer makes files outside the tree writable
  through symbolic links.
- The sandbox no longer reveals the working directory through
  `path.absolute` and `path.relative` (they take absolute paths only), or
  which files exist outside it through `require`'s error.
- Zip archives whose entries share data, whose local headers disagree
  with the directory, or whose entries claim too much are refused when
  opened, so extracting an archive from anywhere writes a bounded amount.
- Windows: batch-file arguments are escaped for cmd.exe (the "BatBadBut"
  injection); programs are found on `PATH` with `PATHEXT` and not from the
  current directory first; only trusted root certificates are trust
  anchors, not intermediates.

Language and tools:
- Interpolated string segments translate exactly like any other code:
  functions with bodies, comments, long strings and if-expressions work
  inside `{...}`.
- A class field whose default also appears in its names (`class Vector3 {
  Z: number = 3 }`), a comment right after a minus, and `return` inside
  `try` with its value on the next line all translate correctly.
- At the prompt, an unclosed type is reported instead of dropping the rest
  of the input; a value whose `__tostring` fails, and completing a class's
  inherited static, no longer crash it; it ends when its terminal closes
  or it is sent SIGTERM (it kept running at full speed).
- `for (k, v in SomeClass)` iterates the class's members (it constructed
  instances forever).
- A script run through a symbolic link finds the modules beside the file
  it links to, as `cluau check` does; `cluau check` accepts requiring a
  module that returns nothing.
- `cluau convert` handles backslash-continued strings at CRLF line ends,
  comments after `/`, multi-line `until` conditions, `T...` returns and
  more, and `--verify` parses both sides with Luau and compares the
  programs, so it no longer passes output that does not compile.
- `cluau test`: `process.exit` in code under test fails that case instead
  of ending the run with its status, and a task a case starts is waited
  for and its failure reported against that case.
- `cluau profile` gives a blocking call's time to the line that made it
  and names task and coroutine bodies.
- `cluau check` types `for (k, v in SomeClass)` over a class the file
  declares; a class field typed by a string continued onto the next line
  no longer shifts the lines after it; `cluau fmt` is idempotent on a
  carriage return inside a line.
- The standard modules written in cluau pass `cluau check` themselves,
  and the tests keep them so.

Language server and debugger:
- Opening a table holding a list of more than six items no longer ends
  the debugged script; an expression or `__tostring` that never returns
  is stopped after two seconds instead of freezing the session.
- Stepping over `task.wait` in a task stops at the task's next line; an
  error that ends a task pauses in the task, with its locals.
- Breakpoints, pause and Stop work during blocking calls without
  `@std/task`; a breakpoint in a file not yet loaded shows unverified until
  it loads; one on a line that only closes a block moves to the block's
  last line with code.
- Every debugger message is valid UTF-8 JSON; expressions see a local
  holding nil over a global of the same name.
- Stepping and breakpoints in `try`, `catch` and `finally` land on the
  script's own lines: a breakpoint on `} catch (e) {` pauses in the catch
  block when it catches.
- The JSON the editor servers read follows RFC 8259.
- The language server's strict-typing diagnostics have stable ranges;
  `require` paths are completed; hover works on a declaration's own name.

Runtime and standard library:
- Tasks: finished tasks are released (a long-running server's memory
  stays flat); a job finishing just as the scheduler looked no longer
  leaves a wait in the script's own code waiting for good; `task.wait(n)`
  never returns early; `task.cancel` takes constant time.
- Processes: one child can be shared by tasks (reads share its output,
  every `wait` gets the exit code); Ctrl-C stops `process.sleep` and every
  wait on a program, with or without `@std/task`, and kills the program; a
  script waiting on standard input in a task exits at once; programs a
  script started are killed when it is interrupted; `cluau x | head -1`
  stops at the closed pipe; a cancelled task's pending child or stdin read
  no longer swallows a line. A run that a Ctrl-C ends also ends the
  programs it started that nothing was waiting on.
- A cancelled task's socket read gives back what it had read, and a
  connection accepted for a cancelled `accept` goes to the next one.
- Output that cannot be written at exit (a full disk, a closed pipe) is
  an error: cluau says so on stderr and exits 1 instead of 0.
- `db:transaction` rolls back and raises when its commit fails (later
  writes were lost); `db:close()` finalizes statements and ends an open
  transaction.
- Regex: matching around empty matches agrees with Perl and Python, `\K`
  matches are kept, memory and the work of one call are bounded, and
  `replace`, `split` and `matches` take time in proportion to the text.
- `json.encode` writes a table as an array only when its keys are exactly
  1..n (string keys of a table with holes were dropped).
- Write errors are reported: `fs.write`, `fs.append`, `fs.writeBuffer`, a
  file's `flush` and `close`, and `io.write`/`io.flush` on a full disk or
  a closed pipe.
- `@std/time` rounds to the millisecond (a millisecond was lost in about
  half of all round trips) and behaves the same on every platform.
- `path.normalize` and `path.absolute` leave no trailing `/`.
- `fs.glob`: a pattern ending in `/` matches only directories, `[` with no
  `]` is an ordinary character, and `?` matches one character, not byte.
- tar reads GNU incremental archives, PAX sizes and negative times
  correctly; url refuses bad ports and keeps empty authorities; csv's
  `records` skips blank lines.

Build and distribution:
- The Windows release no longer needs the Visual C++ redistributable; the
  macOS releases run on macOS 11 and later.
- The releases, packages and installers ship THIRD_PARTY_NOTICES.md with
  the licences of the libraries in the `cluau` tool.
- libcurl is built with HTTP and HTTPS only, as documented (curl 8.23
  dropped the switch the build used, so every protocol was in).
- An installer update whose new binary does not run changes nothing and
  shows the error; `install.ps1 -Uninstall` removes only what it
  installed.
- CMake 3.21 to 3.23 configure again; a host using add_subdirectory keeps
  its own BUILD_SHARED_LIBS and BUILD_TESTING.

Behaviour changes:
- `//` inside an interpolated string's `{...}` starts a comment, as it
  does everywhere else; floor division is `~/`.
- `encoding.fromBase64` refuses text that is not the exact encoding of its
  bytes; `json.encode` refuses a table with two keys that are the same
  JSON key (`[1]` and `["1"]`).
- `process.exit` takes only whole numbers 0 to 255; a script containing a
  NUL byte is an error; times outside the years 0 to 9999 are errors; the
  `interrupted` error carries no source position.
- `csv.records` skips blank lines; `url.parse` refuses a port that is not
  0 to 65535; `@std/args` gives a flag with `short = "h"` the `-h`.
- @std/server reads requests more strictly (header names, control
  characters, exactly one Host, versions 1.0 and 1.1) and drops a
  handler's framing headers; @std/http changes the method on redirects as
  RFC 9110 says.
- One regex call gives up past about ten million steps plus a thousand
  per byte of text.
- `cluau convert --verify` fails input that Luau does not parse.

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
