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
