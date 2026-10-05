/*
 * Copyright (c) 2026 SugaBlaz
 * This software is released under the MIT License.
 * https://github.com/SugaBlaz/PRJ_SUGA-27-ANDROID-TERMUX
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509_vfy.h>

#if defined(__GNUC__) || defined(__clang__)
#define EXPORT __attribute__((visibility("default")))
#else
#define EXPORT
#endif

#define SOCKET int
#define INVALID_SOCKET (-1)
#define CLOSE_SOCKET(s) close((s))

#define MAX_TOTAL_REQUESTS INT64_C(9223372036854775806)
#define MAX_THREADS 64
#define MAX_DELAY_MS 60000

#define CONNECT_TIMEOUT_MS 5000
#define IO_TIMEOUT_MS 15000

/*
 * poll() is intentionally sliced into short intervals so an
 * external stop request is noticed promptly even when no signal
 * arrives on the worker thread.
 */
#define STOP_POLL_SLICE_MS 100

#define RESPONSE_BUFFER_SIZE 16384

typedef void (*ProgressCallback)(int delta);

typedef struct {
    int64_t total_ok;
    int64_t total_err;
    double elapsed;
} TestResult;

typedef struct {
    char *scheme;
    char *host;
    char *port;
    char *path;
    bool use_tls;
} UrlParts;

typedef struct {
    const char *host;
    const char *port;
    const char *path;
    bool use_tls;
    bool verify_certificate;
    bool verify_hostname;

    int64_t requests;
    int delay_ms;

    struct addrinfo *addresses;
    SSL_CTX *ssl_ctx;

    ProgressCallback callback;

    int64_t local_ok;
    int64_t local_err;
} WorkerArgs;

typedef enum {
    REQUEST_FAILED = 0,
    REQUEST_OK = 1,
    REQUEST_CANCELLED = 2
} RequestStatus;

/*
 * SIGINT handler state and programmatic stop state are separate:
 *
 * - g_sigint_requested is written only by the signal handler.
 * - g_stop_requested is used by normal code / exported API.
 *
 * Keeping the signal handler limited to sig_atomic_t avoids
 * calling non-async-signal-safe functions from SIGINT.
 */
static volatile sig_atomic_t g_sigint_requested = 0;
static _Atomic bool g_stop_requested = false;

/*
 * Only one test may own the process-wide SIGINT handler at a time.
 */
static pthread_mutex_t g_run_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_previous_sigint_valid = false;
static struct sigaction g_previous_sigint_action;

/* ---------------------------------------------------------
 * Cancellation helpers
 * --------------------------------------------------------- */

static inline bool stop_requested(void)
{
    return g_sigint_requested != 0 ||
           atomic_load_explicit(
               &g_stop_requested,
               memory_order_acquire
           );
}

static void handle_sigint(int signal_number)
{
    (void)signal_number;

    /*
     * This is the only operation performed from the handler.
     */
    g_sigint_requested = 1;
}

EXPORT void http_tester_request_stop(void)
{
    atomic_store_explicit(
        &g_stop_requested,
        true,
        memory_order_release
    );
}

EXPORT void http_tester_clear_stop(void)
{
    atomic_store_explicit(
        &g_stop_requested,
        false,
        memory_order_release
    );
}

/* Backwards-friendly alias for callers that prefer a descriptive name. */
EXPORT void stop_http_stress_test(void)
{
    http_tester_request_stop();
}

static int install_sigint_handler(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));

    action.sa_handler = handle_sigint;

    if (sigemptyset(&action.sa_mask) != 0) {
        return -1;
    }

    /*
     * Do not use SA_RESTART. Interrupted poll/send/recv calls can
     * return EINTR, allowing the code to notice cancellation.
     */
    action.sa_flags = 0;

    if (
        sigaction(
            SIGINT,
            &action,
            &g_previous_sigint_action
        ) != 0
    ) {
        return -1;
    }

    g_previous_sigint_valid = true;
    return 0;
}

static void restore_sigint_handler(void)
{
    if (!g_previous_sigint_valid) {
        return;
    }

    (void)sigaction(
        SIGINT,
        &g_previous_sigint_action,
        NULL
    );

    g_previous_sigint_valid = false;
}

/* ---------------------------------------------------------
 * Small helpers
 * --------------------------------------------------------- */

static char *dup_range(const char *start, size_t len)
{
    char *out = malloc(len + 1);

    if (!out) {
        return NULL;
    }

    memcpy(out, start, len);
    out[len] = '\0';

    return out;
}

static void free_url_parts(UrlParts *url)
{
    if (!url) {
        return;
    }

    free(url->scheme);
    free(url->host);
    free(url->port);
    free(url->path);

    memset(url, 0, sizeof(*url));
}

static bool is_ip_literal(const char *host)
{
    struct in_addr ipv4;
    struct in6_addr ipv6;

    if (!host) {
        return false;
    }

    return inet_pton(AF_INET, host, &ipv4) == 1 ||
           inet_pton(AF_INET6, host, &ipv6) == 1;
}

static bool is_ipv6_literal(const char *host)
{
    struct in6_addr ipv6;

    return host &&
           inet_pton(AF_INET6, host, &ipv6) == 1;
}

static bool is_valid_port_string(const char *port)
{
    char *end = NULL;
    unsigned long value;

    if (!port || *port == '\0') {
        return false;
    }

    errno = 0;
    value = strtoul(port, &end, 10);

    if (
        errno != 0 ||
        end == port ||
        *end != '\0' ||
        value < 1 ||
        value > 65535
    ) {
        return false;
    }

    return true;
}

/* ---------------------------------------------------------
 * URL parser
 * --------------------------------------------------------- */

static int parse_url(const char *input, UrlParts *out)
{
    const char *cursor;
    const char *authority_end;
    const char *path_start;
    size_t authority_len;

    if (!input || !out || input[0] == '\0') {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    cursor = input;

    if (strncasecmp(cursor, "https://", 8) == 0) {
        out->scheme = dup_range("https", 6);
        out->use_tls = true;
        cursor += 8;
    } else if (strncasecmp(cursor, "http://", 7) == 0) {
        out->scheme = dup_range("http", 4);
        out->use_tls = false;
        cursor += 7;
    } else {
        out->scheme = dup_range("http", 4);
        out->use_tls = false;
    }

    if (!out->scheme || *cursor == '/') {
        goto fail;
    }

    authority_end = cursor;

    while (
        *authority_end != '\0' &&
        *authority_end != '/' &&
        *authority_end != '?' &&
        *authority_end != '#'
    ) {
        authority_end++;
    }

    authority_len =
        (size_t)(authority_end - cursor);

    if (authority_len == 0) {
        goto fail;
    }

    /*
     * Credentials are intentionally unsupported.
     */
    for (size_t i = 0; i < authority_len; ++i) {
        if (cursor[i] == '@') {
            goto fail;
        }
    }

    if (*cursor == '[') {
        const char *closing =
            memchr(cursor, ']', authority_len);

        if (!closing || closing == cursor + 1) {
            goto fail;
        }

        out->host =
            dup_range(
                cursor + 1,
                (size_t)(closing - cursor - 1)
            );

        if (!out->host) {
            goto fail;
        }

        if (closing + 1 < authority_end) {
            if (closing[1] != ':') {
                goto fail;
            }

            out->port =
                dup_range(
                    closing + 2,
                    (size_t)(
                        authority_end -
                        (closing + 2)
                    )
                );

            if (!out->port) {
                goto fail;
            }
        }
    } else {
        const char *colon =
            memchr(
                cursor,
                ':',
                authority_len
            );

        if (colon) {
            /*
             * Unbracketed IPv6 is ambiguous and therefore rejected.
             */
            if (
                memchr(
                    colon + 1,
                    ':',
                    (size_t)(
                        authority_end -
                        colon - 1
                    )
                )
            ) {
                goto fail;
            }

            out->host =
                dup_range(
                    cursor,
                    (size_t)(colon - cursor)
                );

            out->port =
                dup_range(
                    colon + 1,
                    (size_t)(
                        authority_end -
                        colon - 1
                    )
                );

            if (!out->host || !out->port) {
                goto fail;
            }
        } else {
            out->host =
                dup_range(
                    cursor,
                    authority_len
                );
        }
    }

    if (!out->host || out->host[0] == '\0') {
        goto fail;
    }

    if (!out->port) {
        const char *default_port =
            out->use_tls ? "443" : "80";

        out->port =
            dup_range(
                default_port,
                strlen(default_port)
            );
    }

    if (
        !out->port ||
        !is_valid_port_string(out->port)
    ) {
        goto fail;
    }

    path_start = authority_end;

    if (*path_start == '#') {
        out->path = dup_range("/", 1);
    } else if (*path_start == '?') {
        size_t query_len = strlen(path_start);
        const char *fragment =
            memchr(
                path_start,
                '#',
                query_len
            );

        if (fragment) {
            query_len =
                (size_t)(
                    fragment - path_start
                );
        }

        out->path =
            malloc(query_len + 2);

        if (out->path) {
            out->path[0] = '/';

            memcpy(
                out->path + 1,
                path_start,
                query_len
            );

            out->path[query_len + 1] = '\0';
        }
    } else if (*path_start == '/') {
        size_t path_len = strlen(path_start);
        const char *fragment =
            memchr(
                path_start,
                '#',
                path_len
            );

        if (fragment) {
            path_len =
                (size_t)(
                    fragment - path_start
                );
        }

        out->path =
            dup_range(
                path_start,
                path_len
            );
    } else if (*path_start == '\0') {
        out->path = dup_range("/", 1);
    } else {
        goto fail;
    }

    if (!out->path || out->path[0] == '\0') {
        goto fail;
    }

    return 0;

fail:
    free_url_parts(out);
    return -1;
}

/* ---------------------------------------------------------
 * Time / wait helpers
 * --------------------------------------------------------- */

static double get_current_time_sec(void)
{
    struct timespec ts;

    if (
        clock_gettime(
            CLOCK_MONOTONIC,
            &ts
        ) != 0
    ) {
        return 0.0;
    }

    return (double)ts.tv_sec +
           ((double)ts.tv_nsec / 1000000000.0);
}

static int wait_for_socket(
    SOCKET sock,
    short events,
    int timeout_ms
)
{
    const double start =
        get_current_time_sec();

    while (!stop_requested()) {
        struct pollfd pfd;
        int remaining_ms;
        int slice_ms;
        int rc;

        if (timeout_ms < 0) {
            remaining_ms =
                STOP_POLL_SLICE_MS;
        } else {
            double elapsed =
                get_current_time_sec() - start;

            remaining_ms =
                timeout_ms -
                (int)(elapsed * 1000.0);

            if (remaining_ms <= 0) {
                return 0;
            }
        }

        slice_ms =
            remaining_ms > STOP_POLL_SLICE_MS
                ? STOP_POLL_SLICE_MS
                : remaining_ms;

        memset(
            &pfd,
            0,
            sizeof(pfd)
        );

        pfd.fd = sock;
        pfd.events = events;

        do {
            rc =
                poll(
                    &pfd,
                    1,
                    slice_ms
                );
        } while (
            rc < 0 &&
            errno == EINTR &&
            !stop_requested()
        );

        if (stop_requested()) {
            return -2;
        }

        if (rc == 0) {
            if (
                timeout_ms >= 0 &&
                (
                    get_current_time_sec() - start
                ) * 1000.0 >= timeout_ms
            ) {
                return 0;
            }

            continue;
        }

        if (rc < 0) {
            return -1;
        }

        /*
         * For POLLIN, HUP can still mean recv()/SSL_read() must be
         * called to consume EOF or remaining data.
         *
         * For POLLOUT, POLLERR/HUP without POLLOUT is an error.
         */
        if (pfd.revents & events) {
            return 1;
        }

        if (
            pfd.revents &
            (POLLERR | POLLNVAL)
        ) {
            return -1;
        }

        if (
            (events & POLLIN) &&
            (pfd.revents & POLLHUP)
        ) {
            return 1;
        }

        return -1;
    }

    return -2;
}

/* ---------------------------------------------------------
 * Socket creation / connection
 * --------------------------------------------------------- */

static SOCKET connect_with_timeout(
    const struct addrinfo *address
)
{
    SOCKET sock;
    int original_flags;
    int so_error = 0;
    socklen_t so_error_len =
        sizeof(so_error);

    if (!address || stop_requested()) {
        return INVALID_SOCKET;
    }

    sock =
        socket(
            address->ai_family,
            address->ai_socktype,
            address->ai_protocol
        );

    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    original_flags =
        fcntl(
            sock,
            F_GETFL,
            0
        );

    if (original_flags < 0) {
        CLOSE_SOCKET(sock);
        return INVALID_SOCKET;
    }

    if (
        fcntl(
            sock,
            F_SETFL,
            original_flags | O_NONBLOCK
        ) < 0
    ) {
        CLOSE_SOCKET(sock);
        return INVALID_SOCKET;
    }

    if (stop_requested()) {
        CLOSE_SOCKET(sock);
        return INVALID_SOCKET;
    }

    int rc =
        connect(
            sock,
            address->ai_addr,
            address->ai_addrlen
        );

    if (rc < 0) {
        if (errno != EINPROGRESS) {
            CLOSE_SOCKET(sock);
            return INVALID_SOCKET;
        }

        rc =
            wait_for_socket(
                sock,
                POLLOUT,
                CONNECT_TIMEOUT_MS
            );

        if (rc != 1) {
            CLOSE_SOCKET(sock);
            return INVALID_SOCKET;
        }

        if (
            getsockopt(
                sock,
                SOL_SOCKET,
                SO_ERROR,
                &so_error,
                &so_error_len
            ) != 0 ||
            so_error != 0
        ) {
            CLOSE_SOCKET(sock);
            return INVALID_SOCKET;
        }
    }

    if (stop_requested()) {
        CLOSE_SOCKET(sock);
        return INVALID_SOCKET;
    }

    /*
     * Keep the socket nonblocking. All HTTP/TLS I/O below uses
     * poll(), which is what lets stop requests interrupt the
     * lifecycle promptly.
     */
    return sock;
}

/* ---------------------------------------------------------
 * Plain HTTP I/O
 * --------------------------------------------------------- */

static bool send_all(
    SOCKET sock,
    const void *buffer,
    size_t length
)
{
    const unsigned char *data =
        (const unsigned char *)buffer;

    size_t sent = 0;

    while (
        sent < length &&
        !stop_requested()
    ) {
        ssize_t rc =
            send(
                sock,
                data + sent,
                length - sent,
                0
            );

        if (rc > 0) {
            sent += (size_t)rc;
            continue;
        }

        if (rc < 0 && errno == EINTR) {
            continue;
        }

        if (
            rc < 0 &&
            (
                errno == EAGAIN ||
                errno == EWOULDBLOCK
            )
        ) {
            int wait_rc =
                wait_for_socket(
                    sock,
                    POLLOUT,
                    IO_TIMEOUT_MS
                );

            if (wait_rc == -2) {
                return false;
            }

            if (wait_rc != 1) {
                return false;
            }

            continue;
        }

        return false;
    }

    return sent == length &&
           !stop_requested();
}

static bool receive_http_response(SOCKET sock)
{
    unsigned char buffer[RESPONSE_BUFFER_SIZE];
    bool got_data = false;

    for (;;) {
        if (stop_requested()) {
            return false;
        }

        ssize_t rc =
            recv(
                sock,
                buffer,
                sizeof(buffer),
                0
            );

        if (rc > 0) {
            got_data = true;
            continue;
        }

        if (rc == 0) {
            return got_data;
        }

        if (errno == EINTR) {
            if (stop_requested()) {
                return false;
            }

            continue;
        }

        if (
            errno == EAGAIN ||
            errno == EWOULDBLOCK
        ) {
            int wait_rc =
                wait_for_socket(
                    sock,
                    POLLIN,
                    IO_TIMEOUT_MS
                );

            if (wait_rc == -2) {
                return false;
            }

            if (wait_rc != 1) {
                return false;
            }

            continue;
        }

        return false;
    }
}

/* ---------------------------------------------------------
 * TLS I/O
 * --------------------------------------------------------- */

static bool send_all_tls(
    SSL *ssl,
    SOCKET sock,
    const void *buffer,
    size_t length
)
{
    const unsigned char *data =
        (const unsigned char *)buffer;

    size_t sent = 0;

    while (
        sent < length &&
        !stop_requested()
    ) {
        size_t remaining =
            length - sent;

        int chunk =
            remaining > (size_t)INT_MAX
                ? INT_MAX
                : (int)remaining;

        int rc =
            SSL_write(
                ssl,
                data + sent,
                chunk
            );

        if (rc > 0) {
            sent += (size_t)rc;
            continue;
        }

        switch (SSL_get_error(ssl, rc)) {
        case SSL_ERROR_WANT_READ:
        {
            int wait_rc =
                wait_for_socket(
                    sock,
                    POLLIN,
                    IO_TIMEOUT_MS
                );

            if (wait_rc == -2 ||
                wait_rc != 1) {
                return false;
            }

            break;
        }

        case SSL_ERROR_WANT_WRITE:
        {
            int wait_rc =
                wait_for_socket(
                    sock,
                    POLLOUT,
                    IO_TIMEOUT_MS
                );

            if (wait_rc == -2 ||
                wait_rc != 1) {
                return false;
            }

            break;
        }

        case SSL_ERROR_ZERO_RETURN:
            return false;

        case SSL_ERROR_SYSCALL:
            if (stop_requested()) {
                return false;
            }

            return false;

        default:
            return false;
        }
    }

    return sent == length &&
           !stop_requested();
}

static bool receive_https_response(
    SSL *ssl,
    SOCKET sock
)
{
    unsigned char buffer[RESPONSE_BUFFER_SIZE];
    bool got_data = false;

    for (;;) {
        if (stop_requested()) {
            return false;
        }

        int rc =
            SSL_read(
                ssl,
                buffer,
                (int)sizeof(buffer)
            );

        if (rc > 0) {
            got_data = true;
            continue;
        }

        switch (SSL_get_error(ssl, rc)) {
        case SSL_ERROR_ZERO_RETURN:
            return got_data;

        case SSL_ERROR_WANT_READ:
        {
            int wait_rc =
                wait_for_socket(
                    sock,
                    POLLIN,
                    IO_TIMEOUT_MS
                );

            if (wait_rc == -2 ||
                wait_rc != 1) {
                return false;
            }

            break;
        }

        case SSL_ERROR_WANT_WRITE:
        {
            int wait_rc =
                wait_for_socket(
                    sock,
                    POLLOUT,
                    IO_TIMEOUT_MS
                );

            if (wait_rc == -2 ||
                wait_rc != 1) {
                return false;
            }

            break;
        }

        case SSL_ERROR_SYSCALL:
            /*
             * With SSL_OP_IGNORE_UNEXPECTED_EOF enabled, an HTTP
             * peer that simply closes TCP after its response can
             * still terminate this read cleanly enough for testing.
             */
            if (
                got_data &&
                errno == 0 &&
                !stop_requested()
            ) {
                return true;
            }

            return false;

        default:
            return false;
        }
    }
}

/* ---------------------------------------------------------
 * TLS context / trust-store handling
 * --------------------------------------------------------- */

static int configure_default_trust_store(
    SSL_CTX *ctx
)
{
    /*
     * First use OpenSSL's configured default paths.
     */
    if (
        SSL_CTX_set_default_verify_paths(ctx) == 1
    ) {
        return 0;
    }

    /*
     * Termux normally exposes $PREFIX. Try its conventional
     * CA bundle/directory as an explicit fallback.
     */
    const char *prefix =
        getenv("PREFIX");

    if (!prefix || *prefix == '\0') {
        return -1;
    }

    size_t prefix_len =
        strlen(prefix);

    const char *suffix_file =
        "/etc/tls/cert.pem";

    const char *suffix_dir =
        "/etc/tls/certs";

    char *ca_file =
        malloc(
            prefix_len +
            strlen(suffix_file) +
            1
        );

    char *ca_dir =
        malloc(
            prefix_len +
            strlen(suffix_dir) +
            1
        );

    if (!ca_file || !ca_dir) {
        free(ca_file);
        free(ca_dir);
        return -1;
    }

    memcpy(
        ca_file,
        prefix,
        prefix_len
    );

    strcpy(
        ca_file + prefix_len,
        suffix_file
    );

    memcpy(
        ca_dir,
        prefix,
        prefix_len
    );

    strcpy(
        ca_dir + prefix_len,
        suffix_dir
    );

    int rc =
        SSL_CTX_load_verify_locations(
            ctx,
            ca_file,
            ca_dir
        ) == 1
            ? 0
            : -1;

    free(ca_file);
    free(ca_dir);

    return rc;
}

static SSL_CTX *create_ssl_context(
    bool verify_certificate
)
{
    SSL_CTX *ctx =
        SSL_CTX_new(
            TLS_client_method()
        );

    if (!ctx) {
        return NULL;
    }

    /*
     * TLS 1.2+ keeps the tester on modern protocol versions.
     */
    if (
        SSL_CTX_set_min_proto_version(
            ctx,
            TLS1_2_VERSION
        ) != 1
    ) {
        SSL_CTX_free(ctx);
        return NULL;
    }

#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    /*
     * Some normal HTTP servers close TCP without TLS close_notify.
     * Treating that as a fatal TLS error makes response completion
     * unreliable for HTTP benchmarking.
     */
    (void)SSL_CTX_set_options(
        ctx,
        SSL_OP_IGNORE_UNEXPECTED_EOF
    );
#endif

    if (verify_certificate) {
        SSL_CTX_set_verify(
            ctx,
            SSL_VERIFY_PEER,
            NULL
        );

        if (
            configure_default_trust_store(ctx) != 0
        ) {
            SSL_CTX_free(ctx);
            return NULL;
        }
    } else {
        /*
         * TLS encryption remains enabled. Only peer
         * authentication is disabled.
         */
        SSL_CTX_set_verify(
            ctx,
            SSL_VERIFY_NONE,
            NULL
        );
    }

    return ctx;
}

/* ---------------------------------------------------------
 * TLS handshake
 * --------------------------------------------------------- */

static SSL *tls_connect(
    SSL_CTX *ctx,
    SOCKET sock,
    const char *host,
    bool verify_hostname
)
{
    if (
        !ctx ||
        sock == INVALID_SOCKET ||
        !host ||
        stop_requested()
    ) {
        return NULL;
    }

    SSL *ssl =
        SSL_new(ctx);

    if (!ssl) {
        return NULL;
    }

    if (
        SSL_set_fd(
            ssl,
            sock
        ) != 1
    ) {
        SSL_free(ssl);
        return NULL;
    }

    /*
     * SNI is sent for DNS names, not IP literals.
     */
    if (!is_ip_literal(host)) {
        if (
            SSL_set_tlsext_host_name(
                ssl,
                host
            ) != 1
        ) {
            SSL_free(ssl);
            return NULL;
        }
    }

    if (verify_hostname) {
        int rc;

        if (is_ip_literal(host)) {
            /*
             * SSL_set1_host() is for host-name matching. For IPv4/IPv6
             * literals use the dedicated IP verification API.
             */
            rc =
                X509_VERIFY_PARAM_set1_ip_asc(
                    SSL_get0_param(ssl),
                    host
                );
        } else {
            rc =
                SSL_set1_host(
                    ssl,
                    host
                );
        }

        if (rc != 1) {
            SSL_free(ssl);
            return NULL;
        }
    }

    while (!stop_requested()) {
        int rc =
            SSL_connect(ssl);

        if (rc == 1) {
            return ssl;
        }

        int error =
            SSL_get_error(
                ssl,
                rc
            );

        if (error == SSL_ERROR_WANT_READ) {
            int wait_rc =
                wait_for_socket(
                    sock,
                    POLLIN,
                    CONNECT_TIMEOUT_MS
                );

            if (wait_rc == 1) {
                continue;
            }

            break;
        }

        if (error == SSL_ERROR_WANT_WRITE) {
            int wait_rc =
                wait_for_socket(
                    sock,
                    POLLOUT,
                    CONNECT_TIMEOUT_MS
                );

            if (wait_rc == 1) {
                continue;
            }

            break;
        }

        break;
    }

    SSL_free(ssl);
    return NULL;
}

/* ---------------------------------------------------------
 * One request lifecycle
 * --------------------------------------------------------- */

static RequestStatus perform_request(
    WorkerArgs *args,
    const struct addrinfo *address,
    const char *request,
    size_t request_len
)
{
    if (
        !args ||
        !address ||
        !request ||
        stop_requested()
    ) {
        return REQUEST_CANCELLED;
    }

    SOCKET sock =
        connect_with_timeout(address);

    if (sock == INVALID_SOCKET) {
        return stop_requested()
            ? REQUEST_CANCELLED
            : REQUEST_FAILED;
    }

    if (stop_requested()) {
        CLOSE_SOCKET(sock);
        return REQUEST_CANCELLED;
    }

    if (!args->use_tls) {
        bool ok = false;

        if (send_all(sock, request, request_len)) {
            if (stop_requested()) {
                CLOSE_SOCKET(sock);
                return REQUEST_CANCELLED;
            }

            ok =
                receive_http_response(sock);
        }

        CLOSE_SOCKET(sock);

        if (stop_requested()) {
            return REQUEST_CANCELLED;
        }

        return ok
            ? REQUEST_OK
            : REQUEST_FAILED;
    }

    SSL *ssl =
        tls_connect(
            args->ssl_ctx,
            sock,
            args->host,
            args->verify_hostname
        );

    if (!ssl) {
        CLOSE_SOCKET(sock);

        return stop_requested()
            ? REQUEST_CANCELLED
            : REQUEST_FAILED;
    }

    /*
     * SSL_get_verify_result() is meaningful when peer verification
     * is enabled. Hostname matching, when requested, is part of the
     * verification parameters used during the handshake.
     */
    if (
        args->verify_certificate &&
        SSL_get_verify_result(ssl) != X509_V_OK
    ) {
        SSL_free(ssl);
        CLOSE_SOCKET(sock);

        return stop_requested()
            ? REQUEST_CANCELLED
            : REQUEST_FAILED;
    }

    if (stop_requested()) {
        SSL_free(ssl);
        CLOSE_SOCKET(sock);
        return REQUEST_CANCELLED;
    }

    bool ok = false;

    if (
        send_all_tls(
            ssl,
            sock,
            request,
            request_len
        )
    ) {
        if (!stop_requested()) {
            ok =
                receive_https_response(
                    ssl,
                    sock
                );
        }
    }

    /*
     * Best-effort shutdown. Since we request Connection: close and
     * the response reader has already consumed EOF, this normally
     * returns immediately.
     */
    (void)SSL_shutdown(ssl);
    SSL_free(ssl);
    CLOSE_SOCKET(sock);

    if (stop_requested()) {
        return REQUEST_CANCELLED;
    }

    return ok
        ? REQUEST_OK
        : REQUEST_FAILED;
}

/* ---------------------------------------------------------
 * HTTP request builder
 * --------------------------------------------------------- */

static bool is_default_port(
    const UrlParts *url
)
{
    if (!url || !url->port) {
        return false;
    }

    return (
        url->use_tls &&
        strcmp(url->port, "443") == 0
    ) || (
        !url->use_tls &&
        strcmp(url->port, "80") == 0
    );
}

static int build_http_request(
    const UrlParts *url,
    char **request_out,
    size_t *request_len_out
)
{
    if (
        !url ||
        !url->host ||
        !url->port ||
        !url->path ||
        !request_out ||
        !request_len_out
    ) {
        return -1;
    }

    bool ipv6 =
        is_ipv6_literal(url->host);

    bool default_port =
        is_default_port(url);

    char *request = NULL;
    size_t needed = 0;

    if (default_port) {
        needed =
            (size_t)snprintf(
                NULL,
                0,
                "GET %s HTTP/1.1\r\n"
                "Host: %s%s%s\r\n"
                "User-Agent: SugaBlaz-HTTP-Tester/1.1\r\n"
                "Accept: */*\r\n"
                "Connection: close\r\n"
                "\r\n",
                url->path,
                ipv6 ? "[" : "",
                url->host,
                ipv6 ? "]" : ""
            );

        request =
            malloc(needed + 1);

        if (!request) {
            return -1;
        }

        (void)snprintf(
            request,
            needed + 1,
            "GET %s HTTP/1.1\r\n"
            "Host: %s%s%s\r\n"
            "User-Agent: SugaBlaz-HTTP-Tester/1.1\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n"
            "\r\n",
            url->path,
            ipv6 ? "[" : "",
            url->host,
            ipv6 ? "]" : ""
        );
    } else {
        needed =
            (size_t)snprintf(
                NULL,
                0,
                "GET %s HTTP/1.1\r\n"
                "Host: %s%s%s:%s\r\n"
                "User-Agent: SugaBlaz-HTTP-Tester/1.1\r\n"
                "Accept: */*\r\n"
                "Connection: close\r\n"
                "\r\n",
                url->path,
                ipv6 ? "[" : "",
                url->host,
                ipv6 ? "]" : "",
                url->port
            );

        request =
            malloc(needed + 1);

        if (!request) {
            return -1;
        }

        (void)snprintf(
            request,
            needed + 1,
            "GET %s HTTP/1.1\r\n"
            "Host: %s%s%s:%s\r\n"
            "User-Agent: SugaBlaz-HTTP-Tester/1.1\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n"
            "\r\n",
            url->path,
            ipv6 ? "[" : "",
            url->host,
            ipv6 ? "]" : "",
            url->port
        );
    }

    *request_out = request;
    *request_len_out = needed;

    return 0;
}

/* ---------------------------------------------------------
 * Delay
 * --------------------------------------------------------- */

static void sleep_delay(int delay_ms)
{
    if (delay_ms <= 0) {
        return;
    }

    struct timespec ts;

    ts.tv_sec =
        delay_ms / 1000;

    ts.tv_nsec =
        (long)(delay_ms % 1000) * 1000000L;

    while (!stop_requested()) {
        if (nanosleep(&ts, &ts) == 0) {
            return;
        }

        if (errno != EINTR) {
            return;
        }
    }
}

/* ---------------------------------------------------------
 * Address selection
 * --------------------------------------------------------- */

static const struct addrinfo *pick_address_for_request(
    const struct addrinfo *addresses,
    int request_index
)
{
    size_t count = 0;
    size_t selected;
    const struct addrinfo *entry;

    if (!addresses) {
        return NULL;
    }

    for (
        entry = addresses;
        entry != NULL;
        entry = entry->ai_next
    ) {
        count++;
    }

    if (count == 0) {
        return NULL;
    }

    selected =
        (size_t)request_index % count;

    entry = addresses;

    while (
        selected-- > 0 &&
        entry->ai_next
    ) {
        entry = entry->ai_next;
    }

    return entry;
}

/*
 * Try every resolved address for this request, starting from the
 * selected address. This avoids failing an otherwise-good request
 * just because one A/AAAA endpoint is temporarily unreachable.
 */
static RequestStatus perform_request_with_fallback(
    WorkerArgs *args,
    int request_index,
    const char *request,
    size_t request_len
)
{
    if (
        !args ||
        !args->addresses ||
        stop_requested()
    ) {
        return REQUEST_CANCELLED;
    }

    /*
     * Count the addrinfo list.
     */
    size_t address_count = 0;

    for (
        const struct addrinfo *entry = args->addresses;
        entry != NULL;
        entry = entry->ai_next
    ) {
        address_count++;
    }

    if (address_count == 0) {
        return REQUEST_FAILED;
    }

    const struct addrinfo *start =
        pick_address_for_request(
            args->addresses,
            request_index
        );

    if (!start) {
        return REQUEST_FAILED;
    }

    /*
     * Walk the list circularly, without allocating an array.
     */
    const struct addrinfo *entry = start;

    for (size_t attempt = 0;
         attempt < address_count;
         ++attempt)
    {
        RequestStatus status =
            perform_request(
                args,
                entry,
                request,
                request_len
            );

        if (status == REQUEST_OK ||
            status == REQUEST_CANCELLED) {
            return status;
        }

        entry =
            entry->ai_next
                ? entry->ai_next
                : args->addresses;
    }

    return stop_requested()
        ? REQUEST_CANCELLED
        : REQUEST_FAILED;
}

/* ---------------------------------------------------------
 * HTTP worker
 * --------------------------------------------------------- */

static void *http_worker(void *arg)
{
    WorkerArgs *args =
        (WorkerArgs *)arg;

    if (!args) {
        return NULL;
    }

    char *request = NULL;
    size_t request_len = 0;

    UrlParts url_view;

    memset(
        &url_view,
        0,
        sizeof(url_view)
    );

    url_view.host =
        (char *)args->host;

    url_view.port =
        (char *)args->port;

    url_view.path =
        (char *)args->path;

    url_view.use_tls =
        args->use_tls;

    if (
        build_http_request(
            &url_view,
            &request,
            &request_len
        ) != 0
    ) {
        args->local_err =
            args->requests;

        if (args->callback) {
            /*
             * These requests were actually assigned but could not
             * be attempted because request construction failed.
             * Report them as completed errors so the UI cannot hang.
             */
            for (int64_t i = 0;
                 i < args->requests;
                 ++i)
            {
                if (stop_requested()) {
                    break;
                }

                args->callback(1);
            }
        }

        return NULL;
    }

    for (
        int i = 0;
        i < args->requests &&
        !stop_requested();
        ++i
    ) {
        RequestStatus status =
            perform_request_with_fallback(
                args,
                i,
                request,
                request_len
            );

        if (status == REQUEST_CANCELLED) {
            break;
        }

        if (status == REQUEST_OK) {
            args->local_ok++;
        } else {
            args->local_err++;
        }

        /*
         * Exactly one callback for every request lifecycle that
         * actually reached a completed success/failure outcome.
         */
        if (args->callback) {
            args->callback(1);
        }

        /*
         * Delay happens only after the request outcome is known.
         * sleep_delay() itself is interruptible.
         */
        if (
            i + 1 < args->requests &&
            !stop_requested()
        ) {
            sleep_delay(args->delay_ms);
        }
    }

    free(request);
    return NULL;
}

/* ---------------------------------------------------------
 * Main implementation
 * --------------------------------------------------------- */

static TestResult run_http_stress_test_impl(
    const char *url_string,
    int64_t total_requests,
    int num_threads,
    ProgressCallback callback,
    int delay_ms,
    int verify_certificate,
    int verify_hostname
)
{
    TestResult result = {0, 0, 0.0};

    UrlParts url = {0};

    struct addrinfo hints;
    struct addrinfo *addresses = NULL;

    SSL_CTX *ssl_ctx = NULL;

    pthread_t *threads = NULL;
    bool *thread_created = NULL;
    WorkerArgs *workers = NULL;

    int actual_threads;
    int64_t base_requests;
    int64_t extra_requests;

    double start_time = 0.0;

    if (
        !url_string ||
        total_requests <= 0 ||
        total_requests > MAX_TOTAL_REQUESTS ||
        num_threads <= 0 ||
        num_threads > MAX_THREADS ||
        delay_ms < 0 ||
        delay_ms > MAX_DELAY_MS
    ) {
        return result;
    }

    if (
        parse_url(
            url_string,
            &url
        ) != 0
    ) {
        return result;
    }

    memset(
        &hints,
        0,
        sizeof(hints)
    );

    hints.ai_family =
        AF_UNSPEC;

    hints.ai_socktype =
        SOCK_STREAM;

    hints.ai_protocol =
        IPPROTO_TCP;

    if (
        getaddrinfo(
            url.host,
            url.port,
            &hints,
            &addresses
        ) != 0
    ) {
        free_url_parts(&url);
        return result;
    }

    if (url.use_tls) {
        ssl_ctx =
            create_ssl_context(
                verify_certificate != 0
            );

        if (!ssl_ctx) {
            freeaddrinfo(addresses);
            free_url_parts(&url);
            return result;
        }
    }

    actual_threads = num_threads;

    if ((int64_t)actual_threads > total_requests) {
        actual_threads =
            (int)total_requests;
    }

    threads =
        calloc(
            (size_t)actual_threads,
            sizeof(*threads)
        );

    thread_created =
        calloc(
            (size_t)actual_threads,
            sizeof(*thread_created)
        );

    workers =
        calloc(
            (size_t)actual_threads,
            sizeof(*workers)
        );

    if (
        !threads ||
        !thread_created ||
        !workers
    ) {
        free(threads);
        free(thread_created);
        free(workers);
        SSL_CTX_free(ssl_ctx);
        freeaddrinfo(addresses);
        free_url_parts(&url);
        return result;
    }

    /*
     * Serialize the process-wide SIGINT handler.
     * trylock avoids deadlocking a caller that accidentally invokes
     * the test recursively/re-entrantly.
     */
    if (
        pthread_mutex_trylock(
            &g_run_mutex
        ) != 0
    ) {
        free(threads);
        free(thread_created);
        free(workers);
        SSL_CTX_free(ssl_ctx);
        freeaddrinfo(addresses);
        free_url_parts(&url);
        return result;
    }

    g_sigint_requested = 0;

    atomic_store_explicit(
        &g_stop_requested,
        false,
        memory_order_release
    );

    if (
        install_sigint_handler() != 0
    ) {
        pthread_mutex_unlock(
            &g_run_mutex
        );

        free(threads);
        free(thread_created);
        free(workers);
        SSL_CTX_free(ssl_ctx);
        freeaddrinfo(addresses);
        free_url_parts(&url);
        return result;
    }

    start_time =
        get_current_time_sec();

    base_requests =
        total_requests /
        actual_threads;

    extra_requests =
        total_requests %
        actual_threads;

    int created_threads = 0;

    for (
        int i = 0;
        i < actual_threads;
        ++i
    ) {
        workers[i].host =
            url.host;

        workers[i].port =
            url.port;

        workers[i].path =
            url.path;

        workers[i].use_tls =
            url.use_tls;

        workers[i].verify_certificate =
            verify_certificate != 0;

        workers[i].verify_hostname =
            verify_hostname != 0;

        workers[i].requests =
            base_requests +
            (i < extra_requests ? 1 : 0);

        workers[i].delay_ms =
            delay_ms;

        workers[i].addresses =
            addresses;

        workers[i].ssl_ctx =
            ssl_ctx;

        workers[i].callback =
            callback;

        workers[i].local_ok = 0;
        workers[i].local_err = 0;

        if (stop_requested()) {
            break;
        }

        if (
            pthread_create(
                &threads[i],
                NULL,
                http_worker,
                &workers[i]
            ) != 0
        ) {
            /*
             * Do NOT fabricate completed callbacks for work that
             * never had an attempt. The final result therefore
             * represents real request outcomes.
             *
             * We continue creating the remaining workers so a single
             * pthread_create failure doesn't automatically destroy
             * the entire run.
             */
            continue;
        }

        thread_created[i] = true;
        created_threads++;
    }

    if (created_threads == 0) {
        restore_sigint_handler();
        pthread_mutex_unlock(&g_run_mutex);

        free(threads);
        free(thread_created);
        free(workers);
        SSL_CTX_free(ssl_ctx);
        freeaddrinfo(addresses);
        free_url_parts(&url);

        return result;
    }

    /*
     * Join worker threads. Workers themselves contain all the
     * cancellation checks, including TLS handshake, SSL_write,
     * SSL_read, plain send/recv, connect polling, and delay sleep.
     */
    for (
        int i = 0;
        i < actual_threads;
        ++i
    ) {
        if (!thread_created[i]) {
            continue;
        }

        (void)pthread_join(
            threads[i],
            NULL
        );

        result.total_ok +=
            workers[i].local_ok;

        result.total_err +=
            workers[i].local_err;
    }

    result.elapsed =
        get_current_time_sec() -
        start_time;

    if (result.elapsed < 0.0) {
        result.elapsed = 0.0;
    }

    restore_sigint_handler();

    free(threads);
    free(thread_created);
    free(workers);

    SSL_CTX_free(ssl_ctx);
    freeaddrinfo(addresses);
    free_url_parts(&url);

    pthread_mutex_unlock(
        &g_run_mutex
    );

    return result;
}

/* ---------------------------------------------------------
 * Public APIs
 * --------------------------------------------------------- */

/*
 * Extended API:
 *
 * verify_certificate:
 *     1 = validate the peer certificate chain.
 *     0 = do not authenticate the peer certificate.
 *
 * verify_hostname:
 *     1 = require the certificate identity to match the URL host.
 *     0 = skip hostname matching.
 *
 * Note:
 * If verify_certificate == 0, verify_hostname does not add peer
 * authentication because certificate verification itself is disabled.
 */

EXPORT TestResult httpS_DoS(
    const char *url_string,
    int64_t total_requests,
    int num_threads,
    ProgressCallback callback,
    int delay_ms,
    int verify_certificate,
    int verify_hostname
)
{
    return run_http_stress_test_impl(
        url_string,
        total_requests,
        num_threads,
        callback,
        delay_ms,
        verify_certificate,
        verify_hostname
    );
}
