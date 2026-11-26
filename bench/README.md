# Benchmarks

Scripts for timing the runtime: a change to how scripts are compiled or run
is measured on them before and after.

| script | measures |
|---|---|
| `compute.cluau` | arithmetic and calls: a recursive Fibonacci and a numeric loop |
| `tables.cluau` | tables, strings and classes: building, sorting, formatting |
| `startup.cluau` | startup: the standard modules written in cluau, loaded and used once |

Each is timed with a release build, interpreted and as native code:

```sh
time build-rel/cluau bench/compute.cluau
time build-rel/cluau --no-native bench/compute.cluau
```

`compare.py` times two builds on these scripts and a one-line `-e`
(the median of several runs each, the builds taking turns; a run that fails
stops it):

```sh
python3 bench/compare.py ~/old/cluau build-rel/cluau
```

0.14.0 against 0.27.0 (Linux x86_64, release builds linked as the releases
are, `-static-libstdc++ -static-libgcc`; native code):

| case | 0.14.0 | 0.27.0 |
|---|---|---|
| `-e 'print(1)'` | 0.8 ms | 0.8 ms |
| `startup.cluau` | 2.4 ms | 2.6 ms |
| `compute.cluau` | 174 ms | 179 ms |
| `tables.cluau` | 645 ms | 657 ms |

Compare builds made the same way: a build that links libstdc++ as a shared
library starts about 0.3 ms later. The libraries added since 0.14.0 (TLS,
SQLite, regular expressions, compression) start only when a script first
uses them.
