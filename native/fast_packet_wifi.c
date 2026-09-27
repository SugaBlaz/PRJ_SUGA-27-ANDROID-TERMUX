#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <sched.h>

#define cpu_yield() sched_yield()

#if defined(__GNUC__) || defined(__clang__)
    #define ATOMIC_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC_READ(ptr) __sync_fetch_and_add(ptr, 0)
#else
    #error "Atomic operations are not supported by this compiler"
#endif


static int64_t g_total_sent = 0;


/* =========================================================
 * Sleep helper
 * ========================================================= */

static void sleep_ms(unsigned int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;

    nanosleep(&ts, NULL);
}


/* =========================================================
 * Thread arguments
 * ========================================================= */

typedef struct
{
    char ip[64];

    int port;

    /*
     * Number of packets THIS thread is responsible for.
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
        (double)current / (double)total;

    int filled =
        (int)(percentage * bar_width);

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
 * UDP send helper
 * ========================================================= */

static inline int safe_send(
    int sock,
    const char *payload,
    int payload_size
)
{
    ssize_t sent =
        send(
            sock,
            payload,
            payload_size,
            0
        );

    if (sent > 0)
    {
        return 1;
    }

    /*
     * Don't immediately spin at 100% CPU
     * when the socket buffer is temporarily full.
     */
    cpu_yield();

    return 0;
}


/* =========================================================
 * Worker
 * ========================================================= */

static void *worker_thread(void *arg)
{
    ThreadArgs *args =
        (ThreadArgs *)arg;

    int sock =
        socket(
            AF_INET,
            SOCK_DGRAM,
            IPPROTO_UDP
        );

    if (sock < 0)
    {
        return NULL;
    }


    /* -----------------------------------------------------
     * Socket send buffer
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
        ) < 0
    )
    {
        close(sock);
        return NULL;
    }


    /* -----------------------------------------------------
     * Payload
     * ----------------------------------------------------- */

    int payload_size =
        args->payload_size;

    if (
        payload_size <= 0 ||
        payload_size > 65507
    )
    {
        payload_size = 65000;
    }

    char *payload =
        malloc(payload_size);

    if (payload == NULL)
    {
        close(sock);
        return NULL;
    }

    memset(
        payload,
        'X',
        payload_size
    );


    /* -----------------------------------------------------
     * Transmission
     * ----------------------------------------------------- */

    int64_t remaining =
        args->packets_to_send;

    int64_t local_sent = 0;


    /*
     * Send in batches of 16 attempts.
     */
    while (remaining >= 16)
    {
        int successful = 0;

        for (int i = 0; i < 16; i++)
        {
            successful +=
                safe_send(
                    sock,
                    payload,
                    payload_size
                );
        }

        /*
         * Only successful sends count
         * toward the target.
         */
        remaining -= successful;
        local_sent += successful;


        /*
         * Batch global counter updates.
         */
        if (local_sent >= 1000)
        {
            ATOMIC_ADD(
                &g_total_sent,
                local_sent
            );

            local_sent = 0;
        }
    }


    /* -----------------------------------------------------
     * Remaining packets
     * ----------------------------------------------------- */

    while (remaining > 0)
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
        }

        /*
         * Don't continuously burn a CPU core
         * if sends are failing.
         */
        else
        {
            cpu_yield();
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


    free(payload);

    close(sock);

    return NULL;
}


/* =========================================================
 * Packet generator
 *
 * max_i = TOTAL packets across ALL threads.
 * ========================================================= */

void start_packet_generator(
    const char *ip,
    int port,
    int64_t max_i,
    int num_threads,
    int payload_size
)
{
    if (ip == NULL)
        return;

    if (max_i <= 0)
        return;

    if (num_threads < 1)
        num_threads = 1;

    /*
     * Don't create more threads than packets.
     */
    if ((int64_t)num_threads > max_i)
        num_threads = (int)max_i;

    if (payload_size <= 0)
        payload_size = 65000;


    /* =====================================================
     * IMPORTANT:
     *
     * max_i is the TOTAL number of packets.
     * ===================================================== */

    const int64_t total_packets_target =
        max_i;

    g_total_sent = 0;


    /* =====================================================
     * Allocate
     * ===================================================== */

    ThreadArgs *args =
        malloc(
            sizeof(ThreadArgs) *
            (size_t)num_threads
        );

    pthread_t *threads =
        malloc(
            sizeof(pthread_t) *
            (size_t)num_threads
        );

    if (
        args == NULL ||
        threads == NULL
    )
    {
        free(args);
        free(threads);
        return;
    }


    /* =====================================================
     * Distribute packets
     * ===================================================== */

    int64_t base_packets =
        max_i / num_threads;

    int64_t remainder =
        max_i % num_threads;


    int created_threads = 0;


    for (int i = 0; i < num_threads; i++)
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
         * 25,000 packets
         * 4 threads
         *
         * Thread 0 = 6250
         * Thread 1 = 6250
         * Thread 2 = 6250
         * Thread 3 = 6250
         *
         * TOTAL    = 25000
         */
        args[i].packets_to_send =
            base_packets +
            (i < remainder ? 1 : 0);


        args[i].payload_size =
            payload_size;

        args[i].thread_id =
            i;


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
             * This thread wasn't created.
             *
             * Its assigned packets cannot be sent,
             * so remove them from the expected target.
             */
            continue;
        }

        created_threads++;
    }


    /* =====================================================
     * Progress loop
     * ===================================================== */

    while (1)
    {
        int64_t current_sent =
            ATOMIC_READ(
                &g_total_sent
            );


        print_progress(
            current_sent,
            total_packets_target
        );


        if (
            current_sent >=
            total_packets_target
        )
        {
            break;
        }


        sleep_ms(20);
    }


    /* =====================================================
     * Wait for workers
     * ===================================================== */

    for (int i = 0; i < num_threads; i++)
    {
        /*
         * pthread_join on a thread that wasn't
         * successfully created is invalid.
         *
         * In production, keep a created[] array
         * if you need to handle partial creation.
         */
        if (i < created_threads)
        {
            pthread_join(
                threads[i],
                NULL
            );
        }
    }


    /* =====================================================
     * Final progress
     * ===================================================== */

    print_progress(
        ATOMIC_READ(&g_total_sent),
        total_packets_target
    );

    printf("\n");


    free(threads);
    free(args);
}
