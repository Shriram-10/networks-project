#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cstdint>
#include <cstring>
#include <arpa/inet.h>

/* Maximum sizes and limits */
#define MAX_FILENAME_LEN 256
#define MAX_RECORDS_PER_PACKET 16
#define MAX_SEGMENTS_PER_PACKET 16
#define MAX_PACKET_SIZE 65507  /* Max UDP payload size */
#define MAX_MISSING_RECORDS 1000

/* Packet type flags */
#define PKT_FILE_HDR 0x01
#define PKT_FILE_HDR_ACK 0x02
#define PKT_DATA 0x03
#define PKT_IS_BLAST_OVER 0x04
#define PKT_REC_MISS 0x05

/* Timeout values in seconds */
#define FILE_HDR_TIMEOUT 5
#define BLAST_OVER_TIMEOUT 3
#define LINGER_TIMEOUT 10

/* Record size options (bytes) */
#define RECORD_SIZE_256 256
#define RECORD_SIZE_512 512
#define RECORD_SIZE_1024 1024

/* Blast size range */
#define MIN_M_VALUE 200
#define MAX_M_VALUE 10000

/* MTU Constraint on packet size */
#define SAFE_MTU_DATA_SIZE 1450

/**
 * Segment descriptor - describes a contiguous range of records in a packet
 * Uses network byte order (big-endian) for cross-platform compatibility
 */
typedef struct {
    uint32_t start_rec;  /* First record number in segment (network order) */
    uint32_t end_rec;    /* Last record number in segment (network order) */
} SegmentDescriptor;

/**
 * FILE_HDR packet - sent by sender to initiate connection
 * All multi-byte fields use network byte order
 */
typedef struct {
    uint8_t packet_type;              /* PKT_FILE_HDR */
    uint8_t reserved[3];              /* Padding for alignment */
    uint64_t file_size;               /* Total file size in bytes (network order) */
    uint32_t record_size;             /* Size of each record (network order) */
    uint32_t M_proposed;              /* Proposed records per blast (network order) */
    char filename[MAX_FILENAME_LEN];  /* Null-terminated filename */
} __attribute__((packed)) FileHeaderPacket;

/**
 * FILE_HDR_ACK packet - receiver's response to FILE_HDR
 */
typedef struct {
    uint8_t packet_type;     /* PKT_FILE_HDR_ACK */
    uint8_t status;          /* 0 = reject, 1 = accept */
    uint8_t reserved[2];     /* Padding */
    uint32_t M_accepted;     /* Negotiated M value (network order) */
} __attribute__((packed)) FileHeaderAckPacket;

/**
 * DATA packet header - contains segment descriptors followed by record data
 * Layout: [DataPacketHeader][SegmentDescriptor array][Record data]
 */
typedef struct {
    uint8_t packet_type;      /* PKT_DATA */
    uint8_t segment_count;    /* Number of segments (N) */
    uint16_t reserved;        /* Padding */
    uint32_t sequence_num;    /* Packet sequence number (network order) */
    uint16_t data_length;     /* Total data payload length (network order) */
    uint16_t checksum;        /* Simple checksum for error detection */
    /* Followed by:
     * - SegmentDescriptor array [segment_count elements]
     * - Actual record data
     */
} __attribute__((packed)) DataPacketHeader;

/**
 * IS_BLAST_OVER packet - queries receiver about missing records
 */
typedef struct {
    uint8_t packet_type;     /* PKT_IS_BLAST_OVER */
    uint8_t reserved[3];     /* Padding */
    uint32_t M_start;        /* First record number in blast (network order) */
    uint32_t M_end;          /* Last record number in blast (network order) */
} __attribute__((packed)) BlastOverPacket;

/**
 * REC_MISS packet - receiver reports missing records
 * Layout: [RecMissPacket][SegmentDescriptor array for missing ranges]
 */
typedef struct {
    uint8_t packet_type;     /* PKT_REC_MISS */
    uint8_t reserved;        /* Padding */
    uint16_t miss_count;     /* Number of missing record ranges (network order) */
    /* Followed by SegmentDescriptor array describing missing ranges */
} __attribute__((packed)) RecMissPacket;

/* ============================================================================
 * BYTE ORDER CONVERSION FUNCTIONS
 * These ensure cross-platform compatibility (Windows ↔ Linux ↔ Mac)
 * ============================================================================ */

// On macOS/BSD, htonll and ntohll are already defined as macros
// We only define them if they don't exist
#if !defined(htonll)
    #if __BYTE_ORDER == __LITTLE_ENDIAN
        #define htonll(x) ((((uint64_t)htonl(x)) << 32) | htonl((x) >> 32))
    #else
        #define htonll(x) (x)
    #endif
#endif

#if !defined(ntohll)
    #if __BYTE_ORDER == __LITTLE_ENDIAN
        #define ntohll(x) ((((uint64_t)ntohl(x)) << 32) | ntohl((x) >> 32))
    #else
        #define ntohll(x) (x)
    #endif
#endif

/**
 * Convert FileHeaderPacket fields to network byte order (before sending)
 */
static inline void file_hdr_to_network(FileHeaderPacket *pkt) {
    pkt->file_size = htonll(pkt->file_size);
    pkt->record_size = htonl(pkt->record_size);
    pkt->M_proposed = htonl(pkt->M_proposed);
}

/**
 * Convert FileHeaderPacket fields to host byte order (after receiving)
 */
static inline void file_hdr_to_host(FileHeaderPacket *pkt) {
    pkt->file_size = ntohll(pkt->file_size);
    pkt->record_size = ntohl(pkt->record_size);
    pkt->M_proposed = ntohl(pkt->M_proposed);
}

/**
 * Convert FileHeaderAckPacket to network byte order
 */
static inline void file_hdr_ack_to_network(FileHeaderAckPacket *pkt) {
    pkt->M_accepted = htonl(pkt->M_accepted);
}

/**
 * Convert FileHeaderAckPacket to host byte order
 */
static inline void file_hdr_ack_to_host(FileHeaderAckPacket *pkt) {
    pkt->M_accepted = ntohl(pkt->M_accepted);
}

/**
 * Convert SegmentDescriptor to network byte order
 */
static inline void segment_to_network(SegmentDescriptor *seg) {
    seg->start_rec = htonl(seg->start_rec);
    seg->end_rec = htonl(seg->end_rec);
}

/**
 * Convert SegmentDescriptor to host byte order
 */
static inline void segment_to_host(SegmentDescriptor *seg) {
    seg->start_rec = ntohl(seg->start_rec);
    seg->end_rec = ntohl(seg->end_rec);
}

/**
 * Convert DataPacketHeader to network byte order
 */
static inline void data_hdr_to_network(DataPacketHeader *hdr) {
    hdr->sequence_num = htonl(hdr->sequence_num);
    hdr->data_length = htons(hdr->data_length);
    hdr->checksum = htons(hdr->checksum);
}

/**
 * Convert DataPacketHeader to host byte order
 */
static inline void data_hdr_to_host(DataPacketHeader *hdr) {
    hdr->sequence_num = ntohl(hdr->sequence_num);
    hdr->data_length = ntohs(hdr->data_length);
    hdr->checksum = ntohs(hdr->checksum);
}

/**
 * Convert BlastOverPacket to network byte order
 */
static inline void blast_over_to_network(BlastOverPacket *pkt) {
    pkt->M_start = htonl(pkt->M_start);
    pkt->M_end = htonl(pkt->M_end);
}

/**
 * Convert BlastOverPacket to host byte order
 */
static inline void blast_over_to_host(BlastOverPacket *pkt) {
    pkt->M_start = ntohl(pkt->M_start);
    pkt->M_end = ntohl(pkt->M_end);
}

/**
 * Convert RecMissPacket to network byte order
 */
static inline void rec_miss_to_network(RecMissPacket *pkt) {
    pkt->miss_count = htons(pkt->miss_count);
}

/**
 * Convert RecMissPacket to host byte order
 */
static inline void rec_miss_to_host(RecMissPacket *pkt) {
    pkt->miss_count = ntohs(pkt->miss_count);
}

/**
 * Calculate simple checksum for data integrity verification
 * Uses XOR-based checksum for simplicity and speed
 */
static inline uint16_t calculate_checksum(const uint8_t *data, size_t len) {
    uint16_t checksum = 0;
    for (size_t i = 0; i < len; i++) {
        checksum ^= data[i];
        checksum = (checksum << 1) | (checksum >> 15);  /* Rotate left */
    }
    return checksum;
}

#endif /* PROTOCOL_H */