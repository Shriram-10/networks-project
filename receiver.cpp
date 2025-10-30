#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <cerrno>
#include <ctime>
#include "protocol.h"

/* Record state tracking */
#define RECORD_MISSING 0
#define RECORD_RECEIVED 1

/* Global state for receiver */
typedef struct {
    int sock_fd;                      /* UDP socket file descriptor */
    struct sockaddr_in sender_addr;   /* Sender's address (learned from first packet) */
    FILE *file;                       /* Output file */
    char filename[MAX_FILENAME_LEN];  /* Filename to write */
    char output_dir[MAX_FILENAME_LEN];/* Directory to store file */
    uint64_t file_size;               /* Total file size */
    uint32_t record_size;             /* Size of each record */
    uint32_t M;                       /* Records per blast (negotiated) */
    uint32_t total_records;           /* Total number of records expected */
    
    /* Record assembly buffer */
    uint8_t *record_buffer;           /* Buffer to hold M records for current blast */
    uint8_t *record_status;           /* Status array: RECORD_MISSING or RECORD_RECEIVED */
    
    /* Current blast tracking */
    uint32_t blast_start;             /* First record number in current blast */
    uint32_t blast_end;               /* Last record number in current blast */
    uint32_t records_received;        /* Count of received records in current blast */
    
    /* Statistics */
    uint32_t packets_received;
    uint32_t total_records_written;
    
    /* Threading for async disk writes */
    pthread_t disk_thread;
    pthread_mutex_t buffer_mutex;
    pthread_cond_t records_ready;
    
    /* Disk write queue */
    uint32_t write_queue_head;        /* Next record to write to disk */
    int transfer_complete;            /* Flag to signal completion */
    int connection_established;       /* Flag for connection state */
} ReceiverState;

/* ============================================================================
 * UTILITY FUNCTIONS
 * ============================================================================ */

/**
 * Print usage information and exit
 */
void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s <port> <output_dir>\n", prog_name);
    fprintf(stderr, "  port: Port to listen on\n");
    fprintf(stderr, "  output_dir: Directory to save the received file\n");
    exit(1);
}

/**
 * Create and bind UDP socket to listen on specified port
 * Returns socket file descriptor or -1 on error
 */
int create_and_bind_socket(int port) {
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return -1;
    }
    
    /* Allow port reuse */
    int reuse = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(sock_fd);
        return -1;
    }
    
    /* Bind to specified port */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock_fd);
        return -1;
    }
    
    printf("Listening on port %d...\n", port);
    return sock_fd;
}

/**
 * Send a packet to the sender
 * Returns 0 on success, -1 on error
 */
int send_to_sender(ReceiverState *state, const void *data, size_t len) {
    ssize_t sent = sendto(state->sock_fd, data, len, 0,
                          (struct sockaddr *)&state->sender_addr,
                          sizeof(state->sender_addr));
    if (sent < 0) {
        perror("sendto");
        return -1;
    }
    return 0;
}

/**
 * Receive a packet with sender address
 * Returns number of bytes received or -1 on error
 */
ssize_t recv_packet(ReceiverState *state, void *buffer, size_t max_len,
                    struct sockaddr_in *src_addr) {
    socklen_t addr_len = sizeof(*src_addr);
    
    ssize_t received = recvfrom(state->sock_fd, buffer, max_len, 0,
                                (struct sockaddr *)src_addr, &addr_len);
    
    if (received < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("recvfrom");
        }
        return -1;
    }
    
    state->packets_received++;
    return received;
}

/* ============================================================================
 * PHASE 1: CONNECTION SETUP
 * ============================================================================ */

/**
 * Process FILE_HDR packet and send acknowledgment
 * Returns 0 on success (connection accepted), -1 on error
 */
int handle_file_header(ReceiverState *state, const FileHeaderPacket *hdr) {
    /* Convert from network byte order */
    FileHeaderPacket hdr_copy = *hdr;
    file_hdr_to_host(&hdr_copy);
    
    printf("\n=== Connection Request ===\n");
    printf("Filename: %s\n", hdr_copy.filename);
    printf("File size: %llu bytes\n", (unsigned long long)hdr_copy.file_size);
    printf("Record size: %u bytes\n", hdr_copy.record_size);
    printf("Proposed M: %u records/blast\n", hdr_copy.M_proposed);
    
    /* Store file parameters */
    snprintf(state->filename, MAX_FILENAME_LEN, "%s/%s", state->output_dir, hdr_copy.filename);
    state->file_size = hdr_copy.file_size;
    state->record_size = hdr_copy.record_size;
    state->total_records = (hdr_copy.file_size + hdr_copy.record_size - 1) / hdr_copy.record_size;

    printf("Receiver: Expecting a total of %u records.\n", state->total_records);
    
    /* Negotiate M based on available memory */
    /* For simplicity, accept sender's proposal or limit to 5000 records */
    const uint32_t max_buffer_mb = 10;  /* 10 MB buffer limit */
    uint32_t max_records = (max_buffer_mb * 1024 * 1024) / hdr_copy.record_size;
    state->M = (hdr_copy.M_proposed < max_records) ? hdr_copy.M_proposed : max_records;
    
    if (state->M < MIN_M_VALUE) {
        fprintf(stderr, "Error: Cannot accommodate minimum M value\n");
        return -1;
    }
    
    printf("Negotiated M: %u records/blast\n", state->M);
    
    /* Allocate buffers for record assembly */
    state->record_buffer = (uint8_t *)malloc(state->M * state->record_size);
    state->record_status = (uint8_t *)malloc(state->M * sizeof(uint8_t));
    
    if (!state->record_buffer || !state->record_status) {
        perror("malloc");
        return -1;
    }
    
    /* Open output file */
    state->file = fopen(state->filename, "wb");
    if (!state->file) {
        perror("fopen");
        fprintf(stderr, "Failed to open output file: %s\n", state->filename);
        return -1;
    }

    printf("Writing to output file: %s\n", state->filename);
    
    /* Send FILE_HDR_ACK */
    FileHeaderAckPacket ack;
    ack.packet_type = PKT_FILE_HDR_ACK;
    ack.status = 1;  /* Accept connection */
    memset(ack.reserved, 0, sizeof(ack.reserved));
    ack.M_accepted = state->M;
    
    file_hdr_ack_to_network(&ack);
    
    printf("Sending FILE_HDR_ACK (M=%u)\n", state->M);
    if (send_to_sender(state, &ack, sizeof(ack)) < 0) {
        return -1;
    }
    
    state->connection_established = 1;
    return 0;
}

/* ============================================================================
 * PHASE 2: DATA RECEPTION AND ASSEMBLY
 * ============================================================================ */

/**
 * Initialize blast tracking for a new blast cycle
 */
void init_blast_cycle(ReceiverState *state, uint32_t start, uint32_t end) {
    state->blast_start = start;
    state->blast_end = end;
    state->records_received = 0;
    
    /* Clear record status array */
    uint32_t blast_size = end - start + 1;
    memset(state->record_status, RECORD_MISSING, blast_size);
    
    printf("\n=== New Blast Cycle ===\n");
    printf("Records: %u - %u (%u records)\n", start, end, blast_size);
}

/**
 * Process a DATA packet and store records in buffer
 * Returns 0 on success, -1 on error
 */
int handle_data_packet(ReceiverState *state, const uint8_t *packet, size_t packet_len) {
    if (packet_len < sizeof(DataPacketHeader)) {
        fprintf(stderr, "Malformed DATA packet (too short)\n");
        return -1;
    }
    
    /* Parse header */
    DataPacketHeader hdr;
    memcpy(&hdr, packet, sizeof(hdr));
    data_hdr_to_host(&hdr);
    
    if (hdr.segment_count == 0 || hdr.segment_count > MAX_SEGMENTS_PER_PACKET) {
        fprintf(stderr, "Invalid segment count: %u\n", hdr.segment_count);
        return -1;
    }
    
    /* Parse segment descriptors */
    const SegmentDescriptor *segments = (const SegmentDescriptor *)(packet + sizeof(DataPacketHeader));
    const uint8_t *data_ptr = packet + sizeof(DataPacketHeader) + 
                              (hdr.segment_count * sizeof(SegmentDescriptor));
    
    /* Verify checksum */
    uint16_t calculated_checksum = calculate_checksum(data_ptr, hdr.data_length);
    if (calculated_checksum != hdr.checksum) {
        fprintf(stderr, "Checksum mismatch! Packet corrupted.\n");
        return -1;
    }
    
    /* Process each segment */
    uint32_t data_offset = 0;
    for (uint8_t seg = 0; seg < hdr.segment_count; seg++) {
        SegmentDescriptor seg_desc;
        memcpy(&seg_desc, &segments[seg], sizeof(seg_desc));
        segment_to_host(&seg_desc);
        
        uint32_t start_rec = seg_desc.start_rec;
        uint32_t end_rec = seg_desc.end_rec;
        
        /* Validate segment is within current blast */
        if (start_rec < state->blast_start || end_rec > state->blast_end) {
            fprintf(stderr, "Segment [%u-%u] outside blast range [%u-%u]\n",
                    start_rec, end_rec, state->blast_start, state->blast_end);
            continue;
        }
        
        /* Copy records to buffer */
        pthread_mutex_lock(&state->buffer_mutex);
        
        for (uint32_t rec = start_rec; rec <= end_rec; rec++) {
            uint32_t buffer_index = rec - state->blast_start;
            
            /* Skip if already received (duplicate) */
            if (state->record_status[buffer_index] == RECORD_RECEIVED) {
                data_offset += state->record_size;
                continue;
            }
            
            /* Copy record data */
            memcpy(state->record_buffer + (buffer_index * state->record_size),
                   data_ptr + data_offset,
                   state->record_size);
            
            state->record_status[buffer_index] = RECORD_RECEIVED;
            state->records_received++;
            data_offset += state->record_size;
        }
        
        pthread_cond_signal(&state->records_ready);
        pthread_mutex_unlock(&state->buffer_mutex);
    }
    
    return 0;
}

/**
 * Build a list of missing record ranges for REC_MISS packet
 * Returns number of missing ranges
 */
uint16_t build_missing_list(ReceiverState *state, SegmentDescriptor *missing_list, 
                             uint16_t max_ranges) {
    uint16_t range_count = 0;
    uint32_t blast_size = state->blast_end - state->blast_start + 1;
    
    uint32_t i = 0;
    while (i < blast_size && range_count < max_ranges) {
        /* Skip received records */
        while (i < blast_size && state->record_status[i] == RECORD_RECEIVED) {
            i++;
        }
        
        if (i >= blast_size) break;
        
        /* Found start of missing range */
        uint32_t range_start = state->blast_start + i;
        uint32_t range_end = range_start;
        
        /* Find end of missing range */
        i++;
        while (i < blast_size && state->record_status[i] == RECORD_MISSING) {
            range_end++;
            i++;
        }
        
        /* Add to missing list */
        missing_list[range_count].start_rec = range_start;
        missing_list[range_count].end_rec = range_end;
        segment_to_network(&missing_list[range_count]);
        
        range_count++;
    }
    
    return range_count;
}

/**
 * Handle IS_BLAST_OVER query and send REC_MISS response
 * Returns 0 on success, -1 on error
 */
int handle_blast_over_query(ReceiverState *state, const BlastOverPacket *query) {
    BlastOverPacket query_copy = *query;
    blast_over_to_host(&query_copy);

    if (state->blast_start != query_copy.M_start || state->blast_end != query_copy.M_end) {
        init_blast_cycle(state, query_copy.M_start, query_copy.M_end);
    }
    
    printf("\nReceived IS_BLAST_OVER(%u, %u)\n", query_copy.M_start, query_copy.M_end);
    printf("Records received: %u / %u\n", state->records_received, 
           state->blast_end - state->blast_start + 1);
    
    /* Build missing records list */
    uint8_t response_buf[MAX_PACKET_SIZE];
    RecMissPacket *rec_miss = (RecMissPacket *)response_buf;
    SegmentDescriptor *missing_list = (SegmentDescriptor *)(response_buf + sizeof(RecMissPacket));
    
    pthread_mutex_lock(&state->buffer_mutex);
    uint16_t miss_count = build_missing_list(state, missing_list, MAX_MISSING_RECORDS);
    pthread_mutex_unlock(&state->buffer_mutex);
    
    /* Construct REC_MISS packet */
    rec_miss->packet_type = PKT_REC_MISS;
    rec_miss->reserved = 0;
    rec_miss->miss_count = miss_count;
    rec_miss_to_network(rec_miss);
    
    size_t response_size = sizeof(RecMissPacket) + (miss_count * sizeof(SegmentDescriptor));
    
    if (miss_count == 0) {
        printf("Blast complete! Sending empty REC_MISS\n");
    } else {
        printf("Sending REC_MISS with %u missing range(s)\n", miss_count);
    }
    
    return send_to_sender(state, response_buf, response_size);
}

/* ============================================================================
 * ASYNC DISK WRITING (CONSUMER THREAD)
 * ============================================================================ */

/**
 * Disk writer thread - writes contiguous records to disk
 * Waits for records to become available and writes them sequentially
 */
void *disk_writer_thread(void *arg) {
    ReceiverState *state = (ReceiverState *)arg;
    
    printf("Disk writer thread started\n");
    
    while (state->total_records_written < state->total_records) {
        pthread_mutex_lock(&state->buffer_mutex);
        
        uint32_t write_count = 0; // Reset write count for this iteration

        // Check if the record we need is within the current blast's buffer
        if (state->write_queue_head >= state->blast_start && 
            state->write_queue_head <= state->blast_end) {

            /* Check for contiguous records starting from write_queue_head */
            uint32_t write_start = state->write_queue_head;
            uint32_t blast_size = state->blast_end - state->blast_start + 1;
            
            /* Find contiguous received records */
            // Check one-by-one starting from write_start
            while (write_start + write_count <= state->blast_end) { 
                uint32_t buffer_index = (write_start + write_count) - state->blast_start;
                
                if (buffer_index >= blast_size || 
                    state->record_status[buffer_index] != RECORD_RECEIVED) {
                    break; // This record is missing, stop counting
                }
                write_count++; // This record is present, count it
            }
            
            if (write_count > 0) {
                /* Write contiguous records to disk */
                for (uint32_t i = 0; i < write_count; i++) {
                    uint32_t buffer_index = (write_start + i) - state->blast_start;
                    uint8_t *record_data = state->record_buffer + (buffer_index * state->record_size);
                    
                    /* Handle last record (might be partial) */
                    size_t bytes_to_write = state->record_size;
                    if (write_start + i == state->total_records - 1) {
                        uint64_t remaining = state->file_size - 
                                             (state->total_records_written * state->record_size);
                        if (remaining < state->record_size) {
                            bytes_to_write = remaining;
                        }
                    }
                    
                    size_t written = fwrite(record_data, 1, bytes_to_write, state->file);
                    if (written != bytes_to_write) {
                        fprintf(stderr, "Disk write error\n");
                        pthread_mutex_unlock(&state->buffer_mutex);
                        return NULL;
                    }
                    
                    state->total_records_written++;
                }
                
                fflush(state->file); // Force data to disk
                
                state->write_queue_head += write_count;
                printf("Wrote %u records to disk (total: %u/%u)\n", 
                       write_count, state->total_records_written, state->total_records);
            }
        }
        
        // --- THIS IS THE NEW, CORRECTED WAIT LOGIC ---
        // If the transfer is NOT complete AND we made NO progress (didn't write anything)
        // then we must wait for more data to arrive.
        if (!state->transfer_complete && write_count == 0) {
            pthread_cond_wait(&state->records_ready, &state->buffer_mutex);
        }
        // --- END NEW LOGIC ---

        pthread_mutex_unlock(&state->buffer_mutex);
    }
    
    printf("\n[Disk Thread] All %u records written to disk. Transfer complete.\n", 
           state->total_records_written);
    printf("Disk writer thread completed: %u records written\n", state->total_records_written);
    return NULL;
}

/* ============================================================================
 * MAIN RECEPTION LOOP
 * ============================================================================ */

/**
 * Main packet processing loop
 */
int receive_file(ReceiverState *state) {
    uint8_t packet_buf[MAX_PACKET_SIZE];
    struct sockaddr_in src_addr;
    int blast_active = 0;
    
    /* Start disk writer thread */
    if (pthread_create(&state->disk_thread, NULL, disk_writer_thread, state) != 0) {
        perror("pthread_create");
        return -1;
    }
    
    /* Set socket timeout for linger period after last blast */
    struct timeval tv;
    tv.tv_sec = LINGER_TIMEOUT;
    tv.tv_usec = 0;

    /* This line applies the timeout to the socket file descriptor */
    if (setsockopt(state->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt (RCVTIMEO)");
        return -1;
    }
    
    while (1) {
        ssize_t received = recv_packet(state, packet_buf, MAX_PACKET_SIZE, &src_addr);
        
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Timeout - check if transfer is complete */
                if (state->total_records_written >= state->total_records) {
                    printf("Transfer complete, exiting linger period\n");
                    break;
                }
            }
            continue;
        }
        
        if (received == 0) continue;
        
        /* Process packet based on type */
        uint8_t packet_type = packet_buf[0];
        
        switch (packet_type) {
            case PKT_FILE_HDR:
                /* Handle retransmitted FILE_HDR */
                if (state->connection_established) {
                    printf("Retransmitting FILE_HDR_ACK\n");
                    FileHeaderAckPacket ack;
                    ack.packet_type = PKT_FILE_HDR_ACK;
                    ack.status = 1;
                    memset(ack.reserved, 0, sizeof(ack.reserved));
                    ack.M_accepted = state->M;
                    file_hdr_ack_to_network(&ack);
                    send_to_sender(state, &ack, sizeof(ack));
                }
                break;
                
            case PKT_DATA:
                if (!blast_active) {
                    /* First data packet of new blast */
                    if (received >= sizeof(DataPacketHeader) + sizeof(SegmentDescriptor)) {
                        DataPacketHeader hdr;
                        memcpy(&hdr, packet_buf, sizeof(hdr));
                        data_hdr_to_host(&hdr);
                        
                        SegmentDescriptor first_seg;
                        memcpy(&first_seg, packet_buf + sizeof(DataPacketHeader), sizeof(first_seg));
                        segment_to_host(&first_seg);
                        
                        uint32_t blast_start = first_seg.start_rec;
                        uint32_t blast_end = blast_start + state->M - 1;
                        if (blast_end >= state->total_records) {
                            blast_end = state->total_records - 1;
                        }
                        
                        init_blast_cycle(state, blast_start, blast_end);
                        blast_active = 1;
                    }
                }
                handle_data_packet(state, packet_buf, received);
                break;
                
            case PKT_IS_BLAST_OVER:
                if (received >= sizeof(BlastOverPacket)) {
                    handle_blast_over_query(state, (BlastOverPacket *)packet_buf);
                    
                    /* Check if blast is complete (no missing records) */
                    if (state->records_received == (state->blast_end - state->blast_start + 1)) {
                        blast_active = 0;  /* Ready for next blast */
                        
                        /* Signal disk writer */
                        pthread_mutex_lock(&state->buffer_mutex);
                        pthread_cond_signal(&state->records_ready);
                        pthread_mutex_unlock(&state->buffer_mutex);
                    }
                }
                break;
                
            default:
                fprintf(stderr, "Unknown packet type: 0x%02x\n", packet_type);
                break;
        }
    }
    
    /* Wait for disk writer to complete */
    state->transfer_complete = 1;
    pthread_cond_broadcast(&state->records_ready);
    pthread_join(state->disk_thread, NULL);
    
    return 0;
}

/* ============================================================================
 * MAIN FUNCTION
 * ============================================================================ */

int main(int argc, char *argv[]) {
    if (argc != 3) {
        print_usage(argv[0]);
    }
    
    int port = atoi(argv[1]);
    const char *output_dir = argv[2];
    
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: Invalid port number\n");
        exit(1);
    }
    
    /* Initialize receiver state */
    ReceiverState state;
    memset(&state, 0, sizeof(state));
    
    /* Copy output dir and remove trailing slash if present */
    strncpy(state.output_dir, output_dir, MAX_FILENAME_LEN - 1);
    size_t len = strlen(state.output_dir);
    if (len > 0 && state.output_dir[len - 1] == '/') {
        state.output_dir[len - 1] = '\0';
    }
    
    /* Create and bind socket */
    state.sock_fd = create_and_bind_socket(port);
    if (state.sock_fd < 0) {
        exit(1);
    }
    
    /* Initialize threading primitives */
    pthread_mutex_init(&state.buffer_mutex, NULL);
    pthread_cond_init(&state.records_ready, NULL);
    
    printf("=== Fast File Transfer Receiver ===\n");
    printf("Saving files to: %s\n", state.output_dir);
    printf("Waiting for connection...\n");
    
    /* Wait for FILE_HDR to establish connection */
    uint8_t packet_buf[MAX_PACKET_SIZE];
    struct sockaddr_in src_addr;
    
    while (!state.connection_established) {
        ssize_t received = recv_packet(&state, packet_buf, MAX_PACKET_SIZE, &src_addr);
        
        if (received > 0 && packet_buf[0] == PKT_FILE_HDR) {
            /* Store sender address */
            state.sender_addr = src_addr;
            
            if (handle_file_header(&state, (FileHeaderPacket *)packet_buf) == 0) {
                break;
            } else {
                fprintf(stderr, "Failed to establish connection\n");
                exit(1);
            }
        }
    }
    
    /* Receive file */
    if (receive_file(&state) < 0) {
        fprintf(stderr, "File reception failed\n");
        exit(1);
    }
    
    /* Print statistics */
    printf("\n=== Transfer Complete ===\n");
    printf("Output file: %s\n", state.filename);
    printf("File size: %llu bytes (%u records)\n", 
           (unsigned long long)state.file_size, state.total_records);
    printf("Packets received: %u\n", state.packets_received);
    printf("Records written: %u\n", state.total_records_written);
    
    /* Cleanup */
    free(state.record_buffer);
    free(state.record_status);
    pthread_mutex_destroy(&state.buffer_mutex);
    pthread_cond_destroy(&state.records_ready);
    if (state.file) fclose(state.file);
    close(state.sock_fd);
    
    return 0;
}