#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/deque.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
typedef SOCKET sock_t;
#  define INVALID_SOCK  INVALID_SOCKET
#  define close_sock    closesocket
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
typedef int sock_t;
#  define INVALID_SOCK  (-1)
#  define close_sock    close
#endif

#include "router-protocol.h"

#define JITTER_BUF_MAX_MS  200
#define JITTER_BUF_BYTES   ((size_t)(ROUTER_SAMPLE_RATE / 1000 * JITTER_BUF_MAX_MS \
                             * ROUTER_CHANNELS * sizeof(float)))

// Consider connection lost if no packet in this many ns
#define RECEIVER_STALE_NS  (3000000000ULL)

// -----------------------------------------------------------------------
// Per-source context
// -----------------------------------------------------------------------
struct receiver_ctx {
    obs_source_t   *source;
    int             track_id;
    uint16_t        listen_port;
    bool            enabled;

    // Network thread
    sock_t          sock;
    bool            sock_open;
    pthread_t       recv_thread;
    volatile bool   thread_running;

    // Jitter buffer (interleaved float stereo)
    struct deque    pcm_buf;
    pthread_mutex_t buf_mutex;

    // Status — written by recv thread, read by UI timer
    volatile uint64_t last_pkt_ns;       // time of last received packet
    volatile uint64_t packets_rx;
    volatile uint64_t packets_dropped;
    volatile uint64_t packets_rx_snap;
    volatile uint64_t snap_time_ns;
    volatile float    packets_per_sec;
    char              sender_ip[INET_ADDRSTRLEN]; // IP of last sender
    pthread_mutex_t   status_mutex;               // guards sender_ip
};

// -----------------------------------------------------------------------
// Receive thread
// -----------------------------------------------------------------------
static void *receiver_thread(void *data)
{
    struct receiver_ctx *ctx = (struct receiver_ctx *)data;
    static uint8_t pkt_buf[ROUTER_MAX_PACKET];

    obs_log(LOG_INFO, "[receiver] Thread started, port %d (track %d)",
            ctx->listen_port, ctx->track_id);

    while (ctx->thread_running) {
        struct sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        int n = (int)recvfrom(ctx->sock, (char *)pkt_buf, sizeof(pkt_buf), 0,
                              (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            os_sleep_ms(1);
            continue;
        }
        if (n < (int)ROUTER_HEADER_SIZE) continue;

        router_packet_header_t *hdr = (router_packet_header_t *)pkt_buf;
        if (ntohl(hdr->magic) != ROUTER_MAGIC)       continue;
        if (hdr->version      != ROUTER_VERSION)      continue;
        if (hdr->track_id     != (uint8_t)ctx->track_id) continue;

        uint32_t num_samples = ntohl(hdr->num_samples);
        uint32_t channels    = hdr->channels;
        uint32_t data_bytes  = ntohl(hdr->data_bytes);

        if ((int)(ROUTER_HEADER_SIZE + data_bytes) > n) continue;
        if (channels == 0 || num_samples == 0)          continue;

        // Record sender IP
        pthread_mutex_lock(&ctx->status_mutex);
        inet_ntop(AF_INET, &from.sin_addr,
                  ctx->sender_ip, sizeof(ctx->sender_ip));
        pthread_mutex_unlock(&ctx->status_mutex);

        // Update timing stats
        ctx->last_pkt_ns = os_gettime_ns();
        ctx->packets_rx++;

        // Update rate
        uint64_t elapsed = ctx->last_pkt_ns - ctx->snap_time_ns;
        if (elapsed >= 1000000000ULL) {
            uint64_t delta       = ctx->packets_rx - ctx->packets_rx_snap;
            ctx->packets_per_sec = (float)((double)delta / ((double)elapsed / 1e9));
            ctx->packets_rx_snap = ctx->packets_rx;
            ctx->snap_time_ns    = ctx->last_pkt_ns;
        }

        // Buffer management
        pthread_mutex_lock(&ctx->buf_mutex);

        // Drop oldest if buffer is overfull
        while (ctx->pcm_buf.size + data_bytes > JITTER_BUF_BYTES
               && ctx->pcm_buf.size > 0) {
            size_t drop = num_samples * ROUTER_CHANNELS * sizeof(float);
            if (drop > ctx->pcm_buf.size) drop = ctx->pcm_buf.size;
            deque_pop_front(&ctx->pcm_buf, NULL, drop);
            ctx->packets_dropped++;
        }

        float *payload = (float *)(pkt_buf + ROUTER_HEADER_SIZE);

        if (channels == 1) {
            // Mono -> stereo
            for (uint32_t i = 0; i < num_samples; i++) {
                float s = payload[i];
                deque_push_back(&ctx->pcm_buf, &s, sizeof(float));
                deque_push_back(&ctx->pcm_buf, &s, sizeof(float));
            }
        } else {
            deque_push_back(&ctx->pcm_buf, payload, data_bytes);
        }

        pthread_mutex_unlock(&ctx->buf_mutex);
    }

    obs_log(LOG_INFO, "[receiver] Thread stopped (track %d)", ctx->track_id);
    return NULL;
}

// -----------------------------------------------------------------------
// OBS audio tick — feeds buffered PCM into OBS
// -----------------------------------------------------------------------
static void receiver_tick(void *data, float seconds)
{
    UNUSED_PARAMETER(seconds);
    struct receiver_ctx *ctx = (struct receiver_ctx *)data;
    if (!ctx->enabled || !ctx->sock_open) return;

    const uint32_t want_samples = 1024;
    const size_t   want_bytes   = want_samples * ROUTER_CHANNELS * sizeof(float);

    pthread_mutex_lock(&ctx->buf_mutex);
    if (ctx->pcm_buf.size < want_bytes) {
        pthread_mutex_unlock(&ctx->buf_mutex);
        return;
    }

    static float interleaved[ROUTER_CHANNELS * 1024];
    deque_pop_front(&ctx->pcm_buf, interleaved, want_bytes);
    pthread_mutex_unlock(&ctx->buf_mutex);

    static float ch0[1024], ch1[1024];
    for (uint32_t i = 0; i < want_samples; i++) {
        ch0[i] = interleaved[i * 2 + 0];
        ch1[i] = interleaved[i * 2 + 1];
    }

    struct obs_source_audio out = {};
    out.data[0]         = (const uint8_t *)ch0;
    out.data[1]         = (const uint8_t *)ch1;
    out.frames          = want_samples;
    out.format          = AUDIO_FORMAT_FLOAT_PLANAR;
    out.samples_per_sec = ROUTER_SAMPLE_RATE;
    out.speakers        = SPEAKERS_STEREO;
    out.timestamp       = os_gettime_ns();
    obs_source_output_audio(ctx->source, &out);
}

// -----------------------------------------------------------------------
// Socket helpers
// -----------------------------------------------------------------------
static bool receiver_open_socket(struct receiver_ctx *ctx)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif
    ctx->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ctx->sock == INVALID_SOCK) {
        obs_log(LOG_WARNING, "[receiver] Failed to create socket");
        return false;
    }

#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(ctx->sock, FIONBIO, &nb);
#else
    fcntl(ctx->sock, F_SETFL, O_NONBLOCK);
#endif

    int reuse = 1;
    setsockopt(ctx->sock, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(ctx->listen_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(ctx->sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        obs_log(LOG_WARNING, "[receiver] Bind failed on port %d", ctx->listen_port);
        close_sock(ctx->sock);
        ctx->sock = INVALID_SOCK;
        return false;
    }

    ctx->sock_open       = true;
    ctx->last_pkt_ns     = 0;
    ctx->packets_rx      = 0;
    ctx->packets_dropped = 0;
    ctx->packets_rx_snap = 0;
    ctx->snap_time_ns    = os_gettime_ns();
    ctx->packets_per_sec = 0.0f;
    memset(ctx->sender_ip, 0, sizeof(ctx->sender_ip));

    ctx->thread_running = true;
    pthread_create(&ctx->recv_thread, NULL, receiver_thread, ctx);

    obs_log(LOG_INFO, "[receiver] Listening on port %d (track %d)",
            ctx->listen_port, ctx->track_id);
    return true;
}

static void receiver_close_socket(struct receiver_ctx *ctx)
{
    if (ctx->thread_running) {
        ctx->thread_running = false;
        pthread_join(ctx->recv_thread, NULL);
    }
    if (ctx->sock_open) {
        close_sock(ctx->sock);
        ctx->sock      = INVALID_SOCK;
        ctx->sock_open = false;
    }
}

static bool receiver_is_live(struct receiver_ctx *ctx)
{
    if (!ctx->enabled || !ctx->sock_open) return false;
    if (ctx->last_pkt_ns == 0) return false;
    return (os_gettime_ns() - ctx->last_pkt_ns) < RECEIVER_STALE_NS;
}

// -----------------------------------------------------------------------
// OBS source callbacks
// -----------------------------------------------------------------------
static const char *receiver_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "Audio Router: Receive Track";
}

static void *receiver_create(obs_data_t *settings, obs_source_t *source)
{
    struct receiver_ctx *ctx = (struct receiver_ctx *)bzalloc(sizeof(*ctx));
    ctx->source = source;
    ctx->sock   = INVALID_SOCK;

    pthread_mutex_init(&ctx->buf_mutex,    NULL);
    pthread_mutex_init(&ctx->status_mutex, NULL);
    deque_init(&ctx->pcm_buf);

    obs_data_set_default_int (settings, "track_id", 1);
    obs_data_set_default_bool(settings, "enabled",  true);

    ctx->track_id    = (int)obs_data_get_int(settings, "track_id");
    ctx->listen_port = (ctx->track_id == 1) ? ROUTER_PORT_TRACK1 : ROUTER_PORT_TRACK2;
    ctx->enabled     = obs_data_get_bool(settings, "enabled");

    if (ctx->enabled) receiver_open_socket(ctx);
    return ctx;
}

static void receiver_destroy(void *data)
{
    struct receiver_ctx *ctx = (struct receiver_ctx *)data;
    receiver_close_socket(ctx);
    deque_free(&ctx->pcm_buf);
    pthread_mutex_destroy(&ctx->buf_mutex);
    pthread_mutex_destroy(&ctx->status_mutex);
    bfree(ctx);
}

static void receiver_update(void *data, obs_data_t *settings)
{
    struct receiver_ctx *ctx = (struct receiver_ctx *)data;
    receiver_close_socket(ctx);
    deque_free(&ctx->pcm_buf);
    deque_init(&ctx->pcm_buf);

    ctx->track_id    = (int)obs_data_get_int(settings, "track_id");
    ctx->listen_port = (ctx->track_id == 1) ? ROUTER_PORT_TRACK1 : ROUTER_PORT_TRACK2;
    ctx->enabled     = obs_data_get_bool(settings, "enabled");

    if (ctx->enabled) receiver_open_socket(ctx);
}

// -----------------------------------------------------------------------
// Properties — live status via timer
// -----------------------------------------------------------------------
static bool receiver_status_timer(obs_properties_t *props,
                                   obs_property_t   *prop,
                                   void             *data)
{
    UNUSED_PARAMETER(prop);
    struct receiver_ctx *ctx = (struct receiver_ctx *)data;

    // Snapshot stats
    pthread_mutex_lock(&ctx->status_mutex);
    char sender_ip[INET_ADDRSTRLEN];
    snprintf(sender_ip, sizeof(sender_ip), "%s", ctx->sender_ip);
    pthread_mutex_unlock(&ctx->status_mutex);

    uint64_t rx      = ctx->packets_rx;
    uint64_t dropped = ctx->packets_dropped;
    float    pps     = ctx->packets_per_sec;

    // Connection status line
    char status[256];
    if (!ctx->enabled) {
        snprintf(status, sizeof(status), "Disabled");
    } else if (!ctx->sock_open) {
        snprintf(status, sizeof(status),
                 "ERROR: could not bind port %d  (already in use?)",
                 (int)ctx->listen_port);
    } else if (receiver_is_live(ctx)) {
        snprintf(status, sizeof(status),
                 "CONNECTED  |  %.0f pkt/s  <--  %s",
                 (double)pps, sender_ip);
    } else if (rx == 0) {
        snprintf(status, sizeof(status),
                 "Listening on port %d — waiting for Gaming PC...",
                 (int)ctx->listen_port);
    } else {
        snprintf(status, sizeof(status),
                 "DISCONNECTED  |  last packet >3s ago  (Gaming PC silent or stopped?)");
    }

    // Stats line
    char stats[128];
    snprintf(stats, sizeof(stats),
             "Packets received: %llu   Dropped (buffer full): %llu",
             (unsigned long long)rx, (unsigned long long)dropped);

    obs_property_t *p_status = obs_properties_get(props, "status_label");
    obs_property_t *p_stats  = obs_properties_get(props, "stats_label");
    if (p_status) obs_property_set_description(p_status, status);
    if (p_stats)  obs_property_set_description(p_stats,  stats);

    return true;
}

static obs_properties_t *receiver_get_properties(void *data)
{
    struct receiver_ctx *ctx = (struct receiver_ctx *)data;

    obs_properties_t *props = obs_properties_create();

    obs_properties_add_bool(props, "enabled", "Enable receiving");

    obs_property_t *track = obs_properties_add_list(props, "track_id",
        "Receive track", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(track, "Track 1  (UDP port 9001)", 1);
    obs_property_list_add_int(track, "Track 2  (UDP port 9002)", 2);

    obs_properties_add_text(props, "hint",
        "Make sure the Gaming PC's Windows Firewall allows UDP 9001/9002 outbound.",
        OBS_TEXT_INFO);

    // ---- Status section ----
    obs_properties_add_text(props, "sep",
        "------------------------------------", OBS_TEXT_INFO);

    // Initial status
    char initial[256];
    if (receiver_is_live(ctx)) {
        pthread_mutex_lock(&ctx->status_mutex);
        snprintf(initial, sizeof(initial),
                 "CONNECTED  |  %.0f pkt/s  <--  %s",
                 (double)ctx->packets_per_sec, ctx->sender_ip);
        pthread_mutex_unlock(&ctx->status_mutex);
    } else {
        snprintf(initial, sizeof(initial),
                 "Listening on port %d — waiting for Gaming PC...",
                 (int)ctx->listen_port);
    }
    obs_properties_add_text(props, "status_label", initial, OBS_TEXT_INFO);

    char stats_init[128];
    snprintf(stats_init, sizeof(stats_init),
             "Packets received: %llu   Dropped: %llu",
             (unsigned long long)ctx->packets_rx,
             (unsigned long long)ctx->packets_dropped);
    obs_properties_add_text(props, "stats_label", stats_init, OBS_TEXT_INFO);

    // Refresh timer — fires every 1000ms while panel is open
    obs_properties_add_timer(props, "status_timer", "Connection status",
                             receiver_status_timer, ctx, 1000);

    return props;
}

static void receiver_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_int (settings, "track_id", 1);
    obs_data_set_default_bool(settings, "enabled",  true);
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------
void register_audio_receiver_source(void)
{
    struct obs_source_info info = {};
    info.id             = "audio_router_receiver";
    info.type           = OBS_SOURCE_TYPE_INPUT;
    info.output_flags   = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name       = receiver_get_name;
    info.create         = receiver_create;
    info.destroy        = receiver_destroy;
    info.update         = receiver_update;
    info.video_tick     = receiver_tick;
    info.get_properties = receiver_get_properties;
    info.get_defaults   = receiver_get_defaults;

    obs_register_source(&info);
    obs_log(LOG_INFO, "Registered audio_router_receiver source");
}
