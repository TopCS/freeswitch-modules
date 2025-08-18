# Repository Guidelines

## Project Structure & Modules
- Source: `mod_dialogflow.c` (module entry/API), `google_glue.cpp|.h` (gRPC/Dialogflow bridge), `parser.cpp|.h` (utility parsing).
- Build files: `Makefile.am` (Autotools) and a local `Makefile` integrating gRPC and generated protos.
- Generated code: C++ files from Google APIs live outside the repo and are referenced via `GENS_DIR`; object files are compiled under `gens_objs/`.
- Config: sample module configs under `conf/`.

## Build, Test, and Run
- From FreeSWITCH root: `make mod_dialogflow` (build) or `make mod_dialogflow-install` (build + install). Ensure `./bootstrap.sh && ./configure` have been run at the repo root.
- From this module dir: `make` (uses `../../../..` build rules). Use `VERBOSE=1 make` to see full commands.
- Prereqs: `pkg-config` for `grpc++`/`grpc`, C++17 toolchain, and generated Dialogflow C++ sources. Set or adjust `GENS_DIR` in `Makefile` to the location of generated `.cc/.h` files.
- Runtime: set `GOOGLE_APPLICATION_CREDENTIALS` to a service account JSON with Dialogflow access. Load the module: `fs_cli -x 'load mod_dialogflow'`.
- Manual test: start a session on a live channel UUID: `fs_cli -x "dialogflow_start <uuid> myproject:production en-US welcome"`; stop with `fs_cli -x "dialogflow_stop <uuid>"`. Subscribe to events: `fs_cli -x 'event plain CUSTOM'`.

## Coding Style & Naming
- Language: C (module) and C++17 (gRPC glue). Match existing style (tabs in C sources, brace placement as in current files).
- Names: keep FreeSWITCH-style `snake_case` for functions; prefix Dialogflow helpers with `google_dialogflow_*`.
- Headers: declare public interfaces in `mod_dialogflow.h` / `google_glue.h`; keep module-local symbols `static`.

## Testing Guidelines
- No unit test suite is present; prefer manual verification via `fs_cli` and event inspection (`dialogflow::intent`, `dialogflow::transcription`, `dialogflow::audio_provided`, `dialogflow::end_of_utterance`, `dialogflow::error`, `dialogflow::webhook_error`).
- Add focused tests only if you introduce isolated helpers (e.g., parser functions), following nearby patterns.

## Local Build & Install Cheat Sheet
- Prereqs: `pkg-config`, CMake ≥ 3.18, C++17 toolchain, `grpc++`, `grpc`, `protobuf`, `speexdsp`, FreeSWITCH `freeswitch` pkg-config. Generated Google APIs C++ sources must be available under `GENS_DIR` (e.g., `/home/andrea-batazzi/dev/gens`).
- Configure (standalone):
  - `cmake -S . -B build -DGENS_DIR=/home/andrea-batazzi/dev/gens -DCMAKE_BUILD_TYPE=Release`
- Build:
  - `cmake --build build -j`
- Install/copia modulo:
  - Se `pkg-config --variable=modulesdir freeswitch` è configurato, puoi usare `cmake --install build`.
  - In alternativa copia manualmente: `install -m 0755 build/mod_dialogflow.so /usr/local/freeswitch/mod/`
- Carica/verifica in FreeSWITCH:
  - `fs_cli -x 'load mod_dialogflow'`
  - `fs_cli -x 'dialogflow_version'` oppure `fs_cli -x 'module_exists mod_dialogflow'`
- Commit tipico:
  - `git add <files>`
  - `git commit -m "<area>: <breve descrizione>"`

## Commits & Pull Requests
- Commits: small, focused, and descriptive (scope: area, e.g., "parser:"). Reference issues when available.
- PRs: include a clear summary, steps to reproduce/verify, config notes (e.g., `GENS_DIR`, credentials), and any logs/events screenshots. Avoid adding new dependencies without discussion.
