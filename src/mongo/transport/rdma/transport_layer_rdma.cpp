#include <arpa/inet.h>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <tuple>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/transport/rdma/transport_layer_rdma.h"

#include "mongo/db/server_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/rdma/rdma_session.h"
#include "mongo/transport/rdma/rdma_socket.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/executor_stats.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace transport {
namespace rdma {



TransportLayerRDMA::Options::Options(const ServerGlobalParams* params)
    : rdmaDevice(params->rdmaDevice),
      port(params->port),
      ipList(params->bind_ips),
      maxConns(params->maxConnsRDMA),
      enableRDMA(params->enableRDMA) {}


TransportLayerRDMA(const Options& opts, ServiceEntryPoint* sep)
    : _ingressReactor(std::make_shared<RDMAReactor>()),
      _egressReactor(std::make_shared<RDMAReactor>()),
      _acceptorReactor(std::make_shared<RMDAReactor>()),
      _listenerOptions(opts),
      _sep(sep),
      _enableRDMA(opts.enableRDMA) {

    // 是否在设置中启用rdma
    if (!_enableRDMA) {
        LOGV2(4800000, "RDMA transport layer is disabled by configuration");
        return;
    }

    // 检验本地是否有RDMA设备
    if (!RDMAManager::get().isRDMAAvailable()) {
        LOGV2(4800001,
              "RDMA is not available on this system, will fall back to TCP if graceful degradation "
              "is enabled");
        return;
    }

    LOGV2(4800002,
          "Creating RDMA transport layer with calculated listen address",
          "device"_attr = _options.rdmaDevice,
          "rdmaPort"_attr = _options.rdmaPort,
          "listenAddr"_attr = _listenAddr.toString());
}

TransportLayerRDMA::~TransportLayerRDMA() {
    if (_running.load())
        shutdown();
}

StatusWith<SessionHandle> TransportLayerRDMA::connect(HostAndPort peer,
                                                      ConnectSSLMode sslMode,
                                                      Milliseconds timeout) {

    if (_isShutdown.load()) {
        return Status(ErrorCodes::ShutdownInProgress, "Transport layer rdma is shutting down");
    }

    //to do init in rdmaGlobalParams
    if (!_enableRDMA) {
        LOGV2_WARNING(, "RDMA not available");
        return Status(ErrorCodes::RDMADeviceNotFound, "RDMA not available");
    }

    // to get RDMASocket
    auto sock = RDMASocket();
    auto status = sock.connect();
    if(!status.isOK()){
        return status;
    }

    //to do RDMASession init
    return std::make_shard<RDMASession>(this, std::move(sock)...);
}

Future<SessionHandle> TransportLayerRDMA::asyncConnect(HostAndPort peer,
                                                       ConnectSSLMode sslMode,
                                                       Milliseconds timeout) {
    // to do
}

Status TransportLayerRDMA::setup() {
    //to do init in rdmaGlobalParams
    if (!_enableRDMA) {
        return Status(ErrorCodes::InvalidOptions,
                      "RDMA transport layer is disabled by configuration");
    }

    auto status = _setupRDMAManager();
    if (!status.isOK()) {
        LOGV2_WARNING(4800003,
                      "RDMA initialization failed, falling back to TCP transport",
                      "error"_attr = status);
    }
    return status;
}

Status TransportLayerRDMA::_setupManager() {
    try {
        auto rdmaMgr = RDMAManager::get();

        // 初始化rdma设备
        auto status = rdmaMgr.initialize(_listenerOptions.rdmaDevice, _listenerOptions.port);
        if (!status.isOK()) {
            return status;
        }
        _rdmaAvailable.store(true);
        return Status::OK();
    } catch (const std::exception& e) {
        return Status(ErrorCodes::RDMAInitError,
    }
}

Status TransportLayerRDMA::start() {
    if (_running.load()) {
        return Status::OK();
    }

    if (!_rdmaAvailable.load()) {
        return Status(ErrorCodes::RDMADeviceNotFound,
                      "RDMA transport layer cannot start because RDMA is not available");
    }

    if (_listenerOptions.isIngress()) {
        auto status = _listen();
        if (!status.isOK()) {
            return status;
        }
    }

    _running.store(true);
    return Status::OK();
}

Status TransportLayerRDMA::_listen() {
    LOGV2(4800005, "Starting RDMA listener");

    try {
        // 设置监听
        auto status = _setupRDMAListener();
        if (!status.isOK()) {
            LOGV2_WARNING(4800010, "Failed to setup RDMA listener", "error"_attr = status);
            return status;
        }

        _accecptConnection();
        return Status::OK();
    } catch (const std::exception& e) {
        return Status(ErrorCodes::RDMAInitError,
                      str::stream() << "Failed to set up RDMA listener: " << e.what());
    }
}

Status TransportLayerRDMA::_setupRDMAListener() {
    LOGV2_DEBUG(4800011, 1, "Setting up RDMA listener");

    // to do
}

void TransportLayerRDMA::_accecptConnection() {
    LOGV2_DEBUG(4800012, 1, "begin to accecpt RDMA connect");
    // to do
}

void TransportLayerRDMA::shutdown() {
    // to do
}

ReactorHandle TransportLayerRDMA::getReactor(WhichReactor which) {
    switch (which) {
        case TransportLayer::kIngress:
            return _ingressReactor;
        case TransportLayer::kEgress:
            return _egressReactor;
        case TransportLayer::kNewReactor:
            return std::make_shared<ASIOReactor>();
    }

    MONGO_UNREACHABLE;
}

}  // namespace rdma
}  // namespace transport
}  // namespace mongo