#Notes:
#1. The make target depends on how eRPC was compiled:
#   * If DTRANSPORT=dpdk, use `make dpdk`
#   * If DTRANSPORT=raw, use `make raw`
#   * If DTRANSPORT=infiniband, use `make infiniband`
LIBS = -lerpc -lpthread -lnuma -ldl
BREAKDOWN ?= 0
BREAKDOWN_FLAGS =
ifeq ($(BREAKDOWN),1)
BREAKDOWN_FLAGS = -DERPC_RDMA_BREAKDOWN
endif

error:
	@echo "Please choose one of the following targets: infiniband, raw, dpdk, clean"
	@exit 2
infiniband:
	g++ -g -std=c++11 -o server server.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_INFINIBAND=true -DERPC_RDMA_BREAKDOWN=true $(BREAKDOWN_FLAGS)
	g++ -g -std=c++11 -o client client.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_INFINIBAND=true -DERPC_RDMA_BREAKDOWN=true $(BREAKDOWN_FLAGS)
	g++ -g -std=c++11 -o multi_client_server multi_client_server.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_INFINIBAND=true -DERPC_RDMA_BREAKDOWN=true $(BREAKDOWN_FLAGS)
	g++ -g -std=c++11 -o multi_client_client multi_client_client.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_INFINIBAND=true -DERPC_RDMA_BREAKDOWN=true $(BREAKDOWN_FLAGS)
	g++ -g -std=c++11 -o pair_server pair_server.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_INFINIBAND=true -DERPC_RDMA_BREAKDOWN=true $(BREAKDOWN_FLAGS)
	g++ -g -std=c++11 -o pair_client pair_client.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_INFINIBAND=true -DERPC_RDMA_BREAKDOWN=true $(BREAKDOWN_FLAGS)
raw:
	g++ -g -std=c++11 -o server server.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_RAW=true $(BREAKDOWN_FLAGS)
	g++ -g -std=c++11 -o client client.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_RAW=true $(BREAKDOWN_FLAGS)
	g++ -g -std=c++11 -o multi_client_server multi_client_server.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_RAW=true $(BREAKDOWN_FLAGS)
	g++ -g -std=c++11 -o multi_client_client multi_client_client.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_RAW=true $(BREAKDOWN_FLAGS)
	g++ -g -std=c++11 -o pair_server pair_server.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_RAW=true $(BREAKDOWN_FLAGS)
	g++ -g -std=c++11 -o pair_client pair_client.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_RAW=true $(BREAKDOWN_FLAGS)
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

	g++ -g -std=c++11 -o multi_client_server multi_client_server.cc \
                -Wno-deprecated-declarations \
                -DERPC_DPDK=true -march=native \
                -I ../src -I ../third_party/asio/include -I /usr/include/dpdk \
                -L ../build $(LIBS) \
                -Wl,--whole-archive $(shell pkg-config --static --libs-only-l libdpdk) -Wl,--no-whole-archive \
                -lnuma -ldl

	g++ -g -std=c++11 -o multi_client_client multi_client_client.cc \
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
clean:
	rm -f server client multi_client_server multi_client_client pair_server pair_client
