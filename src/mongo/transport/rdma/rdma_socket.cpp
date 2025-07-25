/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published By MongoDB.Inc
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

// 首先包含完整的类型定义
#include "mongo/transport/rdma/rdma_socket_readwrite.h"
#include "mongo/transport/rdma/rdma_negotiation_manager.h"
#include "mongo/transport/rdma/rdma_core_connection.h"
#include "mongo/transport/rdma/rdma_socket.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/db/dbmessage.h"

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

namespace {
// Default buffer size for RDMA operations
constexpr size_t kDefaultBufferSize = 1024 * 1024;  // 1MB

// Number of buffers per socket for send/receive
constexpr size_t kNumBuffers = 16;

// Size of RDMA magic bytes for connection negotiation
constexpr size_t kMagicSize = 8;

// Magic bytes for RDMA connection negotiation
const char kRDMAMagic[kMagicSize] = {'M', 'D', 'B', 'R', 'D', 'M', 'A', '1'};

// Default socket options for TCP fallback
void setDefaultSocketOptions(asio::generic::stream_protocol::socket& socket) {
    asio::error_code ec;

    socket.set_option(asio::socket_base::keep_alive(true), ec);
    if (ec) {
        LOGV2_WARNING(
            4700175, "Failed to set keep_alive socket option", "error"_attr = ec.message());
    }

    socket.set_option(asio::ip::tcp::no_delay(true), ec);
    if (ec) {
        LOGV2_WARNING(
            4700176, "Failed to set tcp::no_delay socket option", "error"_attr = ec.message());
    }
}
}  // namespace

// RDMACompletionContext implementation
RDMACompletionContext::RDMACompletionContext(asio::io_context& ioContext)
    : _ioContext(ioContext), _canceled(false) {}

RDMACompletionContext::~RDMACompletionContext() {}

void RDMACompletionContext::complete(size_t bytesTransferred) {
    if (!_canceled.load() && _handler) {
        asio::post(_ioContext,
                   [this, bytesTransferred]() { _handler(asio::error_code(), bytesTransferred); });
    }
}

void RDMACompletionContext::completeWithError(const std::error_code& ec) {
    if (!_canceled.load() && _handler) {
        asio::post(_ioContext, [this, ec]() { _handler(ec, 0); });
    }
}

void RDMACompletionContext::cancel() {
    _canceled.store(true);
}

// RDMASocket implementation

RDMASocket::RDMASocket(asio::io_context& ioContext)
    : _ioContext(ioContext),
      _tcpSocket(std::make_unique<asio::generic::stream_protocol::socket>(ioContext)),
      _qp(nullptr),
      _sendCQ(nullptr),
      _recvCQ(nullptr),
      _compChannel(nullptr),
      _state(ConnectionState::NOT_CONNECTED),
      _nonBlocking(false),
      _isRdma(false),
      _stopPolling(false),
      _useReadWriteMode(true) {  // 默认启用READ/Write模式
    
    // 检查配置是否显式禁用READ/Write模式
    auto rdmaOptions = getRDMAOptions();
    if (!rdmaOptions.rdmaUseReadWriteMode) {
        _useReadWriteMode = false;
        LOGV2_WARNING(4701400, "RDMA READ/Write mode disabled, falling back to Send/Recv (not recommended)");
    } else {
        LOGV2_DEBUG(4701401, 2, "RDMA READ/Write mode enabled (default for optimal performance)");
    }
}

RDMASocket::~RDMASocket() {
    close();
}

RDMASocket::RDMASocket(RDMASocket&& other) noexcept
    : _ioContext(other._ioContext),
      _tcpSocket(std::move(other._tcpSocket)),
      _qp(other._qp),
      _sendCQ(other._sendCQ),
      _recvCQ(other._recvCQ),
      _compChannel(other._compChannel),
      _state(other._state.load()),
      _localEndpoint(std::move(other._localEndpoint)),
      _remoteEndpoint(std::move(other._remoteEndpoint)),
      _localSockAddr(std::move(other._localSockAddr)),
      _remoteSockAddr(std::move(other._remoteSockAddr)),
      _nonBlocking(other._nonBlocking),
      _isRdma(other._isRdma),
      _stopPolling(other._stopPolling.load()) {
    // Reset the moved-from object
    other._qp = nullptr;
    other._sendCQ = nullptr;
    other._recvCQ = nullptr;
    other._compChannel = nullptr;
    other._state = ConnectionState::NOT_CONNECTED;
    other._nonBlocking = false;
    other._isRdma = false;
    other._stopPolling = false;
}

RDMASocket& RDMASocket::operator=(RDMASocket&& other) noexcept {
    if (this != &other) {
        // Clean up current resources
        close();

        // Move from other
        _tcpSocket = std::move(other._tcpSocket);
        _qp = other._qp;
        _sendCQ = other._sendCQ;
        _recvCQ = other._recvCQ;
        _compChannel = other._compChannel;
        _state = other._state.load();
        _nonBlocking = other._nonBlocking;
        _isRdma = other._isRdma;
        _stopPolling = other._stopPolling.load();
        _localEndpoint = std::move(other._localEndpoint);
        _remoteEndpoint = std::move(other._remoteEndpoint);
        _localSockAddr = std::move(other._localSockAddr);
        _remoteSockAddr = std::move(other._remoteSockAddr);

        // Reset the moved-from object
        other._qp = nullptr;
        other._sendCQ = nullptr;
        other._recvCQ = nullptr;
        other._compChannel = nullptr;
        other._state = ConnectionState::NOT_CONNECTED;
        other._nonBlocking = false;
        other._isRdma = false;
        other._stopPolling = false;
    }
    return *this;
}

void RDMASocket::open(const protocol_type& protocol) {
    // Open the TCP socket
    _tcpSocket->open(protocol);

    // Set default socket options
    setDefaultSocketOptions(*_tcpSocket);

    setState(ConnectionState::NOT_CONNECTED);
}

void RDMASocket::close() {
    // 1. 首先设置停止标志，避免在持有锁时操作
    _stopPolling.store(true);
    
    // 2. 通知所有等待的条件变量，让可能等待的线程醒来
    _cv.notify_all();
    
    // 3. 安全停止轮询线程（在获取任何锁之前完成）
    if (_pollingThread && _pollingThread->joinable()) {
        LOGV2_DEBUG(4700923, 2, "Stopping RDMA polling thread");
        
        // 使用超时机制避免无限等待
        std::atomic<bool> threadJoined{false};
        std::thread timeoutThread([this, &threadJoined]() {
            try {
                _pollingThread->join();
                threadJoined.store(true);
                LOGV2_DEBUG(4700924, 2, "RDMA polling thread stopped successfully");
            } catch (const std::exception& e) {
                threadJoined.store(true);
                LOGV2_WARNING(4700925, "Exception while stopping polling thread", 
                             "error"_attr = e.what());
            }
        });
        
        // 等待最多5秒
        auto startTime = std::chrono::steady_clock::now();
        auto timeoutDuration = std::chrono::seconds(5);
        
        while (!threadJoined.load()) {
            auto now = std::chrono::steady_clock::now();
            if (now - startTime >= timeoutDuration) {
                LOGV2_ERROR(4700931, "Timeout waiting for polling thread to stop, detaching thread");
                _pollingThread->detach();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        if (timeoutThread.joinable()) {
            timeoutThread.join();
        }
    }
    _pollingThread.reset();
    
    // 4. 现在可以安全地获取锁并清理资源
    stdx::lock_guard<Mutex> lk(_mutex);
    
    // Close the TCP socket
    asio::error_code ec;
    if (_tcpSocket && _tcpSocket->is_open()) {
        _tcpSocket->close(ec);
        if (ec) {
            LOGV2_WARNING(4700160, "Error closing TCP socket", "error"_attr = ec.message());
        }
    }

    // 在轮询线程完全停止后，安全地取消和清理异步操作
    if (_readContext) {
        _readContext->cancel();
        _readContext.reset();
    }
    if (_writeContext) {
        _writeContext->cancel();
        _writeContext.reset();
    }

    // Destroy RDMA resources
    if (_qp) {
        RDMAManager::get().destroyQP(_qp);
        _qp = nullptr;
    }

    // Free buffers and clean up memory mappings
    {
        stdx::lock_guard<Mutex> recvLk(_recvMutex);
        // Clean up memory mappings for receive buffers
        for (const auto& entry : _recvBuffers) {
            if (entry.data) {
                RDMAMemoryManager::deregisterMemory(entry.data);
                free(entry.data);
            }
        }
        _recvBuffers.clear();
        std::queue<RDMABufferEntry*>().swap(_freeRecvBuffers);
        std::queue<RDMABufferEntry*>().swap(_completedRecvBuffers);
    }
    {
        stdx::lock_guard<Mutex> sendLk(_sendMutex);
        // Clean up memory mappings for send buffers
        for (const auto& entry : _sendBuffers) {
            if (entry.data) {
                RDMAMemoryManager::deregisterMemory(entry.data);
                free(entry.data);
            }
        }
        _sendBuffers.clear();
        std::queue<RDMABufferEntry*>().swap(_freeSendBuffers);
    }

    setState(ConnectionState::CLOSED);
}

bool RDMASocket::is_open() const {
    // Consider the socket open if we're connected through either RDMA or TCP
    return _state == ConnectionState::CONNECTED || (_tcpSocket && _tcpSocket->is_open());
}

void RDMASocket::non_blocking(bool mode) {
    _nonBlocking = mode;
    if (_tcpSocket) {
        _tcpSocket->non_blocking(mode);
    }
}

bool RDMASocket::non_blocking() const {
    return _nonBlocking;
}

void RDMASocket::cancel() {
    // Cancel any pending async operations
    if (_readContext) {
        _readContext->cancel();
    }
    if (_writeContext) {
        _writeContext->cancel();
    }

    // Cancel any TCP operations
    if (_tcpSocket) {
        asio::error_code ec;
        _tcpSocket->cancel(ec);
        if (ec) {
            LOGV2_WARNING(
                4700161, "Error canceling TCP socket operations", "error"_attr = ec.message());
        }
    }
}

RDMASocket::native_handle_type RDMASocket::native_handle() const {
    // Return the TCP socket handle, since the RDMA QP doesn't have an FD type
    return _tcpSocket->native_handle();
}

void RDMASocket::connect(const endpoint_type& endpoint) {
    asio::error_code ec;
    connect(endpoint, ec);
    if (ec) {
        throw asio::system_error(ec);
    }
}

void RDMASocket::connect(const endpoint_type& endpoint, asio::error_code& ec) {
    // If RDMA is not available or not enabled, fall back to TCP
    const auto& rdmaOptions = getRDMAOptions();
    if (!rdmaOptions.enableRDMA || !RDMAManager::get().isRDMAAvailable()) {
        _tcpSocket->connect(endpoint, ec);
        if (!ec) {
            _isRdma = false;
            setState(ConnectionState::CONNECTED);
        } else {
            setState(ConnectionState::ERROR);
        }
        return;
    }

    // Set state to connecting
    setState(ConnectionState::CONNECTING);

    // First use TCP to establish the initial connection for parameter exchange
    _tcpSocket->connect(endpoint, ec);
    if (ec) {
        setState(ConnectionState::ERROR);
        return;
    }

    // Save the remote endpoint
    _remoteEndpoint = endpoint;

    // Try to initialize RDMA
    auto status = initializeRDMA();
    if (!status.isOK()) {
        LOGV2_WARNING(4700162,
                      "Failed to initialize RDMA connection, falling back to TCP",
                      "error"_attr = status.reason());

        // Fall back to TCP
        _isRdma = false;
        setState(ConnectionState::CONNECTED);
        ec = asio::error_code();
        return;
    }

    // Exchange RDMA connection parameters
    negotiateRDMACapabilities();

    // Check if RDMA negotiation was successful
    if (_isRdma) {
        setState(ConnectionState::CONNECTED);
        LOGV2(4700163, "RDMA connection established", "remote"_attr = "RDMA endpoint");
    } else {
        LOGV2(4700164, "Using TCP fallback connection", "remote"_attr = "TCP endpoint");
    }

    ec = asio::error_code();
}

RDMASocket::endpoint_type RDMASocket::local_endpoint() const {
    if (_tcpSocket) {
        return _tcpSocket->local_endpoint();
    }
    return endpoint_type();
}

RDMASocket::endpoint_type RDMASocket::remote_endpoint() const {
    if (_tcpSocket) {
        return _tcpSocket->remote_endpoint();
    }
    return endpoint_type();
}

void RDMASocket::shutdown(asio::socket_base::shutdown_type type) {
    asio::error_code ec;
    shutdown(type, ec);
    if (ec) {
        throw asio::system_error(ec);
    }
}

void RDMASocket::shutdown(asio::socket_base::shutdown_type type, asio::error_code& ec) {
    // Always shutdown the TCP socket
    if (_tcpSocket) {
        _tcpSocket->shutdown(type, ec);
    }

    // If we're using RDMA, handle shutdown specially
    if (_isRdma) {
        setState(ConnectionState::CLOSING);

        // Complete any pending operations
        completePendingOperations();

        setState(ConnectionState::CLOSED);
    }
}

void RDMASocket::setState(ConnectionState state) {
    auto previousState = _state.exchange(state);

    // Notify any waiting threads if the state has changed
    if (previousState != state) {
        _cv.notify_all();
    }
}

Status RDMASocket::initializeRDMA() {
    // Check if RDMA is available
    auto& rdmaManager = RDMAManager::get();
    if (!rdmaManager.isRDMAAvailable()) {
        return Status(ErrorCodes::RDMAError, "RDMA is not available");
    }

    // Initialize RDMA manager if not already done
    if (!rdmaManager.isInitialized()) {
        const auto& rdmaOptions = getRDMAOptions();
        auto status = rdmaManager.initialize(rdmaOptions.rdmaDevice, rdmaOptions.rdmaPort);
        if (!status.isOK()) {
            return status;
        }
    }

    // Create queue pair
    auto swQP = rdmaManager.createQP();
    if (!swQP.isOK()) {
        return swQP.getStatus();
    }
    _qp = swQP.getValue();

    // Get the completion queues
    _sendCQ = _qp->send_cq;
    _recvCQ = _qp->recv_cq;
    _compChannel = rdmaManager.getCompChannel();

    // Transition QP to INIT state
    auto status = rdmaManager.setQPState(_qp, QPConnectionState::INIT);
    if (!status.isOK()) {
        return status;
    }

    // Get local connection info
    auto swConnInfo = rdmaManager.getLocalConnectionInfo(_qp);
    if (!swConnInfo.isOK()) {
        return swConnInfo.getStatus();
    }
    _localConnInfo = swConnInfo.getValue();

    // Allocate and register buffers
    status = allocateBuffers();
    if (!status.isOK()) {
        return status;
    }

    // Post initial receive requests
    status = postInitialRecvs();
    if (!status.isOK()) {
        return status;
    }

    // Start polling for completions
    startPolling();

    return Status::OK();
}

Status RDMASocket::allocateBuffers() {
    auto& rdmaManager = RDMAManager::get();
    ibv_pd* pd = rdmaManager.getPD();
    return allocateBuffersWithPD(pd);
}

Status RDMASocket::allocateBuffers(ibv_qp* qp) {
    if (!qp || !qp->pd) {
        return Status(ErrorCodes::RDMAError, "Invalid QP or QP protection domain");
    }
    return allocateBuffersWithPD(qp->pd);
}

Status RDMASocket::allocateBuffersWithPD(ibv_pd* pd) {
    LOGV2_DEBUG(4700918, 1, "Allocating RDMA buffers with PD", 
                "pd"_attr = reinterpret_cast<uintptr_t>(pd));

    // Allocate send buffers
    try {
        _sendBuffers.reserve(kNumBuffers);

        for (size_t i = 0; i < kNumBuffers; ++i) {
            RDMABufferEntry entry;

            // Allocate a buffer
            void* buffer = aligned_alloc(4096, kDefaultBufferSize);
            if (!buffer) {
                return Status(ErrorCodes::RDMAMemoryRegisterError,
                              "Failed to allocate send buffer memory");
            }

            // Register memory region
            auto swMr = RDMAMemoryManager::registerMemory(
                pd, buffer, kDefaultBufferSize, IBV_ACCESS_LOCAL_WRITE);
            if (!swMr.isOK()) {
                free(buffer);
                return swMr.getStatus();
            }

            // Store memory region mapping for later lookup
            RDMAMemoryManager::storeMemoryRegion(buffer, swMr.getValue());
            
            LOGV2_DEBUG(4700919, 2, "Stored send buffer memory mapping", 
                        "buffer"_attr = reinterpret_cast<uintptr_t>(buffer),
                        "mr"_attr = reinterpret_cast<uintptr_t>(swMr.getValue()),
                        "lkey"_attr = swMr.getValue()->lkey);

            entry.mr = swMr.getValue();
            entry.data = static_cast<char*>(buffer);
            entry.size = kDefaultBufferSize;
            entry.offset = 0;
            entry.completed = true;
            entry.error = false;

            _sendBuffers.push_back(entry);
            _freeSendBuffers.push(&_sendBuffers.back());
        }
        
        LOGV2_DEBUG(4700910, 1, "Allocated send buffers", 
                    "count"_attr = _sendBuffers.size(),
                    "freeCount"_attr = _freeSendBuffers.size());
    } catch (const std::exception& e) {
        return Status(ErrorCodes::RDMAMemoryRegisterError,
                      str::stream() << "Failed to allocate send buffers: " << e.what());
    }

    // Allocate receive buffers
    try {
        _recvBuffers.reserve(kNumBuffers);

        for (size_t i = 0; i < kNumBuffers; ++i) {
            RDMABufferEntry entry;

            // Allocate a buffer
            void* buffer = aligned_alloc(4096, kDefaultBufferSize);
            if (!buffer) {
                return Status(ErrorCodes::RDMAMemoryRegisterError,
                              "Failed to allocate receive buffer memory");
            }

            // Register memory region
            auto swMr = RDMAMemoryManager::registerMemory(
                pd, buffer, kDefaultBufferSize, IBV_ACCESS_LOCAL_WRITE);
            if (!swMr.isOK()) {
                free(buffer);
                return swMr.getStatus();
            }

            // Store memory region mapping for later lookup
            RDMAMemoryManager::storeMemoryRegion(buffer, swMr.getValue());
            
            LOGV2_DEBUG(4700920, 2, "Stored receive buffer memory mapping", 
                        "buffer"_attr = reinterpret_cast<uintptr_t>(buffer),
                        "mr"_attr = reinterpret_cast<uintptr_t>(swMr.getValue()),
                        "lkey"_attr = swMr.getValue()->lkey);

            entry.mr = swMr.getValue();
            entry.data = static_cast<char*>(buffer);
            entry.size = kDefaultBufferSize;
            entry.offset = 0;
            entry.completed = false;
            entry.error = false;

            _recvBuffers.push_back(entry);
            _freeRecvBuffers.push(&_recvBuffers.back());
        }
        
        LOGV2_DEBUG(4700911, 1, "Allocated receive buffers", 
                    "count"_attr = _recvBuffers.size(),
                    "freeCount"_attr = _freeRecvBuffers.size());
    } catch (const std::exception& e) {
        return Status(ErrorCodes::RDMAMemoryRegisterError,
                      str::stream() << "Failed to allocate receive buffers: " << e.what());
    }

    return Status::OK();
}

Status RDMASocket::postInitialRecvs() {
    stdx::lock_guard<Mutex> lk(_recvMutex);

    // Post receive requests for all free buffers
    while (!_freeRecvBuffers.empty()) {
        auto bufferEntry = _freeRecvBuffers.front();
        _freeRecvBuffers.pop();

        auto status = postRecv(bufferEntry->data, bufferEntry->size);
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

void RDMASocket::negotiateRDMACapabilities() {
    // Get local RDMA capabilities
    auto localCaps = RDMAManager::get().getLocalCapabilities();

    // Send magic bytes to indicate RDMA support
    asio::error_code ec;
    asio::write(*_tcpSocket, asio::buffer(kRDMAMagic, kMagicSize), ec);
    if (ec) {
        LOGV2_WARNING(4700175, "Failed to send RDMA magic bytes", "error"_attr = ec.message());
        _isRdma = false;
        return;
    }

    // Serialize and send local capabilities
    std::string capsStr = str::stream()
        << localCaps.supportsRDMA << "," << localCaps.maxInlineData << "," << localCaps.maxSge
        << "," << localCaps.maxSendWr << "," << localCaps.maxRecvWr << "," << localCaps.rdmaVersion;

    uint32_t capsSize = static_cast<uint32_t>(capsStr.size());
    asio::write(*_tcpSocket, asio::buffer(&capsSize, sizeof(capsSize)), ec);
    if (ec) {
        LOGV2_WARNING(
            4700166, "Failed to send local capabilities size", "error"_attr = ec.message());
        _isRdma = false;
        return;
    }

    asio::write(*_tcpSocket, asio::buffer(capsStr), ec);
    if (ec) {
        LOGV2_WARNING(4700167, "Failed to send local capabilities", "error"_attr = ec.message());
        _isRdma = false;
        return;
    }

    // Send local connection info
    std::string connInfoStr = str::stream()
        << _localConnInfo.lid << "," << _localConnInfo.qpn << "," << _localConnInfo.psn << ","
        << _localConnInfo.maxInlineData;

    uint32_t connInfoSize = static_cast<uint32_t>(connInfoStr.size());
    asio::write(*_tcpSocket, asio::buffer(&connInfoSize, sizeof(connInfoSize)), ec);
    if (ec) {
        LOGV2_WARNING(
            4700168, "Failed to send local connection info size", "error"_attr = ec.message());
        _isRdma = false;
        return;
    }

    asio::write(*_tcpSocket, asio::buffer(connInfoStr), ec);
    if (ec) {
        LOGV2_WARNING(4700169, "Failed to send local connection info", "error"_attr = ec.message());
        _isRdma = false;
        return;
    }

    // Receive magic bytes from peer
    char peerMagic[kMagicSize];
    asio::read(*_tcpSocket, asio::buffer(peerMagic, kMagicSize), ec);
    if (ec || std::memcmp(peerMagic, kRDMAMagic, kMagicSize) != 0) {
        LOGV2_WARNING(4700170, "Peer does not support RDMA", "error"_attr = ec.message());
        _isRdma = false;
        return;
    }

    // Receive peer capabilities
    RDMACapabilities peerCaps;
    uint32_t peerCapsSize;
    asio::read(*_tcpSocket, asio::buffer(&peerCapsSize, sizeof(peerCapsSize)), ec);
    if (ec) {
        LOGV2_WARNING(
            4700171, "Failed to receive peer capabilities size", "error"_attr = ec.message());
        _isRdma = false;
        return;
    }

    std::vector<char> peerCapsBuffer(peerCapsSize);
    asio::read(*_tcpSocket, asio::buffer(peerCapsBuffer), ec);
    if (ec) {
        LOGV2_WARNING(4700172, "Failed to receive peer capabilities", "error"_attr = ec.message());
        _isRdma = false;
        return;
    }

    std::string peerCapsStr(peerCapsBuffer.begin(), peerCapsBuffer.end());
    std::vector<std::string> parts;
    str::splitStringDelim(peerCapsStr, &parts, ',');
    if (parts.size() != 6) {
        LOGV2_WARNING(4700173, "Invalid peer capabilities format", "data"_attr = peerCapsStr);
        _isRdma = false;
        return;
    }

    try {
        peerCaps.supportsRDMA = (parts[0] == "1");
        peerCaps.maxInlineData = std::stoul(parts[1]);
        peerCaps.maxSge = std::stoul(parts[2]);
        peerCaps.maxSendWr = std::stoul(parts[3]);
        peerCaps.maxRecvWr = std::stoul(parts[4]);
        peerCaps.rdmaVersion = parts[5];
    } catch (const std::exception& e) {
        LOGV2_WARNING(4700174, "Failed to parse peer capabilities", "error"_attr = e.what());
        _isRdma = false;
        return;
    }

    // Check if peer supports RDMA
    if (!peerCaps.supportsRDMA) {
        _isRdma = false;
        return;
    }

    // Receive peer connection info
    _remoteConnInfo = {};
    uint32_t peerConnInfoSize;
    asio::read(*_tcpSocket, asio::buffer(&peerConnInfoSize, sizeof(peerConnInfoSize)), ec);
    if (ec) {
        LOGV2_WARNING(
            4700180, "Failed to receive peer connection info size", "error"_attr = ec.message());
        _isRdma = false;
        return;
    }

    std::vector<char> peerConnInfoBuffer(peerConnInfoSize);
    asio::read(*_tcpSocket, asio::buffer(peerConnInfoBuffer), ec);
    if (ec) {
        LOGV2_WARNING(
            4700181, "Failed to receive peer connection info", "error"_attr = ec.message());
        _isRdma = false;
        return;
    }

    std::string peerConnInfoStr(peerConnInfoBuffer.begin(), peerConnInfoBuffer.end());
    parts.clear();
    str::splitStringDelim(peerConnInfoStr, &parts, ',');
    if (parts.size() != 4) {
        LOGV2_WARNING(
            4700177, "Invalid peer connection info format", "data"_attr = peerConnInfoStr);
        _isRdma = false;
        return;
    }

    try {
        _remoteConnInfo.lid = std::stoul(parts[0]);
        _remoteConnInfo.qpn = std::stoul(parts[1]);
        _remoteConnInfo.psn = std::stoul(parts[2]);
        _remoteConnInfo.maxInlineData = std::stoul(parts[3]);
        _remoteConnInfo.valid = true;
    } catch (const std::exception& e) {
        LOGV2_WARNING(4700178, "Failed to parse peer connection info", "error"_attr = e.what());
        _isRdma = false;
        return;
    }

    // Transition QP to RTS state using peer info
    auto status = RDMAManager::get().setQPState(_qp, QPConnectionState::RTS, &_remoteConnInfo);
    if (!status.isOK()) {
        LOGV2_WARNING(4700179, "Failed to transition QP to RTS", "error"_attr = status.reason());
        _isRdma = false;
        return;
    }

    // RDMA connection established
    _isRdma = true;
}

Status RDMASocket::postRecv(void* buffer, size_t length) {
    ibv_mr* mr = RDMAMemoryManager::getMemoryRegion(buffer);
    if (!mr) {
        LOGV2_ERROR(4700914, "Memory region not found for receive buffer", 
                    "buffer"_attr = reinterpret_cast<uintptr_t>(buffer),
                    "length"_attr = length,
                    "qp"_attr = reinterpret_cast<uintptr_t>(_qp),
                    "pd"_attr = reinterpret_cast<uintptr_t>(_qp ? _qp->pd : nullptr));
        return Status(ErrorCodes::RDMAMemoryRegisterError, "Memory region not found for receive buffer");
    }
    
    LOGV2_DEBUG(4700915, 2, "Found memory region for receive buffer", 
                "buffer"_attr = reinterpret_cast<uintptr_t>(buffer),
                "mr"_attr = reinterpret_cast<uintptr_t>(mr),
                "lkey"_attr = mr->lkey);

    ibv_sge sge;
    sge.addr = reinterpret_cast<uint64_t>(buffer);
    sge.length = length;
    sge.lkey = mr->lkey;

    ibv_recv_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.next = nullptr;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    // Use buffer address as wr_id for easy lookup
    wr.wr_id = reinterpret_cast<uint64_t>(buffer);

    ibv_recv_wr* bad_wr;
    int ret = ibv_post_recv(_qp, &wr, &bad_wr);
    if (ret) {
        return Status(ErrorCodes::RDMAPostError,
                      str::stream() << "Failed to post receive request: " << strerror(errno));
    }

    return Status::OK();
}

Status RDMASocket::postSend(const void* buffer, size_t length, bool inline_data) {
    ibv_mr* mr = RDMAMemoryManager::getMemoryRegion(const_cast<void*>(buffer));
    if (!mr) {
        LOGV2_ERROR(4700916, "Memory region not found for send buffer", 
                    "buffer"_attr = reinterpret_cast<uintptr_t>(buffer),
                    "length"_attr = length,
                    "qp"_attr = reinterpret_cast<uintptr_t>(_qp),
                    "pd"_attr = reinterpret_cast<uintptr_t>(_qp ? _qp->pd : nullptr));
        return Status(ErrorCodes::RDMAMemoryRegisterError, "Memory region not found for send buffer");
    }
    
    LOGV2_DEBUG(4700917, 2, "Found memory region for send buffer", 
                "buffer"_attr = reinterpret_cast<uintptr_t>(buffer),
                "mr"_attr = reinterpret_cast<uintptr_t>(mr),
                "lkey"_attr = mr->lkey);

    ibv_sge sge;
    sge.addr = reinterpret_cast<uint64_t>(buffer);
    sge.length = length;
    sge.lkey = mr->lkey;

    ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.next = nullptr;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;

    // Use buffer address as wr_id for easy lookup
    wr.wr_id = reinterpret_cast<uint64_t>(buffer);

    // Set inline flag if requested
    // 重要修复：只有在消息很小且驱动支持时才使用内联发送
    // 使用保守的64字节限制，避免超过硬件能力
    const uint32_t maxInlineData = RDMAManager::get().getMaxInlineDataSize();
    const size_t safeInlineLimit = std::min<size_t>(64, maxInlineData);
    
    if (inline_data && length <= safeInlineLimit && _qp) {
        wr.send_flags |= IBV_SEND_INLINE;
        LOGV2_DEBUG(4700921, 3, "Using inline send", 
                    "length"_attr = length,
                    "safeInlineLimit"_attr = safeInlineLimit,
                    "maxInlineData"_attr = maxInlineData);
    } else if (inline_data) {
        // 如果请求内联但不满足条件，记录警告
        LOGV2_DEBUG(4700922, 2, "Inline send requested but not used", 
                    "length"_attr = length,
                    "safeInlineLimit"_attr = safeInlineLimit,
                    "maxInlineData"_attr = maxInlineData,
                    "reason"_attr = (length > safeInlineLimit ? "message too large" : "QP not ready"));
    }

    // Always signal completion
    wr.send_flags |= IBV_SEND_SIGNALED;

    ibv_send_wr* bad_wr;
    int ret = ibv_post_send(_qp, &wr, &bad_wr);
    if (ret) {
        return Status(ErrorCodes::RDMAPostError,
                      str::stream() << "Failed to post send request: " << strerror(errno));
    }

    return Status::OK();
}

void RDMASocket::startPolling() {
    // Create a thread to poll for completions
    _stopPolling = false;
    
    // 使用条件变量确保线程完全启动
    stdx::condition_variable threadStarted;
    stdx::mutex threadStartMutex;
    bool started = false;
    
    _pollingThread = std::make_unique<stdx::thread>([this, &threadStarted, &threadStartMutex, &started]() {
        // 通知主线程：轮询线程已启动
        {
            stdx::lock_guard<stdx::mutex> lk(threadStartMutex);
            started = true;
        }
        threadStarted.notify_one();
        
        LOGV2_DEBUG(4700190, 2, "RDMA polling thread started");

        try {
            while (!_stopPolling.load() && _state.load() != ConnectionState::CLOSED) {
                pollOnce();

                // Sleep for a short time to avoid busy waiting
                stdx::this_thread::sleep_for(stdx::chrono::milliseconds(1));
            }
        } catch (const std::exception& e) {
            LOGV2_ERROR(4700935, "Exception in RDMA polling thread", 
                       "error"_attr = e.what());
            // 设置错误状态但不让线程崩溃
            setState(ConnectionState::ERROR);
        }

        LOGV2_DEBUG(4700191, 2, "RDMA polling thread stopped");
    });
    
    // 等待线程完全启动后再返回
    {
        stdx::unique_lock<stdx::mutex> lk(threadStartMutex);
        threadStarted.wait(lk, [&started] { return started; });
    }
    
    LOGV2_DEBUG(4700922, 2, "RDMA polling thread startup confirmed");
}

void RDMASocket::pollOnce() {
    try {
        // 检查Socket是否已关闭，避免访问已销毁的对象
        if (_state.load() == ConnectionState::CLOSED || _stopPolling.load()) {
            return;
        }

        // Poll completion queue for sends
        int numCompletions = 0;
        ibv_wc wc[10];

        // Poll send completions
        numCompletions = ibv_poll_cq(_sendCQ, 10, wc);
        if (numCompletions > 0) {
            for (int i = 0; i < numCompletions; ++i) {
                void* buffer = reinterpret_cast<void*>(wc[i].wr_id);

                // Find the buffer entry - 使用try_lock避免死锁
                bool found = false;
                {
                    stdx::unique_lock<Mutex> lk(_sendMutex, stdx::try_to_lock);
                    if (!lk.owns_lock()) {
                        // 如果无法获取锁，跳过这次处理，下次再试
                        LOGV2_DEBUG(4700932, 3, "Failed to acquire send mutex, skipping completion processing");
                        return;
                    }
                    
                    for (auto& entry : _sendBuffers) {
                        if (entry.data == buffer) {
                            // Mark as completed
                            entry.completed = true;
                            entry.error = (wc[i].status != IBV_WC_SUCCESS);

                            // Add to free list
                            _freeSendBuffers.push(&entry);

                            found = true;
                            break;
                        }
                    }
                }

                if (!found) {
                    LOGV2_WARNING(4700195,
                                  "Received completion for unknown send buffer",
                                  "wr_id"_attr = wc[i].wr_id);
                } else if (wc[i].status != IBV_WC_SUCCESS) {
                    LOGV2_WARNING(
                        4700196, "Send completion event with error", "status"_attr = wc[i].status);
                }
            }

            // Notify waiting write operation if any - 添加安全检查
            if (_writeContext && !_writeContext->isCanceled() && 
                _state.load() != ConnectionState::CLOSED) {
                _writeContext->complete(numCompletions);
                _writeContext.reset();
            }
        }

        // Poll receive completions
        numCompletions = ibv_poll_cq(_recvCQ, 10, wc);
        if (numCompletions > 0) {
            for (int i = 0; i < numCompletions; ++i) {
                void* buffer = reinterpret_cast<void*>(wc[i].wr_id);

                // Find the buffer entry - 使用try_lock避免死锁
                bool found = false;
                {
                    stdx::unique_lock<Mutex> lk(_recvMutex, stdx::try_to_lock);
                    if (!lk.owns_lock()) {
                        // 如果无法获取锁，跳过这次处理，下次再试
                        LOGV2_DEBUG(4700933, 3, "Failed to acquire recv mutex, skipping completion processing");
                        return;
                    }
                    
                    for (auto& entry : _recvBuffers) {
                        if (entry.data == buffer) {
                            // Mark as completed
                            entry.completed = true;
                            entry.error = (wc[i].status != IBV_WC_SUCCESS);

                            // Add to completed list
                            _completedRecvBuffers.push(&entry);

                            found = true;
                            break;
                        }
                    }
                }

                if (!found) {
                    LOGV2_WARNING(4700197,
                                  "Received completion for unknown receive buffer",
                                  "wr_id"_attr = wc[i].wr_id);
                } else if (wc[i].status != IBV_WC_SUCCESS) {
                    LOGV2_WARNING(
                        4700198, "Receive completion event with error", "status"_attr = wc[i].status);
                }
            }

            // Notify waiting read operation if any - 添加安全检查
            if (_readContext && !_readContext->isCanceled() && 
                _state.load() != ConnectionState::CLOSED) {
                _readContext->complete(numCompletions);
                _readContext.reset();
            }
        }
    } catch (const std::exception& e) {
        LOGV2_ERROR(4700934, "Exception in RDMA polling", 
                   "error"_attr = e.what());
        // 设置错误状态但不抛出异常，避免导致程序崩溃
        setState(ConnectionState::ERROR);
        
        // 通知可能等待的操作
        _cv.notify_all();
    }
}

void RDMASocket::completePendingOperations() {
    // Cancel any pending read operation
    if (_readContext) {
        _readContext->completeWithError(asio::error::operation_aborted);
    }

    // Cancel any pending write operation
    if (_writeContext) {
        _writeContext->completeWithError(asio::error::operation_aborted);
    }
}

void RDMASocket::processCompletions() {
    // Process any pending completions
    pollOnce();
}

void RDMASocket::handleError(const Status& status) {
    LOGV2_WARNING(4700199, "RDMA socket error", "error"_attr = status.reason());

    setState(ConnectionState::ERROR);

    // Complete any pending operations with error
    completePendingOperations();
}

void RDMASocket::setupTCPFallback() {
    // Already done in constructor
    _isRdma = false;
}

Future<Message> RDMASocket::asyncSourceMessage(const BatonHandle& baton) {
    if (!_isRdma) {
        // TCP fallback implementation
        return asyncTCPSourceMessage(baton);
    }

    // Check if using READ/Write mode
    if (_useReadWriteMode && _readWriteSocket) {
        LOGV2_DEBUG(4701402, 3, "Using RDMA READ/Write mode for message source");
        return _readWriteSocket->asyncSourceMessage(baton);
    }

    // Traditional RDMA Send/Recv implementation
    return asyncRDMASourceMessage(baton);
}

Future<void> RDMASocket::asyncSinkMessage(Message message, const BatonHandle& baton) {
    if (!_isRdma) {
        // TCP fallback implementation
        return asyncTCPSinkMessage(message, baton);
    }

    // Check if using READ/Write mode
    if (_useReadWriteMode && _readWriteSocket) {
        LOGV2_DEBUG(4701403, 3, "Using RDMA READ/Write mode for message sink", 
                    "messageSize"_attr = message.size());
        return _readWriteSocket->asyncSinkMessage(std::move(message), baton);
    }

    // Traditional RDMA Send/Recv implementation
    return asyncRDMASinkMessage(std::move(message), baton);
}

Future<void> RDMASocket::asyncTCPSinkMessage(Message message, const BatonHandle& baton) {
    if (!_tcpSocket || !_tcpSocket->is_open()) {
        return Future<void>::makeReady(
            Status(ErrorCodes::SocketException, "TCP socket is not open"));
    }

    // Create promise/future pair
    auto [promise, future] = makePromiseFuture<void>();

    try {
        // Get message buffer and size
        const char* msgData = message.buf();
        size_t msgSize = message.size();

        if (!msgData || msgSize == 0) {
            return Future<void>::makeReady(Status(ErrorCodes::BadValue, "Invalid message data"));
        }

        LOGV2_DEBUG(4700250, 3, "TCP message send starting", "messageSize"_attr = msgSize);

        // Asynchronously write the complete message
        asio::async_write(*_tcpSocket,
                          asio::buffer(msgData, msgSize),
                          [message = std::move(message), promise = std::move(promise)](
                              const std::error_code& ec, size_t bytesTransferred) mutable {
                              if (ec) {
                                  LOGV2_WARNING(4700251,
                                                "TCP socket write error",
                                                "error"_attr = ec.message(),
                                                "bytesTransferred"_attr = bytesTransferred);

                                  auto status =
                                      Status(ErrorCodes::SocketException,
                                             str::stream() << "TCP write failed: " << ec.message());
                                  promise.setError(status);
                                  return;
                              }

                              LOGV2_DEBUG(4700252,
                                          3,
                                          "TCP message sent successfully",
                                          "bytesTransferred"_attr = bytesTransferred,
                                          "messageSize"_attr = message.size());

                              promise.emplaceValue();
                          });

    } catch (const std::exception& e) {
        LOGV2_WARNING(4700253, "Exception in TCP async write", "error"_attr = e.what());
        return Future<void>::makeReady(
            Status(ErrorCodes::SocketException,
                   str::stream() << "TCP async write exception: " << e.what()));
    }

    return std::move(future);
}

Future<void> RDMASocket::asyncRDMASinkMessage(Message message, const BatonHandle& baton) {
    if (_state.load() != ConnectionState::CONNECTED) {
        return Future<void>::makeReady(
            Status(ErrorCodes::SocketException, "RDMA socket is not connected"));
    }

    // Get message data
    const char* msgData = message.buf();
    size_t msgSize = message.size();
    
    // LOG message to sink
    LOGV2_DEBUG(4700921, 2, "RDMA message to sink", 
                "messageSize"_attr = msgSize,
                "messageData"_attr = std::string(msgData, msgSize));
    if (message.operation() == dbMsg) {
        try {
            auto opMsg = mongo::OpMsg::parse(message);
            LOGV2_DEBUG(4700922, 2, "RDMA DbMessage to content",
                    "body"_attr = redact(opMsg.body.toString()));
        } catch (const std::exception& e) {
            LOGV2_WARNING(4700923, "Exception in RDMA async sink message", "error"_attr = e.what());
        }
    }

    if (msgSize == 0) {
        return Future<void>::makeReady(Status::OK());
    }

    // Create promise/future pair
    auto [promise, future] = makePromiseFuture<void>();

    try {
        // Find a free send buffer
        RDMABufferEntry* sendBuffer = nullptr;
        {
            stdx::lock_guard<Mutex> lk(_sendMutex);
            LOGV2_DEBUG(4700912, 2, "Checking send buffers", 
                        "freeCount"_attr = _freeSendBuffers.size(),
                        "messageSize"_attr = msgSize);
            
            if (_freeSendBuffers.empty()) {
                // No free buffers available, wait for completion or return error
                LOGV2_ERROR(4700913, "No free RDMA send buffers available", 
                           "totalBuffers"_attr = _sendBuffers.size(),
                           "messageSize"_attr = msgSize);
                promise.setError(
                    Status(ErrorCodes::ExceededMemoryLimit, "No free RDMA send buffers available"));
                return std::move(future);
            }

            sendBuffer = _freeSendBuffers.front();
            _freeSendBuffers.pop();
        }

        // Check if message fits in a single buffer
        // 如果单条消息 buffer 大小不够，需要多个 buffer 拼接
        if (msgSize > sendBuffer->size) {
            // Return buffer to free list
            {
                stdx::lock_guard<Mutex> lk(_sendMutex);
                _freeSendBuffers.push(sendBuffer);
            }

            promise.setError(Status(ErrorCodes::BadValue,
                                    str::stream() << "Message size (" << msgSize
                                                  << ") exceeds RDMA buffer size ("
                                                  << sendBuffer->size << ")"));
            return std::move(future);
        }

        // Copy message data to RDMA buffer
        std::memcpy(sendBuffer->data, msgData, msgSize);
        sendBuffer->offset = 0;
        sendBuffer->size = msgSize;
        sendBuffer->completed = false;
        sendBuffer->error = false;

        // Determine if we should use inline data
        const auto& rdmaOptions = getRDMAOptions();
        bool useInline = (msgSize <= static_cast<size_t>(rdmaOptions.rdmaMaxInlineData));
        
        // 重要安全修复：对于较大的消息，强制禁用内联发送
        // 内联发送会在post_send时立即复制数据，但如果消息太大可能导致内存访问越界
        const size_t SAFE_INLINE_LIMIT = 64;  // 使用保守的64字节限制
        if (useInline && msgSize > SAFE_INLINE_LIMIT) {
            useInline = false;
            LOGV2_DEBUG(4700924, 2, "Disabling inline send for large message", 
                        "messageSize"_attr = msgSize,
                        "safeInlineLimit"_attr = SAFE_INLINE_LIMIT,
                        "originalInlineLimit"_attr = rdmaOptions.rdmaMaxInlineData);
        }

        // Set up completion context for this operation
        _writeContext = std::make_shared<RDMACompletionContext>(_ioContext);
        auto promisePtr = std::make_shared<Promise<void>>(std::move(promise));
        _writeContext->setHandler([this, sendBuffer, message, promisePtr](
                                      const std::error_code& ec, size_t bytesTransferred) mutable {
            if (ec) {
                LOGV2_WARNING(4700210,
                              "RDMA send completion error",
                              "error"_attr = ec.message(),
                              "bytesTransferred"_attr = bytesTransferred);

                // Return buffer to free list
                {
                    stdx::lock_guard<Mutex> lk(_sendMutex);
                    sendBuffer->completed = true;
                    _freeSendBuffers.push(sendBuffer);
                }

                auto status = Status(ErrorCodes::SocketException,
                                     str::stream() << "RDMA send failed: " << ec.message());
                promisePtr->setError(status);
            } else {
                LOGV2_DEBUG(4700211,
                            3,
                            "RDMA message sent successfully",
                            "bytesTransferred"_attr = bytesTransferred,
                            "messageSize"_attr = message.size());

                // Return buffer to free list
                {
                    stdx::lock_guard<Mutex> lk(_sendMutex);
                    sendBuffer->completed = true;
                    _freeSendBuffers.push(sendBuffer);
                }

                promisePtr->emplaceValue();
            }
        });

        // Post the send operation
        auto status = postSend(sendBuffer->data, msgSize, useInline);
        if (!status.isOK()) {
            // Return buffer to free list
            {
                stdx::lock_guard<Mutex> lk(_sendMutex);
                _freeSendBuffers.push(sendBuffer);
            }

            _writeContext.reset();

            LOGV2_WARNING(4700212, "Failed to post RDMA send", "error"_attr = status.reason());
            promisePtr->setError(status);
            return std::move(future);
        }

        LOGV2_DEBUG(4700213,
                    3,
                    "RDMA send posted successfully",
                    "messageSize"_attr = msgSize,
                    "useInline"_attr = useInline);

    } catch (const std::exception& e) {
        LOGV2_WARNING(4700214, "Exception in RDMA async send", "error"_attr = e.what());

        // Clean up write context
        if (_writeContext) {
            _writeContext->cancel();
            _writeContext.reset();
        }

        return Future<void>::makeReady(
            Status(ErrorCodes::SocketException,
                   str::stream() << "RDMA async send exception: " << e.what()));
    }

    return std::move(future);
}

Future<Message> RDMASocket::asyncTCPSourceMessage(const BatonHandle& baton) {
    if (!_tcpSocket || !_tcpSocket->is_open()) {
        return Future<Message>::makeReady(
            Status(ErrorCodes::SocketException, "TCP socket is not open"));
    }

    // Create promise/future pair
    auto [promise, future] = makePromiseFuture<Message>();

    try {
        // First, read the message header to get the message size
        auto headerBuffer = std::make_shared<std::array<char, sizeof(MSGHEADER::Value)>>();

        asio::async_read(
            *_tcpSocket,
            asio::buffer(headerBuffer->data(), headerBuffer->size()),
            [this, headerBuffer, promise = std::move(promise), baton](
                const std::error_code& ec, size_t bytesTransferred) mutable {
                if (ec) {
                    LOGV2_WARNING(4700220,
                                  "TCP socket read header error",
                                  "error"_attr = ec.message(),
                                  "bytesTransferred"_attr = bytesTransferred);

                    auto status =
                        Status(ErrorCodes::SocketException,
                               str::stream() << "TCP read header failed: " << ec.message());
                    promise.setError(status);
                    return;
                }

                // Parse the header to get message length
                MSGHEADER::ConstView headerView(headerBuffer->data());
                int32_t messageLength = headerView.getMessageLength();

                if (messageLength < static_cast<int32_t>(sizeof(MSGHEADER::Value)) ||
                    messageLength > 64 * 1024 * 1024) {  // 64MB max
                    auto status =
                        Status(ErrorCodes::InvalidBSON,
                               str::stream() << "Invalid message length: " << messageLength);
                    promise.setError(status);
                    return;
                }

                // Allocate buffer for the complete message
                auto messageBuffer = SharedBuffer::allocate(messageLength);

                // Copy header to message buffer
                std::memcpy(messageBuffer.get(), headerBuffer->data(), headerBuffer->size());

                // Read the rest of the message
                size_t remainingBytes = messageLength - headerBuffer->size();
                if (remainingBytes == 0) {
                    // Message consists only of header
                    Message message(std::move(messageBuffer));
                    promise.emplaceValue(std::move(message));
                    return;
                }

                asio::async_read(
                    *_tcpSocket,
                    asio::buffer(messageBuffer.get() + headerBuffer->size(), remainingBytes),
                    [messageBuffer, promise = std::move(promise)](
                        const std::error_code& ec, size_t bytesTransferred) mutable {
                        if (ec) {
                            LOGV2_WARNING(4700221,
                                          "TCP socket read body error",
                                          "error"_attr = ec.message(),
                                          "bytesTransferred"_attr = bytesTransferred);

                            auto status =
                                Status(ErrorCodes::SocketException,
                                       str::stream() << "TCP read body failed: " << ec.message());
                            promise.setError(status);
                            return;
                        }

                        // Create and return the message
                        Message message(std::move(messageBuffer));

                        LOGV2_DEBUG(4700222,
                                    3,
                                    "TCP message received successfully",
                                    "messageSize"_attr = message.size());

                        promise.emplaceValue(std::move(message));
                    });
            });
    } catch (const std::exception& e) {
        LOGV2_WARNING(4700223, "Exception in TCP async read", "error"_attr = e.what());
        return Future<Message>::makeReady(
            Status(ErrorCodes::SocketException,
                   str::stream() << "TCP async read exception: " << e.what()));
    }

    return std::move(future);
}

Future<Message> RDMASocket::asyncRDMASourceMessage(const BatonHandle& baton) {
    if (_state.load() != ConnectionState::CONNECTED) {
        return Future<Message>::makeReady(
            Status(ErrorCodes::SocketException, "RDMA socket is not connected"));
    }

    // Create promise/future pair
    auto [promise, future] = makePromiseFuture<Message>();

    try {
        // Check for completed receive buffers
        RDMABufferEntry* recvBuffer = nullptr;
        {
            stdx::lock_guard<Mutex> lk(_recvMutex);
            if (!_completedRecvBuffers.empty()) {
                recvBuffer = _completedRecvBuffers.front();
                _completedRecvBuffers.pop();
            }
        }

        if (recvBuffer) {
            // We have a completed buffer, process it immediately
            if (recvBuffer->error) {
                // Return buffer to free list
                {
                    stdx::lock_guard<Mutex> lk(_recvMutex);
                    recvBuffer->completed = false;
                    recvBuffer->error = false;
                    _freeRecvBuffers.push(recvBuffer);
                }

                // Post a new receive operation
                auto postStatus = postRecv(recvBuffer->data, recvBuffer->size);
                if (!postStatus.isOK()) {
                    LOGV2_WARNING(4700240,
                                  "Failed to post receive request",
                                  "error"_attr = postStatus.reason());
                }

                return Future<Message>::makeReady(
                    Status(ErrorCodes::SocketException, "RDMA receive error"));
            }

            // Parse the message from the buffer
            if (recvBuffer->size < sizeof(MSGHEADER::Value)) {
                // Return buffer to free list
                {
                    stdx::lock_guard<Mutex> lk(_recvMutex);
                    recvBuffer->completed = false;
                    recvBuffer->error = false;
                    _freeRecvBuffers.push(recvBuffer);
                }

                // Post a new receive operation
                auto postStatus = postRecv(recvBuffer->data, recvBuffer->size);
                if (!postStatus.isOK()) {
                    LOGV2_WARNING(4700241,
                                  "Failed to post receive request",
                                  "error"_attr = postStatus.reason());
                }

                return Future<Message>::makeReady(
                    Status(ErrorCodes::InvalidBSON, "Received incomplete message header"));
            }

            // Parse message header
            MSGHEADER::ConstView headerView(recvBuffer->data);
            int32_t messageLength = headerView.getMessageLength();

            if (messageLength < static_cast<int32_t>(sizeof(MSGHEADER::Value)) ||
                messageLength > static_cast<int32_t>(recvBuffer->size)) {
                // Return buffer to free list
                {
                    stdx::lock_guard<Mutex> lk(_recvMutex);
                    recvBuffer->completed = false;
                    recvBuffer->error = false;
                    _freeRecvBuffers.push(recvBuffer);
                }

                // Post a new receive operation
                auto postStatus = postRecv(recvBuffer->data, recvBuffer->size);
                if (!postStatus.isOK()) {
                    LOGV2_WARNING(4700242,
                                  "Failed to post receive request",
                                  "error"_attr = postStatus.reason());
                }

                return Future<Message>::makeReady(
                    Status(ErrorCodes::InvalidBSON,
                           str::stream() << "Invalid message length: " << messageLength));
            }

            // Create message buffer and copy data
            auto messageBuffer = SharedBuffer::allocate(messageLength);
            std::memcpy(messageBuffer.get(), recvBuffer->data, messageLength);

            // Return receive buffer to free list and post new receive
            {
                stdx::lock_guard<Mutex> lk(_recvMutex);
                recvBuffer->completed = false;
                recvBuffer->error = false;
                _freeRecvBuffers.push(recvBuffer);
            }

            // Post a new receive operation
            auto postStatus = postRecv(recvBuffer->data, recvBuffer->size);
            if (!postStatus.isOK()) {
                LOGV2_WARNING(
                    4700243, "Failed to post receive request", "error"_attr = postStatus.reason());
            }

            // Create and return the message
            Message message(std::move(messageBuffer));

            LOGV2_DEBUG(4700230,
                        3,
                        "RDMA message received successfully",
                        "messageSize"_attr = message.size());

            return Future<Message>::makeReady(std::move(message));
        }

        // No completed buffer available, set up async wait
        _readContext = std::make_shared<RDMACompletionContext>(_ioContext);
        auto promisePtr = std::make_shared<Promise<Message>>(std::move(promise));
        _readContext->setHandler(
            [this, promisePtr](const std::error_code& ec, size_t bytesTransferred) mutable {
                if (ec) {
                    LOGV2_WARNING(4700231,
                                  "RDMA receive completion error",
                                  "error"_attr = ec.message(),
                                  "bytesTransferred"_attr = bytesTransferred);

                    auto status = Status(ErrorCodes::SocketException,
                                         str::stream() << "RDMA receive failed: " << ec.message());
                    promisePtr->setError(status);
                    return;
                }

                // Retry reading from completed buffers
                auto future = asyncRDMASourceMessage(nullptr);
                std::move(future).getAsync([promisePtr](StatusWith<Message> result) mutable {
                    if (result.isOK()) {
                        promisePtr->emplaceValue(std::move(result.getValue()));
                    } else {
                        promisePtr->setError(result.getStatus());
                    }
                });
            });

        LOGV2_DEBUG(4700232, 3, "RDMA receive wait setup");

    } catch (const std::exception& e) {
        LOGV2_WARNING(4700233, "Exception in RDMA async read", "error"_attr = e.what());

        // Clean up read context
        if (_readContext) {
            _readContext->cancel();
            _readContext.reset();
        }

        return Future<Message>::makeReady(
            Status(ErrorCodes::SocketException,
                   str::stream() << "RDMA async read exception: " << e.what()));
    }

    return std::move(future);
}

HostAndPort RDMASocket::remoteAddr() const {
    if (_tcpSocket && _tcpSocket->is_open()) {
        try {
            auto endpoint = _tcpSocket->remote_endpoint();
            auto sockAddr = endpointToSockAddr(endpoint);
            return HostAndPort(sockAddr.getAddr(), sockAddr.getPort());
        } catch (const std::exception& e) {
            LOGV2_WARNING(4700260, "Failed to get remote address", "error"_attr = e.what());
        }
    }
    return HostAndPort(_remoteSockAddr.getAddr(), _remoteSockAddr.getPort());
}

HostAndPort RDMASocket::localAddr() const {
    if (_tcpSocket && _tcpSocket->is_open()) {
        try {
            auto endpoint = _tcpSocket->local_endpoint();
            auto sockAddr = endpointToSockAddr(endpoint);
            return HostAndPort(sockAddr.getAddr(), sockAddr.getPort());
        } catch (const std::exception& e) {
            LOGV2_WARNING(4700261, "Failed to get local address", "error"_attr = e.what());
        }
    }
    return HostAndPort(_localSockAddr.getAddr(), _localSockAddr.getPort());
}

const SockAddr& RDMASocket::remoteSockAddr() const {
    if (_tcpSocket && _tcpSocket->is_open()) {
        try {
            auto endpoint = _tcpSocket->remote_endpoint();
            // Update cached address (const_cast is needed here for caching)
            const_cast<RDMASocket*>(this)->_remoteSockAddr = endpointToSockAddr(endpoint);
        } catch (const std::exception& e) {
            LOGV2_WARNING(4700262, "Failed to get remote socket address", "error"_attr = e.what());
        }
    }
    return _remoteSockAddr;
}

const SockAddr& RDMASocket::localSockAddr() const {
    if (_tcpSocket && _tcpSocket->is_open()) {
        try {
            auto endpoint = _tcpSocket->local_endpoint();
            // Update cached address (const_cast is needed here for caching)
            const_cast<RDMASocket*>(this)->_localSockAddr = endpointToSockAddr(endpoint);
        } catch (const std::exception& e) {
            LOGV2_WARNING(4700263, "Failed to get local socket address", "error"_attr = e.what());
        }
    }
    return _localSockAddr;
}

bool RDMASocket::isConnected() const {
    // 检查RDMA连接状态
    if (_isRdma && _rdmaCoreConnection) {
        return _rdmaCoreConnection->isValid() && _state.load() == ConnectionState::CONNECTED;
    }
    
    // 检查TCP连接状态
    return _state.load() == ConnectionState::CONNECTED || (_tcpSocket && _tcpSocket->is_open());
}

void RDMASocket::setRDMAConnection(std::unique_ptr<RDMACoreConnection> connection) {
    if (_rdmaCoreConnection && _rdmaCoreConnection->isValid()) {
        LOGV2_WARNING(4700901, "RDMA Core connection already set, replacing");
    }
    
    _rdmaCoreConnection = std::move(connection);
    
    // 验证连接有效性
    if (_rdmaCoreConnection && _rdmaCoreConnection->isValid()) {
        
        // 优先使用READ/Write模式
        if (_useReadWriteMode) {
            auto status = initializeReadWriteMode();
            if (status.isOK()) {
                _isRdma = true;
                setState(ConnectionState::CONNECTED);
                LOGV2(4700900, "RDMA READ/Write mode initialized successfully", 
                      "cmId"_attr = reinterpret_cast<uintptr_t>(_rdmaCoreConnection->getCMId()));
                return;
            } else {
                LOGV2_WARNING(4700901, "Failed to initialize READ/Write mode, falling back to Send/Recv", 
                             "error"_attr = status);
                // 继续执行传统模式初始化
            }
        }
        
        // 传统Send/Recv模式初始化
        initializeSendRecvMode();
        
    } else {
        LOGV2_WARNING(4700902, "RDMA Core connection is not valid");
        _isRdma = false;
        setState(ConnectionState::ERROR);
    }
}

Status RDMASocket::initializeReadWriteMode() {
    try {
        // 检查必要的前置条件
        if (!_rdmaCoreConnection || !_rdmaCoreConnection->isValid()) {
            return Status(ErrorCodes::InternalError, "Invalid RDMA core connection for READ/write mode");
        }
        
        if (!_tcpSocket || !_tcpSocket->is_open()) {
            return Status(ErrorCodes::InternalError, "TCP socket required for RDMA negotiation");
        }
        
        LOGV2_DEBUG(4701405, 2, "Initializing RDMA READ/Write mode");
        
        // 解决socket类型转换问题：从generic socket重建TCP socket
        auto tcpSocket = createTCPSocketFromGeneric();
        if (!tcpSocket) {
            return Status(ErrorCodes::InternalError, "Failed to create TCP socket from generic socket");
        }
        
        // 创建协商管理器
        _negotiationManager = std::make_unique<RDMANegotiationManager>(*tcpSocket);
        if (!_negotiationManager) {
            return Status(ErrorCodes::InternalError, "Failed to create RDMA negotiation manager");
        }
        
        // 创建READ/Write socket
        _readWriteSocket = std::make_unique<RDMASocketReadWrite>(_ioContext);
        
        // 创建连接的共享引用而不是移动
        // 由于RDMACoreConnection不支持拷贝，我们使用引用共享的方式
        auto connectionForReadWrite = createSharedConnection();
        if (!connectionForReadWrite || !connectionForReadWrite->isValid()) {
            return Status(ErrorCodes::InternalError, "Failed to create shared connection for read/write mode");
        }
        
        // 初始化READ/Write socket
        auto status = _readWriteSocket->initialize(std::move(connectionForReadWrite), std::move(_negotiationManager));
        if (!status.isOK()) {
            _readWriteSocket.reset();
            _negotiationManager.reset();
            // 清理TCP socket
            if (tcpSocket) {
                asio::error_code ec;
                tcpSocket->close(ec);
                tcpSocket.reset();
            }
            return status;
        }
        
        // 保存TCP socket引用以备后续使用
        _tcpSocketForNegotiation = std::move(tcpSocket);
        
        LOGV2(4700910, "RDMA READ/Write mode initialized successfully");
        return Status::OK();
        
    } catch (const std::exception& e) {
        _readWriteSocket.reset();
        _negotiationManager.reset();
        return Status(ErrorCodes::InternalError, 
                     str::stream() << "Exception initializing READ/write mode: " << e.what());
    }
}

void RDMASocket::initializeSendRecvMode() {
    // 从RDMACoreConnection获取RDMA资源
    _qp = _rdmaCoreConnection->getQP();
    _sendCQ = _rdmaCoreConnection->getSendCQ();
    _recvCQ = _rdmaCoreConnection->getRecvCQ();
    _compChannel = RDMAManager::get().getCompChannel();
    
    LOGV2_DEBUG(4700904, 1, "RDMA resources obtained from connection", 
                "qp"_attr = reinterpret_cast<uintptr_t>(_qp),
                "sendCQ"_attr = reinterpret_cast<uintptr_t>(_sendCQ),
                "recvCQ"_attr = reinterpret_cast<uintptr_t>(_recvCQ));
    
    // 分配缓冲区（使用QP关联的PD）
    auto status = allocateBuffers(_qp);
    if (!status.isOK()) {
        LOGV2_ERROR(4700905, "Failed to allocate RDMA buffers", "error"_attr = status.reason());
        _isRdma = false;
        setState(ConnectionState::ERROR);
        return;
    }
    
    LOGV2_DEBUG(4700906, 1, "RDMA buffers allocated successfully", 
                "sendBuffers"_attr = _sendBuffers.size(),
                "recvBuffers"_attr = _recvBuffers.size());
    
    // 投递初始接收请求
    status = postInitialRecvs();
    if (!status.isOK()) {
        LOGV2_ERROR(4700907, "Failed to post initial receives", "error"_attr = status.reason());
        _isRdma = false;
        setState(ConnectionState::ERROR);
        return;
    }
    
    LOGV2_DEBUG(4700908, 1, "Initial receive requests posted successfully");
    
    // 启动轮询线程
    startPolling();
    
    LOGV2_DEBUG(4700909, 1, "RDMA polling thread started");
    
    _isRdma = true;
    setState(ConnectionState::CONNECTED);
    LOGV2_WARNING(4700911, "RDMA Send/Recv mode initialized (READ/Write mode is recommended)", 
                 "cmId"_attr = reinterpret_cast<uintptr_t>(_rdmaCoreConnection->getCMId()),
                 "qp"_attr = reinterpret_cast<uintptr_t>(_qp));
}

void RDMASocket::setAddresses(const HostAndPort& local, const HostAndPort& remote) {
    _localSockAddr = SockAddr(StringData(local.host()), local.port(), AF_UNSPEC);
    _remoteSockAddr = SockAddr(StringData(remote.host()), remote.port(), AF_UNSPEC);
    
    LOGV2_DEBUG(4700903, 1, "RDMA Socket addresses set", 
                "local"_attr = local.toString(),
                "remote"_attr = remote.toString());
}

std::unique_ptr<asio::ip::tcp::socket> RDMASocket::createTCPSocketFromGeneric() {
    try {
        if (!_tcpSocket || !_tcpSocket->is_open()) {
            LOGV2_WARNING(4701406, "Generic socket is not open, cannot create TCP socket");
            return nullptr;
        }
        
        // 获取原始socket描述符
        auto nativeHandle = _tcpSocket->native_handle();
        
        // 创建新的TCP socket
        auto tcpSocket = std::make_unique<asio::ip::tcp::socket>(_ioContext);
        
        // 从原始句柄分配新的socket
        asio::error_code ec;
        tcpSocket->assign(asio::ip::tcp::v4(), nativeHandle, ec);
        if (ec) {
            LOGV2_WARNING(4701407, "Failed to assign native handle to TCP socket", 
                         "error"_attr = ec.message());
            return nullptr;
        }
        
        // 获取分配后的端点信息用于日志
        auto localEndpoint = tcpSocket->local_endpoint(ec);
        auto remoteEndpoint = tcpSocket->remote_endpoint(ec);
        
        LOGV2_DEBUG(4701408, 2, "Successfully created TCP socket from generic socket", 
                    "localEndpoint"_attr = (ec ? "unknown" : localEndpoint.address().to_string()),
                    "remoteEndpoint"_attr = (ec ? "unknown" : remoteEndpoint.address().to_string()));
        
        return tcpSocket;
        
    } catch (const std::exception& e) {
        LOGV2_WARNING(4701409, "Exception creating TCP socket from generic socket", 
                     "error"_attr = e.what());
        return nullptr;
    }
}

std::unique_ptr<RDMACoreConnection> RDMASocket::createSharedConnection() {
    try {
        if (!_rdmaCoreConnection || !_rdmaCoreConnection->isValid()) {
            LOGV2_WARNING(4701410, "Original RDMA connection is not valid");
            return nullptr;
        }
        
        // 由于RDMACoreConnection不支持拷贝构造，我们需要创建一个新的连接实例
        // 这里我们共享同一个 rdma_cm_id，但创建一个新的包装对象
        struct rdma_cm_id* sharedCmId = _rdmaCoreConnection->getCMId();
        if (!sharedCmId) {
            LOGV2_WARNING(4701411, "Cannot get CM ID from RDMA connection");
            return nullptr;
        }
        
        // 创建新的连接对象，共享同一个CM ID
        auto sharedConnection = std::make_unique<RDMACoreConnection>(sharedCmId);
        if (!sharedConnection->isValid()) {
            LOGV2_WARNING(4701412, "Created shared connection is not valid");
            return nullptr;
        }
        
        LOGV2_DEBUG(4701413, 2, "Successfully created shared RDMA connection", 
                    "cmId"_attr = reinterpret_cast<uintptr_t>(sharedCmId));
        
        return sharedConnection;
        
    } catch (const std::exception& e) {
        LOGV2_WARNING(4701414, "Exception creating shared RDMA connection", 
                     "error"_attr = e.what());
        return nullptr;
    }
}

}  // namespace rdma
}  // namespace transport
}  // namespace mongo
