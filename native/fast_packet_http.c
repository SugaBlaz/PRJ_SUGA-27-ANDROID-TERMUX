#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <pthread.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#define EXPORT __attribute__((visibility("default")))
#define CLOSE_SOCKET(s) close(s)
#define SOCKET int
#define INVALID_SOCKET -1
#define SLEEP_MS(ms) usleep((ms) * 1000)
#define ATOMIC_ADD(ptr, val) \
    atomic_fetch_add_explicit((ptr), (val), memory_order_relaxed)

#define ATOMIC_READ(ptr) \
    atomic_load_explicit((ptr), memory_order_relaxed)

#define MAX_TOTAL_REQUESTS 2147483647

#ifdef __cplusplus
extern "C" {
#endif

static int send_all(
    SOCKET sock,
    const char *buffer,
    int length,
    int flags
) {
    int total_sent = 0;

    while (total_sent < length) {
        int sent = send(
            sock,
            buffer + total_sent,
            length - total_sent,
            flags
        );

        if (sent > 0) {
            total_sent += sent;
            continue;
        }

        if (sent < 0 && errno == EINTR) {
            continue;
        }

        return -1;
    }

    return total_sent;
}

typedef void (*ProgressCallback)(int delta);

typedef struct {
    int64_t total_ok;
    int64_t total_err;
    double elapsed;
} TestResult;

typedef struct {
    struct sockaddr_storage target_addr;
    socklen_t addr_len;
    int family;
    int socktype;
    int protocol;
    char request_buffer[2048];
    int req_len;
    int64_t requests_per_thread;
    int64_t success_count;
    int64_t error_count;
    _Atomic int64_t *global_completed;
} WorkerArgs;

static int parse_url(
    const char *url,
    char *hostname,
    size_t hostname_size,
    char *port,
    size_t port_size,
    char *path,
    size_t path_size
) {
    if (
        url == NULL ||
        hostname == NULL ||
        port == NULL ||
        path == NULL ||
        hostname_size == 0 ||
        port_size == 0 ||
        path_size == 0
    ) {
        return -1;
    }

    hostname[0] = '\0';
    port[0] = '\0';
    path[0] = '\0';

    /*
     * Skip optional scheme.
     *
     * Supported examples:
     *   example.com
     *   http://example.com
     *   https://example.com
     */
    const char *host_start = strstr(url, "://");

    if (host_start != NULL) {
        host_start += 3;
    } else {
        host_start = url;
    }

    if (*host_start == '\0') {
        return -1;
    }

    /*
     * Find the beginning of the path.
     */
    const char *path_start = strchr(host_start, '/');

    const char *host_end;

    if (path_start != NULL) {
        host_end = path_start;
    } else {
        host_end = host_start + strlen(host_start);
    }

    /*
     * Extract host[:port].
     */
    size_t host_port_len =
        (size_t)(host_end - host_start);

    if (host_port_len == 0) {
        return -1;
    }

    /*
     * Find ':' inside host[:port].
     *
     * This intentionally handles normal IPv4/domain URLs.
     */
    const char *colon = NULL;

    for (const char *p = host_start; p < host_end; p++) {
        if (*p == ':') {
            colon = p;
            break;
        }
    }

    if (colon != NULL) {
        size_t hostname_len =
            (size_t)(colon - host_start);

        size_t port_len =
            (size_t)(host_end - colon - 1);

        if (hostname_len == 0 || port_len == 0) {
            return -1;
        }

        if (hostname_len >= hostname_size ||
            port_len >= port_size) {
            return -1;
        }

        memcpy(
            hostname,
            host_start,
            hostname_len
        );

        hostname[hostname_len] = '\0';

        memcpy(
            port,
            colon + 1,
            port_len
        );

        port[port_len] = '\0';
    } else {
        if (host_port_len >= hostname_size) {
            return -1;
        }

        memcpy(
            hostname,
            host_start,
            host_port_len
        );

        hostname[host_port_len] = '\0';

        /*
         * Default HTTP port.
         */
        if (port_size < 3) {
            return -1;
        }

        strcpy(port, "80");
    }

    /*
     * Extract path.
     */
    if (path_start != NULL) {
        size_t path_len = strlen(path_start);

        if (path_len >= path_size) {
            return -1;
        }

        memcpy(
            path,
            path_start,
            path_len
        );

        path[path_len] = '\0';
    } else {
        if (path_size < 2) {
            return -1;
        }

        strcpy(path, "/");
    }

    return 0;
}

// OS-Agnostic High Precision Timer
static double get_current_time_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + (ts.tv_nsec / 1000000000.0);
}

static int connect_with_timeout(
    SOCKET sock,
    const struct sockaddr *addr,
    socklen_t addr_len,
    int timeout_ms
) {
    int flags = fcntl(sock, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }

    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }

    int result = connect(sock, addr, addr_len);

    if (result == 0) {
        /*
         * Connection completed immediately.
         * Restore blocking mode.
         */
        fcntl(sock, F_SETFL, flags);
        return 0;
    }

    if (errno != EINPROGRESS) {
        fcntl(sock, F_SETFL, flags);
        return -1;
    }

    struct pollfd pfd = {
        .fd = sock,
        .events = POLLOUT,
        .revents = 0
    };

    int poll_result = poll(&pfd, 1, timeout_ms);

    if (poll_result <= 0) {
        fcntl(sock, F_SETFL, flags);
        return -1;
    }

    int socket_error = 0;
    socklen_t error_len = sizeof(socket_error);

    if (getsockopt(
            sock,
            SOL_SOCKET,
            SO_ERROR,
            &socket_error,
            &error_len
        ) < 0) {
        fcntl(sock, F_SETFL, flags);
        return -1;
    }

    if (socket_error != 0) {
        fcntl(sock, F_SETFL, flags);
        errno = socket_error;
        return -1;
    }

    /*
     * Connection succeeded.
     * Restore the original blocking mode.
     */
    if (fcntl(sock, F_SETFL, flags) < 0) {
        return -1;
    }

    return 0;
}

void *http_worker(void *ptr) {
    WorkerArgs *args = (WorkerArgs *)ptr;
    SOCKET sock = INVALID_SOCKET;
    int local_completed = 0;
    const int batch_size = 100; // Increased batch size for extreme throughput

    char drain_buffer[8192]; // Large buffer to rapidly drain OS TCP window

    // Setup socket send flags (suppress SIGPIPE on Linux)
    int send_flags = 0;
#ifndef _WIN32
    send_flags |= MSG_NOSIGNAL;
#endif

    for (int64_t i = 0; i < args->requests_per_thread; i++) {
        if (sock == INVALID_SOCKET) {
            sock = socket(args->family, args->socktype, args->protocol);
            if (sock == INVALID_SOCKET) goto ERROR_STATE;

            // Deep socket tuning
            int flag = 1;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&flag, sizeof(int));

            // Set receive timeout so threads don't deadlock on slow servers (1 second)
            struct timeval recv_timeout = {
                .tv_sec = 1,
                .tv_usec = 0
            };

            struct timeval send_timeout = {
                .tv_sec = 5,
                .tv_usec = 0
            };

            setsockopt(
                sock,
                SOL_SOCKET,
                SO_RCVTIMEO,
                &recv_timeout,
                sizeof(recv_timeout)
            );

            setsockopt(
                sock,
                SOL_SOCKET,
                SO_SNDTIMEO,
                &send_timeout,
                sizeof(send_timeout)
            );

            if (connect_with_timeout(
                    sock,
                    (struct sockaddr *)&args->target_addr,
                    args->addr_len,
                    5000
                ) != 0) {

                CLOSE_SOCKET(sock);
                sock = INVALID_SOCKET;
                goto ERROR_STATE;
            }
        }

        // Fire request
        if (send_all(
            sock,
            args->request_buffer,
            args->req_len,
            send_flags
        ) == args->req_len) {

        int bytes = recv(
            sock,
            drain_buffer,
            sizeof(drain_buffer),
            0
        );

        if (bytes > 0) {
            args->success_count++;
        } else {
            CLOSE_SOCKET(sock);
            sock = INVALID_SOCKET;
            args->error_count++;
        }
    } else {
        CLOSE_SOCKET(sock);
        sock = INVALID_SOCKET;
        goto ERROR_STATE;
    }

        goto REQUEST_COMPLETE;

    ERROR_STATE:
        args->error_count++;

    REQUEST_COMPLETE:
        local_completed++;
        if (local_completed >= batch_size) {
            ATOMIC_ADD(args->global_completed, local_completed);
            local_completed = 0;
        }
    }

    if (sock != INVALID_SOCKET) {
        CLOSE_SOCKET(sock);
    }

    if (local_completed > 0) {
        ATOMIC_ADD(args->global_completed, local_completed);
    }


    return NULL;
}

EXPORT TestResult run_http_stress_test(const char *url, int total_requests, int num_threads, ProgressCallback cb) {
    TestResult res = {0, 0, 0.0};
    if (url == NULL) {
        return res;
    }

    if (total_requests <= 0) {
        return res;
    }

    if (total_requests > MAX_TOTAL_REQUESTS) {
        return res;
    }

    if (num_threads <= 0) {
        num_threads = 1;
    }

    if (num_threads > total_requests) {
        num_threads = total_requests;
    }

    int base_requests = total_requests / num_threads;
    int remainder = total_requests % num_threads;
    int64_t actual_total = total_requests;

    pthread_t *threads = malloc(sizeof(pthread_t) * num_threads);

    bool *thread_started =
        calloc(num_threads, sizeof(bool));

    if (threads == NULL || thread_started == NULL) {
        free(threads);
        free(thread_started);
        return res;
    }

    char hostname[256];
    char port[10];
    char path[512];

    if (parse_url(
            url,
            hostname,
            sizeof(hostname),
            port,
            sizeof(port),
            path,
            sizeof(path)
        ) != 0) {

        res.total_err = total_requests;

        free(thread_started);
        free(threads);

        return res;
    }

    // ==========================================
    // 1. CENTRALIZED DNS RESOLUTION (Done once)
    // ==========================================
    struct addrinfo hints, *dns_res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname, port, &hints, &dns_res) != 0) {
        res.total_err = actual_total;

        free(thread_started);
        free(threads);

        return res; 
    }

    WorkerArgs *args = malloc(sizeof(WorkerArgs) * num_threads);

    if (args == NULL) {
        free(thread_started);
        free(threads);
        freeaddrinfo(dns_res);
        return res;
    }

    _Atomic int64_t global_completed = 0;

    static const char *HTTP_FORMAT = 
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36\r\n"
             "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8\r\n"
             "Accept-Language: en-US,en;q=0.9\r\n"
             "Sec-Ch-Ua: \"Chromium\";v=\"124\", \"Google Chrome\";v=\"124\", \"Not-A.Brand\";v=\"99\"\r\n"
             "Sec-Ch-Ua-Mobile: ?0\r\n"
             "Sec-Ch-Ua-Platform: \"Windows\"\r\n"
             "Sec-Fetch-Dest: document\r\n"
             "Sec-Fetch-Mode: navigate\r\n"
             "Sec-Fetch-Site: none\r\n"
             "Sec-Fetch-User: ?1\r\n"
             "Upgrade-Insecure-Requests: 1\r\n"
             "Connection: keep-alive\r\n\r\n";

    char prebaked_req[2048];
    
    // [FIX] Actually generate the request and capture its exact length!
    int prebaked_len = snprintf(prebaked_req, sizeof(prebaked_req), HTTP_FORMAT, path, hostname);
        if (prebaked_len < 0) {
            freeaddrinfo(dns_res);
            free(args);
            free(thread_started);
            free(threads);
            return res;
        }

        if ((size_t)prebaked_len >= sizeof(prebaked_req)) {
            prebaked_len = sizeof(prebaked_req) - 1;
        }

        for (int i = 0; i < num_threads; i++) {
            memcpy(
                &args[i].target_addr,
                dns_res->ai_addr,
                dns_res->ai_addrlen
            );
    
            args[i].addr_len = dns_res->ai_addrlen;
            args[i].family = dns_res->ai_family;
            args[i].socktype = dns_res->ai_socktype;
            args[i].protocol = dns_res->ai_protocol;
    
            memcpy(
                args[i].request_buffer,
                prebaked_req,
                prebaked_len + 1
            );
    
            args[i].req_len = prebaked_len;
    
            args[i].requests_per_thread =
                base_requests + (i < remainder ? 1 : 0);
    
            args[i].success_count = 0;
            args[i].error_count = 0;
            args[i].global_completed = &global_completed;
    
            int rc = pthread_create(
                &threads[i],
                NULL,
                http_worker,
                &args[i]
            );

            if (rc == 0) {
                thread_started[i] = true;
            } else {
                /*
                 * This worker never started.
                 * Account for its work as failed so the
                 * progress counter can still reach actual_total.
                 */
                args[i].error_count =
                    args[i].requests_per_thread;
    
                ATOMIC_ADD(
                    &global_completed,
                    args[i].requests_per_thread
                );
            }
        }

    freeaddrinfo(dns_res);

    double start_time = get_current_time_sec();

    int64_t last_reported = 0;

    while (last_reported < actual_total) {
        SLEEP_MS(25);

        int64_t current =
            ATOMIC_READ(&global_completed);

        int64_t delta =
            current - last_reported;

        if (delta > 0) {
            if (cb != NULL) {
                cb((int)delta);
            }

            last_reported = current;
        }
    }

    for (int i = 0; i < num_threads; i++) {
        if (!thread_started[i]) {
            continue;
        }

        pthread_join(threads[i], NULL);

        res.total_ok += args[i].success_count;
        res.total_err += args[i].error_count;
    }

    double end_time = get_current_time_sec();
    res.elapsed = end_time - start_time;

    free(thread_started);
    free(threads);
    free(args);

    return res;
}

#ifdef __cplusplus
}
#endif
