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

# Step 2: Create src directory
echo "Creating src directory..."
mkdir -p src

# Step 3: Add .vpython3 to src directory
echo "Creating .vpython3 file in src directory..."
cp .vpython3 src

# Step 4: Configure gclient
echo "Configuring gclient..."
gclient config https://github.com/AvatarRobotics/webrtcsays.ai.git

#Step 5: Create .gclient file
#echo "Creating .gclient file in src directory..."
echo "solutions = [
  {
    'name': 'src',
    'url': 'https://github.com/AvatarRobotics/webrtcsays.ai.git',
    "deps_file": "DEPS",
    "managed": False,    
    "custom_deps": {},
  },
]
target_os = ["linux"]" > .gclient

# Step 6: Move .gclient to src directory
mv .gclient src

# Step 7: Sync the repository
echo "Syncing repository with gclient..."
gclient sync

if [ -d "src" ]; then
    echo "src di    rectory already exists"
else
    echo "src directory not created"
    exit 1
fi


# Step 5: Navigate to src directory
cd src

# Step 6: Pull latest changes from the repository
echo "Pulling latest changes from WebRTCsays.ai repository..."
git checkout avatar
git pull https://github.com/AvatarRobotics/WebRTCsays.ai avatar

# Step 7: Generate build files with GN
if [ "$BUILD_TYPE" = "debug" ]; then
    echo "Generating build files for debug mode..."
    gn gen out/debug --args="is_debug=true rtc_include_opus=true rtc_enable_symbol_export=true rtc_build_examples=true rtc_use_speech_audio_devices=false"
else
    echo "Generating build files for release mode..."
    gn gen out/release --args="is_debug=false rtc_include_opus=true rtc_enable_symbol_export=true rtc_build_examples=true rtc_use_speech_audio_devices=false"
fi

# Step 8: Build the project with Ninja
if [ "$BUILD_TYPE" = "debug" ]; then
    echo "Building in debug mode..."
    ninja -C out/debug direct_app
else
    echo "Building in release mode..."
    ninja -C out/release direct_app
fi

echo "Build completed successfully!"

echo "To test the application, you can run:"
if [ "$BUILD_TYPE" = "debug" ]; then
    ./out/debug/direct_app --help
else
    ./out/release/direct_app --help
fi 
