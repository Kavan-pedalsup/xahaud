#!/bin/bash -u
# We use set -e and bash with -u to bail on first non zero exit code of any
# processes launched or upon any unbound variable.
# We use set -x to print commands before running them to help with
# debugging.
set -ex

echo "START INSIDE CONTAINER - FULL"

# Set default BUILD_CORES if not defined
: ${BUILD_CORES:=8}

echo "-- BUILD CORES: $BUILD_CORES"

umask 0000;

echo "Fixing CentOS 7 EOL"

sed -i 's/mirrorlist/#mirrorlist/g' /etc/yum.repos.d/CentOS-*
sed -i 's|#baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|g' /etc/yum.repos.d/CentOS-*
yum clean all
yum-config-manager --disable centos-sclo-sclo

####

mkdir -p /io
cd /io

mkdir -p .nih_c
mkdir -p .nih_toolchain
cd .nih_toolchain &&
yum install -y wget lz4 lz4-devel git llvm13-static.x86_64 llvm13-devel.x86_64 devtoolset-10-binutils zlib-static ncurses-static -y \
  devtoolset-7-gcc-c++ \
  devtoolset-9-gcc-c++ \
  devtoolset-10-gcc-c++ \
  snappy snappy-devel \
  zlib zlib-devel \
  lz4-devel \
  libasan &&
export PATH=`echo $PATH | sed -E "s/devtoolset-9/devtoolset-7/g"` &&
echo "-- Install ZStd 1.1.3 --" &&
yum install epel-release -y &&
ZSTD_VERSION="1.1.3" &&
( wget -nc -q -O zstd-${ZSTD_VERSION}.tar.gz https://github.com/facebook/zstd/archive/v${ZSTD_VERSION}.tar.gz; echo "" ) &&
tar xzvf zstd-${ZSTD_VERSION}.tar.gz &&
cd zstd-${ZSTD_VERSION} &&
make -j$BUILD_CORES install &&
cd .. &&
echo "-- Install Cmake 3.23.1 --" &&
pwd &&
( wget -nc -q https://github.com/Kitware/CMake/releases/download/v3.23.1/cmake-3.23.1-linux-x86_64.tar.gz; echo "" ) &&
tar -xzf cmake-3.23.1-linux-x86_64.tar.gz -C /hbb/ &&
echo "-- Install Boost 1.86.0 --" &&
pwd &&
( wget -nc -q https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.gz; echo "" ) &&
tar -xzf boost_1_86_0.tar.gz &&
cd boost_1_86_0 && ./bootstrap.sh && ./b2 link=static -j$BUILD_CORES && ./b2 install &&
cd ../ &&
# Copy Boost to the expected location
mkdir -p /usr/local/src/ &&
cp -r boost_1_86_0 /usr/local/src/ &&
echo "-- Install Protobuf 3.20.0 --" &&
pwd &&
( wget -nc -q https://github.com/protocolbuffers/protobuf/releases/download/v3.20.0/protobuf-all-3.20.0.tar.gz; echo "" ) &&
tar -xzf protobuf-all-3.20.0.tar.gz &&
cd protobuf-3.20.0/ &&
./autogen.sh && ./configure --prefix=/usr --disable-shared link=static && make -j$BUILD_CORES && make install &&
cd .. &&
echo "-- Build LLD --" &&
pwd &&
ln -sf /usr/bin/llvm-config-13 /usr/bin/llvm-config &&
mv /opt/rh/devtoolset-9/root/usr/bin/ar /opt/rh/devtoolset-9/root/usr/bin/ar-9 &&
ln -sf /opt/rh/devtoolset-10/root/usr/bin/ar /opt/rh/devtoolset-9/root/usr/bin/ar &&
( wget -nc -q https://github.com/llvm/llvm-project/releases/download/llvmorg-13.0.1/lld-13.0.1.src.tar.xz; echo "" ) &&
( wget -nc -q https://github.com/llvm/llvm-project/releases/download/llvmorg-13.0.1/libunwind-13.0.1.src.tar.xz; echo "" ) &&
tar -xf lld-13.0.1.src.tar.xz &&
tar -xf libunwind-13.0.1.src.tar.xz &&
cp -r libunwind-13.0.1.src/include libunwind-13.0.1.src/src lld-13.0.1.src/ &&
cd lld-13.0.1.src &&
rm -rf build CMakeCache.txt &&
mkdir -p build &&
cd build &&
cmake .. -DLLVM_LIBRARY_DIR=/usr/lib64/llvm13/lib/ -DCMAKE_INSTALL_PREFIX=/usr/lib64/llvm13/ -DCMAKE_BUILD_TYPE=Release &&
make -j$BUILD_CORES install &&
ln -s /usr/lib64/llvm13/lib/include/lld /usr/include/lld &&
cp /usr/lib64/llvm13/lib/liblld*.a /usr/local/lib/ &&
cd ../../ &&
echo "-- Build WasmEdge --" &&
( wget -nc -q https://github.com/WasmEdge/WasmEdge/archive/refs/tags/0.11.2.zip; echo ""; unzip -o 0.11.2.zip; ) &&
cd WasmEdge-0.11.2 &&
( mkdir -p build; echo "" ) &&
cd build &&
export BOOST_ROOT="/usr/local/src/boost_1_86_0" &&
export Boost_LIBRARY_DIRS="/usr/local/lib" &&
export BOOST_INCLUDEDIR="/usr/local/src/boost_1_86_0" &&
export PATH=`echo $PATH | sed -E "s/devtoolset-7/devtoolset-9/g"` &&
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DWASMEDGE_BUILD_SHARED_LIB=OFF \
    -DWASMEDGE_BUILD_STATIC_LIB=ON \
    -DWASMEDGE_BUILD_AOT_RUNTIME=ON \
    -DWASMEDGE_FORCE_DISABLE_LTO=ON \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DWASMEDGE_LINK_LLVM_STATIC=ON \
    -DWASMEDGE_BUILD_PLUGINS=OFF \
    -DWASMEDGE_LINK_TOOLS_STATIC=ON \
    -DBoost_NO_BOOST_CMAKE=ON -DLLVM_DIR=/usr/lib64/llvm13/lib/cmake/llvm/ -DLLVM_LIBRARY_DIR=/usr/lib64/llvm13/lib/ &&
make -j$BUILD_CORES install &&
export PATH=`echo $PATH | sed -E "s/devtoolset-9/devtoolset-10/g"` &&
cp -r include/api/wasmedge /usr/include/ &&
cd /io/ &&
echo "-- Build Rippled --" &&
echo "MOVING TO [ build-core.sh ]"

cd /io

# Save current environment to .env file
printenv > .env.temp
cat .env.temp | grep '=' | sed s/\\\(^[^=]\\+=\\\)/\\1\\\"/g|sed s/\$/\\\"/g > .env
rm .env.temp

echo "Persisting ENV:"
cat .env

# Create a deps summary
mkdir -p /usr/local
DEPS_SUMMARY="/usr/local/deps-summary.txt"
echo "Dependencies built at: $(date)" > $DEPS_SUMMARY
echo "Boost dir: $(ls -la /usr/local/src/boost_1_86_0 2>/dev/null || echo 'NOT FOUND')" >> $DEPS_SUMMARY
echo "ZStd version: $(zstd --version | head -n 1 || echo 'NOT FOUND')" >> $DEPS_SUMMARY
echo "Protobuf version: $(protoc --version || echo 'NOT FOUND')" >> $DEPS_SUMMARY 
echo "CMAKE version: $(/hbb/cmake-3.23.1-linux-x86_64/bin/cmake --version | head -n 1 || echo 'NOT FOUND')" >> $DEPS_SUMMARY

echo "-------- DEPENDENCY SUMMARY --------"
cat $DEPS_SUMMARY
echo "------------------------------------"

echo "END INSIDE CONTAINER - FULL"

echo "-- Built with env vars:"