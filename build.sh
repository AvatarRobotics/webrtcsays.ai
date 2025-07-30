#!/bin/bash

# build-webrtcsays.sh - Build script for WebRTCsays.ai project

set -e

# Function to display usage information
usage() {
    echo "Usage: $0 [-d|--debug] [-r|--release] [-h|--help]"
    echo "  -d, --debug   : Build in debug mode (default)"
    echo "  -r, --release : Build in release mode"
    echo "  -h, --help    : Display this help message"
    exit 1
}

# Default build type is debug
BUILD_TYPE="debug"

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--debug)
            BUILD_TYPE="debug"
            shift
            ;;
        -r|--release)
            BUILD_TYPE="release"
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
done

echo "Build type: $BUILD_TYPE"

# Step 1: Add depot_tools to PATH
export PATH=~/depot_tools:$PATH
if ! command -v gclient &> /dev/null; then
    echo "depot_tools not found in ~/depot_tools. Please install depot_tools and add it to your PATH."
    exit 1
fi

echo "Creating src directory..."
mkdir -p src

echo "Configuring gclient..."

echo "Creating .gclient file in src directory..."

REMOTE_URL=$(git config --get remote.origin.url)

echo "solutions = [
  {
    'name': 'src',
    'url': '$REMOTE_URL',
    "deps_file": "DEPS",
    "managed": False,    
    "custom_deps": {},glcin
  },
]
target_os = ["linux"]" > .gclient

gclient config $REMOTE_URL

# Move .gclient to src directory
echo "Adding .vpython3 and .gclient files in src directory..."
cp .vpython3 src
cp .gclient src


# Step 7: Sync the repository
echo "Syncing repository with gclient..."
gclient sync

if [ -d "src" ]; then
    echo "src directory already exists"
else
    echo "src directory not created"
    exit 1
fi

cd src

echo "Pulling latest changes from WebRTCsays.ai repository..."

if [ "$BUILD_TYPE" = "debug" ]; then
    echo "Generating build files for debug mode..."
    gn gen out/debug --args="is_debug=true rtc_include_opus=true rtc_enable_symbol_export=true rtc_build_examples=true rtc_use_speech_audio_devices=false"
else
    echo "Generating build files for release mode..."
    gn gen out/release --args="is_debug=false rtc_include_opus=true rtc_enable_symbol_export=true rtc_build_examples=true rtc_use_speech_audio_devices=false"
fi

if [ "$BUILD_TYPE" = "debug" ]; then
    echo "Building in debug mode..."
    ninja -C out/debug direct_app
else
    echo "Building in release mode..."
    ninja -C out/release direct_app
fi

# Landmines step: only run if the script exists
if [ -f "src/tools_webrtc/get_landmines.py" ]; then
  python3 src/build/landmines.py --landmine-scripts src/tools_webrtc/get_landmines.py --src-dir src
else
  echo "Skipping landmines: src/tools_webrtc/get_landmines.py not found"
fi

echo "Build completed successfully!"

echo "To test the application, you can run:"
if [ "$BUILD_TYPE" = "debug" ]; then
    ./out/debug/direct_app --help
else
    ./out/release/direct_app --help
fi 
