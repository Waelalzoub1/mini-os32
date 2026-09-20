// arrays, global arrays, 2D arrays, array of structs
#include <stdio.h>
int g[5];
int grid[3][4];
struct P { int x; int y; };
struct P pts[3];
int main() {
    int i; int j;
    for (i = 0; i < 5; i = i + 1) g[i] = i * i;
    printf("%d %d %d\n", g[0], g[2], g[4]);
    for (i = 0; i < 3; i = i + 1) for (j = 0; j < 4; j = j + 1) grid[i][j] = i * 10 + j;
    printf("%d %d %d\n", grid[0][0], grid[1][2], grid[2][3]);
    char local[3][8];
    local[1][0] = 'h'; local[1][1] = 'i'; local[1][2] = 0;
    printf("%s\n", local[1]);
    pts[1].x = 7; pts[1].y = 9;
    printf("%d %d %d\n", pts[1].x + pts[1].y, sizeof(g), sizeof(grid));
    return 0;
}
