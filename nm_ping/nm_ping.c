#include <assert.h>
#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <ifaddrs.h>
#include <net/ethernet.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <poll.h>

#define MAC_FORMAT "%02X:%02X:%02X:%02X:%02X:%02X"
#define MAC_VALUES(v) (v)[0], (v)[1], (v)[2], (v)[3], (v)[4], (v)[5]
#define IP4_FORMAT "%d.%d.%d.%d"
#define IP4_VALUES(v) (v)[0], (v)[1], (v)[2], (v)[3]
#define ITERATIONS 100000

#define PINGSRV_PORT 10000

#ifdef DEBUG_LOG
#define dprintf printf
#else
#define dprintf(...)
#endif

static void clientLoop();
static void serverLoop();
static uint8_t gSrcMac[ETHER_ADDR_LEN];
static struct in_addr gSrcAddr;
static uint8_t gDstMac[ETHER_ADDR_LEN];
static struct in_addr gDstAddr;
static struct nm_desc *gNetMap;
static int gIterationsLeft = ITERATIONS;

typedef struct Packet
{
    struct ether_header eth;
    struct ip ip;
    struct udphdr udp;
    uint64_t index;
} __attribute__((packed)) Packet;

static bool getMacAddress(const char *ifname, uint8_t *mac) {
    struct ifaddrs *addrs;
    bool found = false;

    if (getifaddrs(&addrs) < 0) {
        return false;
    }

    for (struct ifaddrs *cur = addrs; cur; cur = cur->ifa_next) {
        struct sockaddr_dl *linkaddr = (struct sockaddr_dl *)cur->ifa_addr;

        if (strcmp(cur->ifa_name, ifname)) {
            continue;
        }

        if (!linkaddr || linkaddr->sdl_family != AF_LINK) {
            continue;
        }

        memcpy(mac, LLADDR(linkaddr), ETHER_ADDR_LEN);
        found = true;
        break;
    }

    freeifaddrs(addrs);
    return found;
}

static bool getIPv4Address(const char *ifname, struct in_addr *addr) {
    struct ifaddrs *addrs;
    bool found = false;

    if (getifaddrs(&addrs) < 0) {
        return false;
    }

    for (struct ifaddrs *cur = addrs; cur; cur = cur->ifa_next) {
        struct sockaddr_in *inaddr = (struct sockaddr_in *)cur->ifa_addr;

        if (strcmp(cur->ifa_name, ifname)) {
            continue;
        }

        if (!inaddr || inaddr->sin_family != AF_INET) {
            continue;
        }

        *addr = inaddr->sin_addr;
        found = true;
        break;
    }

    freeifaddrs(addrs);
    return found;
}

static uint64_t xsum(const void *buf, size_t len)
{
    const uint16_t *hwords = (const uint16_t *)buf;
    uint64_t sum = 0;

    for (int i = 0; i < len >> 1; ++i) {
        sum += ntohs(hwords[i]);
    }

    if (len & 1) {
        sum += ((uint8_t *)buf)[len - 1] << 8;
    }

    return sum;
}

static uint16_t finsum(uint64_t sum)
{
    while (sum > 0xFFFF) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return htons(~(uint16_t)sum);
}

static uint16_t ipsum(struct ip *ip)
{
    return finsum(xsum(ip, offsetof(struct ip, ip_sum)) +
        xsum((uint8_t*)ip + offsetof(struct ip, ip_src),
            (ip->ip_hl << 2) - offsetof(struct ip, ip_src)));
}

static uint16_t udpsum(struct ip *ip, struct udphdr *udp, void *data)
{
    struct ippseudo ips;

    ips.ippseudo_src = ip->ip_src;
    ips.ippseudo_dst = ip->ip_dst;
    ips.ippseudo_pad = 0;
    ips.ippseudo_p = ip->ip_p;
    ips.ippseudo_len = udp->uh_ulen;

    return finsum(
        xsum(&ips, sizeof(ips)) +
        xsum(udp, offsetof(struct udphdr, uh_sum)) +
        xsum(data, ntohs(udp->uh_ulen) - sizeof(struct udphdr)));
}

static void usage()
{
    fprintf(stderr,
        "Usage:\n"
        "  %s <iface>                      # Starts server\n"
        "  %s <iface> <dest-mac> <dest-ip> # Starts client\n\n",
        getprogname(), getprogname());
    exit(1);
}

int main(int argc, const char **argv)
{
    char ifname[256];

    if (argc < 2) {
        usage();
    }

    if (!getMacAddress(argv[1], gSrcMac)) {
        fprintf(stderr, "Unable to get local MAC address\n");
        return 1;
    }

    if (!getIPv4Address(argv[1], &gSrcAddr)) {
        fprintf(stderr, "Unable to get local IP address\n");
        return 1;
    }

    if (argc > 2) {
        uint32_t serverAddrBytes[ETHER_ADDR_LEN];

        if (argc < 4) {
            usage();
        }

        if (sscanf(argv[2], MAC_FORMAT, serverAddrBytes, serverAddrBytes + 1,
            serverAddrBytes + 2, serverAddrBytes + 3, serverAddrBytes + 4,
            serverAddrBytes + 5) != ETHER_ADDR_LEN) {

            fprintf(stderr, "Invalid server MAC address.\n");
            return 1;
        }

        for (int i = 0; i < ETHER_ADDR_LEN; ++i) {
            gDstMac[i] = (uint8_t)serverAddrBytes[i];
        }

        if (inet_pton(AF_INET, argv[3], &gDstAddr) != 1) {
            fprintf(stderr, "Unable to parse destination IP address\n");
            return 1;
        }
    }

    snprintf(ifname, sizeof(ifname), "netmap:%s", argv[1]);
    printf("Opening %s (" MAC_FORMAT ")\n", argv[1], MAC_VALUES(gSrcMac));
    gNetMap = nm_open(ifname, 0, 0, 0);

    if (!gNetMap) {
        printf("Can't access netmap.\n");
        return 1;
    }

    if (argc > 2) {
        printf("Pinging " MAC_FORMAT ", ip " IP4_FORMAT ".\n",
            MAC_VALUES(gDstMac), IP4_VALUES((uint8_t *)&gDstAddr.s_addr));
        clientLoop();
    } else {
        serverLoop();
    }

    nm_close(gNetMap);

    return 0;
}

static void printArp(struct arphdr *arp)
{
    uint8_t *sha = (uint8_t *)ar_sha(arp);
    uint8_t *spa = (uint8_t *)ar_spa(arp);
    uint8_t *tha = (uint8_t *)ar_tha(arp);
    uint8_t *tpa = (uint8_t *)ar_tpa(arp);

    printf("ARP(%d): sha=" MAC_FORMAT " spa=" IP4_FORMAT
        " tha=" MAC_FORMAT " tpa=" IP4_FORMAT "\n",
        ntohs(arp->ar_op), MAC_VALUES(sha), IP4_VALUES(spa),
        MAC_VALUES(tha), IP4_VALUES(tpa));
}

static void onArpReceived(struct nm_pkthdr *hdr, uint8_t *buf)
{
    struct arphdr *arp = (struct arphdr *)(buf + sizeof(struct ether_header));

    if (hdr->len < sizeof(struct ether_header) + sizeof(struct arphdr) ||
        ntohs(arp->ar_hrd) != ARPHRD_ETHER ||
        ntohs(arp->ar_pro) != ETHERTYPE_IP ||
        arp->ar_hln != ETHER_ADDR_LEN ||
        arp->ar_pln != sizeof(struct in_addr) ||
        hdr->len < sizeof(struct ether_header) +
            sizeof(struct arphdr) +
            arp->ar_hln * 2 +
            arp->ar_pln * 2) {

        return;
    }

    uint8_t *sha = (uint8_t *)ar_sha(arp);
    uint8_t *spa = (uint8_t *)ar_spa(arp);
    uint8_t *tpa = (uint8_t *)ar_tpa(arp);

    printArp(arp);

    if (memcmp(tpa, &gSrcAddr, sizeof(struct in_addr)) ||
        ntohs(arp->ar_op) != ARPOP_REQUEST) {
        return;
    }

    struct {
        struct ether_header eth;
        struct arphdr arp;
        uint8_t sha[ETHER_ADDR_LEN];
        struct in_addr spa;
        uint8_t tha[ETHER_ADDR_LEN];
        struct in_addr tpa;
    } __attribute__((packed)) reply;

    memcpy(reply.eth.ether_dhost, sha, ETHER_ADDR_LEN);
    memcpy(reply.eth.ether_shost, gSrcMac, ETHER_ADDR_LEN);
    reply.eth.ether_type = htons(ETHERTYPE_ARP);
    reply.arp.ar_hrd = htons(ARPHRD_ETHER);
    reply.arp.ar_pro = htons(ETHERTYPE_IP);
    reply.arp.ar_hln = ETHER_ADDR_LEN;
    reply.arp.ar_pln = sizeof(struct in_addr);
    reply.arp.ar_op = htons(ARPOP_REPLY);
    memcpy(reply.sha, gSrcMac, ETHER_ADDR_LEN);
    memcpy(&reply.spa, &gSrcAddr, sizeof(struct in_addr));
    memcpy(reply.tha, sha, ETHER_ADDR_LEN);
    memcpy(&reply.tpa, spa, sizeof(struct in_addr));

    printf("--> ");
    printArp(&reply.arp);

    if (!nm_inject(gNetMap, &reply, sizeof(reply))) {
        fprintf(stderr, "Failed to send ARP reply\n");
    }
}

static void clientLoop()
{
    uint64_t index = 0;
    bool waiting = false;
    struct pollfd pfd;
    uint8_t *buf;
    struct nm_pkthdr hdr;

    pfd.fd = NETMAP_FD(gNetMap);
    pfd.events = POLLIN;

    // Give the link a bit of time to come back up after initializing netmap.
    sleep(2);

    while (gIterationsLeft) {
        if (!waiting) {
            Packet pkt = {};

            waiting = true;
            memcpy(pkt.eth.ether_dhost, gDstMac, sizeof(gDstMac));
            memcpy(pkt.eth.ether_shost, gSrcMac, sizeof(gSrcMac));
            pkt.eth.ether_type = htons(ETHERTYPE_IP);
            pkt.ip.ip_v = IPVERSION;
            pkt.ip.ip_hl = sizeof(pkt.ip) >> 2;
            pkt.ip.ip_tos = IPTOS_LOWDELAY;
            pkt.ip.ip_len = htons(sizeof(pkt) - sizeof(struct ether_header));
            pkt.ip.ip_id = 0;
            pkt.ip.ip_off = htons(IP_DF);
            pkt.ip.ip_ttl = IPDEFTTL;
            pkt.ip.ip_p = IPPROTO_UDP;
            pkt.ip.ip_src = gSrcAddr;
            pkt.ip.ip_dst = gDstAddr;
            pkt.ip.ip_sum = ipsum(&pkt.ip);
            pkt.udp.uh_sport = htons(PINGSRV_PORT);
            pkt.udp.uh_dport = htons(PINGSRV_PORT);
            pkt.udp.uh_ulen = htons(
                sizeof(pkt) - sizeof(struct ip) - sizeof(struct ether_header));
            pkt.index = ++index;
            pkt.udp.uh_sum = udpsum(&pkt.ip, &pkt.udp, &pkt.index);

            if (!nm_inject(gNetMap, &pkt, sizeof(Packet))) {
                fprintf(stderr, "Failed to write packet to TX chain.\n");
                return;
            }

            dprintf("--> %016" PRIx64 "\n", index);
        }

        while ((buf = nm_nextpkt(gNetMap, &hdr))) {
            struct ether_header *eth = (struct ether_header *)buf;
            Packet *pkt = (Packet *)buf;
            uint16_t etype = ntohs(eth->ether_type);

            if (etype == ETHERTYPE_ARP) {
                onArpReceived(&hdr, buf);
                continue;
            }

            if (etype != ETHERTYPE_IP ||
                hdr.len < sizeof(struct ether_header) + sizeof(struct ip) ||
                pkt->ip.ip_p != IPPROTO_UDP) {

                continue;
            }

            if (hdr.len < sizeof(Packet)) {
                continue;
            }

            if (memcmp(pkt->eth.ether_dhost, gSrcMac, ETHER_ADDR_LEN)) {
                continue;
            }

            if (memcmp(pkt->eth.ether_shost, gDstMac, ETHER_ADDR_LEN)) {
                continue;
            }

            if (ntohs(pkt->eth.ether_type) != ETHERTYPE_IP) {
                continue;
            }


            if (pkt->ip.ip_src.s_addr != gDstAddr.s_addr) {
                continue;
            }

            if (pkt->ip.ip_dst.s_addr != gSrcAddr.s_addr) {
                continue;
            }

            if (ntohs(pkt->udp.uh_dport) != PINGSRV_PORT) {
                continue;
            }

            if (pkt->index != index) {
                fprintf(stderr, "Index mismatch in ping response.\n");
                return;
            }

            waiting = false;
            gIterationsLeft--;

            if ((gIterationsLeft % 1000) == 0) {
                printf("Iterations left: %d\n", gIterationsLeft);
            }
        }

        if (waiting) {
            poll(&pfd, 1, -1);
        }
    }
}

static void serverLoop()
{
}

