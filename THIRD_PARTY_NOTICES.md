# Third-Party Notices

Clay is licensed under the MIT License (see [LICENSE](LICENSE)). It includes or
uses the third-party software listed below. Each component remains under its own
license; the full license texts are in the files referenced here.

## Bundled in this repository (`deps/`)

These are compiled into the `clay` binary and into programs built with
`clay --compile`.

| Component | Version | License | Copyright | License text |
|-----------|---------|---------|-----------|--------------|
| [cJSON](https://github.com/DaveGamble/cJSON) | 1.7.19 | MIT | © 2009-2017 Dave Gamble and cJSON contributors | [deps/cjson/LICENSE](deps/cjson/LICENSE) |
| [Crossline](https://github.com/jcwangxp/Crossline) | 1.0 | MIT | © 2019 JC Wang | [deps/crossline/LICENSE](deps/crossline/LICENSE) |
| [SQLite](https://sqlite.org/) | 3.54.1 | Public domain | — | [sqlite.org/copyright.html](https://sqlite.org/copyright.html) (dedication in the header of `deps/sqlite/sqlite3.c`) |
| [hash](https://github.com/mseminatore/hash) (submodule) | — | MIT | © 2025 Mark Seminatore | [deps/hash/LICENSE](deps/hash/LICENSE) |

## Development and build tools (not part of the `clay` binary)

| Component | Used for | License | Copyright | License text |
|-----------|----------|---------|-----------|--------------|
| [testy](https://github.com/mseminatore/testy) (submodule) | unit tests | MIT | © 2023 Mark Seminatore | [deps/testy/LICENSE](deps/testy/LICENSE) |
| [vmgen](https://github.com/mseminatore/vmgen) (submodule) | generating `src/vm/generated/` | MIT | © 2026 Mark Seminatore | [deps/vmgen/LICENSE](deps/vmgen/LICENSE) |
| [libyaml](https://github.com/yaml/libyaml), [tomlc99](https://github.com/cktan/tomlc99) | fetched by vmgen's build to parse specs | MIT | their respective authors | in each project's repository |

The code in `src/vm/generated/` is produced by vmgen from `src/vm/clay.yaml` and
is distributed under Clay's MIT License.

## Optional system libraries

Not included in this repository. When present at build time, Clay links against
them dynamically; if you redistribute a `clay` binary built with them, include
their notices as their licenses require.

| Library | Enables | License |
|---------|---------|---------|
| [OpenSSL](https://www.openssl.org/) 3.x | HTTPS in `Http` (Linux/macOS) | Apache License 2.0 |
| [SDL2](https://www.libsdl.org/) | the `Graphics` prototype | zlib License |

On Windows, HTTPS uses the operating system's WinHTTP.
