# freq — Word Frequency Counter

A high-performance single-threaded word-frequency counter written in C++17.

## What it does

* Reads an input text file.
* Extracts words — sequences of latin letters `a-zA-Z`; everything else is a separator.
* Lowercases all letters.
* Outputs `count word` pairs, sorted by **frequency descending**, then **alphabetically ascending**.

### Example

```
$ cat in.txt
The time has come, the Walrus said, to talk of many things...

$ ./freq in.txt out.txt

$ cat out.txt
2 the
1 come
1 has
1 many
1 of
1 said
1 talk
1 things
1 time
1 to
1 walrus
```

## Performance design

| Technique                                  | Benefit                                     |
| ------------------------------------------ | ------------------------------------------- |
| `mmap` + `MADV_SEQUENTIAL`                 | Zero-copy file read, OS prefetch            |
| Compile-time lowercase lookup table        | Branch-free character classification        |
| `unordered_map` pre-reserved to 1M buckets | Avoid rehashing on large vocabularies       |
| Single `std::vector<char>` output buffer   | One `write()` syscall for the entire result |
| `-O3 -march=native`                        | Full CPU-specific optimisation              |

## Requirements

* Linux (uses POSIX `mmap`, `madvise`)
* CMake ≥ 3.10
* GCC ≥ 7 or Clang ≥ 5 with C++17 support

## Running

```bash
cmake --preset win-release-config
cmake --build --preset win-release-build
cmake --build --preset win-release-build --clean-first

.\build\win-release\freq.exe [input.txt] [output.txt]
```
