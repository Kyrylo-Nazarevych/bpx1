/* BPX-1 bench: puzzles and games. Select one with -DSEL=<n>:
     1 games.sudoku   backtracking sudoku solver (row, column and box bit masks, cells in reading
                      order) on generated puzzles
     2 games.connect4 connect-four on a 6 x 5 board: alpha-beta negamax self-play, with win detection
                      along four directions
     3 games.life     Conway's Game of Life on a 64 x 48 torus from a random start
     4 games.maze     mazes: generation by randomised depth-first search, a wall follower, then
                      shortest path by breadth-first flood fill and by A* with a binary heap

   Self-checks (exit codes 1..19): every solved grid is a valid sudoku that keeps the clues;
   alpha-beta gives the same value as plain minimax; a glider moves one cell diagonally in four
   generations; the wall follower reaches the exit and A* finds the flood-fill distance. */
#include "rt.h"

#if SEL == 1
#define PUZZLES 30
#define NODE_LIMIT 60000
static u8 grid[81], clue[81];
static u16 rowm[9], colm[9], boxm[9];
static u8 row_of[81], col_of[81], box_of[81];
static u32 nodes;

/* 1 = solved, 0 = no solution from here, -1 = gave up (node limit) */
static int solve(int pos) {
    while (pos < 81 && grid[pos]) pos++;
    if (pos == 81) return 1;
    int r = row_of[pos], c = col_of[pos], b = box_of[pos];
    u32 used = (u32)(rowm[r] | colm[c] | boxm[b]);
    for (int digit = 1; digit <= 9; digit++) {
        u32 bit = 1u << digit;
        if (used & bit) continue;
        if (++nodes > NODE_LIMIT) return -1;
        grid[pos] = (u8)digit;
        rowm[r] |= (u16)bit;
        colm[c] |= (u16)bit;
        boxm[b] |= (u16)bit;
        int res = solve(pos + 1);
        if (res) return res;
        grid[pos] = 0;
        rowm[r] &= (u16)~bit;
        colm[c] &= (u16)~bit;
        boxm[b] &= (u16)~bit;
    }
    return 0;
}
static void shuffle3(u8 *p) {                       /* random order of 0, 1, 2 */
    p[0] = 0; p[1] = 1; p[2] = 2;
    for (int i = 2; i > 0; i--) {
        u32 j = rt_rand_n((u32)i + 1);
        u8 t = p[i];
        p[i] = p[j];
        p[j] = t;
    }
}
/* A valid full grid (a shuffled copy of the pattern (3r + r/3 + c) mod 9), with `holes` cells blanked. */
static void make_puzzle(int holes) {
    u8 digit[9], rows[9], cols[9], band[3], in[3];
    for (int i = 0; i < 9; i++) digit[i] = (u8)(i + 1);
    for (int i = 8; i > 0; i--) {
        u32 j = rt_rand_n((u32)i + 1);
        u8 t = digit[i];
        digit[i] = digit[j];
        digit[j] = t;
    }
    shuffle3(band);
    for (int b = 0; b < 3; b++) {
        shuffle3(in);
        for (int k = 0; k < 3; k++) rows[3 * b + k] = (u8)(3 * band[b] + in[k]);
    }
    shuffle3(band);
    for (int b = 0; b < 3; b++) {
        shuffle3(in);
        for (int k = 0; k < 3; k++) cols[3 * b + k] = (u8)(3 * band[b] + in[k]);
    }
    for (int r = 0; r < 9; r++)
        for (int c = 0; c < 9; c++) grid[9 * r + c] = digit[(3 * rows[r] + rows[r] / 3 + cols[c]) % 9];
    while (holes > 0) {
        u32 p = rt_rand_n(81);
        if (grid[p]) { grid[p] = 0; holes--; }
    }
}

int main(void) {
    u32 chk = 0;
    int gave_up = 0;
    for (int i = 0; i < 81; i++) {
        row_of[i] = (u8)(i / 9);
        col_of[i] = (u8)(i % 9);
        box_of[i] = (u8)(i / 27 * 3 + i % 9 / 3);
    }
    for (int p = 0; p < PUZZLES; p++) {
        rt_srand(2001 + (u32)p);
        make_puzzle(44 + p % 5 * 2);                    /* 44 .. 52 empty cells */
        for (int i = 0; i < 9; i++) rowm[i] = colm[i] = boxm[i] = 0;
        for (int i = 0; i < 81; i++) {
            clue[i] = grid[i];
            if (grid[i]) {
                u16 bit = (u16)(1u << grid[i]);
                rowm[row_of[i]] |= bit;
                colm[col_of[i]] |= bit;
                boxm[box_of[i]] |= bit;
            }
        }
        nodes = 0;
        int res = solve(0);
        if (res == 0) return 1;                         /* a generated puzzle always has a solution */
        if (res < 0) gave_up++;
        else {
            for (int i = 0; i < 9; i++)
                if (rowm[i] != 0x3fe || colm[i] != 0x3fe || boxm[i] != 0x3fe) return 2;
            for (int i = 0; i < 81; i++) {
                if (clue[i] && clue[i] != grid[i]) return 3;
                chk = chk * 31 + grid[i];
            }
        }
        chk = chk * 31 + nodes;
    }
    return rt_check(chk + (u32)gave_up, 0x32ed39);
}

#elif SEL == 2
#define BW 6
#define BH 5
#define DEPTH 4
#define GAMES 2
static s8 board[BW][BH];                /* 0 empty, +1 and -1 the two players */
static int height[BW];
static u32 nodes;

/* Did the stone just placed at (col, row) complete four in a row? */
static int wins(int col, int row, int who) {
    static const s8 dx[4] = {1, 0, 1, 1}, dy[4] = {0, 1, 1, -1};
    for (int dir = 0; dir < 4; dir++) {
        int n = 1, x, y;
        for (x = col + dx[dir], y = row + dy[dir]; x >= 0 && x < BW && y >= 0 && y < BH && board[x][y] == who;
             x += dx[dir], y += dy[dir]) n++;
        for (x = col - dx[dir], y = row - dy[dir]; x >= 0 && x < BW && y >= 0 && y < BH && board[x][y] == who;
             x -= dx[dir], y -= dy[dir]) n++;
        if (n >= 4) return 1;
    }
    return 0;
}
static int evaluate(int who) {          /* stones in the centre columns count more */
    static const s8 weight[BW] = {1, 2, 3, 3, 2, 1};
    int s = 0;
    for (int x = 0; x < BW; x++)
        for (int y = 0; y < height[x]; y++) s += board[x][y] * weight[x];
    return s * who;
}
static const u8 order[BW] = {2, 3, 1, 4, 0, 5};
static int negamax(int who, int depth, int alpha, int beta) {
    int best = -1000, moved = 0;
    nodes++;
    for (int i = 0; i < BW; i++) {
        int col = order[i], row = height[col], score;
        if (row >= BH) continue;
        moved = 1;
        board[col][row] = (s8)who;
        height[col]++;
        if (wins(col, row, who)) score = 100 + depth;       /* the sooner the better */
        else if (depth == 0) score = evaluate(who);
        else score = -negamax(-who, depth - 1, -beta, -alpha);
        height[col]--;
        board[col][row] = 0;
        if (score > best) best = score;
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;                           /* cut-off */
    }
    return moved ? best : 0;
}
static int minimax(int who, int depth) {                    /* the same value, without pruning */
    int best = -1000, moved = 0;
    for (int i = 0; i < BW; i++) {
        int col = order[i], row = height[col], score;
        if (row >= BH) continue;
        moved = 1;
        board[col][row] = (s8)who;
        height[col]++;
        if (wins(col, row, who)) score = 100 + depth;
        else if (depth == 0) score = evaluate(who);
        else score = -minimax(-who, depth - 1);
        height[col]--;
        board[col][row] = 0;
        if (score > best) best = score;
    }
    return moved ? best : 0;
}
static int best_move(int who, int depth, int *value) {      /* column to play, or -1 if the board is full */
    int best = -1000, at = -1, alpha = -1000;
    for (int i = 0; i < BW; i++) {
        int col = order[i], row = height[col], score;
        if (row >= BH) continue;
        board[col][row] = (s8)who;
        height[col]++;
        if (wins(col, row, who)) score = 100 + depth;
        else score = -negamax(-who, depth - 1, -1000, -alpha);
        height[col]--;
        board[col][row] = 0;
        if (score > best) { best = score; at = col; }
        if (best > alpha) alpha = best;
    }
    *value = best;
    return at;
}

int main(void) {
    u32 chk = 0;
    for (int game = 0; game < GAMES; game++) {
        int who = 1, moves = 0, winner = 0;
        rt_srand(2101 + (u32)game);
        for (int x = 0; x < BW; x++) {
            height[x] = 0;
            for (int y = 0; y < BH; y++) board[x][y] = 0;
        }
        for (;;) {
            int col, value = 0;
            if (moves < 3) col = (int)rt_rand_n(BW);        /* random opening */
            else {
                col = best_move(who, DEPTH, &value);
                if (col < 0) break;                         /* board full: draw */
                if (moves == 3 || moves == 8) {
                    int a = negamax(who, 2, -1000, 1000), b = minimax(who, 2);
                    if (a != b) return 1;
                }
            }
            int row = height[col];
            board[col][row] = (s8)who;
            height[col]++;
            moves++;
            chk = chk * 31 + (u32)col + (u32)(value + 200);
            if (wins(col, row, who)) { winner = who; break; }
            who = -who;
        }
        chk = chk * 31 + (u32)moves + (u32)(winner + 1);
    }
    return rt_check(chk + nodes, 0x3c2ab4);
}

#elif SEL == 3
#define LW 64
#define LH 48
#define GENERATIONS 56
static u8 g0[LW * LH], g1[LW * LH];
static int xl[LW], xr[LW];
static int births, deaths;

/* One generation from src to dst on a w x h torus. Returns the new population. */
static int life_step(const u8 *src, u8 *dst, int w, int h) {
    int pop = 0;
    for (int x = 0; x < w; x++) {
        xl[x] = x == 0 ? w - 1 : x - 1;
        xr[x] = x == w - 1 ? 0 : x + 1;
    }
    for (int y = 0; y < h; y++) {
        const u8 *up = src + (y == 0 ? h - 1 : y - 1) * w, *mid = src + y * w;
        const u8 *down = src + (y == h - 1 ? 0 : y + 1) * w;
        u8 *out = dst + y * w;
        for (int x = 0; x < w; x++) {
            int l = xl[x], r = xr[x];
            int n = up[l] + up[x] + up[r] + mid[l] + mid[r] + down[l] + down[x] + down[r];
            if (mid[x]) {
                if (n < 2 || n > 3) { out[x] = 0; deaths++; }
                else { out[x] = 1; pop++; }
            } else if (n == 3) { out[x] = 1; births++; pop++; }
            else out[x] = 0;
        }
    }
    return pop;
}

int main(void) {
    static const u8 glider[5][2] = {{1, 0}, {2, 1}, {0, 2}, {1, 2}, {2, 2}};
    u32 chk = 0;
    u8 *a = g0, *b = g1;
    /* rule check: a glider on an empty 12 x 12 torus moves by (1, 1) in four generations */
    for (int i = 0; i < 144; i++) a[i] = 0;
    for (int i = 0; i < 5; i++) a[(glider[i][1] + 3) * 12 + glider[i][0] + 3] = 1;
    for (int gen = 0; gen < 4; gen++) {
        life_step(a, b, 12, 12);
        u8 *t = a; a = b; b = t;
    }
    for (int i = 0; i < 144; i++) b[i] = 0;
    for (int i = 0; i < 5; i++) b[(glider[i][1] + 4) * 12 + glider[i][0] + 4] = 1;
    if (memcmp(a, b, 144) != 0) return 1;

    rt_srand(2201);
    births = deaths = 0;
    for (int i = 0; i < LW * LH; i++) a[i] = rt_rand_n(100) < 30;
    for (int gen = 0; gen < GENERATIONS; gen++) {
        int pop = life_step(a, b, LW, LH);
        u8 *t = a; a = b; b = t;
        if (pop == 0) return 2;                             /* this start does not die out */
        chk = chk * 31 + (u32)pop;
    }
    return rt_check(chk + (u32)births * 3 + (u32)deaths, 0x05099b);
}

#elif SEL == 4
#define CW 24                           /* maze cells */
#define CH 16
#define GW (2 * CW + 1)                 /* grid squares: cells, walls between them, border */
#define GH (2 * CH + 1)
#define SQUARES (GW * GH)
#define MAZES 11
#define INF 0x3fffffff
static u8 wall[SQUARES], closed[SQUARES], sq_x[SQUARES], sq_y[SQUARES];
static u16 stack[CW * CH];
static int flood[SQUARES], queue[SQUARES], cost[SQUARES];
static int heap_f[4 * SQUARES], heap_s[4 * SQUARES], heap_n;
static const int step[4] = {1, GW, -1, -GW};        /* east, south, west, north */
#define SQ(cx, cy) ((2 * (cy) + 1) * GW + 2 * (cx) + 1)
#define START SQ(0, 0)
#define GOAL SQ(CW - 1, CH - 1)

static void make_maze(void) {
    static const s8 dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
    int cx = 0, cy = 0, sp = 0;
    for (int i = 0; i < SQUARES; i++) wall[i] = 1;
    wall[SQ(0, 0)] = 0;
    for (;;) {
        int opts[4], n = 0;
        for (int d = 0; d < 4; d++) {               /* neighbours not yet visited */
            int nx = cx + dx[d], ny = cy + dy[d];
            if (nx >= 0 && nx < CW && ny >= 0 && ny < CH && wall[SQ(nx, ny)]) opts[n++] = d;
        }
        if (n) {
            int d = opts[rt_rand_n((u32)n)];
            wall[SQ(cx, cy) + step[d]] = 0;         /* open the wall in between */
            stack[sp++] = (u16)(cy << 8 | cx);
            cx += dx[d];
            cy += dy[d];
            wall[SQ(cx, cy)] = 0;
        } else {
            if (sp == 0) break;
            sp--;
            cx = stack[sp] & 0xff;
            cy = stack[sp] >> 8;
        }
    }
}
/* Keep the right hand on the wall. Returns the number of steps to the goal, or -1. */
static int wall_follower(void) {
    int pos = START, dir = 0, steps = 0;
    while (pos != GOAL) {
        int right = (dir + 1) & 3, left = (dir + 3) & 3;
        if (!wall[pos + step[right]]) dir = right;
        else if (!wall[pos + step[dir]]) { }
        else if (!wall[pos + step[left]]) dir = left;
        else dir = (dir + 2) & 3;
        pos += step[dir];
        if (++steps > 4 * SQUARES) return -1;
    }
    return steps;
}
static int flood_fill(void) {           /* distance of every open square from START; returns GOAL's */
    int head = 0, tail = 0;
    for (int i = 0; i < SQUARES; i++) flood[i] = -1;
    flood[START] = 0;
    queue[tail++] = START;
    while (head < tail) {
        int u = queue[head++];
        for (int d = 0; d < 4; d++) {
            int v = u + step[d];
            if (!wall[v] && flood[v] < 0) {
                flood[v] = flood[u] + 1;
                queue[tail++] = v;
            }
        }
    }
    return flood[GOAL];
}
static void heap_push(int f, int s) {
    int i = heap_n++;
    while (i > 0) {
        int p = (i - 1) >> 1;
        if (heap_f[p] <= f) break;
        heap_f[i] = heap_f[p];
        heap_s[i] = heap_s[p];
        i = p;
    }
    heap_f[i] = f;
    heap_s[i] = s;
}
static int heap_pop(void) {
    int top = heap_s[0], lf, ls, i = 0;
    heap_n--;
    lf = heap_f[heap_n];
    ls = heap_s[heap_n];
    for (;;) {
        int c = 2 * i + 1;
        if (c >= heap_n) break;
        if (c + 1 < heap_n && heap_f[c + 1] < heap_f[c]) c++;
        if (heap_f[c] >= lf) break;
        heap_f[i] = heap_f[c];
        heap_s[i] = heap_s[c];
        i = c;
    }
    heap_f[i] = lf;
    heap_s[i] = ls;
    return top;
}
static int manhattan(int s) {
    int ax = sq_x[GOAL] - sq_x[s], ay = sq_y[GOAL] - sq_y[s];
    if (ax < 0) ax = -ax;
    if (ay < 0) ay = -ay;
    return ax + ay;
}
static int astar(int *expanded) {       /* length of a shortest path START -> GOAL, or -1 */
    for (int i = 0; i < SQUARES; i++) { cost[i] = INF; closed[i] = 0; }
    heap_n = 0;
    cost[START] = 0;
    heap_push(manhattan(START), START);
    while (heap_n > 0) {
        int u = heap_pop();
        if (closed[u]) continue;        /* an older, worse entry */
        closed[u] = 1;
        (*expanded)++;
        if (u == GOAL) return cost[u];
        for (int d = 0; d < 4; d++) {
            int v = u + step[d];
            if (wall[v] || closed[v]) continue;
            if (cost[u] + 1 < cost[v]) {
                cost[v] = cost[u] + 1;
                heap_push(cost[v] + manhattan(v), v);
            }
        }
    }
    return -1;
}

int main(void) {
    u32 chk = 0;
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) { sq_x[y * GW + x] = (u8)x; sq_y[y * GW + x] = (u8)y; }
    for (int m = 0; m < MAZES; m++) {
        int expanded = 0;
        rt_srand(2301 + (u32)m);
        make_maze();
        int walk = wall_follower();
        if (walk < 0) return 1;
        int perfect = flood_fill();
        if (perfect < 0 || perfect > walk) return 2;
        for (int k = 0; k < 30 + 25 * (m % 4); k++) {       /* open extra walls: loops, several routes */
            int x = 1 + (int)rt_rand_n(GW - 2), y = 1 + (int)rt_rand_n(GH - 2);
            if ((x ^ y) & 1) wall[y * GW + x] = 0;
        }
        int shortest = flood_fill();
        int found = astar(&expanded);
        if (found != shortest) return 3;
        if (shortest > perfect) return 4;
        chk = chk * 31 + (u32)walk + (u32)perfect * 3 + (u32)shortest * 5 + (u32)expanded * 7;
    }
    return rt_check(chk, 0x146208);
}

#else
#error "games.c: build with -DSEL=1..4"
#endif
