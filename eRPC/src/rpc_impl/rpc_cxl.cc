#ifdef ERPC_CXL
#include "rpc.h"
#include "transport_impl/cxl/cxl_transport.h"

namespace erpc {

// 显式实例化CXL传输的RPC模板
template class Rpc<CXLTransport>;

}  // namespace erpc

#endif