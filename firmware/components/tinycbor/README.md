# tinycbor (vendored)

Source: [intel/tinycbor v0.6.0](https://github.com/intel/tinycbor) — MIT
license, copy in `LICENSES/LICENSE.tinycbor`.

Files in `include/` and `src/` are unmodified upstream copies. The set is
trimmed to what the firmware actually links:

| Kept                                       | Dropped (reason)                                      |
|--------------------------------------------|-------------------------------------------------------|
| `cborencoder.c`                            | `cborpretty*.c` / `cbortojson.c` (debug only)         |
| `cborencoder_close_container_checked.c`    | `cborvalidation.c` (parser already enforces what we need) |
| `cborencoder_float.c`                      | `open_memstream.c` (POSIX shim; we never use it)      |
| `cborerrorstrings.c`                       | `parsetags.pl`, `tags.txt` (build-time helpers)       |
| `cborparser.c`, `cborparser_dup_string.c`  |                                                       |
| `cborparser_float.c`                       |                                                       |
| `cbor.h`, `tinycbor-version.h` (public)    |                                                       |
| `cborinternal_p.h`, `compilersupport_p.h`, `utf8_p.h` (private) |                                  |

Refresh procedure:

```
git clone --depth 1 --branch <tag> https://github.com/intel/tinycbor /tmp/tc
cp /tmp/tc/src/{cbor.h,tinycbor-version.h} firmware/components/tinycbor/include/
cp /tmp/tc/src/{cborencoder.c,cborencoder_close_container_checked.c,\
cborencoder_float.c,cborerrorstrings.c,cborparser.c,cborparser_dup_string.c,\
cborparser_float.c,cborinternal_p.h,compilersupport_p.h,utf8_p.h} firmware/components/tinycbor/src/
cp /tmp/tc/LICENSE firmware/components/tinycbor/LICENSES/LICENSE.tinycbor
```

Then re-run the host test (`make -C firmware/host-tests/cbor_codec test`) and
the firmware build (`idf.py build`).
