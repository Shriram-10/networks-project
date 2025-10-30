#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <cerrno>
#include <fstream>
#include <ctime>
#include "protocol.h"

/* Global state for sender */
typedef struct {
    int sock_fd;                      /* UDP socket file descriptor */
    struct sockaddr_in dest_addr;     /* Destination (garbler/receiver) address */
    char filename[MAX_FILENAME_LEN];                       /* File to send */
    uint64_t file_size;               /* Total file size */
    uint32_t record_size;             /* Size of each record */
    uint32_t M;                       /* Records per blast (negotiated) */
    uint32_t total_records;           /* Total number of records in file */
    
    /* Statistics */
    uint32_t packets_sent;
    time_t start_time;                /* Transfer start time */
    time_t end_time;                  /* Transfer end time */
    
    /* Threading for async disk reads */
    pthread_t disk_thread;
    pthread_mutex_t buffer_mutex;
    pthread_cond_t buffer_not_empty;
    pthread_cond_t buffer_not_full;
    
    uint8_t *record_buffer;           /* Circular buffer for records */
    uint32_t buffer_capacity;         /* Max records in buffer */
    uint32_t buffer_count;            /* Current records in buffer */
    uint32_t buffer_head;             /* Read position */
    uint32_t buffer_tail;             /* Write position */
    
    int transfer_complete;            /* Flag to signal completion */
} SenderState;

/* ============================================================================
 * UTILITY FUNCTIONS
 * ============================================================================ */

/**
 * Print usage information and exit
 */
void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s <target_ip> <target_port> <filepath> <record_size> <M_initial>\n", prog_name);
    fprintf(stderr, "  target_ip: IP address of receiver/garbler\n");
    fprintf(stderr, "  target_port: Port of receiver/garbler\n");
    fprintf(stderr, "  filepath: Full path to the file to transfer\n");
    fprintf(stderr, "  record_size: 256, 512, or 1024 bytes\n");
    fprintf(stderr, "  M_initial: Initial records per blast (200-10000)\n");
    exit(1);
}

/**
 * Write throughput data to CSV file
 */
void write_to_csv(const char* csv_file, double loss_rate, double throughput_mbps, 
                  double duration, uint32_t packets_sent) {
    std::ofstream file;
    bool file_exists = (access(csv_file, F_OK) == 0);
    
    // Open file in append mode
    file.open(csv_file, std::ios::app);
    if (!file.is_open()) {
        fprintf(stderr, "Error: Could not open CSV file: %s\n", csv_file);
        return;
    }
    
    // Write header if file is new
    if (!file_exists) {
        file << "loss_rate,throughput_mbps,duration_seconds,packets_sent\n";
    }
    
    // Write data
    file << loss_rate << "," << throughput_mbps << "," << duration << "," << packets_sent << "\n";
    file.close();
    
    printf("Data written to CSV: %s\n", csv_file);
}

/**
 * Get file size using stat system call
 */
uint64_t get_file_size(const char *filename) {
    struct stat st;
    if (stat(filename, &st) != 0) {
        perror("stat");
        return 0;
    }
    return (uint64_t)st.st_size;
}

/**
 * Create and bind UDP socket
 * Returns socket file descriptor or -1 on error
 */
int create_udp_socket() {
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return -1;
    }
    
    /* Set socket timeout for receives */
    struct timeval tv;
    tv.tv_sec = FILE_HDR_TIMEOUT;
    tv.tv_usec = 0;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt");
        close(sock_fd);
        return -1;
    }
    
    return sock_fd;
}

/**
 * Send a packet with error checking
 * Returns 0 on success, -1 on error
 */
int send_packet(SenderState *state, const void *data, size_t len) {
    ssize_t sent = sendto(state->sock_fd, data, len, 0,
                          (struct sockaddr *)&state->dest_addr,
                          sizeof(state->dest_addr));
    if (sent < 0) {
        perror("sendto");
        return -1;
    }
    state->packets_sent++;
    return 0;
}

/**
 * Receive a packet with timeout
 * Returns number of bytes received, -1 on error, 0 on timeout
 */
ssize_t recv_packet(SenderState *state, void *buffer, size_t max_len) {
    struct sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);
    
    ssize_t received = recvfrom(state->sock_fd, buffer, max_len, 0,
                                (struct sockaddr *)&src_addr, &addr_len);
    
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  /* Timeout */
        }
        perror("recvfrom");
        return -1;
    }
    
    return received;
}

/* ============================================================================
 * PHASE 1: CONNECTION SETUP
 * ============================================================================ */

/**
 * Send FILE_HDR packet to initiate connection
 * Returns 0 on success, -1 on error
 */
int send_file_header(SenderState *state, const char *filename) {
    FileHeaderPacket hdr;
    memset(&hdr, 0, sizeof(hdr));
    
    hdr.packet_type = PKT_FILE_HDR;
    hdr.file_size = state->file_size;
    hdr.record_size = state->record_size;
    hdr.M_proposed = state->M;
    
    // Use basename of the file, not the full path
    const char *basename = strrchr(filename, '/');
    if (basename) {
        strncpy(hdr.filename, basename + 1, MAX_FILENAME_LEN - 1);
    } else {
        strncpy(hdr.filename, filename, MAX_FILENAME_LEN - 1);
    }
    
    /* Convert to network byte order */
    file_hdr_to_network(&hdr);
    
    printf("Sending FILE_HDR: file=%s, size=%llu, record_size=%u, M=%u\n",
           hdr.filename, (unsigned long long)state->file_size, state->record_size, state->M);
    
    return send_packet(state, &hdr, sizeof(hdr));
}

/**
 * Wait for FILE_HDR_ACK with retransmission
 * Returns 0 on success (connection established), -1 on failure
 */
int wait_for_file_header_ack(SenderState *state, const char *filepath) {
    FileHeaderAckPacket ack;
    int attempts = 0;
    const int max_attempts = 5;
    
    while (attempts < max_attempts) {
        ssize_t received = recv_packet(state, &ack, sizeof(ack));
        
        if (received > 0 && ack.packet_type == PKT_FILE_HDR_ACK) {
            file_hdr_ack_to_host(&ack);
            
            if (ack.status == 1) {
                state->M = ack.M_accepted;
                printf("Connection established! M negotiated to %u\n", state->M);
                return 0;
            } else {
                fprintf(stderr, "Receiver rejected connection\n");
                return -1;
            }
        }
        
        /* Timeout - retransmit FILE_HDR */
        attempts++;
        printf("FILE_HDR_ACK timeout, retransmitting (%d/%d)...\n", attempts, max_attempts);
        if (send_file_header(state, filepath) < 0) {
            return -1;
        }
    }
    
    fprintf(stderr, "Failed to establish connection after %d attempts\n", max_attempts);
    return -1;
}

/**
 * Perform Phase 1: Connection setup
 * Returns 0 on success, -1 on failure
 */
int establish_connection(SenderState *state, const char *filepath) {
    if (send_file_header(state, filepath) < 0) {
        return -1;
    }
    
    return wait_for_file_header_ack(state, filepath);
}

/* ============================================================================
 * ASYNC DISK READING (PRODUCER THREAD)
 * ============================================================================ */

/**
 * Disk reader thread - reads records from file and fills buffer
 * This runs asynchronously to allow continuous packet transmission
 */
void *disk_reader_thread(void *arg) {
    SenderState *state = (SenderState *)arg;
    uint32_t records_read = 0;

    FILE *file = fopen(state->filename, "rb");
    if (!file) {
        perror("fopen (disk_reader_thread)");
        return NULL;
    }
    
    printf("Disk reader thread started\n");
    
    while (records_read < state->total_records) {
        /* Allocate temporary record buffer */
        uint8_t *record = (uint8_t *)malloc(state->record_size);
        if (!record) {
            fprintf(stderr, "Failed to allocate record buffer\n");
            break;
        }
        
        size_t bytes_read = fread(record, 1, state->record_size, file);

        if (bytes_read == 0) {
            if (ferror(file)) { 
                perror("[Disk Thread] fread error");
            } else if (feof(file)) {
                printf("[Disk Thread] feof() is true. End of file reached unexpectedly.\n");
            } else {
                printf("[Disk Thread] fread returned 0, but no error and no EOF.\n");
            }
            free(record);
            break;
        }
        
        /* Pad with zeros if partial record (last record of file) */
        if (bytes_read < state->record_size) {
            memset(record + bytes_read, 0, state->record_size - bytes_read);
        }
        
        /* Wait for space in buffer */
        pthread_mutex_lock(&state->buffer_mutex);
        while (state->buffer_count >= state->buffer_capacity && !state->transfer_complete) {
            pthread_cond_wait(&state->buffer_not_full, &state->buffer_mutex);
        }
        
        if (state->transfer_complete) {
            pthread_mutex_unlock(&state->buffer_mutex);
            free(record);
            break;
        }
        
        /* Copy record to circular buffer */
        memcpy(state->record_buffer + (state->buffer_tail * state->record_size),
               record, state->record_size);
        state->buffer_tail = (state->buffer_tail + 1) % state->buffer_capacity;
        state->buffer_count++;
        
        pthread_cond_signal(&state->buffer_not_empty);
        pthread_mutex_unlock(&state->buffer_mutex);
        
        free(record);
        records_read++;
    }
    
    fclose(file);
    return NULL;
}

/* ============================================================================
 * PHASE 2: DATA TRANSFER (BLAST PROTOCOL)
 * ============================================================================ */

/**
 * Construct a data packet from available records in buffer
 * Pulls up to MAX_RECORDS_PER_PACKET from buffer and creates a single segment
 * Returns packet size or -1 on error
 */
ssize_t construct_data_packet(SenderState *state, uint8_t *packet_buf,
                               uint32_t start_rec, uint32_t *records_packed) {
    DataPacketHeader *hdr = (DataPacketHeader *)packet_buf;
    SegmentDescriptor *segments = (SegmentDescriptor *)(packet_buf + sizeof(DataPacketHeader));
    uint8_t *data_ptr = packet_buf + sizeof(DataPacketHeader) + sizeof(SegmentDescriptor);

    /* --- Dynamic Packet Sizing Logic --- */
    
    // 1. Calculate how many records fit within a safe MTU payload
    uint32_t mtu_record_limit = SAFE_MTU_DATA_SIZE / state->record_size;
    
    // Ensure we can always send at least 1 record
    if (mtu_record_limit == 0) {
        mtu_record_limit = 1;
    }

    // 2. The *actual* limit is the SMALLER of the protocol's max (16) and the MTU's max
    uint32_t packet_record_limit = (mtu_record_limit < MAX_RECORDS_PER_PACKET) ? 
                                    mtu_record_limit : MAX_RECORDS_PER_PACKET;
    
    /* Wait for at least one record in buffer */
    pthread_mutex_lock(&state->buffer_mutex);
    while (state->buffer_count == 0 && start_rec < state->total_records) {
        pthread_cond_wait(&state->buffer_not_empty, &state->buffer_mutex);
    }
    
    if (state->buffer_count == 0) {
        pthread_mutex_unlock(&state->buffer_mutex);
        return 0;
    }
    
    /* Pull up to our 'packet_record_limit' records from buffer */
    uint32_t records_to_send = (state->buffer_count < packet_record_limit) ?
                                state->buffer_count : packet_record_limit;
    
    for (uint32_t i = 0; i < records_to_send; i++) {
        memcpy(data_ptr + (i * state->record_size),
               state->record_buffer + (state->buffer_head * state->record_size),
               state->record_size);
        
        state->buffer_head = (state->buffer_head + 1) % state->buffer_capacity;
        state->buffer_count--;
    }
    
    pthread_cond_signal(&state->buffer_not_full);
    pthread_mutex_unlock(&state->buffer_mutex);
    
    /* Fill packet header */
    hdr->packet_type = PKT_DATA;
    hdr->segment_count = 1;  /* Single contiguous segment */
    hdr->sequence_num = start_rec;
    hdr->data_length = records_to_send * state->record_size;
    
    /* Fill segment descriptor */
    segments[0].start_rec = start_rec;
    segments[0].end_rec = start_rec + records_to_send - 1;
    
    /* Calculate checksum */
    hdr->checksum = calculate_checksum(data_ptr, hdr->data_length);
    
    /* Convert header to network byte order */
    segment_to_network(&segments[0]);
    data_hdr_to_network(hdr);
    
    *records_packed = records_to_send;
    
    return sizeof(DataPacketHeader) + sizeof(SegmentDescriptor) + 
           (records_to_send * state->record_size);
}

/**
 * Send one blast cycle (M records)
 * Returns 0 on success, -1 on error
 */
int send_blast(SenderState *state, uint32_t start_rec, uint32_t end_rec) {
    uint8_t packet_buf[MAX_PACKET_SIZE];
    uint32_t current_rec = start_rec;
    
    printf("Sending blast: records %u-%u\n", start_rec, end_rec);
    
    while (current_rec <= end_rec) {
        uint32_t records_packed = 0;
        ssize_t packet_size = construct_data_packet(state, packet_buf, current_rec, &records_packed);
        
        if (packet_size <= 0) {
            fprintf(stderr, "Failed to construct packet\n");
            return -1;
        }
        
        if (send_packet(state, packet_buf, packet_size) < 0) {
            return -1;
        }
        
        current_rec += records_packed;
    }
    
    return 0;
}

/**
 * Send IS_BLAST_OVER query packet
 */
int send_blast_over_query(SenderState *state, uint32_t start_rec, uint32_t end_rec) {
    BlastOverPacket pkt;
    pkt.packet_type = PKT_IS_BLAST_OVER;
    memset(pkt.reserved, 0, sizeof(pkt.reserved));
    pkt.M_start = start_rec;
    pkt.M_end = end_rec;
    
    blast_over_to_network(&pkt);
    
    printf("Sending IS_BLAST_OVER(%u, %u)\n", start_rec, end_rec);
    return send_packet(state, &pkt, sizeof(pkt));
}

/**
 * Retransmit specific records based on missing list
 * Reads records from file and sends them
 */
int retransmit_records(SenderState *state, SegmentDescriptor *missing_list, 
                       uint16_t miss_count) {
    uint8_t packet_buf[MAX_PACKET_SIZE];

    FILE *file = fopen(state->filename, "rb");
    if (!file) {
        perror("fopen (retransmit_records)");
        return -1;
    }
    
    printf("Retransmitting %u missing range(s)\n", miss_count);

    /* --- MTU-Aware Logic --- */
    uint32_t mtu_record_limit = SAFE_MTU_DATA_SIZE / state->record_size;
    if (mtu_record_limit == 0) {
        mtu_record_limit = 1;
    }
    uint32_t packet_record_limit = (mtu_record_limit < MAX_RECORDS_PER_PACKET) ? 
                                    mtu_record_limit : MAX_RECORDS_PER_PACKET;
    /* ---------------------------------- */
    
    for (uint16_t i = 0; i < miss_count; i++) {
        uint32_t start_rec = missing_list[i].start_rec;
        uint32_t end_rec = missing_list[i].end_rec;
        uint32_t range_size = end_rec - start_rec + 1;
        
        printf("  Range: %u-%u (%u records)\n", start_rec, end_rec, range_size);
        
        /* Seek to the correct position in file */
        if (fseek(file, start_rec * state->record_size, SEEK_SET) != 0) {
            perror("fseek");
            fclose(file);
            return -1;
        }
        
        /* Send records in packets of up to 16 records each */
        uint32_t current_rec = start_rec;
        while (current_rec <= end_rec) {
            DataPacketHeader *hdr = (DataPacketHeader *)packet_buf;
            SegmentDescriptor *seg = (SegmentDescriptor *)(packet_buf + sizeof(DataPacketHeader));
            uint8_t *data_ptr = packet_buf + sizeof(DataPacketHeader) + sizeof(SegmentDescriptor);
            
            /* Determine how many records to send in this packet */
            uint32_t records_to_send = end_rec - current_rec + 1;
            if (records_to_send > packet_record_limit) {
                records_to_send = packet_record_limit;
            }
            
            /* Read records from file */
            for (uint32_t j = 0; j < records_to_send; j++) {
                size_t bytes_read = fread(data_ptr + (j * state->record_size), 
                                         1, state->record_size, file);
                if (bytes_read < state->record_size) {
                    /* Pad last record if partial */
                    memset(data_ptr + (j * state->record_size) + bytes_read, 
                           0, state->record_size - bytes_read);
                }
            }
            
            /* Fill packet header */
            hdr->packet_type = PKT_DATA;
            hdr->segment_count = 1;
            hdr->sequence_num = current_rec;
            hdr->data_length = records_to_send * state->record_size;
            hdr->checksum = calculate_checksum(data_ptr, hdr->data_length);
            
            /* Fill segment descriptor */
            seg->start_rec = current_rec;
            seg->end_rec = current_rec + records_to_send - 1;
            
            /* Convert to network byte order */
            segment_to_network(seg);
            data_hdr_to_network(hdr);
            
            /* Send packet */
            size_t packet_size = sizeof(DataPacketHeader) + sizeof(SegmentDescriptor) + 
                                (records_to_send * state->record_size);
            
            if (send_packet(state, packet_buf, packet_size) < 0) {
                return -1;
            }
            
            current_rec += records_to_send;
        }
    }
    
    fclose(file);
    return 0;
}

/**
 * Wait for REC_MISS response and handle retransmissions
 * Returns 0 if blast complete (empty REC_MISS), 1 if retransmissions needed, -1 on error
 */
int wait_for_rec_miss(SenderState *state, uint32_t /*blast_start*/, uint32_t /*blast_end*/) {
    uint8_t response_buf[MAX_PACKET_SIZE];
    
    /* Set shorter timeout for REC_MISS */
    struct timeval tv;
    tv.tv_sec = BLAST_OVER_TIMEOUT;
    tv.tv_usec = 0;
    if (setsockopt(state->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt");
        return -1;
    }
    
    ssize_t received = recv_packet(state, response_buf, MAX_PACKET_SIZE);
    
    if (received <= 0) {
        /* Timeout or error */
        return 1;  /* Need to retry IS_BLAST_OVER */
    }
    
    if (response_buf[0] != PKT_REC_MISS) {
        fprintf(stderr, "Unexpected packet type: 0x%02x\n", response_buf[0]);
        return -1;
    }
    
    /* Parse REC_MISS packet */
    RecMissPacket *rec_miss = (RecMissPacket *)response_buf;
    rec_miss_to_host(rec_miss);
    
    uint16_t miss_count = rec_miss->miss_count;
    
    if (miss_count == 0) {
        printf("Blast complete! No missing records.\n");
        return 0;  /* Blast complete */
    }
    
    /* Extract missing segments */
    SegmentDescriptor *missing_list = (SegmentDescriptor *)(response_buf + sizeof(RecMissPacket));
    
    /* Convert segments from network byte order */
    for (uint16_t i = 0; i < miss_count; i++) {
        segment_to_host(&missing_list[i]);
    }
    
    /* Retransmit missing records */
    if (retransmit_records(state, missing_list, miss_count) < 0) {
        return -1;
    }
    
    return 1;  /* Retransmissions sent, need another IS_BLAST_OVER */
}

/**
 * Main transfer function - implements the blast protocol
 */
int transfer_file(SenderState *state) {
    /* Start disk reader thread */
    if (pthread_create(&state->disk_thread, NULL, disk_reader_thread, state) != 0) {
        perror("pthread_create");
        return -1;
    }
    
    /* Give disk reader a moment to start filling buffer */
    usleep(10000);  // 10ms
    
    uint32_t current_rec = 0;
    
    while (current_rec < state->total_records) {
        uint32_t blast_start = current_rec;
        uint32_t blast_end = current_rec + state->M - 1;
        if (blast_end >= state->total_records) {
            blast_end = state->total_records - 1;
        }
        
        /* Send blast */
        if (send_blast(state, blast_start, blast_end) < 0) {
            return -1;
        }
        
        /* Query for missing records and handle retransmissions */
        int blast_complete = 0;
        int attempts = 0;
        const int max_attempts = 10;
        
        while (!blast_complete && attempts < max_attempts) {
            /* Send IS_BLAST_OVER query */
            if (send_blast_over_query(state, blast_start, blast_end) < 0) {
                return -1;
            }
            
            /* Wait for REC_MISS and handle retransmissions */
            int result = wait_for_rec_miss(state, blast_start, blast_end);
            
            if (result == 0) {
                /* Blast complete */
                blast_complete = 1;
            } else if (result < 0) {
                /* Error */
                return -1;
            }
            /* else: retransmissions sent, loop continues */
            
            attempts++;
        }
        
        if (!blast_complete) {
            fprintf(stderr, "Failed to complete blast after %d attempts\n", max_attempts);
            return -1;
        }
        
        current_rec = blast_end + 1;
    }
    
    state->transfer_complete = 1;
    pthread_cond_broadcast(&state->buffer_not_full);
    pthread_cond_broadcast(&state->buffer_not_empty);
    pthread_join(state->disk_thread, NULL);
    
    return 0;
}

/* ============================================================================
 * MAIN FUNCTION
 * ============================================================================ */

int main(int argc, char *argv[]) {
    if (argc != 8) {
        fprintf(stderr, "Usage: %s <target_ip> <target_port> <filepath> <record_size> <M_initial> <loss_rate> <csv_file>\n", argv[0]);
        fprintf(stderr, "  target_ip: IP address of receiver/garbler\n");
        fprintf(stderr, "  target_port: Port of receiver/garbler\n");
        fprintf(stderr, "  filepath: Full path to the file to transfer\n");
        fprintf(stderr, "  record_size: 256, 512, or 1024 bytes\n");
        fprintf(stderr, "  M_initial: Initial records per blast (200-10000)\n");
        fprintf(stderr, "  loss_rate: Packet loss rate used by garbler (for logging)\n");
        fprintf(stderr, "  csv_file: Path to CSV file for results\n");
        exit(1);
    }
    
    /* Parse arguments */
    const char *target_ip = argv[1];
    int target_port = atoi(argv[2]);
    const char *filepath = argv[3];
    uint32_t record_size = atoi(argv[4]);
    uint32_t M_initial = atoi(argv[5]);
    double loss_rate = atof(argv[6]);
    const char *csv_file = argv[7];
    
    /* Validate arguments */
    if (record_size != 256 && record_size != 512 && record_size != 1024) {
        fprintf(stderr, "Error: record_size must be 256, 512, or 1024\n");
        exit(1);
    }
    
    if (M_initial < MIN_M_VALUE || M_initial > MAX_M_VALUE) {
        fprintf(stderr, "Error: M must be between %d and %d\n", MIN_M_VALUE, MAX_M_VALUE);
        exit(1);
    }
    
    /* Initialize sender state */
    SenderState state;
    memset(&state, 0, sizeof(state));
    
    state.file_size = get_file_size(filepath);
    if (state.file_size == 0) {
        fprintf(stderr, "Error: File is empty or cannot be read\n");
        exit(1);
    }

    strncpy(state.filename, filepath, MAX_FILENAME_LEN - 1);
    state.filename[MAX_FILENAME_LEN - 1] = '\0';
    
    state.record_size = record_size;
    state.M = M_initial;
    state.total_records = (state.file_size + record_size - 1) / record_size;
    
    /* Create socket */
    state.sock_fd = create_udp_socket();
    if (state.sock_fd < 0) {
        exit(1);
    }
    
    /* Setup destination address */
    memset(&state.dest_addr, 0, sizeof(state.dest_addr));
    state.dest_addr.sin_family = AF_INET;
    state.dest_addr.sin_port = htons(target_port);
    if (inet_pton(AF_INET, target_ip, &state.dest_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid IP address: %s\n", target_ip);
        exit(1);
    }
    
    /* Initialize threading primitives */
    pthread_mutex_init(&state.buffer_mutex, NULL);
    pthread_cond_init(&state.buffer_not_empty, NULL);
    pthread_cond_init(&state.buffer_not_full, NULL);
    
    /* Allocate record buffer (buffer for async disk reads) */
    state.buffer_capacity = 1000;  /* Buffer up to 1000 records */
    state.record_buffer = (uint8_t *)malloc(state.buffer_capacity * state.record_size);
    if (!state.record_buffer) {
        perror("malloc");
        exit(1);
    }
    
    printf("=== Fast File Transfer over UDP ===\n");
    printf("Target: %s:%d\n", target_ip, target_port);
    printf("File: %s (%llu bytes, %u records)\n", filepath, 
           (unsigned long long)state.file_size, state.total_records);
    printf("Record size: %u bytes\n", record_size);
    printf("Initial M: %u records/blast\n\n", M_initial);
    
    /* Phase 1: Establish connection */
    if (establish_connection(&state, filepath) < 0) {
        fprintf(stderr, "Failed to establish connection\n");
        exit(1);
    }
    
    state.start_time = time(NULL);
    
    /* Phase 2: Transfer file */
    if (transfer_file(&state) < 0) {
        fprintf(stderr, "File transfer failed\n");
        exit(1);
    }
    
    state.end_time = time(NULL);
    
    /* Calculate and print statistics */
    double duration = difftime(state.end_time, state.start_time);
    if (duration < 1.0) {
        duration = 1.0; // Avoid division by zero for very fast transfers
    }
    
    // Throughput in Megabytes per second (Mbps)
    double throughput_mbps = (double)(state.file_size * 8) / (duration * 1024 * 1024);

    printf("\n=== Transfer Complete ===\n");
    printf("Total packets sent: %u\n", state.packets_sent);
    printf("Total transfer time: %.2f seconds\n", duration);
    printf("Throughput rate: %.2f Mbps\n", throughput_mbps);

    // Write to CSV
    write_to_csv(csv_file, loss_rate, throughput_mbps, duration, state.packets_sent);
    
    /* Cleanup */
    free(state.record_buffer);
    pthread_mutex_destroy(&state.buffer_mutex);
    pthread_cond_destroy(&state.buffer_not_empty);
    pthread_cond_destroy(&state.buffer_not_full);
    close(state.sock_fd);
    
    return 0;
}