#pragma once
#include <stdint.h>

// Default ports for each track (UDP)
#define ROUTER_PORT_TRACK1  9001
#define ROUTER_PORT_TRACK2  9002
#define ROUTER_MAGIC        0x4F415200  // "OAR\0"
#define ROUTER_VERSION      1
#define ROUTER_MAX_SAMPLES  4096
#define ROUTER_CHANNELS     2           // stereo
#define ROUTER_SAMPLE_RATE  48000

// Packet sent over UDP for each audio chunk
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;         // ROUTER_MAGIC
    uint8_t  version;       // ROUTER_VERSION
    uint8_t  track_id;      // 1 or 2
    uint8_t  channels;      // number of channels
    uint8_t  reserved;
    uint32_t sample_rate;
    uint32_t num_samples;   // samples per channel in this packet
    uint64_t timestamp_ns;  // sender monotonic clock (ns)
    uint32_t sequence;      // wrapping sequence number
    uint32_t data_bytes;    // byte length of PCM data that follows
    // followed by: float32 interleaved PCM  [channels * num_samples floats]
} router_packet_header_t;
#pragma pack(pop)

#define ROUTER_HEADER_SIZE  sizeof(router_packet_header_t)
#define ROUTER_MAX_PACKET   (ROUTER_HEADER_SIZE + ROUTER_CHANNELS * ROUTER_MAX_SAMPLES * sizeof(float))
