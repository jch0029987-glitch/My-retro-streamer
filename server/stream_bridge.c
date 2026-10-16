#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>

#define LOG_TAG "RetroStreamNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT 3
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY 9
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY 31

#define MAX_UDP_PACKET_SIZE 1200

static bool g_server_running = false;
static void* g_core_handle = NULL;
static JavaVM* g_vm = NULL;

// UDP Streaming State (Wi-Fi Direct Target)
static int g_udp_socket = -1;
static struct sockaddr_in g_client_addr;
static bool g_has_client = false;
static uint32_t g_frame_sequence = 0;

// Emulation Thread State
static bool g_emulation_running = false;
static pthread_t g_emulation_thread;

jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

enum retro_pixel_format {
    RETRO_PIXEL_FORMAT_0RGB1555 = 0,
    RETRO_PIXEL_FORMAT_XRGB8888 = 1,
    RETRO_PIXEL_FORMAT_RGB565   = 2,
    RETRO_PIXEL_FORMAT_UNKNOWN  = 3
};

struct retro_game_info {
    const char *path;
    const void *data;
    size_t size;
    const char *meta;
};

// Libretro callback typedefs
typedef void (*retro_video_refresh_t)(const void *data, unsigned width, unsigned height, size_t pitch);
typedef bool (*retro_environment_t)(unsigned cmd, void *data);
typedef void (*retro_input_poll_t)(void);
typedef int16_t (*retro_input_state_t)(unsigned port, unsigned device, unsigned index, unsigned id);
typedef void (*retro_audio_sample_t)(int16_t left, int16_t right);
typedef size_t (*retro_audio_sample_batch_t)(const int16_t *data, size_t frames);

// Dummy stubs to prevent null pointer dereferences on core ticks
static void dummy_input_poll(void) {}
static int16_t dummy_input_state(unsigned port, unsigned device, unsigned index, unsigned id) { return 0; }
static void dummy_audio_sample(int16_t left, int16_t right) {}
static size_t dummy_audio_sample_batch(const int16_t *data, size_t frames) { return frames; }

static void video_refresh_callback(const void *data, unsigned width, unsigned height, size_t pitch) {
    if (!data || !g_has_client || g_udp_socket < 0 || width == 0 || height == 0) return;

    size_t data_size = pitch * height;
    const uint8_t *pixel_ptr = (const uint8_t *)data;

    uint32_t total_chunks = (data_size + MAX_UDP_PACKET_SIZE - 1) / MAX_UDP_PACKET_SIZE;
    g_frame_sequence++;

    for (uint16_t i = 0; i < total_chunks; i++) {
        size_t offset = (size_t)i * MAX_UDP_PACKET_SIZE;
        if (offset >= data_size) break;

        size_t chunk_size = (data_size - offset > MAX_UDP_PACKET_SIZE) ? MAX_UDP_PACKET_SIZE : (data_size - offset);

        uint8_t buffer[MAX_UDP_PACKET_SIZE + 8];

        uint32_t net_seq = htonl(g_frame_sequence);
        uint16_t net_idx = htons(i);
        uint16_t net_tot = htons((uint16_t)total_chunks);

        memcpy(buffer, &net_seq, 4);
        memcpy(buffer + 4, &net_idx, 2);
        memcpy(buffer + 6, &net_tot, 2);
        memcpy(buffer + 8, pixel_ptr + offset, chunk_size);

        ssize_t sent = sendto(g_udp_socket, buffer, chunk_size + 8, 0,
                              (struct sockaddr *)&g_client_addr, sizeof(g_client_addr));
        if (sent < 0) {
            if (g_frame_sequence % 300 == 0) {
                LOGE("sendto failed for frame seq %u chunk %u", g_frame_sequence, i);
            }
        }

        // Micro-pacing delay to prevent choking the Wi-Fi Direct interface buffer
        usleep(30);
    }
}

static bool environmental_callback(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            if (data != NULL) {
                enum retro_pixel_format *fmt = (enum retro_pixel_format *)data;
                *fmt = RETRO_PIXEL_FORMAT_RGB565;
            }
            return true;

        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
            if (data != NULL) {
                const char **dir = (const char **)data;
                *dir = "/data/user/0/com.jeremy.retro/files";
            }
            return true;
        }

        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
            if (data != NULL) {
                const char **dir = (const char **)data;
                *dir = "/data/user/0/com.jeremy.retro/files/saves";
            }
            return true;
        }

        default:
            return false;
    }
}

// Libretro function pointers
static void (*libretro_init)(void) = NULL;
static void (*libretro_deinit)(void) = NULL;
static bool (*libretro_set_environment)(retro_environment_t) = NULL;
static bool (*libretro_set_video_refresh)(retro_video_refresh_t) = NULL;
static void (*libretro_set_input_poll)(retro_input_poll_t) = NULL;
static void (*libretro_set_input_state)(retro_input_state_t) = NULL;
static void (*libretro_set_audio_sample)(retro_audio_sample_t) = NULL;
static void (*libretro_set_audio_sample_batch)(retro_audio_sample_batch_t) = NULL;
static bool (*libretro_load_game)(const struct retro_game_info *info) = NULL;
static void (*libretro_run)(void) = NULL;

JNIEXPORT void JNICALL
Java_com_jeremy_retro_NativeStreamServer_setUdpTarget(JNIEnv *env, jobject thiz, jstring clientIp, jint port) {
    const char *ip = (*env)->GetStringUTFChars(env, clientIp, NULL);
    if (!ip) return;

    if (g_udp_socket >= 0) {
        close(g_udp_socket);
        g_udp_socket = -1;
    }

    g_udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_socket < 0) {
        LOGE("Failed to create Wi-Fi Direct UDP socket");
        (*env)->ReleaseStringUTFChars(env, clientIp, ip);
        return;
    }

    int sndbuf = 1024 * 1024;
    setsockopt(g_udp_socket, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    memset(&g_client_addr, 0, sizeof(g_client_addr));
    g_client_addr.sin_family = AF_INET;
    g_client_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &g_client_addr.sin_addr) <= 0) {
        LOGE("Invalid Wi-Fi Direct IP format: %s", ip);
        g_has_client = false;
    } else {
        g_has_client = true;
        LOGI("Wi-Fi Direct UDP stream target successfully bound to %s:%d with enlarged send buffer", ip, (int)port);
    }

    (*env)->ReleaseStringUTFChars(env, clientIp, ip);
}

JNIEXPORT jboolean JNICALL
Java_com_jeremy_retro_NativeStreamServer_startNativeServer(JNIEnv *env, jobject thiz, jint port) {
    g_server_running = true;
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_jeremy_retro_NativeStreamServer_stopNativeServer(JNIEnv *env, jobject thiz) {
    g_emulation_running = false;
    if (libretro_deinit) libretro_deinit();
    if (g_udp_socket >= 0) {
        close(g_udp_socket);
        g_udp_socket = -1;
    }
    g_has_client = false;
    if (g_core_handle) {
        dlclose(g_core_handle);
        g_core_handle = NULL;
    }
    g_server_running = false;
}

JNIEXPORT jboolean JNICALL
Java_com_jeremy_retro_NativeStreamServer_isNativeServerRunning(JNIEnv *env, jobject thiz) {
    return g_server_running ? JNI_TRUE : JNI_FALSE;
}

static void* emulation_thread_func(void *arg) {
    LOGI("Emulation thread loop starting...");
    while (g_emulation_running && libretro_run) {
        libretro_run();
    }
    LOGI("Emulation thread loop exited.");
    return NULL;
}

JNIEXPORT jboolean JNICALL
Java_com_jeremy_retro_NativeStreamServer_loadAndRunCore(JNIEnv *env, jobject thiz, jstring corePath, jstring romPath) {
    const char *c_core_path = (*env)->GetStringUTFChars(env, corePath, NULL);
    const char *c_rom_path = (*env)->GetStringUTFChars(env, romPath, NULL);

    g_core_handle = dlopen(c_core_path, RTLD_LAZY | RTLD_GLOBAL);
    if (!g_core_handle) {
        LOGE("dlopen failed: %s", dlerror());
        (*env)->ReleaseStringUTFChars(env, corePath, c_core_path);
        (*env)->ReleaseStringUTFChars(env, romPath, c_rom_path);
        return JNI_FALSE;
    }

    libretro_init = dlsym(g_core_handle, "retro_init");
    libretro_deinit = dlsym(g_core_handle, "retro_deinit");
    libretro_set_environment = dlsym(g_core_handle, "retro_set_environment");
    libretro_set_video_refresh = dlsym(g_core_handle, "retro_set_video_refresh");
    libretro_set_input_poll = dlsym(g_core_handle, "retro_set_input_poll");
    libretro_set_input_state = dlsym(g_core_handle, "retro_set_input_state");
    libretro_set_audio_sample = dlsym(g_core_handle, "retro_set_audio_sample");
    libretro_set_audio_sample_batch = dlsym(g_core_handle, "retro_set_audio_sample_batch");
    libretro_load_game = dlsym(g_core_handle, "retro_load_game");
    libretro_run = dlsym(g_core_handle, "retro_run");

    if (!libretro_init || !libretro_run || !libretro_load_game || !libretro_set_environment) {
        LOGE("Missing core symbols.");
        dlclose(g_core_handle);
        g_core_handle = NULL;
        return JNI_FALSE;
    }

    libretro_set_environment(environmental_callback);
    if (libretro_set_video_refresh) libretro_set_video_refresh(video_refresh_callback);
    if (libretro_set_input_poll) libretro_set_input_poll(dummy_input_poll);
    if (libretro_set_input_state) libretro_set_input_state(dummy_input_state);
    if (libretro_set_audio_sample) libretro_set_audio_sample(dummy_audio_sample);
    if (libretro_set_audio_sample_batch) libretro_set_audio_sample_batch(dummy_audio_sample_batch);

    libretro_init();

    struct retro_game_info info = {0};
    info.path = c_rom_path;

    if (!libretro_load_game(&info)) {
        LOGE("retro_load_game failed.");
        libretro_deinit();
        dlclose(g_core_handle);
        g_core_handle = NULL;
        return JNI_FALSE;
    }

    g_emulation_running = true;
    if (pthread_create(&g_emulation_thread, NULL, emulation_thread_func, NULL) != 0) {
        LOGE("pthread_create failed.");
        g_emulation_running = false;
        libretro_deinit();
        dlclose(g_core_handle);
        g_core_handle = NULL;
        return JNI_FALSE;
    }

    LOGI("loadAndRunCore completed successfully, Wi-Fi Direct streaming ready.");

    (*env)->ReleaseStringUTFChars(env, corePath, c_core_path);
    (*env)->ReleaseStringUTFChars(env, romPath, c_rom_path);
    return JNI_TRUE;
}
