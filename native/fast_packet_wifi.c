#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <sched.h>
#define cpu_yield() sched_yield()

// Atomic packet counter for ultra-fast thread-safe progress reporting
#if defined(__GNUC__) || defined(__clang__)
    #define ATOMIC_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC_READ(ptr) __sync_fetch_and_add(ptr, 0)
#endif

static int64_t g_total_sent = 0;

static void sleep_ms(unsigned int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;

    nanosleep(&ts, NULL);
}

typedef struct {
    char ip[64];
    int port;
    int64_t packets_to_send;
    int payload_size;
    int thread_id;
} ThreadArgs;

// Inline progress bar renderer
void print_progress(long long current, long long total) {
    if (total <= 0) return;
    int bar_width = 40;
    double percentage = (double)current / total;
    if (percentage > 1.0) percentage = 1.0;
    int filled = (int)(percentage * bar_width);

    printf("\r[");
    for (int i = 0; i < bar_width; i++) {
        if (i < filled) printf("=");
        else if (i == filled) printf(">");
        else printf(" ");
    }
    printf("] %3.1f%% (%lld/%lld)", percentage * 100.0, current, total);
    fflush(stdout);
}

// Robust send helper: returns 1 if packet sent, 0 if transient buffer saturation
static inline int safe_send(int sock, const char* payload, int p_size) {
    if (send(sock, payload, p_size, 0) > 0) {
        return 1;
    }
    // Yield CPU briefly on socket buffer full condition instead of killing thread
    cpu_yield();
    return 0;
}


void* worker_thread(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return NULL;
    }

    // 1. Expand socket send buffer to maximum (32MB per worker socket)
    int sndbuf = 32 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));

    struct sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(args->port);
    inet_pton(AF_INET, args->ip, &target.sin_addr);

    // Lock socket route to target IP/Port
    connect(sock, (struct sockaddr*)&target, sizeof(target));

    // Allocate dynamic payload buffer
    int p_size = args->payload_size;
    if (p_size <= 0 || p_size > 65507) p_size = 65000; // Standard max valid UDP payload

    char* payload = (char*)malloc(p_size);
    if (!payload) {
        close(sock);
        return NULL;
    }
    memset(payload, 'X', p_size);

    int64_t remaining = args->packets_to_send;
    int64_t local_sent = 0;

    // Hot unrolled transmission loop with resilient error handling
    while (remaining >= 16) {
        int s = 0;
        s += safe_send(sock, payload, p_size);
        s += safe_send(sock, payload, p_size);
        s += safe_send(sock, payload, p_size);
        s += safe_send(sock, payload, p_size);

        s += safe_send(sock, payload, p_size);
        s += safe_send(sock, payload, p_size);
        s += safe_send(sock, payload, p_size);
        s += safe_send(sock, payload, p_size);

        s += safe_send(sock, payload, p_size);
        s += safe_send(sock, payload, p_size);
        s += safe_send(sock, payload, p_size);
        s += safe_send(sock, payload, p_size);

        s += safe_send(sock, payload, p_size);
        s += safe_send(sock, payload, p_size);
        s += safe_send(sock, payload, p_size);
        s += safe_send(sock, payload, p_size);

        remaining -= s;
        local_sent += s;

        // Batch global atomic counter updates every 1,000 sent packets
        if (local_sent >= 1000) {
            ATOMIC_ADD(&g_total_sent, local_sent);
            local_sent = 0;
        }
    }

    // Process remaining tail packets
    while (remaining > 0) {
        if (safe_send(sock, payload, p_size)) {
            remaining--;
            local_sent++;
        }
    }

    if (local_sent > 0) {
        ATOMIC_ADD(&g_total_sent, local_sent);
    }

    free(payload);

    close(sock);
    return NULL;
}

void start_packet_generator(const char* ip, int port, int64_t max_i, int num_threads, int payload_size) {
    if (num_threads < 1) num_threads = 1;
    if (payload_size <= 0) payload_size = 65000;

    g_total_sent = 0;

    int64_t total_packets_target;
    ThreadArgs* args = (ThreadArgs*)malloc(sizeof(ThreadArgs) * num_threads);

    pthread_t* threads = (pthread_t*)malloc(sizeof(pthread_t) * num_threads);
    for (int i = 0; i < num_threads; i++) {
        strncpy(args[i].ip, ip, sizeof(args[i].ip) - 1);
        args[i].ip[sizeof(args[i].ip) - 1] = '\0';
        args[i].port = port;
        args[i].packets_to_send = max_i;
        args[i].payload_size = payload_size;
        args[i].thread_id = i;
        pthread_create(&threads[i], NULL, worker_thread, &args[i]);
    }

    // Asynchronous UI progress loop
    while (1) {
        long long current_sent = ATOMIC_READ(&g_total_sent);
        print_progress(current_sent, total_packets_target);

        if (current_sent >= total_packets_target) break;

        sleep_ms(20);
    }

    // Join all worker threads to guarantee complete cleanup
    for (int i = 0; i < num_threads; i++) pthread_join(threads[i], NULL);

    // Render final 100% status
    print_progress(ATOMIC_READ(&g_total_sent), total_packets_target);
    printf("\n");

    free(threads);
    free(args);
}