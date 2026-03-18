#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <google/protobuf/message.h>

// MSG_NOSIGNAL no existe en macOS — usar SO_NOSIGPIPE en el socket en su lugar
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// Resultado de recv_message
struct RecvResult {
    bool        ok;
    uint8_t     type;
    std::string payload;
};

// Lee exactamente `len` bytes del socket. Devuelve false si hay error o EOF.
inline bool recv_exact(int sockfd, char* buf, size_t len) {
    size_t received = 0;
    while (received < len) {
        ssize_t n = recv(sockfd, buf + received, len - received, 0);
        if (n <= 0) return false;
        received += static_cast<size_t>(n);
    }
    return true;
}

// Envía un mensaje protobuf con framing:
//   [tipo (1B)] [longitud (4B big-endian)] [payload (N bytes)]
inline bool send_message(int sockfd, uint8_t type, const google::protobuf::Message& msg) {
    std::string payload;
    if (!msg.SerializeToString(&payload)) return false;

    uint32_t length_net = htonl(static_cast<uint32_t>(payload.size()));
    char header[5];
    header[0] = static_cast<char>(type);
    memcpy(header + 1, &length_net, 4);

    // Envía el header
    if (send(sockfd, header, 5, MSG_NOSIGNAL) != 5) return false;

    // Envía el payload
    size_t sent = 0;
    while (sent < payload.size()) {
        ssize_t n = send(sockfd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Envía bytes ya serializados directamente (útil para broadcasts: serializar una vez, enviar N veces).
inline bool send_raw(int sockfd, uint8_t type, const std::string& payload) {
    uint32_t length_net = htonl(static_cast<uint32_t>(payload.size()));
    char header[5];
    header[0] = static_cast<char>(type);
    memcpy(header + 1, &length_net, 4);

    if (send(sockfd, header, 5, MSG_NOSIGNAL) != 5) return false;

    size_t sent = 0;
    while (sent < payload.size()) {
        ssize_t n = send(sockfd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Recibe un mensaje con framing. Devuelve {false, 0, ""} si hay error o desconexión.
inline RecvResult recv_message(int sockfd) {
    char header[5];
    if (!recv_exact(sockfd, header, 5)) return {false, 0, ""};

    uint8_t  type = static_cast<uint8_t>(header[0]);
    uint32_t length_net;
    memcpy(&length_net, header + 1, 4);
    uint32_t length = ntohl(length_net);

    std::string payload(length, '\0');
    if (length > 0) {
        if (!recv_exact(sockfd, &payload[0], length)) return {false, 0, ""};
    }
    return {true, type, std::move(payload)};
}
