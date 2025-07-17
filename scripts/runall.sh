#!/bin/bash

# BrightSign NPU Voice Extension - Complete Build Script
# This script executes all the steps from the README.md file

set -e  # Exit on any error

# Parse command line arguments
AUTO_MODE=false
if [[ "$1" == "--auto" ]]; then
    AUTO_MODE=true
    echo "Running in automatic mode - no user prompts"
fi

# Function to prompt user to continue (unless in auto mode)
prompt_continue() {
    if [[ "$AUTO_MODE" == "false" ]]; then
        echo
        echo "Press Enter to continue, or Ctrl+C to exit..."
        read -r
    fi
}

# Function to check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Function to display step header
step_header() {
    echo
    echo "========================================"
    echo "$1"
    echo "========================================"
}

# Ensure we're in the project root
if [[ ! -f "README.md" || ! -f "bsext_init" ]]; then
    echo "Error: This script must be run from the project root directory"
    exit 1
fi

export project_root=$(pwd)
echo "Project root: $project_root"

# Step 0 - Setup
step_header "Step 0 - Setup"
echo "Setting up the development environment..."

# Check prerequisites
echo "Checking prerequisites..."
if ! command_exists docker; then
    echo "Error: Docker is not installed. Please install Docker first."
    exit 1
fi

if ! command_exists git; then
    echo "Error: Git is not installed. Please install Git first."
    exit 1
fi

if ! command_exists cmake; then
    echo "Error: CMake is not installed. Please install CMake first."
    exit 1
fi

echo "All prerequisites are installed."
prompt_continue

# Clone supporting repositories
echo "Cloning supporting Rockchip repositories..."
cd "${project_root}"
mkdir -p toolkit && cd toolkit

if [[ ! -d "rknn-toolkit2" ]]; then
    git clone https://github.com/airockchip/rknn-toolkit2.git --depth 1 --branch v2.3.0
fi

if [[ ! -d "rknn_model_zoo" ]]; then
    git clone https://github.com/airockchip/rknn_model_zoo.git --depth 1 --branch v2.3.0
fi

cd -
echo "Repositories cloned successfully."
prompt_continue

# Install BSOS SDK
echo "Installing BSOS SDK..."
cd "${project_root}"

export BRIGHTSIGN_OS_MAJOR_VERION=9.0
export BRIGHTSIGN_OS_MINOR_VERION=189
export BRIGHTSIGN_OS_VERSION=${BRIGHTSIGN_OS_MAJOR_VERION}.${BRIGHTSIGN_OS_MINOR_VERION}

# Download BrightSign OS sources if not already present
if [[ ! -f "brightsign-${BRIGHTSIGN_OS_VERSION}-src-dl.tar.gz" ]]; then
    echo "Downloading BrightSign OS sources..."
    wget https://brightsignbiz.s3.amazonaws.com/firmware/opensource/${BRIGHTSIGN_OS_MAJOR_VERION}/${BRIGHTSIGN_OS_VERSION}/brightsign-${BRIGHTSIGN_OS_VERSION}-src-dl.tar.gz
    wget https://brightsignbiz.s3.amazonaws.com/firmware/opensource/${BRIGHTSIGN_OS_MAJOR_VERION}/${BRIGHTSIGN_OS_VERSION}/brightsign-${BRIGHTSIGN_OS_VERSION}-src-oe.tar.gz
fi

# Extract if not already extracted
if [[ ! -d "brightsign-oe" ]]; then
    echo "Extracting BrightSign OS sources..."
    tar -xzf brightsign-${BRIGHTSIGN_OS_VERSION}-src-dl.tar.gz
    tar -xzf brightsign-${BRIGHTSIGN_OS_VERSION}-src-oe.tar.gz
    
    # Apply custom recipes
    rsync -av bsoe-recipes/ brightsign-oe/
    
    # Clean up
    rm brightsign-${BRIGHTSIGN_OS_VERSION}-src-dl.tar.gz
    rm brightsign-${BRIGHTSIGN_OS_VERSION}-src-oe.tar.gz
fi

echo "BrightSign OS sources prepared."
prompt_continue

# Build SDK using Docker
echo "Building SDK using Docker..."
if [[ ! -f "Dockerfile" ]]; then
    wget https://raw.githubusercontent.com/brightsign/extension-template/refs/heads/main/Dockerfile
fi

# Build Docker image if not already built
if [[ "$(docker images -q bsoe-build 2> /dev/null)" == "" ]]; then
    echo "Building Docker image for SDK build..."
    docker build --rm --build-arg USER_ID=$(id -u) --build-arg GROUP_ID=$(id -g) --ulimit memlock=-1:-1 -t bsoe-build .
fi

mkdir -p srv

# Check if SDK is already built
if [[ ! -f "brightsign-x86_64-cobra-toolchain-${BRIGHTSIGN_OS_VERSION}.sh" ]]; then
    echo "Building SDK in Docker container..."
    echo "This may take several hours depending on your system..."
    
    docker run -it --rm \
        -v $(pwd)/brightsign-oe:/home/builder/bsoe -v $(pwd)/srv:/srv \
        bsoe-build /bin/bash -c "cd /home/builder/bsoe/build && MACHINE=cobra ./bsbb brightsign-sdk"
    
    # Copy the SDK to project root
    cp brightsign-oe/build/tmp-glibc/deploy/sdk/brightsign-x86_64-cobra-toolchain-${BRIGHTSIGN_OS_VERSION}.sh ./
fi

# Install SDK
if [[ ! -d "sdk" ]]; then
    echo "Installing SDK..."
    ./brightsign-x86_64-cobra-toolchain-${BRIGHTSIGN_OS_VERSION}.sh -d ./sdk -y
    
    # Patch SDK with Rockchip libraries
    echo "Patching SDK with Rockchip libraries..."
    cd sdk/sysroots/aarch64-oe-linux/usr/lib
    if [[ ! -f "librknnrt.so" ]]; then
        wget https://github.com/airockchip/rknn-toolkit2/raw/v2.3.2/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so
    fi
    cd "${project_root}"
fi

echo "SDK installation complete."
prompt_continue

# Step 1 - Compile ONNX Models
step_header "Step 1 - Compile ONNX Models for Rockchip NPU"
echo "Compiling models for the Rockchip NPU..."

# Build Docker image for model compilation
cd "${project_root}/toolkit/rknn-toolkit2/rknn-toolkit2/docker/docker_file/ubuntu_20_04_cp38"
if [[ "$(docker images -q rknn_tk2 2> /dev/null)" == "" ]]; then
    echo "Building Docker image for model compilation..."
    docker build --rm -t rknn_tk2 -f Dockerfile_ubuntu_20_04_for_cp38 .
fi

# Download models
cd "${project_root}/toolkit/rknn_model_zoo/"
mkdir -p examples/RetinaFace/model/RK3588
pushd examples/RetinaFace/model
if [[ ! -f "RetinaFace_mobile320.onnx" ]]; then
    echo "Downloading RetinaFace model..."
    chmod +x ./download_model.sh && ./download_model.sh
fi
popd

# Download Whisper models
mkdir -p examples/whisper/model/RK3588
pushd examples/whisper/model
if [[ ! -f "whisper_decoder_base_20s.onnx" ]]; then
    echo "Downloading Whisper models..."
    chmod +x ./download_model.sh && ./download_model.sh
fi
popd

echo "Models downloaded."
prompt_continue

# Compile models for RK3588
echo "Compiling models for RK3588 (XT-5 players)..."
docker run -it --rm -v $(pwd):/zoo rknn_tk2 /bin/bash \
    -c "cd /zoo/examples/RetinaFace/python && python convert.py ../model/RetinaFace_mobile320.onnx rk3588 i8 ../model/RK3588/RetinaFace.rknn"

docker run -it --rm -v $(pwd):/zoo rknn_tk2 /bin/bash \
    -c "cd /zoo/examples/whisper/python && python convert.py ../model/whisper_decoder_base_20s.onnx rk3588 fp ../model/RK3588/whisper_decoder_base_20s.rknn"

docker run -it --rm -v $(pwd):/zoo rknn_tk2 /bin/bash \
    -c "cd /zoo/examples/whisper/python && python convert.py ../model/whisper_encoder_base_20s.onnx rk3588 fp ../model/RK3588/whisper_encoder_base_20s.rknn"

echo "RK3588 models compiled."
prompt_continue

# Compile models for RK3568
echo "Compiling models for RK3568 (LS-5 players)..."
mkdir -p examples/RetinaFace/model/RK3568
mkdir -p examples/whisper/model/RK3568

docker run -it --rm -v $(pwd):/zoo rknn_tk2 /bin/bash \
    -c "cd /zoo/examples/RetinaFace/python && python convert.py ../model/RetinaFace_mobile320.onnx rk3568 i8 ../model/RK3568/RetinaFace.rknn"

docker run -it --rm -v $(pwd):/zoo rknn_tk2 /bin/bash \
    -c "cd /zoo/examples/whisper/python && python convert.py ../model/whisper_decoder_base_20s.onnx rk3568 fp ../model/RK3568/whisper_decoder_base_20s.rknn"

docker run -it --rm -v $(pwd):/zoo rknn_tk2 /bin/bash \
    -c "cd /zoo/examples/whisper/python && python convert.py ../model/whisper_encoder_base_20s.onnx rk3568 fp ../model/RK3568/whisper_encoder_base_20s.rknn"

echo "RK3568 models compiled."
prompt_continue

# Step 3 - Build for XT5
step_header "Step 3 - Build and Test on XT5"
echo "Building application for XT5 players..."

cd "${project_root}"
source ./sdk/environment-setup-aarch64-oe-linux

# Build for RK3588 (XT5)
echo "Building for RK3588 (XT5)..."
rm -rf build_xt5
mkdir -p build_xt5 && cd build_xt5

cmake .. -DOECORE_TARGET_SYSROOT="${OECORE_TARGET_SYSROOT}" -DTARGET_SOC="rk3588"
make
make install

echo "XT5 build complete."
prompt_continue

# Build for RK3568 (LS5)
echo "Building for RK3568 (LS5)..."
cd "${project_root}"
source ./sdk/environment-setup-aarch64-oe-linux

rm -rf build_ls5
mkdir -p build_ls5 && cd build_ls5

cmake .. -DOECORE_TARGET_SYSROOT="${OECORE_TARGET_SYSROOT}" -DTARGET_SOC="rk3568"
make
make install

echo "LS5 build complete."
prompt_continue

# Step 4 - Package Extension
step_header "Step 4 - Package the Extension"
echo "Packaging the extension..."

cd "${project_root}"

# Copy extension scripts
cp bsext_init install/ && chmod +x install/bsext_init
cp sh/uninstall.sh install/ && chmod +x install/uninstall.sh

# Create development zip
cd "${project_root}/install"
rm -f ../voice-dev-*.zip
zip -r ../voice-dev-$(date +%s).zip ./

echo "Development package created."
prompt_continue

# Create extension package
echo "Creating extension package..."
../sh/make-extension-lvm

# Create extension zip
rm -f ../voice-demo-*.zip
zip ../voice-demo-$(date +%s).zip ext_npu_voice*

# Clean up
rm -rf ext_npu_voice*

echo "Extension package created."

# Final summary
step_header "Build Complete!"
echo "All steps completed successfully!"
echo
echo "Generated files:"
echo "- voice-dev-*.zip: Development package for testing"
echo "- voice-demo-*.zip: Extension package for installation"
echo
echo "Next steps:"
echo "1. Transfer the voice-demo-*.zip file to your BrightSign player"
echo "2. Follow the installation instructions in the README.md"
echo "3. Reboot the player to activate the extension"
echo
echo "For development/testing, use the voice-dev-*.zip package."
echo "For production deployment, use the voice-demo-*.zip package."

echo "Build script completed successfully!"