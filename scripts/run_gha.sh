#!/bin/bash

# BrightSign NPU Voice Extension - Complete Build Script
# This script automates all the steps from the README.md

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Global variables
AUTO_MODE=false
SKIP_ARCH_CHECK=false
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WITHOUT_IMAGE=false
WITHOUT_SDK=false
WITHOUT_MODELS=false

BRIGHTSIGN_OS_MAJOR_VERSION=${BRIGHTSIGN_OS_MAJOR_VERSION:-9.1}
BRIGHTSIGN_OS_MINOR_VERSION=${BRIGHTSIGN_OS_MINOR_VERSION:-52}


# get the directory of this script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"


# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -y|--auto)
            AUTO_MODE=true
            shift
            ;;
        --skip-arch-check)
            SKIP_ARCH_CHECK=true
            shift
            ;; 
        -v|--version)
            if [[ $2 =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
                BRIGHTSIGN_OS_MAJOR_VERSION="${2%.*}"
                BRIGHTSIGN_OS_MINOR_VERSION="${2##*.}"
            elif [[ $2 =~ ^[0-9]+\.[0-9]+$ ]]; then
                BRIGHTSIGN_OS_MAJOR_VERSION="$2"
                BRIGHTSIGN_OS_MINOR_VERSION="0"
            else
                echo "Invalid version format: $2. Use major.minor or major.minor.patch"
                exit 1
            fi
            shift 2 
            ;;
        --major)
            BRIGHTSIGN_OS_MAJOR_VERSION="$2"; shift 2 
            ;;
        --minor)
            BRIGHTSIGN_OS_MINOR_VERSION="$2"; shift 2 
            ;;
        --without-image)
            WITHOUT_IMAGE=true; shift 
            ;;
        --without-models)
            WITHOUT_MODELS=true; shift 
            ;;
        --without-sdk)
            WITHOUT_SDK=true; shift 
            ;;
        -h|--help)
            echo "Usage: $0 [-auto|--auto] [--skip-arch-check]"
            echo "  -y, --auto: Run all steps without prompting for confirmation"
            echo "  --skip-arch-check: Skip x86_64 architecture check (for testing)"
            echo "  -v, --version VERSION  Set BrightSign OS version (e.g., 9.1.52)"
            echo "  --major VERSION        Set major.minor version (e.g., 9.1)"
            echo "  --minor VERSION        Set minor version number (e.g., 52)"
            echo "  --without-image        Don't build the Docker image"
            echo "  --without-models       Don't prepare toolkit for building models"
            echo "  --without-sdk          Don't build the SDK"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use -h for help"
            exit 1
            ;;
    esac
done

# Function to print colored output
print_status() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_header() {
    echo -e "\n${BLUE}=== $1 ===${NC}\n"
}

# Function to prompt user for continuation
prompt_continue() {
    if [ "$AUTO_MODE" = true ]; then
        print_status "Auto mode: Continuing automatically..."
        return 0
    fi

    local message="$1"
    echo -e "\n${YELLOW}NEXT STEPS:${NC}"
    echo "$message"
    echo
    read -p "Do you want to continue? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        print_status "Exiting..."
        exit 0
    fi
}

# Function to check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Function to check if docker is running
check_docker_running() {
    if ! docker info >/dev/null 2>&1; then
        print_error "Docker is not running. Please start Docker and try again."
        exit 1
    fi
}

# STEP 0: Setup
step0_setup() {
    print_header "STEP 0: Setup"
    
    # Check Docker
    if ! command_exists docker; then
        print_error "Docker is not installed. Please install Docker first:"
        print_error "https://docs.docker.com/engine/install/"
        exit 1
    fi
    check_docker_running
    print_status "Docker is installed and running"

    # Check other required tools
    if ! command_exists git; then
        print_error "Git is not installed. Please install git first."
        exit 1
    fi
    
    if ! command_exists cmake; then
        print_error "CMake is not installed. Please install cmake first."
        exit 1
    fi
    
    if ! command_exists wget; then
        print_error "wget is not installed. Please install wget first."
        exit 1
    fi

    print_status "All required tools are installed"

    # Set project root environment variable
    export project_root="$PROJECT_ROOT"
    print_status "Project root set to: $project_root"

    # Set OS version variables
    export BRIGHTSIGN_OS_VERSION=${BRIGHTSIGN_OS_MAJOR_VERSION}.${BRIGHTSIGN_OS_MINOR_VERSION}


    print_status "Step 0 completed successfully!"
    
    print_warning "MANUAL STEP REQUIRED: You need to unsecure your BrightSign player"
    print_warning "Follow the instructions in the README.md under 'Unsecure the Player'"
    print_warning "This involves connecting serial cable and using boot commands"
}

# STEP 1: Build docker image
step1_build_docker_image() {

    print_header "STEP 1: Build Docker Image"

    # Build SDK in Docker
    if [ ! -f "Dockerfile" ]; then
        print_status "Downloading Dockerfile..."
        wget https://raw.githubusercontent.com/brightsign/extension-template/refs/heads/main/Dockerfile
    fi

    if ! docker images | grep -q "bsoe-build"; then
        print_status "Building BSOS Docker image..."
        docker build --rm --build-arg USER_ID=$(id -u) --build-arg GROUP_ID=$(id -g) --ulimit memlock=-1:-1 -t bsoe-build .
    else
        print_status "BSOS Docker image already exists"
    fi

    mkdir -p srv   

    print_status "Step 1 completed successfully!"
}

step2_build_bs_sdk() {

    print_header "STEP 2: Build BrightSign OS SDK"

    # Install BSOS SDK
    print_status "Setting up BrightSign OS SDK..."   
    
    # Prepare docker command

    # Set default target if not provided
    if [ -z "$TARGET" ]; then
        TARGET="brightsign-sdk"
    fi

    # Build quiet flag
    QUIET_FLAG=""
    if [ "$QUIET" = true ]; then
        QUIET_FLAG="-q"
    fi

    # Build command construction
    BUILD_CMD="MACHINE=cobra ./bsbb $QUIET_FLAG ${TARGET}"

    # Container commands
    CONTAINER_CMD=""
    echo "Will apply patches and build $TARGET..."
    CONTAINER_CMD="$CONTAINER_CMD /usr/local/bin/setup-patches.sh && "

    # Add the main build command
    CONTAINER_CMD="$CONTAINER_CMD $BUILD_CMD"

    # Check if SDK already exists
    if [ ! -f "brightsign-x86_64-cobra-toolchain-${BRIGHTSIGN_OS_VERSION}.sh" ]; then
        print_status "Building BrightSign SDK (this may take several hours)..."
        docker run ${docker_flags} --rm \
            -v $(pwd)/bsoe-recipes:/home/builder/patches:ro \
            -v $(pwd)/sh:/home/builder/host-scripts:ro \
            -v $(pwd)/srv:/srv \
            -w /home/builder/bsoe/brightsign-oe/build \
            bsoe-build \
            bash -c "${CONTAINER_CMD}"
        
        # Copy the SDK
        # cp brightsign-oe/build/tmp-glibc/deploy/sdk/brightsign-x86_64-cobra-toolchain-${BRIGHTSIGN_OS_VERSION}.sh ./
        cp srv/brightsign-x86_64-cobra-toolchain-${BRIGHTSIGN_OS_VERSION}.sh ./
    else
        print_status "SDK already exists"
    fi

    print_status "Step 2 completed successfully!"

}

step3_install_bs_sdk() {

    print_header "STEP 3: Install BrightSign SDK"

    # Install SDK
    if [ ! -d "sdk" ]; then
        print_status "Installing SDK..."
        ./brightsign-x86_64-cobra-toolchain-${BRIGHTSIGN_OS_VERSION}.sh -d ./sdk -y
        
        # Patch SDK with Rockchip libraries
        cd sdk/sysroots/aarch64-oe-linux/usr/lib
        if [ ! -f "librknnrt.so" ]; then
            wget --progress=dot:mega https://github.com/airockchip/rknn-toolkit2/raw/v2.3.2/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so
        fi
        cd "$project_root"
    else
        print_status "SDK already installed"
    fi

    print_status "Step 3 completed successfully!"

}

# STEP 4: Compile ONNX Models
step4_compile_models() {

    print_header "STEP 4: Compile ONNX Models for Rockchip NPU"

    # Clone supporting repositories
    print_status "Cloning supporting Rockchip repositories..."
    cd "$project_root"
    mkdir -p toolkit && cd toolkit

    if [ ! -d "rknn-toolkit2" ]; then
        git clone https://github.com/airockchip/rknn-toolkit2.git --depth 1 --branch v2.3.0
    else
        print_status "rknn-toolkit2 already exists"
    fi

    if [ ! -d "rknn_model_zoo" ]; then
        git clone https://github.com/airockchip/rknn_model_zoo.git --depth 1 --branch v2.3.0
    else
        print_status "rknn_model_zoo already exists"
    fi

    cd "$project_root"
   
    cd "$project_root/toolkit/rknn-toolkit2/rknn-toolkit2/docker/docker_file/ubuntu_20_04_cp38"
    echo 'RUN pip3 install openai-whisper==20231117 onnx onnxsim' >> Dockerfile_ubuntu_20_04_for_cp38
    echo 'RUN pip3 install ./rknn_toolkit_lite2/packages/rknn_toolkit_lite2-1.6.0-cp310-cp310-linux_aarch64.whl || echo "Optional RKNN Lite install skipped"' >> Dockerfile_ubuntu_20_04_for_cp38
    
    # Build Docker image for model compilation
    docker build --rm -t rknn_tk2 -f Dockerfile_ubuntu_20_04_for_cp38 .

    # Download RetinaFace model
    cd "$project_root/toolkit/rknn_model_zoo/"
    mkdir -p examples/RetinaFace/model/RK3588
    pushd examples/RetinaFace/model
    if [ ! -f "RetinaFace_mobile320.onnx" ]; then
        print_status "Downloading RetinaFace model..."
        chmod +x ./download_model.sh && ./download_model.sh
    else
        print_status "RetinaFace model already downloaded"
    fi
    popd

    # Compile RetinaFace model for RK3588 (XT-5 players)
    if [ ! -f "examples/RetinaFace/model/RK3588/RetinaFace.rknn" ]; then
        print_status "Compiling model for RK3588 (XT-5 players)..."
        docker run -it --rm -v $(pwd):/zoo rknn_tk2 /bin/bash \
            -c "cd /zoo/examples/RetinaFace/python && python convert.py ../model/RetinaFace_mobile320.onnx rk3588 i8 ../model/RK3588/RetinaFace.rknn"
    else
        print_status "RK3588 RetinaFace model already compiled"
    fi
   
    # Download whisper models
    cd "$project_root/toolkit/rknn_model_zoo/"
    mkdir -p examples/whisper/model/RK3588
    if [ ! -f "examples/whisper/model/whisper_decoder_base.onnx" ] ||
       [ ! -f "examples/whisper/model/whisper_encoder_base.onnx" ]; then
        print_status "Downloading Whisper models..."
        docker run -it --rm -v $(pwd):/zoo rknn_tk2 /bin/bash -c "cd /zoo/examples/whisper/python && python export_onnx.py --model_type base --n_mels 80"
    else
        print_status "Whisper models already downloaded"
    fi
    
    # Compile whisper models for RK3588 (XT-5 players)
    if [ ! -f "examples/whisper/model/RK3588/whisper_decoder_base.rknn" ] ||
       [ ! -f "examples/whisper/model/RK3588/whisper_encoder_base.rknn" ]; then
        print_status "Compiling model for RK3588 (XT-5 players)..."
        docker run -it --rm -v $(pwd):/zoo rknn_tk2 /bin/bash \
        -c "cd /zoo/examples/whisper/python && python convert.py ../model/whisper_decoder_base.onnx rk3588 fp ../model/RK3588/whisper_decoder_base.rknn"
        docker run -it --rm -v $(pwd):/zoo rknn_tk2 /bin/bash \
        -c "cd /zoo/examples/whisper/python && python convert.py ../model/whisper_encoder_base.onnx rk3588 fp ../model/RK3588/whisper_encoder_base.rknn"
    else
        print_status "RK3588 model already compiled"
    fi

    # Copy models to install directory
    mkdir -p "$project_root/install/RK3588/model"
        
    cp examples/RetinaFace/model/RK3588/RetinaFace.rknn "$project_root/install/RK3588/model/"
    cp examples/whisper/model/RK3588/whisper_encoder_base.rknn "$project_root/install/RK3588/model/"
    cp examples/whisper/model/RK3588/whisper_decoder_base.rknn "$project_root/install/RK3588/model/"
    cp examples/whisper/model/mel_80_filters.txt "$project_root/install/RK3588/model/"
    cp examples/whisper/model/vocab_en.txt "$project_root/install/RK3588/model/"

    print_status "Step 4 completed successfully!"
}

# STEP 5: Build and Test on XT5
step5_build_xt5() {
    print_header "STEP 5: Build and Test on XT5"
    
    cd "$project_root"
    
    # Source the SDK environment
    source ./sdk/environment-setup-aarch64-oe-linux

    # Build for XT5 (RK3588)
    print_status "Building for XT5 (RK3588)..."
    rm -rf build_xt5
    mkdir -p build_xt5 && cd build_xt5
    
    cmake .. -DOECORE_TARGET_SYSROOT="${OECORE_TARGET_SYSROOT}" -DTARGET_SOC="rk3588"
    make
    make install
    
    cd "$project_root"
    
    print_status "Step 5 completed successfully!"
}

# STEP 6: Package the Extension
step6_package() {
    print_header "STEP 6: Package the Extension"
    
    cd "$project_root"
    
    # Copy extension scripts
    cp bsext_init install/ && chmod +x install/bsext_init
    cp sh/uninstall.sh install/ && chmod +x install/uninstall.sh

    # Create development package
    cd "$project_root/install"
    rm -f ../voice-dev-*.zip
    zip -r "../voice-dev-$(date +%s).zip" ./
    
    # Create production extension
    ../sh/make-extension-lvm
    rm -f ../voice-ext-*.zip
    zip "../voice-ext-$(date +%s).zip" ext_npu_voice*
    rm -rf ext_npu_voice*

    cd "$project_root"
    
    print_status "Step 6 completed successfully!"
    print_status "Development package: voice-dev-*.zip"
    print_status "Production extension: voice-demo-*.zip"
}

# Main execution
main() {
    print_header "BrightSign NPU Voice Extension - Complete Build"
    
    if [ "$AUTO_MODE" = true ]; then
        print_status "Running in automatic mode - no prompts"
    else
        print_status "Running in interactive mode - will prompt between steps"
    fi
    
    print_status "Project root: $PROJECT_ROOT"
    
    # Check architecture
    if [ "$(uname -m)" != "x86_64" ] && [ "$SKIP_ARCH_CHECK" != true ]; then
        print_error "This script requires x86_64 architecture"
        print_error "Current architecture: $(uname -m)"
        print_error "Use --skip-arch-check to bypass this check for testing"
        exit 1
    elif [ "$SKIP_ARCH_CHECK" = true ]; then
        print_warning "Skipping architecture check - this is for testing only"
    fi
    
    # Execute steps
    # Step 0 - Basic setup
    step0_setup

    # Step 1 - Build Docker Image
    if [[ "$WITHOUT_IMAGE" != true ]]; then
        step1_build_docker_image
    else
        print_status "Skipping preparation of docker image as per --without-image option"
    fi

    # Step 2 - Build BrightSign SDK
    if [[ "$WITHOUT_SDK" != true ]]; then
        step2_build_bs_sdk
    else
        print_status "Skipping preparation of BrightSign SDK as per --without-sdk option"
    fi

    # Step 3 - Install BrightSign SDK
    step3_install_bs_sdk
    
    # Step 4 - Compile models
    if [[ "$WITHOUT_MODELS" != true ]]; then
        step4_compile_models
    else
        print_status "Skipping preparation of models toolkit as per --without-models option"
    fi

    # Step 5 - Build app for XT5
    step5_build_xt5

    # Step 6 - Package the Extension
    step6_package
    
    print_header "BUILD COMPLETE"
    print_status "All steps completed successfully!"
    print_status "Check the install directory for the built files"
    print_status "Development package: voice-dev-*.zip"
    print_status "Production extension: voice-demo-*.zip"

    print_warning "Don't forget to unsecure your BrightSign player as described in the README!"
}

# Run main function
main "$@"
