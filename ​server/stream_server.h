#ifndef STREAM_SERVER_H
#define STREAM_SERVER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int port;
} StreamServerConfig;

typedef struct StreamServer StreamServer;

StreamServer* stream_server_create(const StreamServerConfig* config);
int stream_server_worker(StreamServer* server);
int stream_server_send_video_frame(StreamServer* server, const uint8_t* data, size_t size, int64_t pts);
void stream_server_destroy(StreamServer* server);

#ifdef __cplusplus
}
#endif

#endif // STREAM_SERVER_H
