#ifndef NET_H
#define NET_H

/* Build an IPv4 address as a single int: MKIP(10,0,2,15) = 10.0.2.15 */
#define MKIP(a,b,c,d) (((a)<<24)|((b)<<16)|((c)<<8)|(d))

/* Extract octets from a packed IP */
#define IP_A(ip) (((ip)>>24)&0xFF)
#define IP_B(ip) (((ip)>>16)&0xFF)
#define IP_C(ip) (((ip)>>8)&0xFF)
#define IP_D(ip) ((ip)&0xFF)

/*
 * sys_udp_send(dst_ip, dst_port, buf, len)
 *   Send len bytes from buf as a UDP packet to dst_ip:dst_port.
 *   Returns bytes sent, or -1 on failure (no route / ARP timeout).
 *
 * sys_udp_recv(my_port, buf, maxlen, src_ip)
 *   Block until a UDP packet arrives on my_port (0 = any port).
 *   Copies payload into buf (up to maxlen bytes).
 *   If src_ip is not 0, writes the sender's IP there.
 *   Returns bytes received, or -1 on timeout (~1 second).
 *
 * sys_net_myip()
 *   Returns this machine's IP as a packed int (always 10.0.2.15
 *   under QEMU user-mode networking).
 */
int sys_udp_send(int dst_ip, int dst_port, void *buf, int len);
int sys_udp_recv(int my_port, void *buf, int maxlen, int *src_ip);
int sys_udp_recv_nb(int my_port, void *buf, int maxlen, int *src_ip);
int sys_net_myip(void);

#endif
