#include "Network.h"
#include <spdlog/spdlog.h>
#include <cstring>

// SDL3_net resolves hostnames asynchronously; bound the waits so a dead DNS or a
// refused port cannot wedge the caller or the receive thread.
static constexpr Sint32 kResolveTimeoutMs = 5000;
static constexpr Sint32 kConnectTimeoutMs = 1000;

// MARK: - Public

void Network::Enable(const char* host, uint16_t port) {
    if (isEnabled) {
        return;
    }

    if (!NET_Init()) {
        SPDLOG_ERROR("[Network] NET_Init: {}", SDL_GetError());
        return;
    }

    networkAddress = NET_ResolveHostname(host);
    if (networkAddress == nullptr || NET_WaitUntilResolved(networkAddress, kResolveTimeoutMs) != NET_SUCCESS) {
        SPDLOG_ERROR("[Network] could not resolve {}: {}", host, SDL_GetError());
        NET_UnrefAddress(networkAddress);
        networkAddress = nullptr;
        NET_Quit();
        return;
    }

    // SDL3_net takes the port when the client is created, not when the host resolves.
    networkPort = port;
    isEnabled = true;

    // First check if there is a thread running, if so, join it
    if (receiveThread.joinable()) {
        receiveThread.join();
    }

    receiveThread = std::thread(&Network::ReceiveFromServer, this);
}

void Network::Disable() {
    if (!isEnabled) {
        return;
    }

    isEnabled = false;
    receiveThread.join();

    NET_UnrefAddress(networkAddress);
    networkAddress = nullptr;
    NET_Quit();
}

void Network::OnIncomingData(char payload[512]) {
}

void Network::OnIncomingJson(nlohmann::json payload) {
}

void Network::OnConnected() {
}

void Network::OnDisconnected() {
}

void Network::ProcessOutgoingPackets() {
}

void Network::SendDataToRemote(const char* payload) {
    SPDLOG_DEBUG("[Network] Sending data: {}", payload);
    if (!NET_WriteToStreamSocket(networkSocket, payload, static_cast<int>(strlen(payload) + 1))) {
        SPDLOG_ERROR("[Network] NET_WriteToStreamSocket: {}", SDL_GetError());
    }
}

void Network::SendJsonToRemote(nlohmann::json payload) {
    SendDataToRemote(payload.dump().c_str());
}

// MARK: - Private

void Network::ReceiveFromServer() {
    while (isEnabled) {
        while (!isConnected && isEnabled) {
            SPDLOG_TRACE("[Network] Attempting to make connection to server...");
            networkSocket = NET_CreateClient(networkAddress, networkPort, 0);
            if (networkSocket != nullptr && NET_WaitUntilConnected(networkSocket, kConnectTimeoutMs) == NET_SUCCESS) {
                isConnected = true;
                receivedData.clear();
                SPDLOG_INFO("[Network] Connection to server established!");

                OnConnected();
                break;
            }

            NET_DestroyStreamSocket(networkSocket);
            networkSocket = nullptr;
        }

        // Listen to socket messages
        while (isConnected && networkSocket && isEnabled) {
            // we check first if socket has data, to not block in the read
            void* sockets[1] = { networkSocket };
            int socketsReady = NET_WaitUntilInputAvailable(sockets, 1, 0);

            if (socketsReady == -1) {
                SPDLOG_ERROR("[Network] NET_WaitUntilInputAvailable: {}", SDL_GetError());
                break;
            }

            // Always process outgoing packets
            ProcessOutgoingPackets();

            if (socketsReady == 0) {
                // No incoming data
                continue;
            }

            char remoteDataReceived[512];
            memset(remoteDataReceived, 0, sizeof(remoteDataReceived));
            int len = NET_ReadFromStreamSocket(networkSocket, remoteDataReceived, sizeof(remoteDataReceived));
            // Unlike SDL2_net, 0 means "nothing available right now" rather than a closed
            // connection; only a negative result is a real failure.
            if (len < 0) {
                SPDLOG_ERROR("[Network] NET_ReadFromStreamSocket: {}", SDL_GetError());
                break;
            }
            if (len == 0) {
                continue;
            }

            HandleRemoteData(remoteDataReceived);

            receivedData.append(remoteDataReceived, len);

            // Proess all complete packets
            size_t delimiterPos = receivedData.find('\0');
            while (delimiterPos != std::string::npos) {
                // Extract the complete packet until the delimiter
                std::string packet = receivedData.substr(0, delimiterPos);
                // Remove the packet (including the delimiter) from the received data
                receivedData.erase(0, delimiterPos + 1);
                HandleRemoteJson(packet);
                // Find the next delimiter
                delimiterPos = receivedData.find('\0');
            }
        }

        if (isConnected) {
            NET_DestroyStreamSocket(networkSocket);
            networkSocket = nullptr;
            isConnected = false;
            receivedData.clear();
            OnDisconnected();
            SPDLOG_INFO("[Network] Ending receiving thread...");
        }
    }
}

void Network::HandleRemoteData(char payload[512]) {
    OnIncomingData(payload);
}

void Network::HandleRemoteJson(std::string payload) {
    SPDLOG_DEBUG("[Network] Received json: {}", payload);
    nlohmann::json jsonPayload;
    try {
        jsonPayload = nlohmann::json::parse(payload);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[Network] Failed to parse json: \n{}\n{}\n", payload, e.what());
        return;
    }

    try {
        OnIncomingJson(jsonPayload);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[Network] Exception handling incoming JSON: {}", e.what());
    } catch (...) { SPDLOG_ERROR("[Network] Unknown exception handling incoming JSON"); }
}
