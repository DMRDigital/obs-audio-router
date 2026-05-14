#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <util/threading.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
typedef SOCKET sock_t;
#  define INVALID_SOCK INVALID_SOCKET
#  define close_sock   closesocket
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
typedef int sock_t;
#  define INVALID_SOCK (-1)
#  define close_sock   close
#endif

#include "router-protocol.h"

// How long without a successful send before we consider the link "stale" (ns)
#define SENDER_STALE_NS  (3000000000ULL)   // 3 seconds

// -----------------------------------------------------------------------
// Per-filter context
// -----------------------------------------------------------------------
struct sender_ctx {
    obs_source_t       *source;
    char                dest_ip[64];
    int                 track_id;
    uint16_t            dest_port;

    sock_t              sock;
    struct sockaddr_in  addr;
    bool                sock_open;

    uint32_t            sequence;
    bool                enabled;

    // Status tracking — written by audio thread, read by UI timer
    volatile uint64_t   last_send_ns;
    volatile uint64_t   packets_sent;
    volatile uint64_t   packets_sent_snap;
    volatile uint64_t   snap_time_ns;
    volatile float      packets_per_sec;
};

// -----------------------------------------------------------------------
// Socket helpers
// -----------------------------------------------------------------------
static bool sender_open_socket(struct sender_ctx *ctx)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif
    ctx->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ctx->sock == INVALID_SOCK) {
        obs_log(LOG_WARNING, "[sender] Failed to create UDP socket");
        return false;
    }
    int bcast = 1;
    setsockopt(ctx->sock, SOL_SOCKET, SO_BROADCAST,
               (const char *)&bcast, sizeof(bcast));

    memset(&ctx->addr, 0, sizeof(ctx->addr));
    ctx->addr.sin_family = AF_INET;
    ctx->addr.sin_port   = htons(ctx->dest_port);
    inet_pton(AF_INET, ctx->dest_ip, &ctx->addr.sin_addr);

    ctx->sock_open         = true;
    ctx->last_send_ns      = 0;
    ctx->packets_sent      = 0;
    ctx->packets_sent_snap = 0;
    ctx->snap_time_ns      = os_gettime_ns();
    ctx->packets_per_sec   = 0.0f;

    obs_log(LOG_INFO, "[sender] Socket open -> %s:%d (track %d)",
            ctx->dest_ip, ctx->dest_port, ctx->track_id);
    return true;
}

static void sender_close_socket(struct sender_ctx *ctx)
{
    if (ctx->sock_open) {
        close_sock(ctx->sock);
        ctx->sock      = INVALID_SOCK;
        ctx->sock_open = false;
    }
}

// -----------------------------------------------------------------------
// Status helpers
// -----------------------------------------------------------------------
static bool sender_is_live(struct sender_ctx *ctx)
{
    if (!ctx->enabled || !ctx->sock_open) return false;
    if (ctx->last_send_ns == 0) return false;
    return (os_gettime_ns() - ctx->last_send_ns) < SENDER_STALE_NS;
}

static void sender_update_rate(struct sender_ctx *ctx)
{
    uint64_t now     = os_gettime_ns();
    uint64_t elapsed = now - ctx->snap_time_ns;
    if (elapsed >= 1000000000ULL) {
        uint64_t delta        = ctx->packets_sent - ctx->packets_sent_snap;
        ctx->packets_per_sec  = (float)((double)delta / ((double)elapsed / 1e9));
        ctx->packets_sent_snap = ctx->packets_sent;
        ctx->snap_time_ns      = now;
    }
}

// -----------------------------------------------------------------------
// OBS filter callbacks
// -----------------------------------------------------------------------
static const char *sender_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "Audio Router: Send Track";
}

static void *sender_create(obs_data_t *settings, obs_source_t *source)
{
    struct sender_ctx *ctx = (struct sender_ctx *)bzalloc(sizeof(*ctx));
    ctx->source = source;
    ctx->sock   = INVALID_SOCK;

    obs_data_set_default_string(settings, "dest_ip",  "192.168.1.100");
    obs_data_set_default_int   (settings, "track_id", 1);
    obs_data_set_default_bool  (settings, "enabled",  true);

    const char *ip = obs_data_get_string(settings, "dest_ip");
    snprintf(ctx->dest_ip, sizeof(ctx->dest_ip), "%s", ip);
    ctx->track_id  = (int)obs_data_get_int(settings, "track_id");
    ctx->dest_port = (ctx->track_id == 1) ? ROUTER_PORT_TRACK1 : ROUTER_PORT_TRACK2;
    ctx->enabled   = obs_data_get_bool(settings, "enabled");

    if (ctx->enabled) sender_open_socket(ctx);
    return ctx;
}

static void sender_destroy(void *data)
{
    struct sender_ctx *ctx = (struct sender_ctx *)data;
    sender_close_socket(ctx);
    bfree(ctx);
}

static void sender_update(void *data, obs_data_t *settings)
{
    struct sender_ctx *ctx = (struct sender_ctx *)data;
    sender_close_socket(ctx);

    const char *ip = obs_data_get_string(settings, "dest_ip");
    snprintf(ctx->dest_ip, sizeof(ctx->dest_ip), "%s", ip);
    ctx->track_id  = (int)obs_data_get_int(settings, "track_id");
    ctx->dest_port = (ctx->track_id == 1) ? ROUTER_PORT_TRACK1 : ROUTER_PORT_TRACK2;
    ctx->enabled   = obs_data_get_bool(settings, "enabled");

    if (ctx->enabled) sender_open_socket(ctx);
}

static struct obs_audio_data *sender_filter_audio(void *data,
                                                   struct obs_audio_data *audio)
{
    struct sender_ctx *ctx = (struct sender_ctx *)data;
    if (!ctx->enabled || !ctx->sock_open || audio->frames == 0)
        return audio;

    static uint8_t pkt_buf[ROUTER_MAX_PACKET];
    router_packet_header_t *hdr = (router_packet_header_t *)pkt_buf;

    uint32_t frames     = audio->frames;
    uint32_t ch         = ROUTER_CHANNELS;
    uint32_t data_bytes = ch * frames * (uint32_t)sizeof(float);

    if (ROUTER_HEADER_SIZE + data_bytes > ROUTER_MAX_PACKET) {
        frames     = (uint32_t)((ROUTER_MAX_PACKET - ROUTER_HEADER_SIZE)
                                / (ch * sizeof(float)));
        data_bytes = ch * frames * (uint32_t)sizeof(float);
    }

    hdr->magic        = htonl(ROUTER_MAGIC);
    hdr->version      = ROUTER_VERSION;
    hdr->track_id     = (uint8_t)ctx->track_id;
    hdr->channels     = (uint8_t)ch;
    hdr->reserved     = 0;
    hdr->sample_rate  = htonl(ROUTER_SAMPLE_RATE);
    hdr->num_samples  = htonl(frames);
    hdr->timestamp_ns = os_gettime_ns();
    hdr->sequence     = htonl(ctx->sequence++);
    hdr->data_bytes   = htonl(data_bytes);

    float *dst = (float *)(pkt_buf + ROUTER_HEADER_SIZE);
    for (uint32_t i = 0; i < frames; i++) {
        for (uint32_t c = 0; c < ch; c++) {
            float *plane = (float *)audio->data[c];
            dst[i * ch + c] = plane ? plane[i] : 0.0f;
        }
    }

    int sent = (int)sendto(ctx->sock, (const char *)pkt_buf,
                           (int)(ROUTER_HEADER_SIZE + data_bytes), 0,
                           (struct sockaddr *)&ctx->addr, sizeof(ctx->addr));
    if (sent > 0) {
        ctx->last_send_ns = os_gettime_ns();
        ctx->packets_sent++;
        sender_update_rate(ctx);
    }

    return audio;
}

// -----------------------------------------------------------------------
// Properties UI — timer refreshes status label every second
// -----------------------------------------------------------------------
static bool sender_status_timer(obs_properties_t *props,
                                 obs_property_t   *prop,
                                 void             *data)
{
    UNUSED_PARAMETER(prop);
    struct sender_ctx *ctx = (struct sender_ctx *)data;

    char status[256];
    if (!ctx->enabled) {
        snprintf(status, sizeof(status), "Disabled");
    } else if (!ctx->sock_open) {
        snprintf(status, sizeof(status), "ERROR: socket could not open (check OBS log)");
    } else if (sender_is_live(ctx)) {
        snprintf(status, sizeof(status),
                 "LIVE  |  %.0f pkt/s  -->  %s : %d",
                 (double)ctx->packets_per_sec,
                 ctx->dest_ip, (int)ctx->dest_port);
    } else if (ctx->packets_sent == 0) {
        snprintf(status, sizeof(status),
                 "Waiting — play audio on this source to start sending");
    } else {
        snprintf(status, sizeof(status),
                 "Stale — no audio in last 3s  (source silent or muted?)");
    }

    obs_property_t *p = obs_properties_get(props, "status_label");
    if (p) obs_property_set_description(p, status);

    return true;  // tell OBS to redraw the panel
}

static obs_properties_t *sender_get_properties(void *data)
{
    struct sender_ctx *ctx = (struct sender_ctx *)data;

    obs_properties_t *props = obs_properties_create();

    obs_properties_add_bool(props, "enabled", "Enable sending");

    obs_property_t *ip = obs_properties_add_text(props, "dest_ip",
        "Streaming PC IP address", OBS_TEXT_DEFAULT);
    obs_property_set_long_description(ip,
        "Local network IP of your streaming PC.\n"
        "Find it by running  ipconfig  on the streaming PC.\n"
        "Usually starts with 192.168.x.x or 10.0.x.x");

    obs_property_t *track = obs_properties_add_list(props, "track_id",
        "Send as track", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(track, "Track 1  (UDP port 9001)", 1);
    obs_property_list_add_int(track, "Track 2  (UDP port 9002)", 2);

    // ---- Status section ----
    obs_properties_add_text(props, "sep",
        "------------------------------------", OBS_TEXT_INFO);

    // Build initial status text
    char initial[256];
    if (sender_is_live(ctx)) {
        snprintf(initial, sizeof(initial),
                 "LIVE  |  %.0f pkt/s  -->  %s : %d",
                 (double)ctx->packets_per_sec, ctx->dest_ip, (int)ctx->dest_port);
    } else {
        snprintf(initial, sizeof(initial), "Waiting for audio...");
    }
    obs_properties_add_text(props, "status_label", initial, OBS_TEXT_INFO);

    // Timer fires every 1000 ms and calls sender_status_timer
    obs_properties_add_timer(props, "status_timer", "Connection status",
                             sender_status_timer, ctx, 1000);

    return props;
}

static void sender_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, "dest_ip",  "192.168.1.100");
    obs_data_set_default_int   (settings, "track_id", 1);
    obs_data_set_default_bool  (settings, "enabled",  true);
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------
void register_audio_sender_filter(void)
{
    struct obs_source_info info = {};
    info.id             = "audio_router_sender";
    info.type           = OBS_SOURCE_TYPE_FILTER;
    info.output_flags   = OBS_SOURCE_AUDIO;
    info.get_name       = sender_get_name;
    info.create         = sender_create;
    info.destroy        = sender_destroy;
    info.update         = sender_update;
    info.filter_audio   = sender_filter_audio;
    info.get_properties = sender_get_properties;
    info.get_defaults   = sender_get_defaults;

    obs_register_source(&info);
    obs_log(LOG_INFO, "Registered audio_router_sender filter");
}
