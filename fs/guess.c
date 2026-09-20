#include <stdio.h>

// save func: board keeps the 10 best (fewest tries) wins, best first
int save(int score) {
int scores[10];
int size = 10;

// no board yet: start a fresh one, 9999 marks an empty slot
if (sys_load("board", scores, sizeof(scores)) < 0) {
for (int i=0;i<size;i++) scores[i] = 9999;
scores[0] = score;
return sys_save("board", scores, sizeof(scores));
}

// find the topmost slot this score beats
int place = size;
for (int i=size-1;i>=0;i--) {
if (scores[i] > score) place = i;
}
// beat nothing: not a top score
if (place == size) return 0;

// shift everything from place down one slot, last one falls off
for (int g=size-1;g>place;g--) scores[g] = scores[g-1];
// put the new score in its place
scores[place] = score;
return sys_save("board", scores, sizeof(scores));
}

// the board file is raw ints (type shows symbols), so print it from here
void show_board(void) {
int scores[10];
if (sys_load("board", scores, sizeof(scores)) < 0) return;
printf("best tries:\n");
for (int i=0;i<10;i++) {
if (scores[i] == 9999) break;
printf("%d. %d\n", i+1, scores[i]);
}
}

int main(void) {

//vars
int guess, secret;
int tries = 0;
secret = sys_ticks() % 100 + 1;

//guessing loop
while (1) {
//get guess
tries++;
printf("type your guess: ");
scanf("%d", &guess);

// if real is lower
if (guess > secret) {
printf("lower\n");
}
//if real is higher
else if (guess < secret) {
printf("higher\n");
}
//if correct
else {
printf("correct!!! it was %d in %d tries\n", secret, tries);
save(tries);
show_board();
break;
}
}
return 0;
}
