#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

// 首先包含完整的类型定义
#include "mongo/db/dbmessage.h"
#include "mongo/rpc/op_msg.h"

#include "mongo/transport/rdma/rdma_manager.h"
#include "mongo/transport/rdma/rdma_socket.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>

#include "mongo/logv2/log.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/rdma/rdma_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/str.h"

#include "asio/buffer.hpp"
#include "asio/read.hpp"
#include "asio/write.hpp"
#include <array>

namespace mongo {
namespace transport {
namespace rdma {

RDMAManager& RDMAManager::get() {
    static RDMAManager instance;
    return instance;
}

}  // namespace rdma
}  // namespace transport
}  // namespace mongo