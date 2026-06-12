// espnow_link.h — wire contract shared between the base-station S3 (sender)
// and the handset C6 (receiver). KEEP THIS FILE IDENTICAL on both sides.
//
// Phase C bring-up payload. ESP-NOW max single-frame payload is 250 bytes;
// every struct here is well under that. __attribute__((packed)) so the byte
// layout is identical regardless of compiler padding on either MCU.
//
// ---- frame dispatch ----
// Every inbound ESP-NOW frame is classified by its FIRST BYTE, not by length
// (lengths can collide as the protocol grows). The four frame families use
// disjoint first-byte ranges:
//   base_event_t.type  : 0..4              (HEARTBEAT / OFF_HOOK / ... / NUMBER)
//   base_ack_t.magic   : 0xAC
//   pair_frame_t.kind  : 0xD0 (DISCOVERY) / 0xD1 (PAIR_REPLY)
// A receiver peeks data[0], matches it to a family, then re-checks len before
// memcpy. This makes the handshake robust even though several structs differ
// only by a few bytes.
//
// ---- channel-sync handshake (Phase-C milestone 2) ----
// The C6 is a real WiFi STA (channel follows the router, usually != 1). The S3
// runs ESP-NOW only and cannot pick a channel freely — it must find and lock
// the C6's channel. Handshake:
//   S3 (SCAN): for ch in 1..13: set_channel(ch); broadcast DISCOVERY; wait.
//   C6:        on DISCOVERY -> unicast PAIR_REPLY (carries its own channel) to
//              the DISCOVERY's src_addr.
//   S3:        on PAIR_REPLY -> learn C6 MAC from src_addr + lock that channel
//              -> PAIRED. Resume unicast heartbeats; C6 acks as before.
//   S3:        if no ack for N seconds while PAIRED -> fall back to SCAN
//              (covers C6 reboot / channel change).
// Peer MAC is LEARNED at pairing, never hardcoded — so swapping in a different
// physical C6 later needs zero firmware change.

#ifndef ESPNOW_LINK_H_
#define ESPNOW_LINK_H_

#include <stdint.h>

// Initial channel the C6 rxtest starts on before it associates to WiFi, and
// the channel the S3 begins its scan from. After pairing this is irrelevant —
// the real channel is discovered dynamically.
#define ESPNOW_LINK_CHANNEL 1

// Channel scan range for the S3 pairing state machine. 2.4GHz channels 1..13
// are valid in CN/EU; the C6's STA channel will be one of these.
#define ESPNOW_SCAN_CH_MIN 1
#define ESPNOW_SCAN_CH_MAX 13

// ---- event frames (S3 -> C6) ----

enum {
    BASE_EVT_HEARTBEAT = 0,
    BASE_EVT_OFF_HOOK = 1,
    BASE_EVT_ON_HOOK = 2,
    BASE_EVT_DIGIT = 3,
    BASE_EVT_NUMBER = 4,
};

typedef struct __attribute__((packed)) {
    uint8_t type;        // one of BASE_EVT_* (0..4)
    uint8_t digit;       // BASE_EVT_DIGIT: the dialed digit 0-9.
                         // BASE_EVT_HEARTBEAT: current hook level — 1 = off-hook
                         //   (handset lifted), 0 = on-hook (on cradle). The C6
                         //   reconciles its conversation state against this each
                         //   heartbeat, so a dropped OFF_HOOK/ON_HOOK edge still
                         //   converges (level-style, not edge-only).
    char number[33];     // valid when type == BASE_EVT_NUMBER (NUL-terminated)
    uint32_t seq;        // monotonic per-frame counter
} base_event_t;

// ---- ack frame (C6 -> S3) ----

typedef struct __attribute__((packed)) {
    uint8_t magic;       // 0xAC — distinguishes ack from a base_event_t
    uint32_t ack_seq;    // echoes the base_event_t.seq being acked
} base_ack_t;

#define BASE_ACK_MAGIC 0xAC

// ---- pairing / channel-sync frames ----
//
// DISCOVERY  : S3 -> broadcast (ff:..:ff). Probe sent on each scanned channel.
// PAIR_REPLY : C6 -> unicast to the DISCOVERY's src_addr. Carries the C6's
//              actual STA channel so the S3 can confirm/lock it. (The S3 also
//              already knows the channel it is currently scanning, but echoing
//              it from the C6 catches the rare race where they disagree.)

enum {
    PAIR_KIND_DISCOVERY = 0xD0,
    PAIR_KIND_REPLY = 0xD1,
};

typedef struct __attribute__((packed)) {
    uint8_t kind;        // PAIR_KIND_DISCOVERY or PAIR_KIND_REPLY
    uint8_t channel;     // REPLY: C6's current STA channel. DISCOVERY: scan ch.
    uint32_t nonce;      // S3 sets it in DISCOVERY; C6 echoes it in REPLY so the
                         // S3 can ignore stale replies from a previous scan.
} pair_frame_t;

#endif  // ESPNOW_LINK_H_
