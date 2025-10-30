#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctime>
#include <cerrno>
#include <fstream>
#include <string>

#define MAX_PACKET_SIZE 65507

/* Connection tracking */
typedef struct {
    struct sockaddr_in sender_addr;    /* Sender's address */
    struct sockaddr_in receiver_addr;  /* Receiver's address */
    int active;                        /* Connection is active */
    time_t last_activity;              /* Last packet timestamp */
} Connection;

// Garbler State 
typedef struct {
    int sock_fd;                       /* UDP socket */
    int listen_port;                   /* Port to listen on */
    char receiver_ip[INET_ADDRSTRLEN]; /* Receiver IP address */
    int receiver_port;                 /* Receiver port */
    double loss_rate;                  /* Packet loss probability (0.0 - 1.0) */
    char csv_file[256];                /* CSV file path for logging */
    
    Connection conn;                   /* Active connection */
    
    /* Statistics */
    uint64_t packets_forwarded;
    uint64_t packets_dropped;
    uint64_t sender_to_receiver;        /* Forwarded S->R */
    uint64_t receiver_to_sender;        /* Forwarded R->S */
    uint64_t dropped_sender_to_receiver;
    uint64_t dropped_receiver_to_sender;
    
    /* Idle detection for final statistics */
    time_t last_packet_time;           /* Timestamp of last packet */
    int idle_timeout_seconds;          /* Seconds to wait before assuming done */
    int final_stats_printed;           /* Flag to print stats only once */
} GarblerState;

/* ============================================================================
 * UTILITY FUNCTIONS
 * ============================================================================ */

/**
 * Print usage information and exit
 */
void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s <listen_port> <receiver_ip> <receiver_port> <loss_rate> <csv_file>\n", prog_name);
    fprintf(stderr, "  listen_port: Port to listen on for sender connections\n");
    fprintf(stderr, "  receiver_ip: IP address of the receiver\n");
    fprintf(stderr, "  receiver_port: Port of the receiver\n");
    fprintf(stderr, "  loss_rate: Packet loss probability (0.0 = no loss, 1.0 = 100%% loss)\n");
    fprintf(stderr, "  csv_file: Path to CSV file for logging results\n");
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s 9000 192.168.1.100 8080 0.10 results.csv   # 10%% packet loss\n", prog_name);
    exit(1);
}
/**
 * Initialize random number generator with current time
 */
void init_random() {
    srand((unsigned int)time(NULL));
}

/**
 * Generate random number between 0.0 and 1.0
 */
double random_double() {
    return (double)rand() / (double)RAND_MAX;
}

/**
 * Decide whether to drop a packet based on loss rate
 * Returns 1 if packet should be dropped, 0 otherwise
 */
int should_drop_packet(double loss_rate) {
    return random_double() < loss_rate;
}

/**
 * Create and bind UDP socket
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
    
    printf("Garbler listening on port %d\n", port);
    return sock_fd;
}

/**
 * Compare two sockaddr_in addresses
 * Returns 1 if addresses match, 0 otherwise
 */
int addr_equals(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return (a->sin_addr.s_addr == b->sin_addr.s_addr) &&
           (a->sin_port == b->sin_port);
}

/**
 * Convert sockaddr_in to string for logging
 */
void addr_to_string(const struct sockaddr_in *addr, char *buffer, size_t buf_size) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
    snprintf(buffer, buf_size, "%s:%d", ip, ntohs(addr->sin_port));
}

/* ============================================================================
 * STATISTICS DISPLAY
 * ============================================================================ */

/**
 * Write loss rate and throughput data to CSV file
 * Called when transfer completes
 */
void write_to_csv(GarblerState *state, const char* csv_file) {
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
        file << "loss_rate,packets_forwarded,packets_dropped,total_packets,actual_loss_percent\n";
    }
    
    // Calculate statistics
    uint64_t total = state->packets_forwarded + state->packets_dropped;
    double actual_loss = (total > 0) ? 
                         (double)state->packets_dropped / total * 100 : 0;
    
    // Write data
    file << state->loss_rate << "," 
         << state->packets_forwarded << "," 
         << state->packets_dropped << "," 
         << total << "," 
         << actual_loss << "\n";
    
    file.close();
    
    printf("Statistics written to CSV: %s\n", csv_file);
}

/**
 * Print final statistics when transfer completes
 */
void print_final_statistics(GarblerState *state, const char* csv_file) {
    uint64_t total = state->packets_forwarded + state->packets_dropped;
    double actual_loss = (total > 0) ? 
                         (double)state->packets_dropped / total * 100 : 0;
    
    printf("\n----------------------------------------\n");
    printf("|   TRANSFER COMPLETE - FINAL STATS     |\n");
    printf("----------------------------------------\n");
    printf("Total packets processed: %llu\n", total);
    printf("  ├─ Forwarded: %llu (%.2f%%)\n", state->packets_forwarded,
           (total > 0) ? (double)state->packets_forwarded / total * 100 : 0);
    printf("  └─ Dropped: %llu (%.2f%%)\n", state->packets_dropped, actual_loss);
    printf("      ├─ Dropped (Sender → Receiver): %llu\n", state->dropped_sender_to_receiver);
    printf("      └─ Dropped (Receiver → Sender): %llu\n", state->dropped_receiver_to_sender);
    
    printf("\nTraffic breakdown (forwarded only):\n");
    printf("  ├─ Sender → Receiver: %llu packets\n", state->sender_to_receiver);
    printf("  └─ Receiver → Sender: %llu packets\n", state->receiver_to_sender);
    printf("----------------------------------------\n\n");
    
    // Write to CSV
    // if (csv_file != NULL) {
    //     write_to_csv(state, csv_file);
    // }
}

/* ============================================================================
 * PACKET FORWARDING LOGIC
 * ============================================================================ */

/**
 * Forward a packet from sender to receiver
 * Applies packet loss simulation
 * Returns 0 on success, -1 on error
 */
int forward_to_receiver(GarblerState *state, const uint8_t *packet, size_t len) {
    /* Simulate packet loss */
    if (should_drop_packet(state->loss_rate)) {
        state->packets_dropped++;
        state->dropped_sender_to_receiver++;
        printf("→ DROP: Packet from sender (%.1f%% loss rate)\n", state->loss_rate * 100);
        return 0;  /* Packet dropped, but not an error */
    }
    
    /* Forward packet to receiver */
    ssize_t sent = sendto(state->sock_fd, packet, len, 0,
                          (struct sockaddr *)&state->conn.receiver_addr,
                          sizeof(state->conn.receiver_addr));
    
    if (sent < 0) {
        perror("sendto (to receiver)");
        return -1;
    }
    
    state->packets_forwarded++;
    state->sender_to_receiver++;
    
    return 0;
}

/**
 * Forward a packet from receiver to sender
 * Applies packet loss simulation
 * Returns 0 on success, -1 on error
 */
int forward_to_sender(GarblerState *state, const uint8_t *packet, size_t len) {
    /* Simulate packet loss */
    if (should_drop_packet(state->loss_rate)) {
        state->packets_dropped++;
        state->dropped_receiver_to_sender++;
        printf("← DROP: Packet from receiver (%.1f%% loss rate)\n", state->loss_rate * 100);
        return 0;  /* Packet dropped, but not an error */
    }
    
    /* Forward packet to sender */
    ssize_t sent = sendto(state->sock_fd, packet, len, 0,
                          (struct sockaddr *)&state->conn.sender_addr,
                          sizeof(state->conn.sender_addr));
    
    if (sent < 0) {
        perror("sendto (to sender)");
        return -1;
    }
    
    state->packets_forwarded++;
    state->receiver_to_sender++;
    
    return 0;
}

/**
 * Process incoming packet and forward to appropriate destination
 * Establishes connection on first packet from sender
 */
int process_packet(GarblerState *state, const uint8_t *packet, size_t len,
                   const struct sockaddr_in *src_addr) {
    char src_str[64];
    addr_to_string(src_addr, src_str, sizeof(src_str));
    
    /* Check if this is from the sender or receiver */
    if (!state->conn.active) {
        /* First packet - establish connection */
        state->conn.sender_addr = *src_addr;
        state->conn.active = 1;
        state->conn.last_activity = time(NULL);
        
        char sender_str[64];
        addr_to_string(&state->conn.sender_addr, sender_str, sizeof(sender_str));
        printf("\n=== Connection Established ===\n");
        printf("Sender: %s\n", sender_str);
        printf("Receiver: %s:%d\n", state->receiver_ip, state->receiver_port);
        printf("Loss rate: %.1f%%\n\n", state->loss_rate * 100);
        
        /* Forward to receiver */
        printf("→ FWD: Sender → Receiver (%zu bytes)\n", len);
        return forward_to_receiver(state, packet, len);
    }
    
    /* Update last activity timestamp */
    state->conn.last_activity = time(NULL);
    
    /* Determine direction and forward */
    if (addr_equals(src_addr, &state->conn.sender_addr)) {
        /* Packet from sender -> forward to receiver */
        printf("→ FWD: Sender → Receiver (%zu bytes)\n", len);
        return forward_to_receiver(state, packet, len);
    } else if (addr_equals(src_addr, &state->conn.receiver_addr)) {
        /* Packet from receiver -> forward to sender */
        printf("← FWD: Receiver → Sender (%zu bytes)\n", len);
        return forward_to_sender(state, packet, len);
    } else {
        /* Packet from unknown source - ignore */
        printf("? IGNORE: Unknown source %s\n", src_str);
        return 0;
    }
}

/* ============================================================================
 * MAIN RELAY LOOP
 * ============================================================================ */

/**
 * Main packet relay loop
 * Continuously receives and forwards packets
 */
void run_garbler(GarblerState *state) {
    uint8_t packet_buf[MAX_PACKET_SIZE];
    struct sockaddr_in src_addr;
    socklen_t addr_len;
    
    /* Set socket timeout to detect idle periods */
    struct timeval tv;
    tv.tv_sec = 2;  /* Check every 2 seconds */
    tv.tv_usec = 0;
    if (setsockopt(state->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt");
        return;
    }
    
    state->idle_timeout_seconds = 5;  /* Consider done after 5 seconds of no traffic */
    state->last_packet_time = time(NULL);
    state->final_stats_printed = 0;
    
    printf("Garbler running... Press Ctrl+C to stop\n\n");
    
    while (1) {
        addr_len = sizeof(src_addr);
        
        /* Receive packet */
        ssize_t received = recvfrom(state->sock_fd, packet_buf, MAX_PACKET_SIZE, 0,
                                    (struct sockaddr *)&src_addr, &addr_len);
        
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                time_t now = time(NULL);
                time_t idle_time = now - state->last_packet_time;
                
                if (idle_time >= state->idle_timeout_seconds) {
                    if (!state->final_stats_printed && state->conn.active) {
                        printf("\n[No traffic for %ld seconds - Transfer appears complete]\n", idle_time);
                        print_final_statistics(state, state->csv_file);  // Pass csv_file here
                        state->final_stats_printed = 1;
                        
                        /* Reset for potential new transfer */
                        state->conn.active = 0;
                    }
                }
                continue;
            }
            perror("recvfrom");
            break;
        }
        
        if (received == 0) {
            continue;
        }
        
        /* Update last packet time */
        state->last_packet_time = time(NULL);
        state->final_stats_printed = 0;  /* Reset if new packets arrive */
        
        /* Process and forward packet */
        process_packet(state, packet_buf, received, &src_addr);
        
        /* Print periodic statistics (every 100 packets) */
        if ((state->packets_forwarded + state->packets_dropped) % 100 == 0) {
            uint64_t total = state->packets_forwarded + state->packets_dropped;
            double actual_loss = (total > 0) ? 
                                 (double)state->packets_dropped / total * 100 : 0;
            
            printf("\n--- Periodic Statistics ---\n");
            // FIX: Use %llu for uint64_t
            printf("Total packets: %llu\n", total);
            printf("Forwarded: %llu (%.1f%%)\n", state->packets_forwarded,
                   (total > 0) ? (double)state->packets_forwarded / total * 100 : 0);
            printf("Dropped: %llu (%.1f%%)\n", state->packets_dropped, actual_loss);
            printf("  └─ S→R: %llu | R→S: %llu\n", 
                   state->dropped_sender_to_receiver, state->dropped_receiver_to_sender);
            printf("Fwd S→R: %llu\n", state->sender_to_receiver);
            printf("Fwd R→S: %llu\n\n", state->receiver_to_sender);
        }
    }
}

/* ============================================================================
 * MAIN FUNCTION
 * ============================================================================ */

int main(int argc, char *argv[]) {
    if (argc != 6) {
        print_usage(argv[0]);
    }
    
    /* Parse command-line arguments */
    int listen_port = atoi(argv[1]);
    const char *receiver_ip = argv[2];
    int receiver_port = atoi(argv[3]);
    double loss_rate = atof(argv[4]);
    const char *csv_file = argv[5];
    
    /* Validate arguments */
    if (listen_port <= 0 || listen_port > 65535) {
        fprintf(stderr, "Error: Invalid listen port\n");
        exit(1);
    }
    
    if (receiver_port <= 0 || receiver_port > 65535) {
        fprintf(stderr, "Error: Invalid receiver port\n");
        exit(1);
    }
    
    if (loss_rate < 0.0 || loss_rate > 1.0) {
        fprintf(stderr, "Error: Loss rate must be between 0.0 and 1.0\n");
        exit(1);
    }
    
    /* Initialize garbler state */
    GarblerState state;
    memset(&state, 0, sizeof(state));
    
    state.listen_port = listen_port;
    strncpy(state.receiver_ip, receiver_ip, sizeof(state.receiver_ip) - 1);
    state.receiver_port = receiver_port;
    state.loss_rate = loss_rate;
    strncpy(state.csv_file, csv_file, sizeof(state.csv_file) - 1);
    
    /* Setup receiver address */
    memset(&state.conn.receiver_addr, 0, sizeof(state.conn.receiver_addr));
    state.conn.receiver_addr.sin_family = AF_INET;
    state.conn.receiver_addr.sin_port = htons(receiver_port);
    
    if (inet_pton(AF_INET, receiver_ip, &state.conn.receiver_addr.sin_addr) <= 0) {
        fprintf(stderr, "Error: Invalid receiver IP address: %s\n", receiver_ip);
        exit(1);
    }
    
    /* Initialize random number generator for packet loss simulation */
    init_random();
    
    /* Create and bind socket */
    state.sock_fd = create_and_bind_socket(listen_port);
    if (state.sock_fd < 0) {
        exit(1);
    }
    
    printf("=== UDP Packet Garbler ===\n");
    printf("Listen port: %d\n", listen_port);
    printf("Forward to: %s:%d\n", receiver_ip, receiver_port);
    printf("Packet loss rate: %.1f%%\n\n", loss_rate * 100);
    // printf("CSV output: %s\n\n", csv_file);
    
    /* Run main relay loop */
    run_garbler(&state);
    
    /* Print final statistics if not already printed */
    if (!state.final_stats_printed && state.conn.active) {
        print_final_statistics(&state, state.csv_file);
    }
    
    /* Cleanup */
    close(state.sock_fd);
    
    return 0;
}