# firmware/third_party

Upstream code we build from a committed copy instead of from its package,
because the published package has a defect we cannot wait out. Each
directory is one library, kept byte for byte except for the change its own
README describes, with that README recording every file's upstream sha256.

| Library | Why | Built by |
|---|---|---|
| [`WebServer-2.0.17-securacv`](WebServer-2.0.17-securacv/README.md) | arduino-esp32 2.0.17's WebServer with the GHSA-8cmm-3887-r32j multipart-boundary fix backported | the core-2 PlatformIO envs that serve HTTP, and the core 2.0.17 Arduino CLI rows |

This directory is a PlatformIO library storage: an env that lists
`../../third_party` in `lib_extra_dirs` can resolve any library in it, ahead
of the framework's own. So only core-2 envs list it, and
`scripts/tests/test_vendored_webserver_core2.py` fails a new directory here
until that test names it.
