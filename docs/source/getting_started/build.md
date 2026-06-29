# Build Guide

This document describes how to build Mooncake.

## PyPI Package
Install the Mooncake Transfer Engine package from PyPI, which includes both Mooncake Transfer Engine and Mooncake Store Python bindings:

**For CUDA-enabled systems:**
```bash
pip install mooncake-transfer-engine
```
📦 **Package Details**: [https://pypi.org/project/mooncake-transfer-engine/](https://pypi.org/project/mooncake-transfer-engine/)

**For non-CUDA systems:**
```bash
pip install mooncake-transfer-engine-non-cuda
```
📦 **Package Details**: [https://pypi.org/project/mooncake-transfer-engine-non-cuda/](https://pypi.org/project/mooncake-transfer-engine-non-cuda/)

> **Note**: The CUDA version includes Mooncake-EP and GPU topology detection, requiring CUDA 12.1+. The non-CUDA version is for environments without CUDA dependencies, but it still requires the system runtime libraries used by the transfer stack. On Ubuntu, install them with `sudo apt-get update && sudo apt-get install -y libcurl4 libibverbs1 rdma-core librdmacm1 libnuma1 liburing2`.
> **Note**: MLU support is currently source-build only. If you need Cambricon MLU memory support, install Neuware and build with `-DUSE_MLU=ON`.

## Source Build

### Recommended Version
- OS: Ubuntu 22.04 LTS+
- cmake: 3.20.x
- gcc: 9.4+

### Install dependencies
Use `dependencies.sh` as the single entry point for source-build dependencies. The script detects the current Linux distribution, installs the matching system packages, initializes submodules, installs yalantinglibs, installs the required Go version, and prints a dependency summary after it finishes.

```bash
sudo bash dependencies.sh
```

Useful options:
- `-y, --yes`: skip the interactive confirmation.
- `--with-spdk`: also install SPDK for NVMe-oF SSD pool support.
- `--with-cuda`, `--with-musa`, `--with-mlu`, `--with-maca`, `--with-ascend`, `--with-ubshmem`, `--with-rocm`, `--with-hygon`, `--with-corex`: require the selected vendor SDK to be present and print the matching CMake options.
- `--check`: print detected toolchain, library, RDMA, and optional SDK information without installing anything.
- `--cmake-args`: print only the CMake options implied by the selected `--with-*` arguments.
- `--skip-submodules`: skip submodule initialization when submodules are already prepared.
- `--skip-yalantinglibs`: skip rebuilding `extern/yalantinglibs`.
- `--skip-go`: skip Go installation when your environment already provides a compatible version.

Examples:
```bash
# Inspect the current environment first.
bash dependencies.sh --check

# Non-interactive dependency installation.
sudo bash dependencies.sh -y

# Install dependencies for NVMe-oF SSD pool builds.
sudo bash dependencies.sh -y --with-spdk

# Validate CUDA before installing common dependencies.
bash dependencies.sh --check --with-cuda

# Validate Neuware and print the CMake flags for MLU builds.
bash dependencies.sh --check --with-mlu

# Generate CMake options from the same dependency choices.
bash dependencies.sh --cmake-args --with-cuda --with-spdk
```

### Build Mooncake
In the root directory of this project, run:

```bash
mkdir -p build
cd build
cmake ..
make -j
sudo make install
```

When building with optional SDKs, use `--cmake-args` to keep the dependency choices and CMake configuration aligned:

```bash
CMAKE_ARGS="$(bash dependencies.sh --cmake-args --with-cuda)"
cmake -S . -B build ${CMAKE_ARGS}
cmake --build build -j
sudo cmake --install build
```

For NVMe-oF SSD pool support, install SPDK through the dependency script and enable `USE_NOF`:

```bash
sudo bash dependencies.sh --with-spdk

CMAKE_ARGS="$(bash dependencies.sh --cmake-args --with-spdk)"
cmake -S . -B build ${CMAKE_ARGS}
cmake --build build -j
sudo cmake --install build
```

`-DUSE_NOF=ON` builds the NoF registration APIs and deployment tools. Use `-DUSE_NOF=OFF` or omit the option when the NVMe-oF SSD pool is not needed.

### Optional accelerator SDKs
`dependencies.sh` installs common build and runtime dependencies from the OS package manager. Vendor SDKs such as CUDA, MUSA, Neuware, MACA, Ascend CANN, ROCm, DTK, and CoreX should still be installed from their vendor distributions, but the script can validate them when you pass the matching `--with-*` option. Run `bash dependencies.sh --check --with-cuda` or `bash dependencies.sh --check --with-mlu` after installing an SDK to confirm what the script detects and which CMake flags to use.

**NVIDIA CUDA / GPUDirect**
Follow the [CUDA Linux installation guide](https://docs.nvidia.com/cuda/cuda-installation-guide-linux/) and enable `nvidia-fs` if you need `cuFile` support. Then make sure CUDA libraries are visible during build and runtime:
```bash
export LIBRARY_PATH=$LIBRARY_PATH:/usr/local/cuda/lib64
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/cuda/lib64
```

```{admonition} GPU-Direct RDMA
:class: note
Mooncake can use the DMA-BUF path for GPU-Direct RDMA, which does **not** require the `nvidia-peermem` kernel module. If you prefer the DMA-BUF path, set `WITH_NVIDIA_PEERMEM=0` before starting Mooncake. If you prefer the legacy `ibv_reg_mr` path, set `WITH_NVIDIA_PEERMEM=1`. See Section 3.7 of the [GPUDirect RDMA guide](https://docs.nvidia.com/cuda/gpudirect-rdma/) for `nvidia-peermem` instructions.
```

**Moore Threads MUSA**
Install the MUSA SDK from the [MUSA SDK installation guide](https://docs.mthreads.com/musa-sdk/musa-sdk-doc-online/install_guide), install `mthreads-peermem` for GPU-Direct RDMA, and expose the runtime libraries:
```bash
export LIBRARY_PATH=$LIBRARY_PATH:/usr/local/musa/lib
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/musa/lib
```

**Cambricon MLU / Neuware**
Install the Cambricon Neuware SDK, then export `NEUWARE_HOME` or pass `-DNEUWARE_ROOT=/path/to/neuware` to CMake:
```bash
export NEUWARE_HOME=/usr/local/neuware
export LIBRARY_PATH=$LIBRARY_PATH:${NEUWARE_HOME}/lib64
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:${NEUWARE_HOME}/lib64

cmake .. -DUSE_MLU=ON -DNEUWARE_ROOT=${NEUWARE_HOME:-/usr/local/neuware}
make -j
```

If your Neuware installation lives outside the default include/library layout, pass `-DMLU_INCLUDE_DIR=/path/to/neuware/include` and `-DMLU_LIB_DIR=/path/to/neuware/lib64`.

**MetaX MACA**
Install the MACA SDK so headers and libraries are available under `MACA_ROOT` or `MACA_HOME`. SDK layouts vary; include both `lib` and `lib64` when needed:
```bash
export MACA_HOME=/opt/maca
export LIBRARY_PATH=$LIBRARY_PATH:${MACA_HOME}/lib:${MACA_HOME}/lib64
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:${MACA_HOME}/lib:${MACA_HOME}/lib64
```

Build with `-DUSE_MACA=ON`. Optional overrides include `-DMACA_ROOT=/path/to/maca`, `-DMACA_INCLUDE_DIR=/path/to/maca/include`, `-DMACA_LIB_DIR=/path/to/maca/lib64`, and `-DMACA_RUNTIME_LIBS="mcruntime;mxc-runtime64;rt"`.

**Huawei Ascend CANN**
Install Ascend CANN Toolkit from [Huawei Ascend documentation](https://www.hiascend.com/document), then source `set_env.sh` in the CANN installation directory:
```bash
source /usr/local/Ascend/cann/set_env.sh
cmake .. -DUSE_ASCEND_DIRECT=ON
make -j
```

Mooncake provides two Ascend NPU transport paths:
- `-DUSE_ASCEND_DIRECT=ON` (**recommended**): Ascend Direct transport based on the ADXL engine. Refer to the [Version Compatibility Guide](https://gitcode.com/cann/hixl/wiki/Mooncake%20+%20HIXL%20%E5%BF%AB%E9%80%9F%E4%B8%8A%E6%89%8B%E6%8C%87%E5%8D%97.md) for details.
- `-DUSE_UBSHMEM=ON`: shared memory transport based on CANN VMM APIs. Requires CANN >= 9.0.0, driver >= 26.0.0, and Lingqu >= 1.5.

## Use Mooncake in Docker Containers
Mooncake supports Docker-based deployment. You can either build the image from
this repository with `docker/mooncake.Dockerfile` or substitute a published
tag that matches the release you want to run.
For the container to use the host's network resources, you need to add the `--device` option when starting the container. The following is an example.

```
# In host
sudo docker build -f docker/mooncake.Dockerfile -t mooncake:from-source .
sudo docker run --net=host --device=/dev/infiniband/uverbs0 --device=/dev/infiniband/rdma_cm --ulimit memlock=-1 -t -i mooncake:from-source /bin/bash
# Run transfer engine in container
cd /Mooncake-main/build/mooncake-transfer-engine/example
./transfer_engine_bench --device_name=ibp6s0 --metadata_server=10.1.101.3:2379 --mode=target --local_server_name=10.1.100.3
```

For SGLang HiCache deployments inside Docker, reserve HugeTLB pages on the host before starting the container and pass the allocator settings through the container environment:

```bash
python3 scripts/check_hicache_hugepage_requirements.py \
  --tp-size 4 \
  --hicache-size 64gb \
  --global-segment-size 8gb \
  --arena-pool-size 56gb \
  --available-hugetlb 512gb

sudo sysctl -w vm.nr_hugepages=262144
grep -E 'HugePages_Total|HugePages_Free|Hugepagesize' /proc/meminfo

sudo docker run --gpus all \
  --net=host \
  --ipc=host \
  --ulimit memlock=-1 \
  --shm-size=128g \
  --device=/dev/infiniband/uverbs0 \
  --device=/dev/infiniband/rdma_cm \
  -e MC_STORE_USE_HUGEPAGE=1 \
  -e MC_STORE_HUGEPAGE_SIZE=2MB \
  -e MOONCAKE_GLOBAL_SEGMENT_SIZE=8gb \
  -e MC_MMAP_ARENA_POOL_SIZE=56gb \
  -t -i mooncake:from-source /bin/bash
```

The `64gb` / `56gb` values above are tuned examples for large HiCache deployments, not defaults. The arena remains disabled unless you explicitly enable it, and if you enable it via gflag without an env override the default pool size is `8gb`. On smaller hosts, start with `8gb` or `16gb` and size upward with the helper. When you want the baseline direct-`mmap()` path instead of the arena, set `MC_DISABLE_MMAP_ARENA=1` (also accepts `true`, `yes`, or `on`) and omit `MC_MMAP_ARENA_POOL_SIZE`. Set it before the first Mooncake mmap-buffer allocation in the process. If you build the image from source with `docker/mooncake.Dockerfile`, that source-built image also installs the helper as `mooncake-hicache-sizing`.
Without `MC_STORE_USE_HUGEPAGE=1`, the arena may opportunistically try hugepages and then retry on regular pages if HugeTLB is unavailable. When `MC_STORE_USE_HUGEPAGE=1` is set, both the arena path and the direct-`mmap()` fallback path require HugeTLB pages. Mooncake will not silently degrade that explicit hugepage request to regular pages.

## Advanced Compile Options
The following options can be used during `cmake ..` to specify whether to compile certain components of Mooncake.
- `-DUSE_CUDA=[ON|OFF]`: Enable GPU memory support (GPUDirect RDMA, NVMe-oF, and GPU-aware TCP transport). **Default: OFF.** Required when transferring GPU memory (e.g., KV cache in vLLM disaggregated serving), even when using TCP protocol.
- `-DUSE_MNNVL=[ON|OFF]`: Enable Multi-Node NVLink transport support, default is OFF. **Note:** `-DUSE_CUDA` is required when `-DUSE_MNNVL` is on (not used when building with `-DUSE_MUSA=ON`, `-DUSE_HIP=ON`, or `-DUSE_MACA=ON`).
- `-DUSE_MUSA=[ON|OFF]`: Enable Moore Threads GPU support via MUSA
- `-DUSE_MACA=[ON|OFF]`: Enable MetaX (Muxi) GPU support via MACA.
- `-DMACA_ROOT=/path/to/maca`: Override the MACA SDK root (`MACA_HOME` env var is also honored; default `/opt/maca`).
- `-DMACA_INCLUDE_DIR=/path/to/include`: Override MACA include directory when `-DUSE_MACA=ON`.
- `-DMACA_LIB_DIR=/path/to/lib64`: Override MACA library directory when `-DUSE_MACA=ON`.
- `-DMACA_RUNTIME_LIBS="mcruntime;mxc-runtime64;rt"`: Override MACA runtime libraries linked by `transfer_engine`.
- `-DUSE_HIP=[ON|OFF]`: Enable AMD GPU support via HIP/ROCm
- `-DUSE_HYGON=[ON|OFF]`: Enable Hygon DCU support via DTK SDK. **Default: OFF.** Uses CUDA-compatible runtime.
- `-DDTK_ROOT=/path/to/dtk`: Override the default DTK SDK root used when `-DUSE_HYGON=ON`. If unset, Mooncake uses `DTK_HOME` or `/opt/dtk`.
- `-DDTK_INCLUDE_DIR=/path/to/include`: Override the DTK include directory when `-DUSE_HYGON=ON`.
- `-DDTK_LIB_DIR=/path/to/lib64`: Override the DTK library directory when `-DUSE_HYGON=ON`.
- `-DUSE_COREX=[ON|OFF]`: Enable Iluvatar CoreX GPU support. **Default: OFF.** Uses CUDA-compatible runtime.
- `-DCOREX_ROOT=/path/to/corex`: Override the default CoreX SDK root used when `-DUSE_COREX=ON`. If unset, Mooncake uses `COREX_HOME` or `/usr/local/corex`.
- `-DCOREX_INCLUDE_DIR=/path/to/include`: Override the CoreX include directory when `-DUSE_COREX=ON`.
- `-DCOREX_LIB_DIR=/path/to/lib`: Override the CoreX library directory when `-DUSE_COREX=ON`.
- `-DUSE_MLU=[ON|OFF]`: Enable Cambricon MLU memory support via Neuware. **Default: OFF.** Supports MLU memory detection, topology discovery, and RDMA registration for Transfer Engine.
- `-DNEUWARE_ROOT=/path/to/neuware`: Override the default Neuware SDK root used when `-DUSE_MLU=ON`. If unset, Mooncake uses `NEUWARE_HOME` or `/usr/local/neuware`.
- `-DMLU_INCLUDE_DIR=/path/to/include`: Override the Neuware include directory when `-DUSE_MLU=ON`.
- `-DMLU_LIB_DIR=/path/to/lib64`: Override the Neuware library directory when `-DUSE_MLU=ON`.
- `-DUSE_EFA=[ON|OFF]`: Enable AWS Elastic Fabric Adapter transport via libfabric. **Default: OFF.** See [EFA Transport](../design/transfer-engine/efa_transport.md) for details.
- `-DUSE_INTRA_NVLINK=[ON|OFF]`: Enable intranode nvlink transport
- `-DUSE_CXL=[ON|OFF]`: Enable CXL support
- `-DWITH_STORE=[ON|OFF]`: Build Mooncake Store component
- `-DWITH_P2P_STORE=[ON|OFF]`: Enable Golang support and build P2P Store component, require go 1.23+
- `-DWITH_RUST_EXAMPLE=[ON|OFF]`: Build the Transfer Engine Rust interface and sample code. **Default: OFF.**
- `-DWITH_STORE_RUST=[ON|OFF]`: Build Mooncake Store Rust bindings and CMake Rust targets. **Default: ON.**
- `-DWITH_EP=[ON|OFF]`: Build the EP (Expert Parallelism) and PG Python extensions for CUDA. Requires CUDA toolkit and PyTorch. Use `-DEP_TORCH_VERSIONS="2.9.1"` (semicolon-separated) to build for specific PyTorch versions, or leave empty to use the currently-installed torch. The CUDA version is detected automatically. **Default: OFF.**
- `-DUSE_REDIS=[ON|OFF]`: Enable Redis-based metadata service for the Transfer Engine, require hiredis
- `-DUSE_HTTP=[ON|OFF]`: Enable Http-based metadata service
- `-DUSE_ETCD=[ON|OFF]`: Enable etcd-based metadata service, require go 1.23+
- `-DSTORE_USE_ETCD=[ON|OFF]`: Enable etcd-based failover for Mooncake Store, require go 1.23+. **Note:** `-DUSE_ETCD` and `-DSTORE_USE_ETCD` are two independent options. Enabling `-DSTORE_USE_ETCD` does **not** depend on `-DUSE_ETCD`
- `-DSTORE_USE_REDIS=[ON|OFF]`: Enable Redis-based failover for Mooncake Store, require hiredis. **Default: OFF.** **Note:** `-DUSE_REDIS` and `-DSTORE_USE_REDIS` are two independent options. Enabling `-DSTORE_USE_REDIS` does **not** depend on `-DUSE_REDIS`.
- `-DBUILD_SHARED_LIBS=[ON|OFF]`: Build Transfer Engine as shared library, default is OFF
- `-DBUILD_UNIT_TESTS=[ON|OFF]`: Build unit tests, default is ON
- `-DBUILD_EXAMPLES=[ON|OFF]`: Build examples, default is ON
- `-DUSE_ASCEND_DIRECT=[ON|OFF]`: Enable Ascend Direct transport and HCCS support via the ADXL engine (**recommended**).
- `-DUSE_UBSHMEM=[ON|OFF]`: Enable Huawei Ascend NPU shared memory transport via CANN VMM APIs.
