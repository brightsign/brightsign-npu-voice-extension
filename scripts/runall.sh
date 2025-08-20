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
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WITHOUT_CLEAN=false
WITHOUT_IMAGE=false
WITHOUT_SDK=false
WITHOUT_MODELS=false
CLEAN_MODE=false
DOCKER_FLAGS=""

BRIGHTSIGN_OS_MAJOR_VERSION=${BRIGHTSIGN_OS_MAJOR_VERSION:-9.1}
BRIGHTSIGN_OS_MINOR_VERSION=${BRIGHTSIGN_OS_MINOR_VERSION:-52}



# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -y|--auto)
            AUTO_MODE=true
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
        --without-models)
            WITHOUT_MODELS=true; shift 
            ;;
        --without-sdk)
            WITHOUT_SDK=true; shift 
            ;;
        --without-clean)
            WITHOUT_CLEAN=true; shift
            ;;	    
        -c|--clean)
            CLEAN_MODE=true; shift 
            ;;
        -h|--help)
            echo "Usage: $0 [-y|--auto] [options]"
            echo "  -y, --auto: Run all steps without prompting for confirmation"
            echo "  -v, --version VERSION  Set BrightSign OS version (e.g., 9.1.52)"
            echo "  --major VERSION        Set major.minor version (e.g., 9.1)"
            echo "  --minor VERSION        Set minor version number (e.g., 52)"
            echo "  --without-clean        Don't remove build_xxx folders"
            echo "  --without-models       Don't prepare toolkit for building models"
            echo "  --without-sdk          Don't build the SDK"
            echo "  --clean                Clean all build artifacts and downloaded files"
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
        wget --progress=dot:giga https://raw.githubusercontent.com/brightsign/extension-template/refs/heads/main/Dockerfile
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
    
    # Extract if not already extracted
    if [ ! -d "brightsign-oe" ]; then
        print_status "Downloading BrightSign OS source..."
        wget --progress=dot:giga "https://brightsignbiz.s3.amazonaws.com/firmware/opensource/${BRIGHTSIGN_OS_MAJOR_VERSION}/${BRIGHTSIGN_OS_VERSION}/brightsign-${BRIGHTSIGN_OS_VERSION}-src-dl.tar.gz"
        wget --progress=dot:giga "https://brightsignbiz.s3.amazonaws.com/firmware/opensource/${BRIGHTSIGN_OS_MAJOR_VERSION}/${BRIGHTSIGN_OS_VERSION}/brightsign-${BRIGHTSIGN_OS_VERSION}-src-oe.tar.gz"
        print_status "Extracting BrightSign OS source..."
        tar -xzf "brightsign-${BRIGHTSIGN_OS_VERSION}-src-dl.tar.gz"
        tar -xzf "brightsign-${BRIGHTSIGN_OS_VERSION}-src-oe.tar.gz"
        
        # Apply custom recipes
        rsync -av bsoe-recipes/ brightsign-oe/
        
        # Clean up
        rm "brightsign-${BRIGHTSIGN_OS_VERSION}-src-dl.tar.gz"
        rm "brightsign-${BRIGHTSIGN_OS_VERSION}-src-oe.tar.gz"
    else
        print_status "BrightSign OS source already extracted"
    fi

    # Check if SDK already exists
    if [ ! -f "brightsign-x86_64-cobra-toolchain-${BRIGHTSIGN_OS_VERSION}.sh" ]; then
        print_status "Building BrightSign SDK (this may take several hours)..."
        docker run ${DOCKER_FLAGS} --rm \
            -v $(pwd)/brightsign-oe:/home/builder/bsoe \
            -v $(pwd)/srv:/srv \
            bsoe-build \
            bash -c "cd /home/builder/bsoe/build && MACHINE=cobra ./bsbb brightsign-sdk"
        
        # Copy the SDK
        cp brightsign-oe/build/tmp-glibc/deploy/sdk/brightsign-x86_64-cobra-toolchain-${BRIGHTSIGN_OS_VERSION}.sh ./
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
            wget https://github.com/airockchip/rknn-toolkit2/raw/v2.3.2/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so
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
        docker run ${DOCKER_FLAGS} --rm -v $(pwd):/zoo rknn_tk2 /bin/bash \
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
        docker run ${DOCKER_FLAGS} --rm -v $(pwd):/zoo rknn_tk2 /bin/bash -c "cd /zoo/examples/whisper/python && python export_onnx.py --model_type base --n_mels 80"
    else
        print_status "Whisper models already downloaded"
    fi
    
    # Compile whisper models for RK3588 (XT-5 players)
    if [ ! -f "examples/whisper/model/RK3588/whisper_decoder_base.rknn" ] ||
       [ ! -f "examples/whisper/model/RK3588/whisper_encoder_base.rknn" ]; then
        print_status "Compiling model for RK3588 (XT-5 players)..."
        docker run ${DOCKER_FLAGS} --rm -v $(pwd):/zoo rknn_tk2 /bin/bash \
        -c "cd /zoo/examples/whisper/python && python convert.py ../model/whisper_decoder_base.onnx rk3588 fp ../model/RK3588/whisper_decoder_base.rknn"
        docker run ${DOCKER_FLAGS} --rm -v $(pwd):/zoo rknn_tk2 /bin/bash \
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
    if [[ "$WITHOUT_CLEAN" != true ]]; then
      rm -rf build_xt5
    fi
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

# Clean function to remove all build artifacts
clean_build_artifacts() {
    print_header "CLEANING BUILD ARTIFACTS"
    
    cd "$PROJECT_ROOT"
    
    # Ask for confirmation unless in auto mode
    if [ "$AUTO_MODE" != true ]; then
        print_warning "This will remove all build artifacts, downloaded files, and compiled models."
        print_warning "The following will be deleted:"
        print_warning "  - Docker images (bsoe-build, rknn_tk2)"
        print_warning "  - Downloaded source archives"
        print_warning "  - BrightSign OS source (brightsign-oe/)"
        print_warning "  - SDK installation (sdk/)"
        print_warning "  - Toolkit directory (toolkit/)"
        print_warning "  - Build directories (build_xt5/)"
        print_warning "  - Install directory (install/)"
        print_warning "  - Service directory (srv/)"
        print_warning "  - Generated packages (*.zip files)"
        echo
        read -p "Are you sure you want to continue? (y/N): " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            print_status "Clean operation cancelled."
            return 0
        fi
    fi
    
    # Remove Docker images
    print_status "Removing Docker images..."
    docker rmi bsoe-build 2>/dev/null || print_warning "bsoe-build image not found"
    docker rmi rknn_tk2 2>/dev/null || print_warning "rknn_tk2 image not found"
    
    # Remove downloaded archives
    print_status "Removing downloaded archives..."
    rm -f brightsign-*.tar.gz
    rm -f brightsign-x86_64-cobra-toolchain-*.sh
    
    # Remove source directories
    print_status "Removing source directories..."
    if [ -d "brightsign-oe" ]; then
        print_status "Removing BrightSign OS source (this may take a moment)..."
        # First try to make all files writable, then remove
        chmod -R u+w brightsign-oe/ 2>/dev/null || true
        rm -rf brightsign-oe/ 2>/dev/null || {
            print_warning "Standard removal failed, trying alternative method..."
            # Alternative method: find and delete files first, then directories
            find brightsign-oe -type f -delete 2>/dev/null || true
            find brightsign-oe -type d -empty -delete 2>/dev/null || true
            # If still exists, try once more with sudo (will prompt if needed)
            if [ -d "brightsign-oe" ]; then
                print_warning "Directory still exists, you may need to manually remove: brightsign-oe/"
                print_warning "You can try: sudo rm -rf brightsign-oe/"
            fi
        }
    fi
    
    # Remove SDK
    print_status "Removing SDK installation..."
    if [ -d "sdk" ]; then
        chmod -R u+w sdk/ 2>/dev/null || true
        rm -rf sdk/ 2>/dev/null || {
            print_warning "SDK removal failed, trying alternative method..."
            find sdk -type f -delete 2>/dev/null || true
            find sdk -type d -empty -delete 2>/dev/null || true
            if [ -d "sdk" ]; then
                print_warning "SDK directory still exists, you may need to manually remove: sdk/"
            fi
        }
    fi
    
    # Remove toolkit
    print_status "Removing toolkit directory..."
    if [ -d "toolkit" ]; then
        chmod -R u+w toolkit/ 2>/dev/null || true
        rm -rf toolkit/ 2>/dev/null || {
            print_warning "Toolkit removal failed, trying alternative method..."
            find toolkit -type f -delete 2>/dev/null || true
            find toolkit -type d -empty -delete 2>/dev/null || true
            if [ -d "toolkit" ]; then
                print_warning "Toolkit directory still exists, you may need to manually remove: toolkit/"
            fi
        }
    fi
    
    # Remove build directories
    print_status "Removing build directories..."
    rm -rf build_xt5/
    
    # Remove install directory
    print_status "Removing install directory..."
    rm -rf install/
    
    # Remove service directory
    print_status "Removing service directory..."
    rm -rf srv/
    
    # Remove generated packages
    print_status "Removing generated packages..."
    rm -f voice-dev-*.zip
    rm -f voice-ext-*.zip
    rm -f voice-demo-*.zip
    
    # Remove downloaded Dockerfile if it exists
    if [ -f "Dockerfile" ] && [ "$(head -1 Dockerfile 2>/dev/null)" = "# This Dockerfile was downloaded" ]; then
        print_status "Removing downloaded Dockerfile..."
        rm -f Dockerfile
    fi
    
    # Final status check
    remaining_dirs=""
    for dir in brightsign-oe sdk toolkit build_xt5 install srv; do
        if [ -d "$dir" ]; then
            remaining_dirs="$remaining_dirs $dir"
        fi
    done
    
    if [ -z "$remaining_dirs" ]; then
        print_status "Clean operation completed successfully!"
        print_status "All build artifacts have been removed."
    else
        print_warning "Clean operation completed with some directories remaining:$remaining_dirs"
        print_warning "These may need manual removal with: sudo rm -rf$remaining_dirs"
    fi
}

# Main execution
main() {
    print_header "BrightSign NPU Voice Extension - Complete Build"
    
    # Handle clean mode
    if [ "$CLEAN_MODE" = true ]; then
        clean_build_artifacts
        exit 0
    fi

    if [ -t 0 ]; then
        DOCKER_FLAGS="-it --rm"
    fi
    
    
    if [ "$AUTO_MODE" = true ]; then
        print_status "Running in automatic mode - no prompts"
    else
        print_status "Running in interactive mode - will prompt between steps"
    fi
    
    print_status "Project root: $PROJECT_ROOT"
    
    # Check architecture
    if [ "$(uname -m)" != "x86_64" ]; then
        print_error "This script requires x86_64 architecture"
        print_error "Current architecture: $(uname -m)"
        exit 1
    fi
    
    # Execute steps
    # Step 0 - Basic setup
    step0_setup
    
    # Only prompt for Docker image build if we're actually going to build it
    if [[ "$WITHOUT_SDK" != true ]]; then
        prompt_continue "We will now build the Docker image for the BrightSign OS development environment."
    fi

    # Step 1 - Build Docker Image
    if [[ "$WITHOUT_SDK" != true ]]; then
        step1_build_docker_image
        prompt_continue "We will now download and build the BrightSign OS SDK. This may take several hours."
    else
        print_status "Skipping preparation of docker image as per --without-sdk option"
    fi

    # Step 2 - Build BrightSign SDK
    if [[ "$WITHOUT_SDK" != true ]]; then
        step2_build_bs_sdk
        prompt_continue "We will now install the BrightSign OS SDK."
    else
        print_status "Skipping preparation of BrightSign SDK as per --without-sdk option"
    fi

    # Step 3 - Install BrightSign SDK
    if [[ "$WITHOUT_SDK" != true ]]; then
        step3_install_bs_sdk
        
        # Only prompt for models if we're going to compile them
        if [[ "$WITHOUT_MODELS" != true ]]; then
            prompt_continue "We will now compile the ONNX models for the Rockchip NPU."
        fi
    else
        print_status "Skipping installation of BrightSign SDK as per --without-sdk option"
    fi
    
    # Step 4 - Compile models
    if [[ "$WITHOUT_MODELS" != true ]]; then
        step4_compile_models
        
        # Only prompt for build if we're going to build
        if [[ "$WITHOUT_SDK" != true ]]; then
            prompt_continue "We will now build the application for XT5 players."
        fi
    else
        print_status "Skipping preparation of models toolkit as per --without-models option"
    fi

    # Step 5 - Build app for XT5
    step5_build_xt5

    # Step 6 - Package the Extension
    step6_package
    
    print_header "BUILD COMPLETE"
    print_status "All steps completed successfully!"
    
    if [[ "$WITHOUT_SDK" != true ]]; then
        print_status "Check the install directory for the built files"
        print_status "Development package: voice-dev-*.zip"
        print_status "Production extension: voice-ext-*.zip"
        print_warning "Don't forget to unsecure your BrightSign player as described in the README!"
    else
        print_status "SDK-dependent steps were skipped. Only setup and model compilation were performed."
    fi
}

# Run main function
main "$@"
