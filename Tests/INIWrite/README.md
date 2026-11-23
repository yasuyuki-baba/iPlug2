# INI persistence failure regression

This display-free Linux executable exercises SWELL's public INI API. It checks
successful writes, failed temporary-file opens, permission denial (when run as
an unprivileged user), buffered writes to /dev/full, failed rename, preservation
of existing string/section settings, cache invalidation and successful retry.
The preceding integration2 revision fails this test by reporting a failed open
as success.

```sh
cmake -S Tests/INIWrite -B build/ini-write -DCMAKE_BUILD_TYPE=Debug
cmake --build build/ini-write --parallel 2
ctest --test-dir build/ini-write --output-on-failure
```

The downstream CheapSynth01 LinuxStartup suite separately verifies initialization
failure propagation and invalid-driver rejection against the actual standalone
executable, including that failures exit cleanly before device lookup.
