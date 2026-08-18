#Notes:
#1. The make target depends on how eRPC was compiled:
#   * If DTRANSPORT=dpdk, use `make dpdk`
#   * If DTRANSPORT=raw, use `make raw`
#   * If DTRANSPORT=infiniband, use `make infiniband`
LIBS = -lerpc -lpthread -lnuma -ldl
USE_NO_CC_QUEUE ?= 1
USE_ONE_SIDE_READ ?= 1

CXL_DEFS = -DERPC_CXL=true
ifeq ($(USE_NO_CC_QUEUE),1)
CXL_DEFS += -DUSE_NO_CC_QUEUE
ifeq ($(USE_ONE_SIDE_READ),1)
CXL_DEFS += -DUSE_ONE_SIDE_READ
endif
endif

error:
	@echo "Please choose one of the following targets: infiniband, raw, dpdk, clean"
	@exit 2
infiniband:
	g++ -g -std=c++11 -o server server.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_INFINIBAND=true
	g++ -g -std=c++11 -o client client.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_INFINIBAND=true
	g++ -g -std=c++11 -o pair_server pair_server.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_INFINIBAND=true
	g++ -g -std=c++11 -o pair_client pair_client.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_INFINIBAND=true
raw:
	g++ -g -std=c++11 -o server server.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_RAW=true
	g++ -g -std=c++11 -o client client.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_RAW=true
	g++ -g -std=c++11 -o pair_server pair_server.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_RAW=true
	g++ -g -std=c++11 -o pair_client pair_client.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_RAW=true
dpdk:
	g++ -g -std=c++11 -o server server.cc \
                -Wno-deprecated-declarations \
                -DERPC_DPDK=true -march=native \
                -I ../src -I ../third_party/asio/include -I /usr/include/dpdk \
                -L ../build $(LIBS) \
                -Wl,--whole-archive $(shell pkg-config --static --libs-only-l libdpdk) -Wl,--no-whole-archive \
                -lnuma -ldl

	g++ -g -std=c++11 -o client client.cc \
                -Wno-deprecated-declarations \
                -DERPC_DPDK=true -march=native \
                -I ../src -I ../third_party/asio/include -I /usr/include/dpdk \
                -L ../build $(LIBS) \
                -Wl,--whole-archive $(shell pkg-config --static --libs-only-l libdpdk) -Wl,--no-whole-archive \
                -lnuma -ldl

	g++ -g -std=c++11 -o pair_server pair_server.cc \
                -Wno-deprecated-declarations \
                -DERPC_DPDK=true -march=native \
                -I ../src -I ../third_party/asio/include -I /usr/include/dpdk \
                -L ../build $(LIBS) \
                -Wl,--whole-archive $(shell pkg-config --static --libs-only-l libdpdk) -Wl,--no-whole-archive \
                -lnuma -ldl

	g++ -g -std=c++11 -o pair_client pair_client.cc \
                -Wno-deprecated-declarations \
                -DERPC_DPDK=true -march=native \
                -I ../src -I ../third_party/asio/include -I /usr/include/dpdk \
                -L ../build $(LIBS) \
                -Wl,--whole-archive $(shell pkg-config --static --libs-only-l libdpdk) -Wl,--no-whole-archive \
                -lnuma -ldl

cxl:
	g++ -g -std=c++17 -o server server.cc \
			$(CXL_DEFS) -march=native \
			-I ../src -I ../third_party/asio/include -I ../../shm-lib/include \
			-L ../build -L ../../shm-lib/build \
			-lerpc -lshm-lib -lpthread -lnuma -ldl -lrt -lmemkind

	g++ -g -std=c++17 -o client client.cc \
			$(CXL_DEFS) -march=native \
                -I ../src -I ../third_party/asio/include -I ../../shm-lib/include \
                -L ../build -L ../../shm-lib/build \
                -lerpc -lshm-lib -lpthread -lnuma -ldl -lrt -lmemkind

	g++ -g -std=c++17 -o pair_server pair_server.cc \
			$(CXL_DEFS) -march=native \
			-I ../src -I ../third_party/asio/include -I ../../shm-lib/include \
			-L ../build -L ../../shm-lib/build \
			-lerpc -lshm-lib -lpthread -lnuma -ldl -lrt -lmemkind

	g++ -g -std=c++17 -o pair_client pair_client.cc \
			$(CXL_DEFS) -march=native \
                -I ../src -I ../third_party/asio/include -I ../../shm-lib/include \
                -L ../build -L ../../shm-lib/build \
                -lerpc -lshm-lib -lpthread -lnuma -ldl -lrt -lmemkind

	g++ -g -std=c++17 -o multi_client_server multi_client_server.cc \
			$(CXL_DEFS) -march=native \
			-I ../src -I ../third_party/asio/include -I ../../shm-lib/include \
			-L ../build -L ../../shm-lib/build \
			-lerpc -lshm-lib -lpthread -lnuma -ldl -lrt -lmemkind

	g++ -g -std=c++17 -o multi_client_client multi_client_client.cc \
			$(CXL_DEFS) -march=native \
                -I ../src -I ../third_party/asio/include -I ../../shm-lib/include \
                -L ../build -L ../../shm-lib/build \
                -lerpc -lshm-lib -lpthread -lnuma -ldl -lrt -lmemkind

clean:
	rm -f server client pair_server pair_client multi_client_server multi_client_client umwait_wakeup_bench
