# Build Guide

This document describes how to build Mooncake.

## Automatic Build

### Recommended Version
- OS: Ubuntu 22.04 LTS+
- cmake: 3.20.x
- gcc: 9.4+

### Steps
1. Install dependencies, stable Internet connection is required:
   ```bash
   bash dependencies.sh
   ```

2. In the root directory of this project, run the following commands:
   ```bash
   mkdir build
   cd build
   cmake ..
   make -j
   ```
3. Install Mooncake python package and mooncake_master executable
   ```bash
   sudo make install
   ```

## Manual Build

### Recommended Version
- cmake: 3.22.x
- boost-devel: 1.66.x
- googletest: 1.12.x
- gcc: 10.2.1
- go: 1.22+
- hiredis
- curl

### Steps

1. Install dependencies from system software repository:
    ```bash
    # For debian/ubuntu
    apt-get install -y build-essential \
                       cmake \
                       libibverbs-dev \
                       libgoogle-glog-dev \
                       libgtest-dev \
                       libjsoncpp-dev \
                       libnuma-dev \
                       libunwind-dev \
                       libpython3-dev \
                       libboost-dev \
                       libssl-dev \
                       pybind11-dev \
                       libcurl4-openssl-dev \
                       libhiredis-dev \
                       pkg-config \
                       patchelf

    # For centos/alibaba linux os
    yum install cmake \
                gflags-devel \
                glog-devel \
                libibverbs-devel \
                numactl-devel \
                gtest \
                gtest-devel \
                boost-devel \
                openssl-devel \
                hiredis-devel \
                libcurl-devel
    ```

    NOTE: You may need to install gtest, glog, gflags from source code:
    ```bash
    git clone https://github.com/gflags/gflags
    git clone https://github.com/google/glog
    git clone https://github.com/abseil/googletest.git
    ```

2. If you want to compile the GPUDirect support module, first follow the instructions in https://docs.nvidia.com/cuda/cuda-installation-guide-linux/ to install CUDA (ensure to enable `nvidia-fs` for proper `cuFile` module compilation). After that:
    1) Configure `LIBRARY_PATH` and `LD_LIBRARY_PATH` to ensure linking of `cuFile`, `cudart`, and other libraries during compilation:
    ```bash
    export LIBRARY_PATH=$LIBRARY_PATH:/usr/local/cuda/lib64
    export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/cuda/lib64
    ```

    ```{admonition} GPU-Direct RDMA
    :class: note
    Mooncake could use the DMA-BUF path for GPU-Direct RDMA, which does **not** require the `nvidia-peermem` kernel module. If you prefer the DMA-BUF path, please set the runtime environment variable `WITH_NVIDIA_PEERMEM=0` before starting Mooncake. If you prefer the legacy `ibv_reg_mr` path (which requires `nvidia-peermem`), set the runtime environment variable `WITH_NVIDIA_PEERMEM=1`. See Section 3.7 of https://docs.nvidia.com/cuda/gpudirect-rdma/ for instructions on installing `nvidia-peermem`.
    ```

3. If you want to compile the Moore Mthreads GPUDirect support module, first follow the instructions in https://docs.mthreads.com/musa-sdk/musa-sdk-doc-online/install_guide to install MUSA. After that:
    1) Install `mthreads-peermem` for enabling GPU-Direct RDMA
    2) Configure `LIBRARY_PATH` and `LD_LIBRARY_PATH` to ensure linking of `musart`, and other libraries during compilation:
    ```bash
    export LIBRARY_PATH=$LIBRARY_PATH:/usr/local/musa/lib
    export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/musa/lib
    ```

4. If you want to compile Cambricon MLU support, first install the Cambricon Neuware SDK. After that:
    1) Export `NEUWARE_HOME` or pass `-DNEUWARE_ROOT=/path/to/neuware` to CMake
    2) Configure `LIBRARY_PATH` and `LD_LIBRARY_PATH` to ensure linking of `cnrt`, `cndrv`, and other Neuware libraries during compilation:
    ```bash
    export NEUWARE_HOME=/usr/local/neuware
    export LIBRARY_PATH=$LIBRARY_PATH:${NEUWARE_HOME}/lib64
    export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:${NEUWARE_HOME}/lib64
    ```

    If your Neuware installation lives outside the default include/library layout, you can also pass:
    ```bash
    cmake .. -DUSE_MLU=ON \
      -DMLU_INCLUDE_DIR=/path/to/neuware/include \
      -DMLU_LIB_DIR=/path/to/neuware/lib64
    ```

    For Cambricon MLU builds, enable the MLU backend explicitly:
    ```bash
    cmake .. -DUSE_MLU=ON -DNEUWARE_ROOT=${NEUWARE_HOME:-/usr/local/neuware}
    make -j
    ```

5. If you want to compile MetaX (Muxi) MACA support (e.g. C500), install the MACA SDK so headers and libraries are available under `MACA_ROOT` (defaults to `MACA_HOME` env var if set, otherwise `/opt/maca`). SDK layouts vary; include both `lib` and `lib64` in runtime paths when needed:
    ```bash
    export MACA_HOME=/opt/maca
    export LIBRARY_PATH=$LIBRARY_PATH:${MACA_HOME}/lib:${MACA_HOME}/lib64
    export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:${MACA_HOME}/lib:${MACA_HOME}/lib64
    ```
    Build with `-DUSE_MACA=ON`. Optional overrides:
    - `-DMACA_ROOT=/path/to/maca`
    - `-DMACA_INCLUDE_DIR=/path/to/maca/include`
    - `-DMACA_LIB_DIR=/path/to/maca/lib64`
    - `-DMACA_RUNTIME_LIBS="mcruntime;mxc-runtime64;rt"` (semicolon-separated CMake list)

6. Install yalantinglibs
    ```bash
    git clone https://github.com/alibaba/yalantinglibs.git
    cd yalantinglibs
    mkdir build && cd build
    cmake .. -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARK=OFF -DBUILD_UNIT_TESTS=OFF
    make -j$(nproc)
    make install
    ```

7. In the root directory of this project, run the following commands:
   ```bash
   mkdir build
   cd build
   cmake ..
   make -j
   ```

8. Install Mooncake python package and mooncake_master executable
   ```bash
   make install
   ```

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

### Build Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_SHARED_LIBS` | OFF | Build Transfer Engine as shared library |
| `BUILD_UNIT_TESTS` | ON | Build unit tests |
| `BUILD_EXAMPLES` | ON | Build examples |

### Component Build

| Option | Default | Description |
|--------|---------|-------------|
| `WITH_STORE` | ON | Build Mooncake Store component |
| `WITH_P2P_STORE` | OFF | Build P2P Store component, requires go 1.23+ |
| `WITH_EP` | OFF | Build EP (Expert Parallelism) and PG Python extensions for CUDA. Requires CUDA toolkit and PyTorch. Use `EP_TORCH_VERSIONS="2.9.1"` (semicolon-separated) to build for specific PyTorch versions, or leave empty to use the currently-installed torch |
| `WITH_RUST_EXAMPLE` | OFF | Build the Transfer Engine Rust interface and sample code |
| `WITH_STORE_RUST` | ON | Build Mooncake Store Rust bindings |

### GPU Backend Support

| GPU | Option | Default | Description |
|-----|--------|---------|-------------|
| NVIDIA | `USE_CUDA` | OFF | GPU memory support (GPUDirect RDMA, NVMe-oF, GPU-aware TCP). Required when transferring GPU memory even with TCP protocol |
| AMD | `USE_HIP` | OFF | GPU support via HIP/ROCm |
| Moore Threads | `USE_MUSA` | OFF | GPU support via MUSA |
| MetaX (Muxi) | `USE_MACA` | OFF | GPU support via MACA |
| | `MACA_ROOT` | `/opt/maca` | MACA SDK root path |
| | `MACA_INCLUDE_DIR` | — | MACA include directory |
| | `MACA_LIB_DIR` | — | MACA library directory |
| | `MACA_RUNTIME_LIBS` | `mcruntime;mxc-runtime64;rt` | MACA runtime libraries linked by `transfer_engine` |
| Hygon DCU | `USE_HYGON` | OFF | GPU support via DTK SDK (CUDA-compatible runtime) |
| | `DTK_ROOT` | `/opt/dtk` | DTK SDK root path |
| | `DTK_INCLUDE_DIR` | — | DTK include directory |
| | `DTK_LIB_DIR` | — | DTK library directory |
| Iluvatar CoreX | `USE_COREX` | OFF | GPU support via CoreX (CUDA-compatible runtime) |
| | `COREX_ROOT` | `/usr/local/corex` | CoreX SDK root path |
| | `COREX_INCLUDE_DIR` | — | CoreX include directory |
| | `COREX_LIB_DIR` | — | CoreX library directory |
| Cambricon MLU | `USE_MLU` | OFF | MLU memory support (memory detection, topology discovery, RDMA registration) |
| | `NEUWARE_ROOT` | `/usr/local/neuware` | Neuware SDK root path |
| | `MLU_INCLUDE_DIR` | — | Neuware include directory |
| | `MLU_LIB_DIR` | — | Neuware library directory |

### Transport Protocols

| Option | Default | Description |
|--------|---------|-------------|
| `USE_MNNVL` | OFF | Multi-Node NVLink transport. Requires `USE_CUDA=ON` unless using MUSA/HIP/MACA |
| `USE_INTRA_NVLINK` | OFF | Intra-Node NVLink transport |
| `USE_EFA` | OFF | AWS Elastic Fabric Adapter transport via libfabric. See [EFA Transport](../design/transfer-engine/efa_transport.md) |
| `USE_CXL` | OFF | CXL transport |

### Metadata Services

| Option | Default | Description |
|--------|---------|-------------|
| `USE_HTTP` | OFF | HTTP-based metadata service for Transfer Engine |
| `USE_ETCD` | OFF | etcd-based metadata service for Transfer Engine, requires go 1.23+ |
| `USE_REDIS` | OFF | Redis-based metadata service for Transfer Engine, requires hiredis |
| `STORE_USE_ETCD` | OFF | etcd-based failover for Mooncake Store, requires go 1.23+. Independent from `USE_ETCD` |
| `STORE_USE_REDIS` | OFF | Redis-based failover for Mooncake Store, requires hiredis. Independent from `USE_REDIS` |

