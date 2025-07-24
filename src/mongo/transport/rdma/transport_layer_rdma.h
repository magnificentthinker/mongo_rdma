#pragma once

#include <functional>
#include <memory>
#include <string>

#include "mongo/base/status_with.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_mode.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_types.h"

namespace mongo {

class ServiceContext;
class ServiceEntryPoint;

namespace transport {
namespace rdma {
class TransportLayerRDMA final : public TransportLayer {
    TransportLayerRDMA(const TransportLayerRDMA&) = delete;
    TransportLayerRDMA& operator=(const TransportLayerRDMA&) = delete;

private:
#define RDMA_MAX_CONN 500;

public:
    struct Options {
        constexpr static auto kIngress = 0x1;
        constexpr static auto kEgress = 0x10;

        explicit Options(const ServerGlobalParams* params);
        Options() = default;

        int mode = kIngress | kEgress;

        bool isIngress() const {
            return mode & kIngress;
        }

        bool isEgress() const {
            return mode & kEgress;
        }

        std::string rdmaDevice;
        int port = ServerGlobalParams::DefaultDBPort;  // port to bind to
        std::vector<std::string> ipList;               // addresses to bind to

        Mode transportMode = Mode::kSynchronous;  // whether accepted sockets should be put into

        size_t maxConns = RDMA_MAX_CONN;  // maximum number of active connections

        bool enableRDMA = false;
    }

    TransportLayerRDMA(const Options& opts, ServiceEntryPoint* sep);

    ~TransportLayerRDMA() override;

    StatusWith<SessionHandle> connect(HostAndPort peer,
                                      ConnectSSLMode sslMode,
                                      Milliseconds timeout) final;

    Future<SessionHandle> asyncConnect(HostAndPort peer,
                                       ConnectSSLMode sslMode,
                                       const ReactorHandle& reactor,
                                       Milliseconds timeout) final;

    Status setup() final;

    ReactorHandle getReactor(WhichReactor which) final;

    Status start() final;

    void shutdown() final;

    int listenerPort() const {
        return _listenerPort;
    }

private:
    Status _setupManager();
    Status _listen();
    Status _setupRDMAListener();
    void _accecptConnection();

private:
    class RDMASession;
    class RDMAReactor;

    using RDMASessionHandle = std::shared_ptr<RDMASession>;
    using ConstRDMASessionHandle = std::shared_ptr<const RDMASession>;

    Mutex _mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "TransportLayerRDMA::_mutex");

    std::shared_ptr<RDMAReactor> _ingressReactor;
    std::shared_ptr<RDMAReactor> _egressReactor;
    std::shared_ptr<RDMAReactor> _acceptorReactor;

    Options _listenerOptions;
    ServiceEntryPoint* const _sep = nullptr;


    AtomicWord<bool> _rdmaAvailable{false};
    AtomicWord<bool> _running{false};
    AtomicWord<bool> _isShutdown{false};
    bool _enableRDMA = false;

    RDMAListener* _rdmaListener = nullptr;
};
}  // namespace rdma
}  // namespace transport

}  // namespace mongo