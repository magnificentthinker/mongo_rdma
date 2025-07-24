#pragma once

#include <atomic>
#include <memory>
#include <queue>
#include <string>
#include <vector>

#include "mongo/transport/asio_utils.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/message.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/rdma/rdma_manager.h"
#include "mongo/transport/rdma/rdma_session.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace transport {
namespace rdma {
class RDMAManager {
    public:
    RDMAManager();
    ~RDMAManager();

    RDMAManager& get();


};
}  // namespace rdma
}  // namespace transport

}  // namespace mongo