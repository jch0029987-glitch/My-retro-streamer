#include <jni.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>

#define MAX_UDP_PACKET_SIZE 1200
#define TARGET_WIDTH 640
#define TARGET_HEIGHT 480
#define EXPECTED_FRAME_SIZE (TARGET_WIDTH * TARGET_HEIGHT * 2) // RGB565
#define TOTAL_CHUNKS 205

static int g_sockfd = -1;
static volatile int g_receiver_running = 0;
static pthread_t g_receiver_thread;

static uint8_t g_frame_buffer[EXPECTED_FRAME_SIZE];
static uint64_t g_chunks_received_bitmap[4] = {0};
static uint32_t g_active_sequence = 0;
static uint64_t g_packets_received_count = 0;
static uint64_t g_frames_rendered_count = 0;

static void reset_chunk_mask(void) {
    memset(g_chunks_received_bitmap, 0, sizeof(g_chunks_received_bitmap));
}

static void mark_chunk_received(uint16_t idx) {
    if (idx >= TOTAL_CHUNKS) return;
    int word_idx = idx / 64;
    int bit_idx = idx % 64;
    g_chunks_received_bitmap[word_idx] |= (1ULL << bit_idx);
}

static int is_frame_complete(void) {
    if (g_chunks_received_bitmap[0] != 0xFFFFFFFFFFFFFFFFULL) return 0;
    if (g_chunks_received_bitmap[1] != 0xFFFFFFFFFFFFFFFFULL) return 0;
    if (g_chunks_received_bitmap[2] != 0xFFFFFFFFFFFFFFFFULL) return 0;
    uint64_t expected_word3_mask = (1ULL << 13) - 1;
    if ((g_chunks_received_bitmap[3] & expected_word3_mask) != expected_word3_mask) return 0;
    return 1;
}

static void* receiver_thread_func(void* arg) {
    (void)arg;
    uint8_t packet_buffer[MAX_UDP_PACKET_SIZE + 8];

    while (g_receiver_running) {
        ssize_t n = recv(g_sockfd, packet_buffer, sizeof(packet_buffer), 0);
        if (n < 8) continue;

        g_packets_received_count++;

        uint32_t seq;
        uint16_t chunk_idx;
        
        // Extract sequence and chunk index (Big Endian network byte order)
        memcpy(&seq, packet_buffer, 4);
        seq = __builtin_bswap32(seq);

        memcpy(&chunk_idx, packet_buffer + 4, 2);
        chunk_idx = __builtin_bswap16(chunk_idx);

        size_t payload_len = n - 8;
        if (payload_len <= 0 || chunk_idx >= TOTAL_CHUNKS) continue;

        if (seq != g_active_sequence) {
            g_active_sequence = seq;
            reset_chunk_mask();
        }

        size_t offset = (size_t)chunk_idx * MAX_UDP_PACKET_SIZE;
        if (offset + payload_len <= EXPECTED_FRAME_SIZE) {
            memcpy(g_frame_buffer + offset, packet_buffer + 8, payload_len);
            mark_chunk_received(chunk_idx);

            if (is_frame_complete()) {
                g_frames_rendered_count++;
                reset_chunk_mask();
            }
        }
    }
    return NULL;
}

JNIEXPORT jbyte JNICALL Java_com_jeremy_retroclient_NativeReceiver_startReceiver(JNIEnv* env, jobject thiz, jint port) {
    (void)env; (void)thiz;
    if (g_receiver_running) return 1;

    g_sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sockfd < 0) return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = __builtin_bswap16((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(g_sockfd);
        g_sockfd = -1;
        return 0;
    }

    g_receiver_running = 1;
    if (pthread_create(&g_receiver_thread, NULL, receiver_thread_func, NULL) != 0) {
        g_receiver_running = 0;
        close(g_sockfd);
        g_sockfd = -1;
        return 0;
    }

    return 1;
}

JNIEXPORT void JNICALL Java_com_jeremy_retroclient_NativeReceiver_stopReceiver(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    g_receiver_running = 0;
    if (g_sockfd >= 0) {
        close(g_sockfd);
        g_sockfd = -1;
    }
    pthread_join(g_receiver_thread, NULL);
}
