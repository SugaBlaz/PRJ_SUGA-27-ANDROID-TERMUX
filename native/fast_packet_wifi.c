#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#include <pthread.h>
#include <sched.h>


/* =========================================================
 * Configuration
 * ========================================================= */

#define PACKET_BATCH_SIZE        16
#define COUNTER_FLUSH_THRESHOLD 1000
#define RETRY_SLEEP_MS           1

/*
 * Maximum UDP payload for IPv4.
 */
#define MAX_UDP_PAYLOAD 65507


/* =========================================================
 * CPU helper
 * ========================================================= */

#define cpu_yield() sched_yield()


/* =========================================================
 * Atomic helpers
 * ========================================================= */

#if defined(__GNUC__) || defined(__clang__)

    #define ATOMIC_ADD(ptr, val) \
        __sync_fetch_and_add((ptr), (val))

    #define ATOMIC_READ(ptr) \
        __sync_fetch_and_add((ptr), 0)

#else

    #error "Atomic operations are not supported by this compiler"

#endif


/* =========================================================
 * Global state
 * ========================================================= */

/*
 * Set by SIGINT.
 *
 * volatile sig_atomic_t is specifically appropriate for
 * communicating between a signal handler and normal code.
 */
static volatile sig_atomic_t g_stop_requested = 0;


/*
 * Global successful-send counter.
 */
static int64_t g_total_sent = 0;


/*
 * Number of workers that have completely exited.
 */
static int g_workers_finished = 0;


/*
 * Number of workers that failed during startup.
 */
static int g_worker_failures = 0;


/*
 * Previous SIGINT handler.
 *
 * We restore Python's original handler after the C function
 * finishes so the rest of the CLI keeps working normally.
 */
static struct sigaction g_previous_sigint_action;
static bool g_previous_sigint_valid = false;


/* =========================================================
 * SIGINT handler
 * ========================================================= */

static void handle_sigint(int signal_number)
{
    (void)signal_number;

    /*
     * Do NOT call printf(), malloc(), free(), etc. here.
     *
     * Just set the flag.
     */
    g_stop_requested = 1;
}


/* =========================================================
 * Install SIGINT handler
 * ========================================================= */

static int install_sigint_handler(void)
{
    struct sigaction action;

    memset(
        &action,
        0,
        sizeof(action)
    );

    action.sa_handler = handle_sigint;

    sigemptyset(
        &action.sa_mask
    );

    /*
     * Do not use SA_RESTART.
     *
     * This is useful because an interrupted blocking syscall
     * such as send() should be allowed to return EINTR.
     */
    action.sa_flags = 0;

    if (
        sigaction(
            SIGINT,
            &action,
            &g_previous_sigint_action
        ) != 0
    )
    {
        return -1;
    }

    g_previous_sigint_valid = true;

    return 0;
}


/* =========================================================
 * Restore previous SIGINT handler
 * ========================================================= */

static void restore_sigint_handler(void)
{
    if (!g_previous_sigint_valid)
        return;

    sigaction(
        SIGINT,
        &g_previous_sigint_action,
        NULL
    );

    g_previous_sigint_valid = false;
}


/* =========================================================
 * Sleep helper
 * ========================================================= */

static void sleep_ms(unsigned int milliseconds)
{
    struct timespec requested;

    requested.tv_sec =
        milliseconds / 1000;

    requested.tv_nsec =
        (long)(milliseconds % 1000)
        * 1000000L;

    while (
        nanosleep(
            &requested,
            &requested
        ) != 0
    )
    {
        if (errno == EINTR)
        {
            if (g_stop_requested)
                return;

            continue;
        }

        break;
    }
}


/* =========================================================
 * Thread arguments
 * ========================================================= */

typedef struct
{
    char ip[64];

    int port;

    /*
     * Number of packets assigned to THIS worker.
     */
    int64_t packets_to_send;

    int payload_size;

    int thread_id;

} ThreadArgs;


/* =========================================================
 * Progress renderer
 * ========================================================= */

static void print_progress(
    int64_t current,
    int64_t total
)
{
    if (total <= 0)
        return;

    if (current < 0)
        current = 0;

    if (current > total)
        current = total;

    const int bar_width = 40;

    double percentage =
        (double)current /
        (double)total;

    int filled =
        (int)(
            percentage *
            bar_width
        );

    if (filled > bar_width)
        filled = bar_width;

    printf("\r[");

    for (int i = 0; i < bar_width; i++)
    {
        if (i < filled)
        {
            printf("=");
        }
        else if (i == filled)
        {
            printf(">");
        }
        else
        {
            printf(" ");
        }
    }

    printf(
        "] %3.1f%% (%lld/%lld)",
        percentage * 100.0,
        (long long)current,
        (long long)total
    );

    fflush(stdout);
}


/* =========================================================
 * Successful-send helper
 * ========================================================= */

static inline int safe_send(
    int sock,
    const char *payload,
    int payload_size
)
{
    /*
     * Check before attempting the syscall.
     */
    if (g_stop_requested)
        return 0;

    ssize_t sent =
        send(
            sock,
            payload,
            (size_t)payload_size,
            0
        );

    /*
     * UDP sends should be all-or-nothing.
     *
     * Only count the datagram if the entire payload was
     * accepted by the kernel.
     */
    if (sent == payload_size)
    {
        return 1;
    }

    /*
     * Interrupted by Ctrl+C.
     */
    if (
        sent < 0 &&
        errno == EINTR &&
        g_stop_requested
    )
    {
        return 0;
    }

    /*
     * Do not spin at 100% CPU if the socket temporarily
     * cannot accept another datagram.
     */
    cpu_yield();

    return 0;
}


/* =========================================================
 * Worker-finished helper
 * ========================================================= */

static void worker_finished(void)
{
    ATOMIC_ADD(
        &g_workers_finished,
        1
    );
}


/* =========================================================
 * Worker thread
 * ========================================================= */

static void *worker_thread(void *argument)
{
    ThreadArgs *args =
        (ThreadArgs *)argument;

    if (args == NULL)
    {
        ATOMIC_ADD(
            &g_worker_failures,
            1
        );

        worker_finished();

        return NULL;
    }


    /* -----------------------------------------------------
     * Create UDP socket
     * ----------------------------------------------------- */

    int sock =
        socket(
            AF_INET,
            SOCK_DGRAM,
            IPPROTO_UDP
        );

    if (sock < 0)
    {
        ATOMIC_ADD(
            &g_worker_failures,
            1
        );

        worker_finished();

        return NULL;
    }


    /* -----------------------------------------------------
     * Increase send buffer
     * ----------------------------------------------------- */

    int sndbuf =
        32 * 1024 * 1024;

    setsockopt(
        sock,
        SOL_SOCKET,
        SO_SNDBUF,
        (const char *)&sndbuf,
        sizeof(sndbuf)
    );


    /* -----------------------------------------------------
     * Target address
     * ----------------------------------------------------- */

    struct sockaddr_in target;

    memset(
        &target,
        0,
        sizeof(target)
    );

    target.sin_family =
        AF_INET;

    target.sin_port =
        htons(args->port);

    if (
        inet_pton(
            AF_INET,
            args->ip,
            &target.sin_addr
        ) != 1
    )
    {
        close(sock);

        ATOMIC_ADD(
            &g_worker_failures,
            1
        );

        worker_finished();

        return NULL;
    }


    /* -----------------------------------------------------
     * Connect UDP socket
     * ----------------------------------------------------- */

    if (
        connect(
            sock,
            (struct sockaddr *)&target,
            sizeof(target)
        ) != 0
    )
    {
        close(sock);

        ATOMIC_ADD(
            &g_worker_failures,
            1
        );

        worker_finished();

        return NULL;
    }


    /* -----------------------------------------------------
     * Payload
     * ----------------------------------------------------- */

    int payload_size =
        args->payload_size;

    if (
        payload_size <= 0 ||
        payload_size > MAX_UDP_PAYLOAD
    )
    {
        payload_size =
            MAX_UDP_PAYLOAD;
    }

    char *payload =
        malloc(
            (size_t)payload_size
        );

    if (payload == NULL)
    {
        close(sock);

        ATOMIC_ADD(
            &g_worker_failures,
            1
        );

        worker_finished();

        return NULL;
    }

    memset(
        payload,
        'X',
        (size_t)payload_size
    );


    /* -----------------------------------------------------
     * Transmission state
     * ----------------------------------------------------- */

    int64_t remaining =
        args->packets_to_send;

    int64_t local_sent = 0;


    /* -----------------------------------------------------
     * Main batch loop
     * ----------------------------------------------------- */

    while (
        remaining >= PACKET_BATCH_SIZE &&
        !g_stop_requested
    )
    {
        int successful = 0;


        /*
         * Send a small batch.
         *
         * Every iteration checks g_stop_requested so a
         * Ctrl+C does not leave this worker running.
         */
        for (
            int i = 0;
            i < PACKET_BATCH_SIZE;
            i++
        )
        {
            if (g_stop_requested)
                break;

            successful +=
                safe_send(
                    sock,
                    payload,
                    payload_size
                );
        }


        /*
         * Only successful datagrams count.
         */
        remaining -= successful;
        local_sent += successful;


        /*
         * Periodically update the global atomic counter.
         */
        if (
            local_sent >=
            COUNTER_FLUSH_THRESHOLD
        )
        {
            ATOMIC_ADD(
                &g_total_sent,
                local_sent
            );

            local_sent = 0;
        }


        /*
         * If every attempt failed and the user has not
         * pressed Ctrl+C, give the system a moment.
         */
        if (
            successful == 0 &&
            !g_stop_requested
        )
        {
            sleep_ms(
                RETRY_SLEEP_MS
            );
        }
    }


    /* -----------------------------------------------------
     * Remaining packets
     * ----------------------------------------------------- */

    while (
        remaining > 0 &&
        !g_stop_requested
    )
    {
        if (
            safe_send(
                sock,
                payload,
                payload_size
            )
        )
        {
            remaining--;
            local_sent++;

            if (
                local_sent >=
                COUNTER_FLUSH_THRESHOLD
            )
            {
                ATOMIC_ADD(
                    &g_total_sent,
                    local_sent
                );

                local_sent = 0;
            }
        }
        else
        {
            sleep_ms(
                RETRY_SLEEP_MS
            );
        }
    }


    /* -----------------------------------------------------
     * Flush local counter
     * ----------------------------------------------------- */

    if (local_sent > 0)
    {
        ATOMIC_ADD(
            &g_total_sent,
            local_sent
        );
    }


    /* -----------------------------------------------------
     * Cleanup
     * ----------------------------------------------------- */

    free(payload);

    close(sock);

    worker_finished();

    return NULL;
}


/* =========================================================
 * Packet generator
 *
 * max_i = TOTAL packets across ALL threads.
 *
 * Return values:
 *
 *     0 = completed normally
 *     1 = interrupted with Ctrl+C
 *    -1 = startup / worker failure
 * ========================================================= */

int start_packet_generator(
    const char *ip,
    int port,
    int64_t max_i,
    int num_threads,
    int payload_size
)
{
    /* -----------------------------------------------------
     * Validate arguments
     * ----------------------------------------------------- */

    if (ip == NULL)
        return -1;

    if (max_i <= 0)
        return -1;

    if (
        port < 1 ||
        port > 65535
    )
    {
        return -1;
    }

    if (num_threads < 1)
        num_threads = 1;

    /*
     * Never create more workers than packets.
     */
    if (
        (int64_t)num_threads >
        max_i
    )
    {
        num_threads =
            (int)max_i;
    }

    if (
        payload_size <= 0 ||
        payload_size > MAX_UDP_PAYLOAD
    )
    {
        payload_size =
            MAX_UDP_PAYLOAD;
    }


    /* -----------------------------------------------------
     * Reset global state
     * ----------------------------------------------------- */

    g_stop_requested = 0;

    g_total_sent = 0;

    g_workers_finished = 0;

    g_worker_failures = 0;


    /* -----------------------------------------------------
     * Allocate worker structures
     * ----------------------------------------------------- */

    ThreadArgs *args =
        calloc(
            (size_t)num_threads,
            sizeof(ThreadArgs)
        );

    pthread_t *threads =
        calloc(
            (size_t)num_threads,
            sizeof(pthread_t)
        );

    bool *thread_started =
        calloc(
            (size_t)num_threads,
            sizeof(bool)
        );

    if (
        args == NULL ||
        threads == NULL ||
        thread_started == NULL
    )
    {
        free(args);
        free(threads);
        free(thread_started);

        return -1;
    }


    /* -----------------------------------------------------
     * Install Ctrl+C handler
     * ----------------------------------------------------- */

    if (
        install_sigint_handler() != 0
    )
    {
        free(args);
        free(threads);
        free(thread_started);

        return -1;
    }


    /* -----------------------------------------------------
     * Distribute TOTAL packets
     * ----------------------------------------------------- */

    int64_t base_packets =
        max_i /
        (int64_t)num_threads;

    int64_t remainder =
        max_i %
        (int64_t)num_threads;


    int created_threads = 0;

    int64_t expected_total = 0;


    /* -----------------------------------------------------
     * Create worker threads
     * ----------------------------------------------------- */

    for (
        int i = 0;
        i < num_threads;
        i++
    )
    {
        memset(
            &args[i],
            0,
            sizeof(ThreadArgs)
        );


        strncpy(
            args[i].ip,
            ip,
            sizeof(args[i].ip) - 1
        );

        args[i].ip[
            sizeof(args[i].ip) - 1
        ] = '\0';


        args[i].port =
            port;


        /*
         * Example:
         *
         * 25000 packets / 4 workers
         *
         * Worker 0 = 6250
         * Worker 1 = 6250
         * Worker 2 = 6250
         * Worker 3 = 6250
         */
        args[i].packets_to_send =
            base_packets +
            (
                i < remainder
                ? 1
                : 0
            );


        args[i].payload_size =
            payload_size;

        args[i].thread_id =
            i;


        /*
         * Do not start additional workers after
         * Ctrl+C has already been received.
         */
        if (g_stop_requested)
            break;


        int rc =
            pthread_create(
                &threads[i],
                NULL,
                worker_thread,
                &args[i]
            );

        if (rc != 0)
        {
            /*
             * Keep trying remaining workers so a single
             * pthread_create failure does not break the
             * entire run.
             */
            continue;
        }


        thread_started[i] = true;

        created_threads++;


        /*
         * Only successfully-created workers contribute
         * to the expected target.
         */
        expected_total +=
            args[i].packets_to_send;
    }


    /* -----------------------------------------------------
     * No workers started
     * ----------------------------------------------------- */

    if (created_threads == 0)
    {
        restore_sigint_handler();

        free(args);
        free(threads);
        free(thread_started);

        return -1;
    }


    /* -----------------------------------------------------
     * Progress loop
     * ----------------------------------------------------- */

    while (!g_stop_requested)
    {
        int64_t current_sent =
            ATOMIC_READ(
                &g_total_sent
            );

        print_progress(
            current_sent,
            expected_total
        );


        /*
         * All expected packets have been sent.
         */
        if (
            current_sent >=
            expected_total
        )
        {
            break;
        }


        /*
         * All workers stopped.
         *
         * This prevents the progress loop from waiting
         * forever if a worker fails during startup.
         */
        int finished =
            ATOMIC_READ(
                &g_workers_finished
            );

        if (
            finished >=
            created_threads
        )
        {
            break;
        }


        sleep_ms(20);
    }


    /* -----------------------------------------------------
     * Tell the workers to stop if Ctrl+C was pressed
     * ----------------------------------------------------- */

    if (g_stop_requested)
    {
        printf(
            "\n[!] Ctrl+C detected. "
            "Stopping worker threads...\n"
        );

        fflush(stdout);
    }


    /* -----------------------------------------------------
     * Join ONLY successfully-created threads
     * ----------------------------------------------------- */

    for (
        int i = 0;
        i < num_threads;
        i++
    )
    {
        if (!thread_started[i])
            continue;

        pthread_join(
            threads[i],
            NULL
        );
    }


    /* -----------------------------------------------------
     * Final progress
     * ----------------------------------------------------- */

    int64_t final_sent =
        ATOMIC_READ(
            &g_total_sent
        );

    print_progress(
        final_sent,
        expected_total
    );

    printf("\n");


    /* -----------------------------------------------------
     * Determine result
     * ----------------------------------------------------- */

    int result;

    if (g_stop_requested)
    {
        result = 1;
    }
    else if (
        g_worker_failures > 0 ||
        final_sent < expected_total
    )
    {
        result = -1;
    }
    else
    {
        result = 0;
    }


    /* -----------------------------------------------------
     * Restore Python's SIGINT handler
     * ----------------------------------------------------- */

    restore_sigint_handler();


    /* -----------------------------------------------------
     * Cleanup
     * ----------------------------------------------------- */

    free(args);
    free(threads);
    free(thread_started);


    return result;
}
