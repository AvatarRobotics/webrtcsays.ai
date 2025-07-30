#!/bin/bash
set -e

# Check if we're already in a git repository, if not, clone it
echo "Checking for repository..."
if [ ! -d ".git" ]; then
    echo "Cloning webrtcsays.ai repository..."
    git clone https://github.com/AvatarRobotics/webrtcsays.ai.git
    cd webrtcsays.ai
else
    echo "Already in a git repository, skipping clone."
fi

#If webrtcsa

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

# FIX 1: Create .gclient in ROOT directory (not in src)
echo "Creating .gclient file in root directory..."
REMOTE_URL=$(git config --get remote.origin.url)

cat << EOF > .gclient
solutions = [
  {
    "name": "src",
    "url": "$REMOTE_URL",
    "deps_file": "DEPS",
    "managed": False,
    "custom_deps": {},
  },
]
target_os = ["linux"]
EOF

# FIX 2: Only copy .vpython3 to src (don't move .gclient)
echo "Adding .vpython3 to src directory..."
cp .vpython3 src

# Step 7: Sync the repository
echo "Syncing repository with gclient..."
gclient sync  # Runs in root where .gclient exists

# Check if src directory exists and has expected content
if [ -d "src" ] && [ -d "src/build" ] && [ -f "src/.gclient" ]; then
    echo "Skipping gclient sync: src directory already exists and appears initialized"
else
    echo "Configuring gclient and syncing repository..."
    # Create .gclient in root
    echo "Creating .gclient file in root directory..."
    REMOTE_URL=$(git config --get remote.origin.url)

    cat << EOF > .gclient
solutions = [
  {
    "name": "src",
    "url": "$REMOTE_URL",
    "deps_file": "DEPS",
    "managed": False,
    "custom_deps": {},
  },
]
target_os = ["linux"]
EOF

    echo "Adding .vpython3 to src directory..."
    cp .vpython3 src

    echo "Syncing repository with gclient..."
    gclient sync

    # Verify critical directories
    if [ ! -d "src/build" ]; then
        echo "ERROR: src/build directory missing after sync!"
        echo "Check gclient sync output for errors"
        exit 1
    fi
fi

cd src

echo "Pulling latest changes from avatar branch..."

# Ensure we're on the avatar branch
echo "Switching to avatar branch..."
git checkout avatar || git checkout -b avatar

# Reset to clean state
echo "Resetting to clean state..."
git reset --hard HEAD
git clean -fd

# Pull latest changes for avatar branch
echo "Pulling latest changes for avatar branch..."
git pull origin avatar || {
    echo "Handling divergent branches..."
    git fetch origin
    git reset --hard origin/avatar
}

# Verify we're on correct commit
echo "Current commit: $(git rev-parse HEAD)"
echo "Current branch: $(git branch --show-current)"

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

# Make self-signed cert.pem and key.pem used for encryption option
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -sha256 -days 3650 -nodes -subj "/C=XX/ST=CA/L=SanFrancisco/O=Avatar/OU=Teleop/CN=avatarrobotics.com"

# Make config.json
cat << EOF > config.json
{
    "mode": "callee",
    "address": "3.93.50.189:3456",
    "room_name": "room101",
    "encryption": true,
    "whisper": false,
    "llama": false,
    "video": true,
    "bonjour": false,
    "language": "en",
    "turns": ["turn:3.93.50.189:5349?transport=udp,webrtcsays.ai,wilddolphin"], 
    "camera": "0,640x480@30",
}
EOF

echo "To test the application, you can run:"
if [ "$BUILD_TYPE" = "debug" ]; then
    ./out/debug/direct_app --help
else
    ./out/release/direct_app --help
fi 

# Add cleanup after successful build
cd ..
echo "Cleaning up directories except src..."
for dir in */ ; do
    if [ "$dir" != "src/" ] ; then
        rm -rf "$dir"
    fi
done

# Replace hardcoded release run with checked general version
if [ "$BUILD_TYPE" = "debug" ]; then
  APP_PATH="src/out/debug/direct_app"
else
  APP_PATH="src/out/release/direct_app"
fi
if [ -f "$APP_PATH" ]; then
  echo "Starting callee..."
  ./"$APP_PATH" --config config.json
else
  echo "Built app not found at $APP_PATH"
fi

