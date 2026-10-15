#include "stream_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <android/log.h>

#define LOG_TAG "StreamServerC"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct StreamServer {
    int sockfd;
    int port;
    bool running;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len;
    bool client_connected;
};

StreamServer* stream_server_create(const StreamServerConfig* config) {
    StreamServer* server = (StreamServer*)malloc(sizeof(StreamServer));
    if (!server) return NULL;

    server->port = config ? config->port : 9999;
    server->running = false;
    server->sockfd = -1;
    server->client_addr_len = sizeof(server->client_addr);
    server->client_connected = false;

    // Create UDP socket for low-latency game streaming packets
    server->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server->sockfd < 0) {
        LOGE("Failed to create socket: %s", strerror(errno));
        free(server);
        return NULL;
    }

    // Allow socket reuse
    int opt = 1;
    setsockopt(server->sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(server->port);

    if (bind(server->sockfd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        LOGE("Failed to bind socket to port %d: %s", server->port, strerror(errno));
        close(server->sockfd);
        free(server);
        return NULL;
    }

    LOGI("Stream server initialized successfully on port %d", server->port);
    return server;
}

int stream_server_worker(StreamServer* server) {
    if (!server || server->sockfd < 0) return -1;

    server->running = true;
    LOGI("Stream server worker started, listening for packets...");

    uint8_t buffer[2048];

    while (server->running) {
        // Non-blocking or timeout-based select/recvfrom loop can go here
        ssize_t bytes_received = recvfrom(
            server->sockfd,
            buffer,
            sizeof(buffer),
            0,
            (struct sockaddr*)&server->client_addr,
            &server->client_addr_len
        );

        if (bytes_received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (!server->running) break;
            LOGE("Error receiving data: %s", strerror(errno));
            continue;
        }

        if (!server->client_connected) {
            server->client_connected = true;
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(server->client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
            LOGI("New client connected from %s:%d", client_ip, ntohs(server->client_addr.sin_port));
        }

        // TODO: Parse incoming control packet (button presses, joystick axes)
        // process_control_packet(buffer, bytes_received);
    }

    LOGI("Stream server worker stopped.");
    return 0;
}

int stream_server_send_video_frame(StreamServer* server, const uint8_t* data, size_t size, int64_t pts) {
    if (!server || !server->client_connected || server->sockfd < 0) {
        return -1; // No client linked yet
    }

    ssize_t sent = sendto(
        server->sockfd,
        data,
        size,
        0,
        (struct sockaddr*)&server->client_addr,
        server->client_addr_len
    );

    if (sent < 0) {
        LOGE("Failed to send video frame packet: %s", strerror(errno));
        return -1;
    }

    return 0;
}

void stream_server_destroy(StreamServer* server) {
    if (!server) return;
    server->running = false;
    if (server->sockfd >= 0) {
        close(server->sockfd);
    }
    free(server);
    LOGI("Stream server destroyed.");
}
