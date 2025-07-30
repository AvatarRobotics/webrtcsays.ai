#!/bin/bash

# Exit on any error
set -e

# Clean up old directories if they exist, but keep src
echo "Cleaning up old directories..."
if [ -f ".gclient" ]; then
    echo "Removing existing .gclient file..."
    rm -f .gclient
fi

# Ensure we're on the avatar branch
echo "Switching to avatar branch..."
git checkout avatar || git checkout -b avatar origin/avatar

# Pull the latest code for avatar branch
echo "Pulling latest code for avatar branch..."
git pull origin avatar || { echo "Pull failed, forcing local branch to match remote..."; git fetch origin; git reset --hard origin/avatar; }

# Verify we're on the correct branch
echo "Current commit: $(git rev-parse HEAD)"
echo "Current branch: $(git branch --show-current)"

# Store the current commit hash for comparison
CURRENT_COMMIT=$(git rev-parse HEAD)
LAST_BUILD_COMMIT_FILE="last_build_commit.txt"

# Create or update .gclient file in the root directory with 'name': 'src'
cat > .gclient << EOF
solutions = [
  {
    "managed": False,
    "name": "src",
    "url": "https://github.com/AvatarRobotics/webrtcsays.ai.git",
    "custom_deps": {},
    "deps_file": "DEPS",
  },
]
EOF

# Check if src directory exists and is initialized
if [ -d "src" ] && [ -d "src/build" ] && [ -f "src/.git/HEAD" ]; then
    echo "Skipping gclient sync as src directory already exists and appears initialized."
else
    echo "Running gclient sync to initialize src directory..."
    gclient sync --nohooks --no-history --shallow
    if [ ! -d "src/build" ]; then
        echo "ERROR: src/build directory missing after gclient sync. Check your DEPS file or sync process."
        exit 1
    fi
fi

# Copy .vpython3 to src directory
cp .vpython3 src/ || { echo "WARNING: .vpython3 not found in root, build may fail if dependencies are incorrect."; }

# Navigate to src directory
cd src

# Check if binary already exists and if code has updates
BINARY_PATH="out/Default/webrtcsays"
LAST_BUILD_COMMIT=""
if [ -f "../$LAST_BUILD_COMMIT_FILE" ]; then
    LAST_BUILD_COMMIT=$(cat ../$LAST_BUILD_COMMIT_FILE)
fi

# Check if binary exists and is executable
if [ -f "$BINARY_PATH" ] && [ -x "$BINARY_PATH" ]; then
    # Test if binary can run (e.g., by checking version or help output)
    if "$BINARY_PATH" --help >/dev/null 2>&1; then
        echo "Binary at $BINARY_PATH is runnable. Cleaning up directories except src..."
        cd ..
        for dir in */ ; do
            if [ "$dir" != "src/" ]; then
                echo "Removing $dir..."
                rm -rf "$dir"
            fi
        done
        cd src
    else
        echo "Binary at $BINARY_PATH exists but is not runnable. Skipping cleanup of other directories."
    fi
else
    echo "Binary at $BINARY_PATH does not exist or is not executable. Skipping cleanup of other directories."
fi

if [ -f "$BINARY_PATH" ] && [ "$CURRENT_COMMIT" = "$LAST_BUILD_COMMIT" ]; then
    echo "Binary already exists at $BINARY_PATH and no code updates detected, skipping build..."
else
    echo "Building WebRTC project..."
    # Add your build commands here, e.g.,
    # gn gen out/Default
    # ninja -C out/Default
    echo "Build completed."
    # Store the current commit hash as the last built commit
    echo "$CURRENT_COMMIT" > ../$LAST_BUILD_COMMIT_FILE
fi

# Run the binary in callee mode
echo "Running binary in callee mode..."
$BINARY_PATH --callee

echo "Script execution completed."

