/* net_loopback.c
 * Sends 5 UDP packets to the machine's own IP on port 9001 and
 * receives each one back.  Tests: ARP, UDP send, QEMU loopback, UDP recv.
 * Compile: cc net_loopback.c
 * Run:     net_loopback
 */
#include "stdio.h"
#include "net.h"

static char send_buf[64];
static char recv_buf[64];

int main() {
    int my_ip = sys_net_myip();
    printf("net_loopback test\n");
    printf("my IP: %d.%d.%d.%d\n",
           IP_A(my_ip), IP_B(my_ip), IP_C(my_ip), IP_D(my_ip));
    printf("sending 5 packets to self on port 9001\n\n");

    int passed = 0;
    int i = 0;
    while (i < 5) {
        snprintf(send_buf, 64, "PKT-%d", i);

        int r = sys_udp_send(my_ip, 9001, send_buf, 5);
        if (r < 0) {
            printf("[%d] FAIL  send returned %d\n", i, r);
            i = i + 1;
            continue;
        }

        int src_ip = 0;
        int n = sys_udp_recv(9001, recv_buf, 63, &src_ip);
        if (n < 0) {
            printf("[%d] FAIL  recv timeout\n", i);
        } else {
            recv_buf[n] = 0;
            printf("[%d] OK    sent \"%s\"  got \"%s\"  from %d.%d.%d.%d\n",
                   i, send_buf, recv_buf,
                   IP_A(src_ip), IP_B(src_ip), IP_C(src_ip), IP_D(src_ip));
            passed = passed + 1;
        }
        i = i + 1;
    }

    printf("\nresult: %d/5 loopback OK\n", passed);
    return 0;
}
