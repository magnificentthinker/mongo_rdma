#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

// 首先包含完整的类型定义
#include "mongo/transport/rdma/rdma_socket.h"
#include "mongo/db/dbmessage.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/rdma/rdma_core_connection.h"
#include "mongo/transport/rdma/rdma_negotiation_manager.h"
#include "mongo/transport/rdma/rdma_socket_readwrite.h"

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

Status RDMASocket::connect(const HostAndPort& peer, Mulliseconds timeout) {
    auto effectiveTimeout = timeout;
    if (timeout < rdmaGlobalParams.getConnectionTimeout()) {
        effectiveTimeout = rdmaGlobalParams.getConnectionTimeout();
        LOGV2_DEBUG(4700817,
                    1,
                    "Using RDMA connection timeout",
                    "originalTimeout"_attr = timeout.count(),
                    "effectiveTimeout"_attr = effectiveTimeout.count());
    }

    try {
        // 1. 建立 TCP 连接，使用有效的timeout参数
        auto ioContext = std::make_shared<asio::io_context>();
        auto tcpSocket = std::make_unique<asio::ip::tcp::socket>(*ioContext);

        asio::ip::tcp::resolver resolver(*ioContext);
        auto endpoints = resolver.resolve(peer.host(), std::to_string(peer.port()));

        // 设置连接超时
        asio::steady_timer timeoutTimer(*ioContext);
        timeoutTimer.expires_after(std::chrono::milliseconds(effectiveTimeout.count()));

        bool connected = false;
        std::error_code connectError;

        // 异步连接
        asio::async_connect(
            *tcpSocket,
            endpoints,
            [&connected, &connectError](const std::error_code& ec, const asio::ip::tcp::endpoint&) {
                if (!ec) {
                    connected = true;
                } else {
                    connectError = ec;
                }
            });

        // 等待连接完成或超时
        auto startTime = std::chrono::steady_clock::now();
        auto timeoutDuration = std::chrono::milliseconds(effectiveTimeout.count());

        while (!connected && !connectError) {
            auto now = std::chrono::steady_clock::now();
            if (now - startTime >= timeoutDuration) {
                connectError = asio::error::timed_out;
                break;
            }
            ioContext->run_one();
        }

        if (connectError) {
            return Status(ErrorCodes::NetworkTimeout,
                          str::stream()
                              << "Failed to do rdma connect to " << peer.toString() << " within "
                              << effectiveTimeout.count() << "ms: " << connectError.message());
        }

        LOGV2_DEBUG(4700814,
                    1,
                    "TCP connection established, starting RDMA negotiation",
                    "peer"_attr = peer.toString());

        // TCP connection ok to do rdma connection infos exchange

        
    } catch (const std::exception& e) {
        return Status(ErrorCodes::RDMAConnectError,
                      str::stream() << "Exception during connection: " << e.what());
    }
}
}  // namespace rdma
}  // namespace transport
}  // namespace mongo