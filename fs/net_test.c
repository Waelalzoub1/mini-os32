/* net_test.c
 * Runs a server (port 9001) and a client (port 9000) in one program using
 * a non-blocking poll loop -- no threads needed, no second window needed.
 *
 * How it works:
 *   Every 10ms the main loop checks both ports with sys_udp_recv_nb().
 *   The client side sends one PING every 500ms (50 loop ticks).
 *   The server side echoes any PING it receives back as a PONG to port 9000.
 *   The test ends when the client has received all 5 PONGs or times out.
 *
 * Compile: cc net_test.c
 * Run:     net_test
 */
#include "stdio.h"
#include "net.h"

static char tx_buf[64];
static char rx_buf[64];

int main() {
    int my_ip = sys_net_myip();
    printf("net_test: server=9001  client=9000\n");
    printf("my IP: %d.%d.%d.%d\n\n",
           IP_A(my_ip), IP_B(my_ip), IP_C(my_ip), IP_D(my_ip));

    int pings_sent  = 0;
    int pongs_recvd = 0;
    int tick        = 0;
    int timeout     = 3000; /* 30 seconds max (3000 x 10ms) */

    while (pongs_recvd < 5 && timeout > 0) {

        /* --- client: send a PING every 500ms (every 50 ticks) --- */
        if (pings_sent < 5 && (tick % 50) == 0) {
            snprintf(tx_buf, 64, "PING-%d", pings_sent);
            sys_udp_send(my_ip, 9001, tx_buf, 6);
            printf("[client] sent \"%s\" to port 9001\n", tx_buf);
            pings_sent = pings_sent + 1;
        }

        /* --- server: check port 9001, echo any packet back to port 9000 --- */
        int src = 0;
        int n = sys_udp_recv_nb(9001, rx_buf, 63, &src);
        if (n > 0) {
            rx_buf[n] = 0;
            printf("[server] got \"%s\" on port 9001, echoing to 9000\n", rx_buf);
            /* build PONG reply */
            rx_buf[1] = 'O'; rx_buf[2] = 'N'; rx_buf[3] = 'G';
            sys_udp_send(my_ip, 9000, rx_buf, n);
        }

        /* --- client: check port 9000 for PONG replies --- */
        src = 0;
        n = sys_udp_recv_nb(9000, rx_buf, 63, &src);
        if (n > 0) {
            rx_buf[n] = 0;
            printf("[client] got \"%s\" on port 9000  -- PASS\n", rx_buf);
            pongs_recvd = pongs_recvd + 1;
        }

        sys_sleep(10);
        tick = tick + 1;
        timeout = timeout - 1;
    }

    printf("\nresult: %d/5 ping-pong OK\n", pongs_recvd);
    return 0;
}
