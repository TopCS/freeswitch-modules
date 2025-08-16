# mod_dialogflow

This is a fork of the drachtio/jambonz dialogflow module for freeswitch.

A Freeswitch module that connects a Freeswitch channel to a [dialogflow agent](https://dialogflow.com/docs/getting-started/first-agent) so that an IVR interaction can be driven completely by dialogflow **CX** logic.



Once a Freeswitch channel is connected to a dialogflow agent, media is streamed to the dialogflow service, which returns information describing the "intent" that was detected, along with transcriptions and audio prompts and text to play to the caller.  The handling of returned audio by the module is two-fold:
1.  If an audio clip was returned, it is *not* immediately played to the caller, but instead is written to a temporary wave file on the Freeswitch server.
2.  Next, a Freeswitch custom event is sent to the application containing the details of the dialogflow response as well as the path to the wave file.

This allows the application whether to decide to play the returned audio clip (via the mod_dptools 'play' command), or to use a text-to-speech service to generate audio using the returned prompt text.

## API

### Commands
The freeswitch module exposes the following API commands:

#### dialogflow_start
```
dialogflow_start <uuid> <project-id> <lang-code> [<event>]
```
Attaches media bug to channel and performs streaming recognize request.
- `uuid` - unique identifier of Freeswitch channel
- `project-id` - the identifier of the dialogflow project to execute, which may optionally include a dialogflow environment, a region and output audio configurations (see below).
- `project-id` - the identifier of the dialogflow project to execute, which may optionally include a dialogflow environment, a region and output audio configurations (see below).
- `lang-code` - a valid dialogflow [language tag](https://dialogflow.com/docs/reference/language) to use for speech recognition
- `event` - name of an initial event to send to dialogflow; e.g. to trigger an initial prompt

When executing a dialogflow project, the environment and region will default to 'draft' and 'us', respectively.

To specify both an environment and a region, provide a value for project-id in the dialogflow_start command as follows:
```
dialogflow-project-id:agent-id:environment:region, i.e myproject:production:eu-west1
```
To specify environment and default to the global region:
```
dialogflow-project-id:agent-id:environment, i.e myproject:production
```
Speaking rate, pitch and volume should take the value of a double.  
Voice Name should take a valid Text-to-speech model name (choose available voices from https://cloud.google.com/text-to-speech/docs/voices). If not set, the Dialogflow service will choose a voice based on the other parameters such as language code and gender. 

Voice Gender should be M for Male, F for Female, N for neutral gender or leave empty for Unspecified.  If not set, the Dialogflow service will choose a voice based on the other parameters such as language code and name. Note that this is only a preference, not requirement. If a voice of the appropriate gender is not available, the synthesizer should substitute a voice with a different gender rather than failing the request.

Effects are applied on the text-to-speech and are used to improve the playback of an audio on different types of hardware. Available effects and information [here](https://cloud.google.com/text-to-speech/docs/audio-profiles#available_audio_profiles).

Sentiment Analysis uses Cloud Natural Language to provide a sentiment score for each user query. To enable send the boolean ```true```.

#### dialogflow_stop
```
dialogflow_stop <uuid> 
```
Stops dialogflow on the channel.

### Events
* `dialogflow::intent` - a dialogflow [intent](https://dialogflow.com/docs/intents) has been detected.
* `dialogflow::transcription` - a transcription has been returned
* `dialogflow::audio_provided` - an audio prompt has been returned from Dialogflow. The module writes the audio to a temporary file and fires this event with JSON `{ "path": "/tmp/....wav|.mp3|.opus" }`. Playback is not automatic unless `DIALOGFLOW_AUTOPLAY` is enabled.
* `dialogflow::end_of_utterance` - dialogflow has detected the end of an utterance
* `dialogflow::error` - dialogflow has returned an error
* `dialogflow::transfer` - module is about to transfer the call; JSON body includes `exten`, `context`, `dialplan`, `intent_display_name`. Also includes `parameters` (from Dialogflow QueryResult) and, when enabled, `query_params`.
* `dialogflow::end_session` - module is about to end the session; JSON body includes `intent_display_name`. Also includes `parameters` (from Dialogflow QueryResult) and, when enabled, `query_params`.

### Dialplan Variables
- `DIALOGFLOW_CHANNEL`: Optional logical channel name to set `QueryParameters.channel` and include in parameters.
- `DIALOGFLOW_PARAMS`: Optional JSON string merged into `QueryParameters.parameters` on start.
- `DIALOGFLOW_AUTOPLAY`: If `true`, auto-play returned TTS on the A-leg.
- `DIALOGFLOW_AUTOPLAY_SYNC`: When `true` and `DIALOGFLOW_AUTOPLAY` is enabled, play the agent audio synchronously and block the next user turn until playback completes. Defaults to `true` when `DIALOGFLOW_AUTOPLAY` is set. Set to `false` to retain legacy async `uuid_broadcast` behavior.
- `DIALOGFLOW_BARGE_IN`: When `true`, do not block listening during agent playback (barge-in enabled). Defaults to `false`.

- `DIALOGFLOW_INCLUDE_QUERY_PARAMS`: When `true`, all Dialogflow events emitted by the module include a `query_params` object with:
  - `channel`: the request-side `QueryParameters.channel` if set (via `DIALOGFLOW_CHANNEL` or JSON `channel`).
  - `payload`: the exact JSON provided in `DIALOGFLOW_PARAMS` (echoed as an object when valid, otherwise omitted).

- `DIALOGFLOW_SESSION_ID`: Optional string to override the Dialogflow session id used in the gRPC path. If unset, the FreeSWITCH call UUID is used.

- `DIALOGFLOW_PASS_ALL_CHANNEL_VARS`: When `true`, include all channel variables as string `QueryParameters.parameters`.
- `DIALOGFLOW_VAR_PREFIXES`: Optional comma-separated allowlist of prefixes to include when above is enabled (e.g., `sip_,caller_,origination_`).

- `DIALOGFLOW_END_SESSION_INTENT`: Intent display name that should end the call. Default: `END SESSION`.
- `DIALOGFLOW_TRANSFER_INTENT`: Intent display name that should transfer the call. Default: `TRANSFER TO HUMAN`.
- `DIALOGFLOW_END_SESSION_PAGE`: Page display name that should end the call (takes precedence if set).
- `DIALOGFLOW_TRANSFER_PAGE`: Page display name that should transfer the call (takes precedence if set).
- `DIALOGFLOW_TRANSFER_EXTEN`: Destination extension used when transfer intent matches (if not provided by DF parameters).
- `DIALOGFLOW_TRANSFER_CONTEXT`: Destination context for transfer. Default: `default`.
- `DIALOGFLOW_TRANSFER_DIALPLAN`: Destination dialplan for transfer. Default: `XML`.
- `DIALOGFLOW_ACTIONS_EMIT_ONLY`: If `true`, do not auto hangup/transfer; only emit the corresponding events for external logic.

Notes:
- Transfer intent also honors DF parameters if present: `transfer_to`/`transfer_target`/`destination`/`exten`, plus optional `context` and `dialplan`.
- Be cautious when enabling `DIALOGFLOW_PASS_ALL_CHANNEL_VARS`; prefer `DIALOGFLOW_VAR_PREFIXES` to limit sensitive data exposure.

### Turn Timing During Playback
By default, when `DIALOGFLOW_AUTOPLAY` is enabled the module now avoids opening a new Dialogflow turn until the returned agent audio has finished playing. This prevents spurious `no_input` while the caller is listening to long prompts.

- To keep barge-in enabled, set `DIALOGFLOW_BARGE_IN=true`.
- To revert to legacy async playback (which may start a new turn during playback), set `DIALOGFLOW_AUTOPLAY_SYNC=false`.
## Usage
When using [drachtio-fsrmf](https://www.npmjs.com/package/drachtio-fsmrf), you can access this API command via the api method on the 'endpoint' object.
```js
ep.api('dialogflow_start', `${ep.uuid} my-agent-uuxr:production en-US welcome`); 
```
## Examples
[drachtio-dialogflow-phone-gateway](https://github.com/davehorton/drachtio-dialogflow-phone-gateway)

## Standalone Build (CMake)
You can build and install this module without a FreeSWITCH source tree using CMake, similar to mod_audio_stream.

Prereqs: FreeSWITCH dev package (pkg-config provides `freeswitch`), gRPC (`grpc++`, `grpc`), Protobuf (`protobuf`), and generated Google APIs C++ sources.

1. Generate/download Google APIs C++ sources and set `GENS_DIR` to the root containing `google/cloud/dialogflow/cx/v3/*.cc` etc.
2. Configure and build:
```
cmake -S . -B build -DGENS_DIR=/path/to/generated
cmake --build build -j
cmake --install build
```
By default, the module installs to the FreeSWITCH modules directory discovered via pkg-config.

### Install Required Packages
You need build tools, FreeSWITCH dev files (with `freeswitch.pc`), gRPC, Protobuf, and SpeexDSP. Examples:

- Ubuntu/Debian (22.04+ recommended):
  - `sudo apt-get update`
  - `sudo apt-get install -y build-essential cmake pkg-config libfreeswitch-dev libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev protobuf-compiler libspeexdsp-dev libssl-dev zlib1g-dev`
  - If you installed FreeSWITCH from source under `/usr/local`, ensure: `export PKG_CONFIG_PATH=/usr/local/freeswitch/lib/pkgconfig:$PKG_CONFIG_PATH`

- Fedora/RHEL (with appropriate repos enabled):
  - `sudo dnf install -y @development-tools cmake pkgconfig freeswitch-devel grpc-devel grpc-plugins protobuf-compiler protobuf-devel speexdsp-devel openssl-devel zlib-devel`

- Alpine:
  - `sudo apk add build-base cmake pkgconfig freeswitch-dev grpc-dev protobuf-dev protobuf speexdsp-dev openssl-dev zlib-dev`

- macOS (Homebrew; FreeSWITCH dev files not officially packaged — use with caution):
  - `brew install cmake pkg-config grpc protobuf speexdsp`
  - Ensure FreeSWITCH headers and `freeswitch.pc` are present in your environment if you built FS locally; set `PKG_CONFIG_PATH` accordingly.

If your distro’s gRPC is too old, build from source per official docs: https://grpc.io/docs/languages/cpp/quickstart/

### Generate Google APIs C++ sources (for GENS_DIR)
This module compiles against generated C++ from Google APIs protos (Dialogflow CX). You need to generate these once and point `GENS_DIR` at the output root.

Prereqs:
- Protobuf compiler and C++ runtime: `protoc`, `libprotobuf-dev`, `protobuf-compiler`
- gRPC C++ and plugins: `grpc`, `grpc++`, `protobuf-compiler-grpc` (provides `grpc_cpp_plugin`)

Steps:
- Clone the protos: `git clone https://github.com/googleapis/googleapis.git` (pick a tag as desired)
- Generate into an output dir, preserving package paths:

```
API_ROOT=/path/to/googleapis
OUT=/absolute/path/to/gens
mkdir -p "$OUT"

# Generate Dialogflow CX v3 protos (+ service stubs)
protoc \
  -I"$API_ROOT" \
  --cpp_out="$OUT" \
  --grpc_out="$OUT" \
  --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
  $(find "$API_ROOT/google/cloud/dialogflow/cx/v3" -name '*.proto')

# Generate a few common Google types used by Dialogflow CX
protoc -I"$API_ROOT" --cpp_out="$OUT" \
  google/type/latlng.proto \
  google/rpc/status.proto

# Generate Google API annotations referenced by Dialogflow protos
protoc -I"$API_ROOT" --cpp_out="$OUT" \
  $(find "$API_ROOT/google/api" -maxdepth 1 -name '*.proto')

# Generate Google Long Running Operations (used by some Dialogflow protos)
protoc -I"$API_ROOT" --cpp_out="$OUT" \
  $(find "$API_ROOT/google/longrunning" -maxdepth 1 -name '*.proto')

# After generation, you should have:
#   $OUT/google/cloud/dialogflow/cx/v3/*.pb.cc, *.grpc.pb.cc, and headers
#   $OUT/google/type/*.pb.cc, $OUT/google/rpc/*.pb.cc, $OUT/google/api/*.pb.cc, $OUT/google/longrunning/*.pb.cc

# Use this as GENS_DIR
cmake -S . -B build -DGENS_DIR="$OUT"
```

Notes:
- Ensure `protoc --version` matches your installed protobuf dev runtime.
- If you see missing includes during build, generate additional imported protos similarly under `google/` in the same `OUT` tree.

Helper script:
- You can also run the included helper: `scripts/gen_googleapis.sh /path/to/googleapis /abs/path/to/gens`
- It validates tooling and generates Dialogflow CX v3 plus required common types.

## Build a .deb
This repo includes CPack config and helper scripts to create a Debian package.

Local (host) build:
- Generate sources (GENS_DIR) as above.
- Run: `scripts/build-deb.sh /abs/path/to/gens`
- Output: `build/freeswitch-mod-dialogflow_*.deb`

Dockerized build (Debian Bookworm):
- Ensure you have Docker and a prepared GENS_DIR on the host.
- Build image while mounting your GENS_DIR:
  - `docker build -f Dockerfile.debian --build-arg GENS_DIR=/gens -t mod_dialogflow:deb .` (then run with a bind mount)
- Or run interactive build with bind mounts:
  - `docker run --rm -v "$PWD":/src -v /abs/path/to/gens:/gens -w /src debian:bookworm-slim bash -lc "apt-get update && apt-get install -y build-essential cmake pkg-config libfreeswitch-dev libspeexdsp-dev libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev protobuf-compiler zlib1g-dev libssl-dev && scripts/build-deb.sh /gens"`

Install .deb:
- `sudo dpkg -i build/freeswitch-mod-dialogflow_*.deb`
- The module installs into the FreeSWITCH modules directory (`modulesdir` from pkg-config).

### Sample Dialplan
Place something like this in your dialplan to start Dialogflow and enable the new behaviors:

```
<include>
  <extension name="dialogflow_demo">
    <condition field="destination_number" expression="^1000$">
      <action application="set" data="GOOGLE_APPLICATION_CREDENTIALS=/path/to/service_account.json"/>
      <action application="set" data="DIALOGFLOW_CHANNEL=voice"/>
      <action application="set" data="DIALOGFLOW_AUTOPLAY=true"/>
      <action application="set" data="DIALOGFLOW_PASS_ALL_CHANNEL_VARS=true"/>
      <action application="set" data="DIALOGFLOW_VAR_PREFIXES=caller_,sip_"/>
      <action application="set" data="DIALOGFLOW_END_SESSION_INTENT=END SESSION"/>
      <!-- Alternatively, match by page name -->
      <action application="set" data="DIALOGFLOW_END_SESSION_PAGE=End Session"/>
      <action application="set" data="DIALOGFLOW_TRANSFER_INTENT=TRANSFER TO HUMAN"/>
      <!-- Alternatively, match by page name -->
      <action application="set" data="DIALOGFLOW_TRANSFER_PAGE=Agent Handoff"/>
      <!-- Optional: emit-only mode (no auto hangup/transfer) -->
      <action application="set" data="DIALOGFLOW_ACTIONS_EMIT_ONLY=false"/>
      <action application="set" data="DIALOGFLOW_TRANSFER_CONTEXT=default"/>
      <action application="set" data="DIALOGFLOW_TRANSFER_DIALPLAN=XML"/>

      <action application="answer"/>
      <action application="api" data="dialogflow_start ${uuid} myproject:myagent:production en-US welcome"/>
      <action application="park"/>
    </condition>
  </extension>
</include>
```
