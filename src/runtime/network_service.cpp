#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "runtime/network_service.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mutex>
#include <iostream>

#pragma comment(lib, "ws2_32.lib")

namespace miami {

class NetworkService : public INetworkService {
public:
    NetworkService() = default;
    ~NetworkService() override { Shutdown(); }

    const char* GetServiceName() const override { return "NetworkService"; }

    bool Initialize() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            m_status = NetworkStatus::Error;
            return false;
        }
        m_operational = true;
        m_status = NetworkStatus::Disconnected;
        return true;
    }

    void Shutdown() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_socket != INVALID_SOCKET) {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
        }
        if (m_operational) {
            WSACleanup();
            m_operational = false;
        }
        m_status = NetworkStatus::Disconnected;
    }

    bool IsOperational() const override { return m_operational; }

    std::string GetStatusDetails() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string statusStr = "Disconnected";
        if (m_status == NetworkStatus::LoopbackOnly) statusStr = "Loopback (127.0.0.1:" + std::to_string(m_port) + ")";
        else if (m_status == NetworkStatus::Connected) statusStr = "Connected";
        return "Network Status: " + statusStr;
    }

    NetworkStatus GetNetworkStatus() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_status;
    }

    bool InitializeLoopback(uint16_t port) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_operational) return false;

        if (m_socket != INVALID_SOCKET) {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
        }

        m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_socket == INVALID_SOCKET) return false;

        // Set non-blocking mode
        u_long mode = 1;
        ioctlsocket(m_socket, FIONBIO, &mode);

        sockaddr_in addr;
        ZeroMemory(&addr, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        if (bind(m_socket, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
            return false;
        }

        m_port = port;
        m_status = NetworkStatus::LoopbackOnly;
        return true;
    }

    bool SendLoopback(const void* data, size_t size) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_socket == INVALID_SOCKET || !data || size == 0) return false;

        sockaddr_in dest;
        ZeroMemory(&dest, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(m_port);
        dest.sin_addr.s_addr = inet_addr("127.0.0.1");

        int bytesSent = sendto(m_socket, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
                               (SOCKADDR*)&dest, sizeof(dest));
        return (bytesSent > 0);
    }

    size_t ReceiveLoopback(void* buffer, size_t bufferSize) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_socket == INVALID_SOCKET || !buffer || bufferSize == 0) return 0;

        sockaddr_in src;
        int srcLen = sizeof(src);
        int bytesRecv = recvfrom(m_socket, reinterpret_cast<char*>(buffer), static_cast<int>(bufferSize), 0,
                                 (SOCKADDR*)&src, &srcLen);
        if (bytesRecv > 0) return static_cast<size_t>(bytesRecv);
        return 0;
    }

    std::string GetLocalIpAddress() const override {
        return "127.0.0.1";
    }

private:
    mutable std::mutex m_mutex;
    bool m_operational = false;
    NetworkStatus m_status = NetworkStatus::Disconnected;
    SOCKET m_socket = INVALID_SOCKET;
    uint16_t m_port = 0;
};

std::unique_ptr<INetworkService> CreateNetworkService() {
    return std::make_unique<NetworkService>();
}

} // namespace miami
