#include "stream_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <android/log.h>

#define LOG_TAG "StreamServer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct StreamServer {
    int socket_fd;
    int port;
    bool is_running;
};

StreamServer* stream_server_create(const StreamServerConfig* config) {
    if (!config) return NULL;

    StreamServer* server = (StreamServer*)calloc(1, sizeof(StreamServer));
    if (!server) return NULL;

    server->port = config->port;
    server->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server->socket_fd < 0) {
        LOGE("Failed to create UDP socket");
        free(server);
        return NULL;
    }

    // Set non-blocking socket
    int flags = fcntl(server->socket_fd, F_GETFL, 0);
    fcntl(server->socket_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(server->port);

    if (bind(server->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOGE("Failed to bind UDP socket on port %d", server->port);
        close(server->socket_fd);
        free(server);
        return NULL;
    }

    server->is_running = true;
    LOGI("Stream server created and bound to port %d", server->port);
    return server;
}

int stream_server_worker(StreamServer* server) {
    if (!server || !server->is_running) return -1;

    uint8_t buffer[2048];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    ssize_t bytes_received = recvfrom(server->socket_fd, buffer, sizeof(buffer), 0,
                                      (struct sockaddr*)&client_addr, &addr_len);

    if (bytes_received > 0) {
        // Non-blocking worker tick handled incoming packet
        LOGI("Received %zd bytes", bytes_received);
    }

    return 0;
}

int stream_server_send_video_frame(StreamServer* server, const uint8_t* data, size_t size, int64_t pts) {
    if (!server || !data || size == 0) return -1;
    // Frame transmission payload handling logic goes here
    return 0;
}

void stream_server_destroy(StreamServer* server) {
    if (!server) return;
    server->is_running = false;
    if (server->socket_fd >= 0) {
        close(server->socket_fd);
    }
    free(server);
    LOGI("Stream server destroyed");
}
