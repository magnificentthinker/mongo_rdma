#pragma once

#include <utility>

#include "mongo/base/system_error.h"
#include "mongo/config.h"
#include "mongo/db/stats/counters.h"
#include "mongo/transport/asio_utils.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/rdma/rdma_socket.h"
#include "mongo/transport/transport_layer_rdma.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/socket_utils.h"
#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_types.h"
#endif

#include "asio.hpp"
#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl.hpp"
#endif

namespace mongo {
namespace transport {
class TransportLayerRDMA::RDMASession final : public Session {
    RDMASession(const RDMASession&) = delete;
    RDMASession& operator=(const RDMASession&) = delete;

public:
    RDMASession(TransportLayerRDMA* tl, RDMASocketHandle socket, bool isIngressSession)
        : _tl(tl), _socket(std::move(socket)) {
        _remote = _socket->remoteAddr();
        _local = _socket->localAddr();
        _remoteSockAddr = _socket->remoteSockAddr();
        _localSockAddr = _socket->localSockAddr();
        _isIngressSession = isIngressSession;

        LOGV2_DEBUG(4800020,
                    2,
                    "Created RDMA session",
                    "local"_attr = _local.toString(),
                    "remote"_attr = _remote.toString());
    }

    ~RDMASession() {
        end();
    }

    TransportLayer* getTransportLayer() const override {
        return _tl;
    }

    const HostAndPort& remote() const override {
        return _remote;
    }

    const HostAndPort& local() const override {
        return _local;
    }

    const SockAddr& remoteAddr() const override {
        return _remoteAddr;
    }

    const SockAddr& localAddr() const override {
        return _localAddr;
    }

    void end() override {
        if (_isConnected.swap(false)) {
            LOGV2_DEBUG(
                4800021,
                2,
                "Ending RDMA session with stack trace",
                "local"_attr = _local.toString(),
                "remote"_attr = _remote.toString(),
                "callStack"_attr = [&]() {
                    // 简单的调用栈跟踪
                    std::string trace = "Session end called";
                    return trace;
                }());
            if (_socket) {
                _socket->close();
            }
        }
    }

    StatusWith<Message> sourceMessage() override {
        try {
            if (!isConnected()) {
                LOGV2_DEBUG(4800022, 2, "Session not connected, returning closed status");
                return Session::ClosedStatus;
            }

            auto msg = _socket->asyncSourceMessage().get();
            return StatusWith<Message>(std::move(msg));
        } catch (const DBException& e) {
            LOGV2_WARNING(4800023, "Exception in sourceMessage", "error"_attr = e.toStatus());
            end();
            return e.toStatus();
        } catch (const std::exception& e) {
            LOGV2_WARNING(4800024, "Exception in sourceMessage", "error"_attr = e.what());
            end();
            return Status(ErrorCodes::SocketException,
                          str::stream() << "Error sourcing message: " << e.what());
        }
    }

    Future<Message> RDMASession::asyncSourceMessage(const BatonHandle& baton) noexcept {
        try {
            if (!isConnected()) {
                return Session::ClosedStatus;
            }

            return _socket->asyncSourceMessage(baton).tapError(
                [this](const Status& status) { end(); });
        } catch (const DBException& e) {
            end();
            return e.toStatus();
        } catch (const std::exception& e) {
            end();
            return Status(ErrorCodes::SocketException,
                          str::stream() << "Error in async source message: " << e.what());
        }
    }

    Status RDMASession::sinkMessage(Message message) noexcept {
        try {
            if (!isConnected()) {
                return Session::ClosedStatus;
            }

            auto status = _socket->asyncSinkMessage(std::move(message)).getNoThrow();
            if (!status.isOK()) {
                end();
            }
            return status;
        } catch (const DBException& e) {
            end();
            return e.toStatus();
        } catch (const std::exception& e) {
            end();
            return Status(ErrorCodes::SocketException,
                          str::stream() << "Error sinking message: " << e.what());
        }
    }

    Future<void> RDMASession::asyncSinkMessage(Message message, const BatonHandle& baton) noexcept {
        try {
            if (!isConnected()) {
                return Session::ClosedStatus;
            }

            return _socket->asyncSinkMessage(std::move(message), baton)
                .onError([this](Status status) {
                    end();
                    return status;
                });
        } catch (const DBException& e) {
            end();
            return e.toStatus();
        } catch (const std::exception& e) {
            end();
            return Status(ErrorCodes::SocketException,
                          str::stream() << "Error in async sink message: " << e.what());
        }
    }

    void cancelAsyncOperations(const BatonHandle& baton = nullptr) override {
        if (_socket) {
            _socket->cancel();
        }
    }

    void setTimeout(boost::optional<Milliseconds> timeout) override {}

    bool isConnected() override {
        return _isConnected.load() && _socket && _socket->isConnected();
    }

protected:
    friend class TransportLayerRDMA;

private:
    using RDMASocketHandle = std::unique_ptr<mongo::transport::rdma::RDMASocket>;

    TransportLayerRDMA* const _tl;

    RDMASocketHandle _socket;

    HostAndPort _remote;
    HostAndPort _local;

    SockAddr _remoteAddr;
    SockAddr _localAddr;

    bool _isIngressSession;
    AtomicWord<bool> _isConnected{true};
};

}  // namespace transport
}  // namespace mongo
