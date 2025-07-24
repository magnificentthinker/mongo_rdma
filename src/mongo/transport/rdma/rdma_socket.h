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

class RDMASocket {

public:
    enum class ConnectionState { NOT_CONNECTED, CONNECTING, CONNECTED, ERROR, CLOSING, CLOSED };

    explicit RDMASocket();
    ~RDMASocket();

    //建立连接
    Status connect(const HostAndPort& peer,Mulliseconds timeout);
    Future<void> asyncConnect(const HostAndPort& peer, const ReactorHandle& reactor,Mulliseconds timeout);
    Status listen(const HostAndPort& local);
    Status accept();
    void close();

    //数据接发
    StatusWith<Message> sourceMessage();
    Future<Message> asyncSourceMessage(const BatonHandle& baton = nullptr);
    Status sinkMessage(Message message);
    Future<void> asyncSinkMessage(Message message, const BatonHandle& baton = nullptr);

    //状态查询
    bool isConnected() const;
    const HostAndPort& remote() const;
    const HostAndPort& local() const;
};

}  // namespace rdma
}  // namespace transport
}  // namespace mongo