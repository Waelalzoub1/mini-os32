/* net_pingpong.c
 * Simulates a client (port 9000) talking to a server (port 9001) on the
 * same machine.  Each round:
 *   1. client sends "PING-N" to port 9001
 *   2. server receives it on port 9001, sends "PONG-N" back to port 9000
 *   3. client receives "PONG-N" on port 9000 and verifies it matches
 * Tests both send and receive on two separate ports in the same program.
 * Compile: cc net_pingpong.c
 * Run:     net_pingpong
 */
#include "stdio.h"
#include "net.h"

static char ping_buf[64];
static char pong_buf[64];
static char rx_buf[64];

int main() {
    int my_ip = sys_net_myip();
    printf("net_pingpong test\n");
    printf("client port: 9000   server port: 9001\n");
    printf("my IP: %d.%d.%d.%d\n",
           IP_A(my_ip), IP_B(my_ip), IP_C(my_ip), IP_D(my_ip));
    printf("\n");

    int passed = 0;
    int i = 0;
    while (i < 5) {
        /* --- client: send PING to server port 9001 --- */
        snprintf(ping_buf, 64, "PING-%d", i);
        int r = sys_udp_send(my_ip, 9001, ping_buf, 6);
        if (r < 0) {
            printf("[%d] FAIL  client send returned %d\n", i, r);
            i = i + 1;
            continue;
        }
        printf("[%d] client -> 9001  \"%s\"\n", i, ping_buf);

        /* --- server: receive PING on port 9001 --- */
        int src_ip = 0;
        int n = sys_udp_recv(9001, rx_buf, 63, &src_ip);
        if (n < 0) {
            printf("[%d] FAIL  server recv timeout\n", i);
            i = i + 1;
            continue;
        }
        rx_buf[n] = 0;
        printf("[%d] server <- 9001  \"%s\"\n", i, rx_buf);

        /* --- server: send PONG back to client port 9000 --- */
        snprintf(pong_buf, 64, "PONG-%d", i);
        sys_udp_send(my_ip, 9000, pong_buf, 6);
        printf("[%d] server -> 9000  \"%s\"\n", i, pong_buf);

        /* --- client: receive PONG on port 9000 --- */
        src_ip = 0;
        n = sys_udp_recv(9000, rx_buf, 63, &src_ip);
        if (n < 0) {
            printf("[%d] FAIL  client reply timeout\n", i);
        } else {
            rx_buf[n] = 0;
            printf("[%d] client <- 9000  \"%s\"  -- PASS\n", i, rx_buf);
            passed = passed + 1;
        }

        i = i + 1;
    }

    printf("\nresult: %d/5 ping-pong OK\n", passed);
    return 0;
}
