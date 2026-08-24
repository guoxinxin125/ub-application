#  安装protobuf
export SN_DEPS="/home/g"
mkdir -p $SN_DEPS/.local $SN_DEPS/src
cd $SN_DEPS/src

# 克隆指定稳定版本（以 3.21.12 为例�?
git clone --branch v3.21.12 --depth 1 https://github.com/protocolbuffers/protobuf.git
cd protobuf

mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=$SN_DEPS/.local \
  -Dprotobuf_BUILD_TESTS=OFF \
  -DCMAKE_C_FLAGS="-pthread" \
  -DCMAKE_CXX_FLAGS="-pthread"
make -j$(nproc)
make install

export PATH=$SN_DEPS/.local/bin:$PATH
export LD_LIBRARY_PATH=$SN_DEPS/.local/lib:$LD_LIBRARY_PATH
export PKG_CONFIG_PATH=$SN_DEPS/.local/lib/pkgconfig:$PKG_CONFIG_PATH
export CMAKE_PREFIX_PATH=$SN_DEPS/.local:$CMAKE_PREFIX_PATH
2. Cmake 版本需�?=3.10，如果不满足
# 下载并安装到 $SN_DEPS/.local（修改版本号为你想要的）
mkdir -p $SN_DEPS/src $SN_DEPS/.local
cd $SN_DEPS/src
wget https://github.com/Kitware/CMake/releases/download/v3.26.4/cmake-3.26.4-linux-x86_64.sh
sh cmake-3.26.4-linux-x86_64.sh --skip-license --prefix=$SN_DEPS/.local

# 生效当前会话
export PATH=$SN_DEPS/.local/bin:$PATH
cmake --version
3. gflags下载
# 下载并编�?gflags �?$SN_DEPS/.local（无 sudo�?
git clone https://github.com/gflags/gflags.git
mkdir -p gflags/build && cd gflags/build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$SN_DEPS/.local
make -j$(nproc)
make install

4. mongo-c-driver安装
cd ~
wget https://github.com/mongodb/mongo-c-driver/releases/download/1.17.7/mongo-c-driver-1.17.7.tar.gz
tar xzf mongo-c-driver-1.17.7.tar.gz
cd mongo-c-driver-1.17.7
mkdir -p cmake-build && cd cmake-build
cmake -DENABLE_AUTOMATIC_INIT_AND_CLEANUP=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$SN_DEPS/.local -DENABLE_TESTS=OFF ../
make -j4
make install
5. 搜索You need to be root to use eRPC（rpc.cc文件），注释�?
// #ifndef _WIN32
//   rt_assert(!getuid(), "You need to be root to use eRPC");
// #endif
6. 编译
cd DmRPC
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=$SN_DEPS/.local -Dgflags_DIR=$SN_DEPS/.local/lib/cmake/gflags   -DSOCIAL_NETWORK_TYPE=erpc -DTRANSPORT=infiniband   -DProtobuf_INCLUDE_DIR=$SN_DEPS/.local/include   -DProtobuf_LIBRARIES=$SN_DEPS/.local/lib/libprotobuf.a   -DProtobuf_PROTOC_EXECUTABLE=$SN_DEPS/.local/bin/protoc
make -j4
7. 运行
cd DmRPC
CONFIG=/home/gxx/DmRPC/cn/app/social_network/config/config.json
./bin/unique_id --config_file=${CONFIG} 
58:205145 WARNG: Modded driver unavailable. Performance will be low.
thread 0: ping_req : 0.00, uniqueid_req : 0.00 
thread 0: ping_req : 0.00, uniqueid_req : 0.00 
thread 0: ping_req : 0.00, uniqueid_req : 0.00 
thread 0: ping_req : 0.00, uniqueid_req : 0.00 

在启动数据库的服务器上需要运�?home/gxx/DmRPC/build/rmem_mn --rmem_server_ip=192.168.12.108 --rmem_server_udp_port=31851 --rmem_size=2 --rmem_server_thread=4
8. Mongodb 配置
执行执行 cat /etc/os-release查看ubuntu版本（val03�?6.04），下载mongorestore工具
# 1. 下载 Ubuntu 16.04 对应版本�?MongoDB Database Tools (包含�?mongorestore)
 wget https://fastdl.mongodb.org/tools/db/mongodb-database-tools-rhel93-aarch64-100.17.0.tgz

  tar -xzf mongodb-database-tools-rhel93-aarch64-100.17.0.tgz

  cd mongodb-database-tools-rhel93-aarch64-100.17.0

  ./bin/mongorestore --version

# 2. 验证 mongorestore 是否可以正常调用
mongorestore --version
启动mongodb，并设置远端可访�?
# �?Node1 上运行（绑定到机�?IP，允许远程访问）
mongod --port 20011 --dbpath ~/mongodb_data/user --logpath ~/mongodb_data/user/mongod.log --bind_ip 192.168.12.108 --fork
mongod --port 20012 --dbpath ~/mongodb_data/user_timeline --logpath ~/mongodb_data/user_timeline/mongod.log --bind_ip 192.168.12.108 --fork
mongod --port 20013 --dbpath ~/mongodb_data/social_network --logpath ~/mongodb_data/social_network/mongod.log --bind_ip 192.168.12.108 --fork
mongod --port 20014 --dbpath ~/mongodb_data/post_storage --logpath ~/mongodb_data/post_storage/mongod.log --bind_ip 192.168.12.108 --fork
查看启动情况
netstat -tlnp | grep mongod
但当前的Mongodb是空的，需要导入测试集数据（我从原deathStarBench项目中导出了一�?mongodb_export），所以导�?
mongorestore --host 192.168.12.108 --port 20011 --gzip --archive=mongo_export/user_archive.gz
mongorestore --host 192.168.12.108 --port 20012 --gzip --archive=mongo_export/user_timeline_archive.gz
mongorestore --host 192.168.12.108 --port 20013 --gzip --archive=mongo_export/social_graph_archive.gz
mongorestore --host 192.168.12.108 --port 20014 --gzip --archive=mongo_export/post_archive.gz

# 关闭命令
mongod --dbpath ~/mongodb_data/user --shutdown
mongod --dbpath ~/mongodb_data/user_timeline --shutdown
mongod --dbpath ~/mongodb_data/social_network --shutdown
mongod --dbpath ~/mongodb_data/post_storage --shutdown
运行之后可以验证一�?
mongo --host 192.168.12.108 --port 20011
# 进入交互后执�?
show dbs
use user
db.user.count() # 会打印用户数，不�?
此外，可以在其它机器上测试是否能够连接到启动数据库的机器
nc -vz 192.168.12.108 20011
Connection to 192.168.12.108 20011 port [tcp/isdnlog] succeeded!
查看当前机器ip
 echo $SSH_CONNECTION