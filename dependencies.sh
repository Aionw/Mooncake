#!/bin/bash
# Copyright 2024 KVCache.AI
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Color definitions
GREEN="\033[0;32m"
BLUE="\033[0;34m"
YELLOW="\033[0;33m"
RED="\033[0;31m"
NC="\033[0m" # No Color

# Configuration
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GITHUB_PROXY=${GITHUB_PROXY:-"https://github.com"}
GOVER=1.25.9
SPDK_VERSION=v23.01.1
OS_RELEASE_FILE=${OS_RELEASE_FILE:-/etc/os-release}

# Function to print section headers
print_section() {
    echo -e "\n${BLUE}=== $1 ===${NC}"
}

# Function to print success messages
print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

# Function to print error messages and exit
print_error() {
    echo -e "${RED}✗ ERROR: $1${NC}"
    exit 1
}

# Function to check command success
check_success() {
    if [ $? -ne 0 ]; then
        print_error "$1"
    fi
}

print_warning() {
    echo -e "${YELLOW}! $1${NC}"
}

usage() {
    echo -e "${YELLOW}Mooncake Dependencies Installer${NC}"
    echo -e "Usage: sudo bash dependencies.sh [OPTIONS]"
    echo -e "\nOptions:"
    echo -e "  -y, --yes              Skip confirmation"
    echo -e "  --with-spdk            Install SPDK ${SPDK_VERSION} for NVMe-oF support"
    echo -e "  --with-cuda            Require CUDA SDK and suggest -DUSE_CUDA=ON"
    echo -e "  --with-musa            Require MUSA SDK and suggest -DUSE_MUSA=ON"
    echo -e "  --with-mlu             Require Neuware SDK and suggest -DUSE_MLU=ON"
    echo -e "  --with-maca            Require MACA SDK and suggest -DUSE_MACA=ON"
    echo -e "  --with-ascend          Require Ascend CANN and suggest -DUSE_ASCEND_DIRECT=ON"
    echo -e "  --with-ubshmem         Require Ascend CANN and suggest -DUSE_UBSHMEM=ON"
    echo -e "  --with-rocm, --with-hip Require ROCm SDK and suggest -DUSE_HIP=ON"
    echo -e "  --with-hygon           Require DTK SDK and suggest -DUSE_HYGON=ON"
    echo -e "  --with-corex           Require CoreX SDK and suggest -DUSE_COREX=ON"
    echo -e "  --skip-submodules      Do not initialize or update git submodules"
    echo -e "  --skip-yalantinglibs   Do not build and install extern/yalantinglibs"
    echo -e "  --skip-go              Do not install Go ${GOVER}"
    echo -e "  --check                Only print detected dependency information; do not install"
    echo -e "  --cmake-args           Print CMake options for requested --with-* SDKs and exit"
    echo -e "  -h, --help             Show this help message and exit"
}

read_os_release_value() {
    local key="$1"
    awk -F= -v key="$key" '
        $1 == key {
            value = $0
            sub(/^[^=]*=/, "", value)
            gsub(/^"|"$/, "", value)
            print value
            exit
        }
    ' "$OS_RELEASE_FILE"
}

# Function to detect OS
detect_os() {
    if [ -f "$OS_RELEASE_FILE" ]; then
        ID=$(read_os_release_value ID)
        VERSION_ID=$(read_os_release_value VERSION_ID)
        OS=$(echo "$ID" | tr '[:upper:]' '[:lower:]')
        OS_VERSION=$VERSION_ID
    elif [ -f /etc/redhat-release ]; then
        OS="centos"
    else
        print_error "Cannot detect OS. Supported OS: Ubuntu, Debian, CentOS, RHEL, Rocky, AlmaLinux, EulerOS, and openEuler."
    fi

    echo -e "${GREEN}Detected OS: $OS ${OS_VERSION:-unknown}${NC}"
}

first_line() {
    "$@" 2>&1 | head -n 1
}

print_tool_info() {
    local name="$1"
    local bin="$2"
    shift 2

    if command -v "$bin" >/dev/null 2>&1; then
        echo -e "  ${GREEN}✓${NC} ${name}: $(first_line "$bin" "$@")"
    else
        echo -e "  ${YELLOW}-${NC} ${name}: not found"
    fi
}

print_pkg_config_info() {
    local name="$1"
    local package="$2"

    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists "$package"; then
        echo -e "  ${GREEN}✓${NC} ${name}: $(pkg-config --modversion "$package")"
    else
        echo -e "  ${YELLOW}-${NC} ${name}: not found by pkg-config"
    fi
}

detect_sdk_path() {
    local env_value="$1"
    shift

    if [ -n "$env_value" ] && [ -d "$env_value" ]; then
        echo "$env_value"
        return
    fi

    for path in "$@"; do
        if [ -d "$path" ]; then
            echo "$path"
            return
        fi
    done
}

sdk_found_line() {
    local name="$1"
    local path="$2"

    if [ -n "$path" ]; then
        echo -e "  ${GREEN}✓${NC} ${name}: found at ${path}"
    else
        echo -e "  ${YELLOW}-${NC} ${name}: not detected"
    fi
}

detect_cuda_sdk() {
    if command -v nvcc >/dev/null 2>&1; then
        local nvcc_path
        nvcc_path=$(command -v nvcc)
        if command -v readlink >/dev/null 2>&1; then
            nvcc_path=$(readlink -f "$nvcc_path")
        fi
        dirname "$(dirname "$nvcc_path")"
        return
    fi
    detect_sdk_path "${CUDA_HOME:-${CUDA_PATH:-}}" /usr/local/cuda
}

detect_musa_sdk() {
    detect_sdk_path "${MUSA_HOME:-}" /usr/local/musa
}

detect_neuware_sdk() {
    detect_sdk_path "${NEUWARE_HOME:-}" /usr/local/neuware
}

detect_maca_sdk() {
    detect_sdk_path "${MACA_HOME:-${MACA_ROOT:-}}" /opt/maca
}

detect_ascend_sdk() {
    detect_sdk_path "${ASCEND_HOME_PATH:-}" /usr/local/Ascend/ascend-toolkit/latest /usr/local/Ascend/cann
}

detect_rocm_sdk() {
    detect_sdk_path "${ROCM_PATH:-}" /opt/rocm
}

detect_dtk_sdk() {
    detect_sdk_path "${DTK_HOME:-${DTK_ROOT:-}}" /opt/dtk
}

detect_corex_sdk() {
    detect_sdk_path "${COREX_HOME:-${COREX_ROOT:-}}" /usr/local/corex
}

print_optional_sdk_info() {
    local cuda_path musa_path neuware_path maca_path ascend_path rocm_path dtk_path corex_path

    print_section "Optional SDK detection"
    if command -v nvcc >/dev/null 2>&1; then
        echo -e "  ${GREEN}✓${NC} CUDA: $(nvcc --version | grep -E 'release|V[0-9]' | tail -n 1)"
    else
        cuda_path=$(detect_cuda_sdk)
        sdk_found_line "CUDA" "$cuda_path"
    fi

    musa_path=$(detect_musa_sdk)
    sdk_found_line "MUSA" "$musa_path"

    neuware_path=$(detect_neuware_sdk)
    sdk_found_line "Neuware" "$neuware_path"

    maca_path=$(detect_maca_sdk)
    sdk_found_line "MACA" "$maca_path"

    ascend_path=$(detect_ascend_sdk)
    sdk_found_line "Ascend CANN" "$ascend_path"

    rocm_path=$(detect_rocm_sdk)
    sdk_found_line "ROCm" "$rocm_path"

    dtk_path=$(detect_dtk_sdk)
    sdk_found_line "DTK" "$dtk_path"

    corex_path=$(detect_corex_sdk)
    sdk_found_line "CoreX" "$corex_path"

    if command -v ibv_devinfo >/dev/null 2>&1; then
        echo -e "  ${GREEN}✓${NC} RDMA tools: ibv_devinfo found"
    elif [ -d /dev/infiniband ]; then
        echo -e "  ${GREEN}✓${NC} RDMA devices: /dev/infiniband exists"
    else
        echo -e "  ${YELLOW}-${NC} RDMA: not detected"
    fi
}

append_cmake_option() {
    if [ -z "$REQUESTED_CMAKE_OPTIONS" ]; then
        REQUESTED_CMAKE_OPTIONS="$1"
    else
        REQUESTED_CMAKE_OPTIONS="${REQUESTED_CMAKE_OPTIONS} $1"
    fi
}

require_sdk() {
    local name="$1"
    local path="$2"
    local cmake_options="$3"
    local hint="$4"

    if [ -n "$path" ]; then
        if [ "$CMAKE_ARGS_ONLY" != true ]; then
            echo -e "  ${GREEN}✓${NC} ${name}: ${path}"
        fi
        append_cmake_option "$cmake_options"
    else
        if [ "$CMAKE_ARGS_ONLY" = true ]; then
            echo "${name}: not detected. ${hint}" >&2
        else
            echo -e "  ${RED}✗${NC} ${name}: not detected"
            if [ -n "$hint" ]; then
                echo -e "    ${YELLOW}${hint}${NC}"
            fi
        fi
        REQUESTED_SDK_MISSING=true
    fi
}

validate_requested_sdks() {
    REQUESTED_CMAKE_OPTIONS=""
    REQUESTED_SDK_MISSING=false

    if [ "$REQUIRE_CUDA" = false ] && [ "$REQUIRE_MUSA" = false ] && \
       [ "$REQUIRE_MLU" = false ] && [ "$REQUIRE_MACA" = false ] && \
       [ "$REQUIRE_ASCEND" = false ] && [ "$REQUIRE_UBSHMEM" = false ] && \
       [ "$REQUIRE_ROCM" = false ] && [ "$REQUIRE_HYGON" = false ] && \
       [ "$REQUIRE_COREX" = false ]; then
        return
    fi

    if [ "$CMAKE_ARGS_ONLY" != true ]; then
        print_section "Requested SDK detection"
    fi
    if [ "$REQUIRE_CUDA" = true ]; then
        require_sdk "CUDA" "$(detect_cuda_sdk)" "-DUSE_CUDA=ON" "Set CUDA_HOME/CUDA_PATH or install CUDA under /usr/local/cuda."
    fi
    if [ "$REQUIRE_MUSA" = true ]; then
        require_sdk "MUSA" "$(detect_musa_sdk)" "-DUSE_MUSA=ON" "Set MUSA_HOME or install MUSA under /usr/local/musa."
    fi
    if [ "$REQUIRE_MLU" = true ]; then
        local neuware_path
        neuware_path=$(detect_neuware_sdk)
        require_sdk "Neuware" "$neuware_path" "-DUSE_MLU=ON -DNEUWARE_ROOT=${neuware_path:-/path/to/neuware}" "Set NEUWARE_HOME or install Neuware under /usr/local/neuware."
    fi
    if [ "$REQUIRE_MACA" = true ]; then
        local maca_path
        maca_path=$(detect_maca_sdk)
        require_sdk "MACA" "$maca_path" "-DUSE_MACA=ON -DMACA_ROOT=${maca_path:-/path/to/maca}" "Set MACA_HOME/MACA_ROOT or install MACA under /opt/maca."
    fi
    if [ "$REQUIRE_ASCEND" = true ]; then
        require_sdk "Ascend CANN" "$(detect_ascend_sdk)" "-DUSE_ASCEND_DIRECT=ON" "Source set_env.sh or set ASCEND_HOME_PATH."
    fi
    if [ "$REQUIRE_UBSHMEM" = true ]; then
        require_sdk "Ascend CANN" "$(detect_ascend_sdk)" "-DUSE_UBSHMEM=ON" "Source set_env.sh or set ASCEND_HOME_PATH."
    fi
    if [ "$REQUIRE_ROCM" = true ]; then
        require_sdk "ROCm" "$(detect_rocm_sdk)" "-DUSE_HIP=ON" "Set ROCM_PATH or install ROCm under /opt/rocm."
    fi
    if [ "$REQUIRE_HYGON" = true ]; then
        local dtk_path
        dtk_path=$(detect_dtk_sdk)
        require_sdk "DTK" "$dtk_path" "-DUSE_HYGON=ON -DDTK_ROOT=${dtk_path:-/path/to/dtk}" "Set DTK_HOME/DTK_ROOT or install DTK under /opt/dtk."
    fi
    if [ "$REQUIRE_COREX" = true ]; then
        local corex_path
        corex_path=$(detect_corex_sdk)
        require_sdk "CoreX" "$corex_path" "-DUSE_COREX=ON -DCOREX_ROOT=${corex_path:-/path/to/corex}" "Set COREX_HOME/COREX_ROOT or install CoreX under /usr/local/corex."
    fi

    if [ "$REQUESTED_SDK_MISSING" = true ]; then
        if [ "$CMAKE_ARGS_ONLY" = true ]; then
            exit 1
        else
            print_error "Requested SDK detection failed. Install the missing vendor SDK or remove the corresponding --with-* option."
        fi
    fi

    if [ "$CMAKE_ARGS_ONLY" != true ]; then
        echo -e "  Suggested CMake options: ${GREEN}${REQUESTED_CMAKE_OPTIONS}${NC}"
    fi
}

print_dependency_info() {
    print_section "Dependency information"
    echo -e "Repository root: ${REPO_ROOT}"
    echo -e "OS: ${OS:-unknown} ${OS_VERSION:-unknown}"
    echo -e "Architecture: $(uname -m)"
    print_tool_info "gcc" gcc --version
    print_tool_info "g++" g++ --version
    print_tool_info "cmake" cmake --version
    print_tool_info "ninja" ninja --version
    print_tool_info "git" git --version
    print_tool_info "go" go version
    print_tool_info "python3" python3 --version
    print_tool_info "pkg-config" pkg-config --version

    print_section "pkg-config libraries"
    print_pkg_config_info "libcurl" libcurl
    print_pkg_config_info "hiredis" hiredis
    print_pkg_config_info "libibverbs" libibverbs
    print_pkg_config_info "liburing" liburing
    print_pkg_config_info "jemalloc" jemalloc
    print_pkg_config_info "yaml-cpp" yaml-cpp
    print_pkg_config_info "protobuf" protobuf
    print_pkg_config_info "grpc++" grpc++

    if [ -d "${REPO_ROOT}/extern/yalantinglibs" ]; then
        echo -e "  ${GREEN}✓${NC} yalantinglibs submodule: ${REPO_ROOT}/extern/yalantinglibs"
    else
        echo -e "  ${YELLOW}-${NC} yalantinglibs submodule: not present"
    fi

    if [ -d "${REPO_ROOT}/extern/spdk" ]; then
        echo -e "  ${GREEN}✓${NC} SPDK source: ${REPO_ROOT}/extern/spdk"
    else
        echo -e "  ${YELLOW}-${NC} SPDK source: not present"
    fi

    print_optional_sdk_info

    print_section "Suggested build flags"
    echo -e "  Base build: cmake .."
    echo -e "  NVMe-oF SSD pool: cmake .. -DUSE_NOF=ON  # after dependencies.sh --with-spdk"
    echo -e "  CUDA memory support: cmake .. -DUSE_CUDA=ON"
    echo -e "  MLU memory support: cmake .. -DUSE_MLU=ON -DNEUWARE_ROOT=/path/to/neuware"
    echo -e "  Ascend Direct: cmake .. -DUSE_ASCEND_DIRECT=ON"
    if [ -n "$REQUESTED_CMAKE_OPTIONS" ]; then
        echo -e "  Requested options: cmake .. ${REQUESTED_CMAKE_OPTIONS}"
    fi
}

# Parse command line arguments
SKIP_CONFIRM=false
INSTALL_SPDK=false
INIT_SUBMODULES=true
INSTALL_YALANTINGLIBS=true
INSTALL_GO=true
CHECK_ONLY=false
CMAKE_ARGS_ONLY=false
REQUIRE_CUDA=false
REQUIRE_MUSA=false
REQUIRE_MLU=false
REQUIRE_MACA=false
REQUIRE_ASCEND=false
REQUIRE_UBSHMEM=false
REQUIRE_ROCM=false
REQUIRE_HYGON=false
REQUIRE_COREX=false
REQUESTED_CMAKE_OPTIONS=""
for arg in "$@"; do
    case $arg in
        -y|--yes)
            SKIP_CONFIRM=true
            ;;
        --with-spdk)
            INSTALL_SPDK=true
            ;;
        --with-cuda)
            REQUIRE_CUDA=true
            ;;
        --with-musa)
            REQUIRE_MUSA=true
            ;;
        --with-mlu)
            REQUIRE_MLU=true
            ;;
        --with-maca)
            REQUIRE_MACA=true
            ;;
        --with-ascend)
            REQUIRE_ASCEND=true
            ;;
        --with-ubshmem)
            REQUIRE_UBSHMEM=true
            ;;
        --with-rocm|--with-hip)
            REQUIRE_ROCM=true
            ;;
        --with-hygon)
            REQUIRE_HYGON=true
            ;;
        --with-corex)
            REQUIRE_COREX=true
            ;;
        --skip-submodules)
            INIT_SUBMODULES=false
            ;;
        --skip-yalantinglibs)
            INSTALL_YALANTINGLIBS=false
            ;;
        --skip-go)
            INSTALL_GO=false
            ;;
        --check)
            CHECK_ONLY=true
            SKIP_CONFIRM=true
            ;;
        --cmake-args)
            CMAKE_ARGS_ONLY=true
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            print_error "Unknown option: $arg"
            ;;
    esac
done

if [ "$CMAKE_ARGS_ONLY" = true ]; then
    validate_requested_sdks
    if [ "$INSTALL_SPDK" = true ]; then
        append_cmake_option "-DUSE_NOF=ON"
    fi
    echo "$REQUESTED_CMAKE_OPTIONS"
    exit 0
fi

# Detect OS
detect_os
validate_requested_sdks
if [ "$INSTALL_SPDK" = true ]; then
    append_cmake_option "-DUSE_NOF=ON"
fi

if [ "$CHECK_ONLY" = true ]; then
    print_dependency_info
    exit 0
fi

if [ "$(id -u)" -ne 0 ]; then
    print_error "Require root permission, try sudo bash dependencies.sh"
fi

# Print welcome message
echo -e "${YELLOW}Mooncake Dependencies Installer${NC}"
echo -e "This script will install all required dependencies for Mooncake."
echo -e "The following components will be installed:"
echo -e "  - System packages (build tools, libraries)"
if [ "$INIT_SUBMODULES" = true ]; then
    echo -e "  - Git submodules (including pybind11 and yalantinglibs)"
else
    echo -e "  - Git submodules: skipped by --skip-submodules"
fi
if [ "$INSTALL_YALANTINGLIBS" = true ]; then
    echo -e "  - yalantinglibs"
else
    echo -e "  - yalantinglibs: skipped by --skip-yalantinglibs"
fi
if [ "$INSTALL_GO" = true ]; then
    echo -e "  - Go $GOVER"
else
    echo -e "  - Go: skipped by --skip-go"
fi
if [ "$INSTALL_SPDK" = true ]; then
    echo -e "  - SPDK (for NVMe-oF support)"
fi
if [ -n "$REQUESTED_CMAKE_OPTIONS" ]; then
    echo -e "  - Requested SDKs validated for: ${REQUESTED_CMAKE_OPTIONS}"
fi
echo

# Ask for confirmation unless -y flag is used
if [ "$SKIP_CONFIRM" = false ]; then
    read -p "Do you want to continue? [Y/n] " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]] && [[ ! $REPLY = "" ]]; then
        echo -e "${YELLOW}Installation cancelled.${NC}"
        exit 0
    fi
fi

# Update package lists
print_section "Updating package lists"
if [ "$OS" = "ubuntu" ] || [ "$OS" = "debian" ]; then
    apt-get update
    check_success "Failed to update package lists"
elif [ "$OS" = "centos" ] || [ "$OS" = "rhel" ] || [ "$OS" = "rocky" ] || [ "$OS" = "almalinux" ] || [ "$OS" = "euleros" ] || [ "$OS" = "openeuler" ]; then
    yum install -y dnf-plugins-core epel-release || true
    yum config-manager --set-enabled powertools || yum config-manager --set-enabled crb || true
    yum clean all
    yum makecache
    check_success "Failed to update package lists"
else
    print_error "Unsupported OS: $OS"
fi

# Install system packages
print_section "Installing system packages"
echo -e "${YELLOW}This may take a few minutes...${NC}"

if [ "$OS" = "ubuntu" ] || [ "$OS" = "debian" ]; then
    SYSTEM_PACKAGES="build-essential \
                     cmake \
                     ninja-build \
                     git \
                     wget \
                     unzip \
                     libibverbs-dev \
                     libgoogle-glog-dev \
                     libgtest-dev \
                     libjsoncpp-dev \
                     libunwind-dev \
                     libnuma-dev \
                     libpython3-dev \
                     libboost-all-dev \
                     libssl-dev \
                     libgrpc-dev \
                     libgrpc++-dev \
                     libprotobuf-dev \
                     libyaml-cpp-dev \
                     protobuf-compiler-grpc \
                     libcurl4-openssl-dev \
                     libhiredis-dev \
                     liburing-dev \
                     libjemalloc-dev \
                     libmsgpack-dev \
                     libzstd-dev \
                     libasio-dev \
                     libxxhash-dev \
                     pkg-config \
                     patchelf \
                     libc6-dev \
                     libc-bin"

    apt-get install -y $SYSTEM_PACKAGES
    check_success "Failed to install system packages"

elif [ "$OS" = "centos" ] || [ "$OS" = "rhel" ] || [ "$OS" = "rocky" ] || [ "$OS" = "almalinux" ] || [ "$OS" = "euleros" ] || [ "$OS" = "openeuler" ]; then
    SYSTEM_PACKAGES="@development \
                     cmake \
                     git \
                     wget \
                     rdma-core-devel \
                     glog-devel \
                     gflags-devel \
                     gtest-devel \
                     jsoncpp-devel \
                     libunwind-devel \
                     numactl-devel \
                     python3-devel \
                     boost1.78-devel \
                     openssl-devel \
                     protobuf-devel \
                     yaml-cpp-devel \
                     libcurl-devel \
                     hiredis-devel \
                     liburing-devel \
                     jemalloc-devel \
                     msgpack-devel \
                     libzstd-devel \
                     pkgconf-pkg-config \
                     elfutils-libelf-devel \
                     patchelf  \
                     xxhash-devel \
                     libbsd-devel"

    yum install -y $SYSTEM_PACKAGES
    check_success "Failed to install system packages"
else
    print_error "Unsupported OS: $OS"
fi

print_success "System packages installed successfully"

if [ "$INIT_SUBMODULES" = true ]; then
    # Initialize and update git submodules
    print_section "Initializing Git Submodules"

    # Check if .gitmodules exists
    if [ -f "${REPO_ROOT}/.gitmodules" ]; then
        echo "Enter repository root: ${REPO_ROOT}"
        cd "${REPO_ROOT}"
        check_success "Failed to change to repository root directory"

        echo "Initializing git submodules..."
        git submodule sync --recursive
        check_success "Failed to sync git submodules"
        git submodule update --init --recursive
        check_success "Failed to initialize git submodules"

        print_success "Git submodules initialized and updated successfully"
    else
        print_error "No .gitmodules file found in ${REPO_ROOT}"
    fi
else
    print_warning "Skipping git submodule initialization"
fi

if [ "$INSTALL_YALANTINGLIBS" = true ]; then
    # Build and install yalantinglibs from submodule
    print_section "Installing yalantinglibs"
    cd "${REPO_ROOT}/extern/yalantinglibs"
    check_success "Failed to change to yalantinglibs submodule directory"

    mkdir -p build
    check_success "Failed to create build directory"
    cd build
    check_success "Failed to change to build directory"

    echo "Configuring yalantinglibs..."
    cmake .. -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARK=OFF -DBUILD_UNIT_TESTS=OFF
    check_success "Failed to configure yalantinglibs"

    echo "Building yalantinglibs (using $(nproc) cores)..."
    cmake --build . -j$(nproc)
    check_success "Failed to build yalantinglibs"

    echo "Installing yalantinglibs..."
    cmake --install .
    check_success "Failed to install yalantinglibs"

    print_success "yalantinglibs installed successfully"
    cd "${REPO_ROOT}"
else
    print_warning "Skipping yalantinglibs installation"
fi

print_section "Verifying essential build tools"

# Verify getconf and ldd (required for glibc version detection in build_wheel.sh)
if [ "$OS" = "ubuntu" ] || [ "$OS" = "debian" ]; then
    if ! command -v getconf >/dev/null 2>&1; then
        print_error "getconf not found after installing system packages. This should not happen."
    fi
    if ! command -v ldd >/dev/null 2>&1; then
        print_error "ldd not found after installing system packages. This should not happen."
    fi
    print_success "getconf found: $(getconf --version 2>&1 | head -1)"
    print_success "ldd found: $(ldd --version 2>&1 | head -1)"
fi

if [ "$INSTALL_GO" = true ]; then
    print_section "Installing Go $GOVER"

    USED_CN_MIRROR=false

    install_go() {
        ARCH=$(uname -m)
        if [ "$ARCH" = "aarch64" ]; then
            ARCH="arm64"
        elif [ "$ARCH" = "x86_64" ]; then
            ARCH="amd64"
        else
            print_error "Unsupported architecture: $ARCH"
        fi

        GO_TARBALL="go$GOVER.linux-$ARCH.tar.gz"

        # Try multiple download mirrors with fallback
        GO_DOWNLOAD_URLS=(
            "https://go.dev/dl/${GO_TARBALL}"
            "https://golang.google.cn/dl/${GO_TARBALL}"
            "https://mirrors.aliyun.com/golang/${GO_TARBALL}"
        )

        DOWNLOAD_SUCCESS=false
        for url in "${GO_DOWNLOAD_URLS[@]}"; do
            echo "Downloading Go $GOVER from ${url}..."
            if wget -q --show-progress --timeout=30 --tries=2 -O "${GO_TARBALL}" "${url}"; then
                DOWNLOAD_SUCCESS=true
                if [[ "$url" != "https://go.dev/dl/${GO_TARBALL}" ]]; then
                    USED_CN_MIRROR=true
                fi
                print_success "Downloaded Go $GOVER from ${url}"
                break
            else
                echo -e "${YELLOW}Failed to download from ${url}, trying next mirror...${NC}"
                rm -f "${GO_TARBALL}"
            fi
        done

        if [ "$DOWNLOAD_SUCCESS" = false ]; then
            print_error "Failed to download Go $GOVER from all mirrors"
        fi

        echo "Installing Go $GOVER..."
        tar -C /usr/local -xzf "${GO_TARBALL}"
        check_success "Failed to install Go $GOVER"

        rm -f "${GO_TARBALL}"
        check_success "Failed to clean up Go installation file"

        print_success "Go $GOVER installed successfully"
    }

    if command -v go &> /dev/null; then
        GO_VERSION=$(go version | awk '{print $3}')
        if [[ "$GO_VERSION" == "go$GOVER" ]]; then
            echo -e "${YELLOW}Go $GOVER is already installed. Skipping...${NC}"
        else
            echo -e "${YELLOW}Found Go $GO_VERSION. Will install Go $GOVER...${NC}"
            install_go
        fi
    else
        install_go
    fi

    # Add Go to PATH if not already there
    if ! grep -q "export PATH=\$PATH:/usr/local/go/bin" ~/.bashrc; then
        echo -e "${YELLOW}Adding Go to your PATH in ~/.bashrc${NC}"
        echo 'export PATH=$PATH:/usr/local/go/bin' >> ~/.bashrc
        echo -e "${YELLOW}Please run 'source ~/.bashrc' or start a new terminal to use Go${NC}"
    fi
    if [ -x /usr/local/go/bin/go ] && ! command -v go >/dev/null 2>&1; then
        export PATH="$PATH:/usr/local/go/bin"
    fi

    # Set GOPROXY only if Go download fell back to a CN mirror
    if [ "$USED_CN_MIRROR" = true ] && [ -z "$GOPROXY" ]; then
        export GOPROXY=https://goproxy.cn,https://goproxy.io,direct
        echo -e "${YELLOW}Detected restricted network (Go was downloaded from a CN mirror).${NC}"
        echo -e "${YELLOW}GOPROXY set to: ${GOPROXY}${NC}"
        if ! grep -q "export GOPROXY=" ~/.bashrc; then
            echo 'export GOPROXY=https://goproxy.cn,https://goproxy.io,direct' >> ~/.bashrc
            echo -e "${YELLOW}GOPROXY added to ~/.bashrc for future sessions${NC}"
        fi
    elif [ -n "$GOPROXY" ]; then
        echo -e "${GREEN}GOPROXY already set to: ${GOPROXY}${NC}"
    fi
else
    print_warning "Skipping Go installation"
fi

# Install SPDK if requested
if [ "$INSTALL_SPDK" = true ]; then
    print_section "Installing SPDK"

    cd "${REPO_ROOT}/extern"
    check_success "Failed to change to extern directory"

    # Remove existing SPDK if present
    if [ -d "spdk" ]; then
        echo -e "${YELLOW}SPDK directory already exists. Removing for fresh install...${NC}"
        rm -rf spdk
        check_success "Failed to remove existing SPDK directory"
    fi

    # Clone SPDK
    echo "Cloning SPDK from ${GITHUB_PROXY}/spdk/spdk.git..."
    git clone ${GITHUB_PROXY}/spdk/spdk.git
    check_success "Failed to clone SPDK"

    cd spdk
    check_success "Failed to change to SPDK directory"

    # Checkout specific version
    echo "Checking out SPDK version ${SPDK_VERSION}..."
    git checkout ${SPDK_VERSION}
    check_success "Failed to checkout SPDK version ${SPDK_VERSION}"

    # Initialize submodules
    echo "Initializing SPDK submodules..."
    git submodule update --init
    check_success "Failed to initialize SPDK submodules"

    # Install SPDK dependencies
    echo "Installing SPDK dependencies..."
    ./scripts/pkgdep.sh
    check_success "Failed to install SPDK dependencies"

    # Configure SPDK with RDMA support
    echo "Configuring SPDK with RDMA support..."
    ./configure --with-rdma
    check_success "Failed to configure SPDK"

    # Build SPDK
    echo "Building SPDK (using $(nproc) cores)..."
    make -j$(nproc)
    check_success "Failed to build SPDK"

    # Install SPDK
    echo "Installing SPDK..."
    make install
    check_success "Failed to install SPDK"

    # Copy DPDK libraries to system library path
    if ls dpdk/build/lib/*.a >/dev/null 2>&1; then
        echo "Copying DPDK libraries to /usr/local/lib..."
        cp dpdk/build/lib/*.a /usr/local/lib/
        check_success "Failed to copy DPDK libraries"
    fi

    print_success "SPDK installed successfully"
    cd "${REPO_ROOT}"
fi

# Return to the repository root
cd "${REPO_ROOT}"

# Print summary
print_section "Installation Complete"
echo -e "${GREEN}All dependencies have been successfully installed!${NC}"
echo -e "The following components were installed:"
echo -e "  ${GREEN}✓${NC} System packages"
[ "$INIT_SUBMODULES" = true ] && echo -e "  ${GREEN}✓${NC} Git submodules" || echo -e "  ${YELLOW}-${NC} Git submodules skipped"
[ "$INSTALL_YALANTINGLIBS" = true ] && echo -e "  ${GREEN}✓${NC} yalantinglibs" || echo -e "  ${YELLOW}-${NC} yalantinglibs skipped"
[ "$INSTALL_GO" = true ] && echo -e "  ${GREEN}✓${NC} Go $GOVER" || echo -e "  ${YELLOW}-${NC} Go skipped"
if [ "$INSTALL_SPDK" = true ]; then
    echo -e "  ${GREEN}✓${NC} SPDK (${SPDK_VERSION})"
fi
if [ -n "$REQUESTED_CMAKE_OPTIONS" ]; then
    echo -e "  ${GREEN}✓${NC} Requested SDKs: ${REQUESTED_CMAKE_OPTIONS}"
fi
echo
print_dependency_info
echo
echo -e "You can now build and run Mooncake."
if [ "$INSTALL_GO" = true ]; then
    echo -e "${YELLOW}Note: You may need to restart your terminal or run 'source ~/.bashrc' to use Go.${NC}"
fi

if [ "$INSTALL_SPDK" = true ]; then
    echo -e "${YELLOW}Note: SPDK requires hugepages and RDMA configuration. Please refer to SPDK documentation for setup.${NC}"
fi
