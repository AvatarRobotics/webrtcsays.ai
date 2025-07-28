# WebRTC now speaks AI language
![alt text](webrtcsaysai.jpg "Logo")

## Building WebRTCsays.ai

### Setup Environment and get code
```bash
# Add depot_tools to your PATH
export PATH=~/Public/depot_tools:$PATH

# Configure and sync gclient
# .gclient in original folder should be like this
 solutions = [
  {
    "name": "src",
    "url": "https://github.com/AvatarRobotics/webrtcsays.ai",
    "deps_file": "DEPS",
    "managed": False,
    "custom_deps": {},
  },
]
target_os = ["ios", "mac", "linux"]
# eof .gclient

gclient config https://github.com/AvatarRobotics/webrtcsays.ai.git

# Add .vpython3 to src
```bash
cat <<EOF > src/.vpython3
# This is a vpython "spec" file.
#
# It describes patterns for python wheel dependencies of the python scripts in
# the chromium repo, particularly for dependencies that have compiled components
# (since pure-python dependencies can be easily vendored into third_party).
#
# When vpython is invoked, it finds this file and builds a python VirtualEnv,
# containing all of the dependencies described in this file, fetching them from
# CIPD (the "Chrome Infrastructure Package Deployer" service). Unlike `pip`,
# this never requires the end-user machine to have a working python extension
# compilation environment. All of these packages are built using:
#   https://chromium.googlesource.com/infra/infra/+/main/infra/tools/dockerbuild/
#
# All python scripts in the repo share this same spec, to avoid dependency
# fragmentation.
#
# If you have depot_tools installed in your $PATH, you can invoke python scripts
# in this repo by running them as you normally would run them, except
# substituting `vpython` instead of `python` on the command line, e.g.:
#   vpython path/to/script.py some --arguments
#
# Read more about `vpython` and how to modify this file here:
#   https://chromium.googlesource.com/infra/infra/+/main/doc/users/vpython.md
 
python_version: "3.11"

# Used by:
#   third_party/catapult
wheel: <
  name: "infra/python/wheels/psutil/${vpython_platform}"
  version: "version:5.8.0.chromium.3"
>

# Used by tools_webrtc/perf/process_perf_results.py.
wheel: <
  name: "infra/python/wheels/httplib2-py3"
  version: "version:0.22.0"
>

wheel: <
  name: "infra/python/wheels/pyparsing-py3"
  version: "version:3.1.1"
>


# Used by:
#   build/toolchain/win
wheel: <
  name: "infra/python/wheels/pywin32/${vpython_platform}"
  version: "version:306"
  match_tag: <
    platform: "win32"
  >
  match_tag: <
    platform: "win_amd64"
  >
>

# GRPC used by iOS test.
wheel: <
  name: "infra/python/wheels/grpcio/${vpython_platform}"
  version: "version:1.57.0"
>

wheel: <
  name: "infra/python/wheels/six-py2_py3"
  version: "version:1.16.0"
>
wheel: <
  name: "infra/python/wheels/pbr-py2_py3"
  version: "version:5.9.0"
>
wheel: <
  name: "infra/python/wheels/funcsigs-py2_py3"
  version: "version:1.0.2"
>
wheel: <
  name: "infra/python/wheels/mock-py3"
  version: "version:4.0.3"
>
wheel: <
  name: "infra/python/wheels/protobuf-py3"
  version: "version:4.25.1"
>
wheel: <
  name: "infra/python/wheels/requests-py3"
  version: "version:2.31.0"
>
wheel: <
  name: "infra/python/wheels/idna-py3"
  version: "version:3.4"
>
wheel: <
  name: "infra/python/wheels/urllib3-py3"
  version: "version:2.1.0"
>
wheel: <
  name: "infra/python/wheels/certifi-py3"
  version: "version:2023.11.17"
>
wheel: <
  name: "infra/python/wheels/charset_normalizer-py3"
  version: "version:3.3.2"
>
wheel: <
  name: "infra/python/wheels/brotli/${vpython_platform}"
  version: "version:1.0.9"
>

# Used by:
#   tools_webrtc/sslroots
wheel: <
  name: "infra/python/wheels/asn1crypto-py2_py3"
  version: "version:1.0.1"
>
EOF
```

gclient sync

# Navigate to the source directory. Original directory can be cleaned up leave "src"
# Yes, I know, WebRTC can be obtuse. 
cd src


# Refresh pull link
git pull https://github.com/AvatarRobotics/WebRTCsays.ai avatar 

```
### Build Scripts
```bash

# For WebRTCsays.ai project, by default, we use "speech" enabled audio.
# Set to false to disable in file webrtc.gni

# Avatar branch sets it to false
rtc_use_speech_audio_devices = false
```
### Build macOS Deployment Target
```bash

```
### Build Linux 
```bash

# Here will be notes specific to Linux build

```
### Generate and build "direct" application 
```bash

# Generate WebRTC example "direct"
gn gen out/debug --args="is_debug=true rtc_include_opus = true rtc_enable_symbol_export=true rtc_build_examples = true"

# Debug build
ninja -C out/debug direct

# Release build
gn gen out/release --args="is_debug=false rtc_include_opus = true rtc_enable_symbol_export=true rtc_build_examples = true"
ninja -C out/release direct

```
### Testing direct peer to peer application
```bash

# Make self-signed cert.pem and key.pem used for encryption option
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -sha256 -days 3650 -nodes -subj "/C=XX/ST=StateName/L=CityName/O=CompanyName/OU=CompanySectionName/CN=CommonNameOrHostname"

# Help with options
./out/debug/direct --help

./out/debug/direct --mode=callee 127.0.0.1:3456 --encryption --webrtc_cert_path=cert.pem --webrtc_key_path=key.pem
./out/debug/direct --mode=caller 127.0.0.1:3456 --encryption --webrtc_cert_path=cert.pem --webrtc_key_path=key.pem

# Run direct with whisper
./out/debug/direct --mode=callee 127.0.0.1:3456 --whisper --encryption
./out/debug/direct --mode=caller 127.0.0.1:3456 --whisper --encryption

```

