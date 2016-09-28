#include <math.h>
#include <time.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <inttypes.h>

#define CONNECT4_WIDTH  7
#define CONNECT4_HEIGHT 6
_Static_assert(CONNECT4_WIDTH * CONNECT4_HEIGHT <= 64, "invalid board size");

#define CONNECT4_SCORE_WIN  1.0f
#define CONNECT4_SCORE_DRAW 0.1f

static uint64_t
rotl(const uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

static uint64_t
xoroshiro128plus(uint64_t *s)
{
    const uint64_t s0 = s[0];
    uint64_t s1 = s[1];
    const uint64_t result = s0 + s1;
    s1 ^= s0;
    s[0] = rotl(s0, 55) ^ s1 ^ (s1 << 14); // a, b
    s[1] = rotl(s1, 36); // c
    return result;
}

static uint64_t
splitmix64(uint64_t *x)
{
    uint64_t z = (*x += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static uint64_t connect4_wins[CONNECT4_WIDTH * CONNECT4_HEIGHT][16];

static void
connect4_startup(void)
{
    static int delta[] = {
        -1, -1, -1,  0, -1,  1, 0,  1, 0, -1, 1,  1, 1,  0, 1, -1,
    };
    for (int y = 0; y < CONNECT4_HEIGHT; y++) {
        for (int x = 0; x < CONNECT4_WIDTH; x++) {
            int i = 0;
            for (int d = 0; d < 8; d++) {
                for (int s = -3; s <= 0; s++) {
                    uint64_t mask = 0;
                    int valid = 1;
                    for (int p = s; p < s + 4; p++) {
                        int xx = x + delta[d * 2 + 0] * p;
                        int yy = y + delta[d * 2 + 1] * p;
                        int shift = yy * CONNECT4_WIDTH + xx;
                        if (xx < 0 || xx >= CONNECT4_WIDTH ||
                            yy < 0 || yy >= CONNECT4_HEIGHT)
                            valid = 0;
                        else
                            mask |= UINT64_C(1) << shift;
                    }
                    if (valid)
                        connect4_wins[y * CONNECT4_WIDTH + x][i++] = mask;
                }
            }
        }
    }
}

enum connect4_result {
    CONNECT4_RESULT_UNRESOLVED,
    CONNECT4_RESULT_DRAW,
    CONNECT4_RESULT_WIN,
};

static enum connect4_result
connect4_check(uint64_t who, uint64_t opponent, int position, uint64_t *how)
{
    for (int i = 0; i < 16; i++) {
        uint64_t mask = connect4_wins[position][i];
        if (mask && (mask & who) == mask) {
            *how = mask;
            return CONNECT4_RESULT_WIN;
        }
    }
    *how = 0;
    if ((who | opponent) == UINT64_C(0x3ffffffffff))
        return CONNECT4_RESULT_DRAW;
    return CONNECT4_RESULT_UNRESOLVED;
}

static int
connect4_valid(uint64_t taken, int play)
{
    uint64_t top = UINT64_C(1) << play;
    return play >= 0 && play < CONNECT4_WIDTH && !(top & taken) ? 1 : 0;
}

static int
connect4_drop(uint64_t taken, int play)
{
    int position = play;
    for (int i = 1; i < CONNECT4_HEIGHT; i++) {
        position += CONNECT4_WIDTH;
        uint64_t mask = UINT64_C(1) << position;
        if (mask & taken)
            return position - CONNECT4_WIDTH;
    }
    return position;
}

#define CONNECT4_NULL ((uint32_t)-1)
#define CONNECT4_WIN0 ((uint32_t)-2)
#define CONNECT4_WIN1 ((uint32_t)-3)
#define CONNECT4_DRAW ((uint32_t)-4)
struct connect4 {
    uint64_t state[2];
    uint64_t rng[2];
    uint32_t nodes_available;
    uint32_t nodes_allocated;
    uint32_t root;
    uint32_t free;
    int turn;
    struct connect4_node {
        uint32_t next[CONNECT4_WIDTH];
        uint32_t playouts[CONNECT4_WIDTH];
        float    score[CONNECT4_WIDTH];
        uint32_t playouts_total;
    } nodes[];
};

static uint32_t
connect4_alloc(struct connect4 *c)
{
    uint32_t node = c->free;
    if (node != CONNECT4_NULL) {
        struct connect4_node *n = c->nodes + node;
        c->nodes_allocated++;
        c->free = n->next[0];
        n->playouts_total = 0;
        for (int i = 0; i < CONNECT4_WIDTH; i++) {
            n->next[i] = CONNECT4_NULL;
            n->playouts[i] = 0;
            n->score[i] = 0;
        }
    }
    return node;
}

static void
connect4_free(struct connect4 *c, uint32_t node)
{
    if (node < CONNECT4_DRAW) {
        struct connect4_node *n = c->nodes + node;
        c->nodes_allocated--;
        for (int i = 0; i < CONNECT4_WIDTH; i++)
            connect4_free(c, n->next[i]);
        n->next[0] = c->free;
        c->free = node;
    }
}

static void
connect4_init(void *buf, size_t bufsize)
{
    struct connect4 *c = buf;
    c->nodes_available = (bufsize - sizeof(*c)) / sizeof(c->nodes[0]);
    c->nodes_allocated = 0;
    c->state[0] = 0;
    c->state[1] = 0;
    c->turn = 0;
    c->free = 0;
    uint64_t seed = time(0);
    c->rng[0] = splitmix64(&seed);
    c->rng[1] = splitmix64(&seed);
    for (uint32_t i = 0; i < c->nodes_available - 1; i++)
        c->nodes[i].next[0] = i + 1;
    c->nodes[c->nodes_available - 1].next[0] = CONNECT4_NULL;
    c->root = connect4_alloc(c);
}

static void
connect4_advance(struct connect4 *c, int play)
{
    assert(connect4_valid(c->state[0] | c->state[1], play));
    int position = connect4_drop(c->state[0] | c->state[1], play);
    c->state[c->turn] |= UINT64_C(1) << position;
    c->turn = !c->turn;
    struct connect4_node *n = c->nodes + c->root;
    uint32_t old_root = c->root;
    c->root = n->next[play];
    n->next[play] = CONNECT4_NULL;
    connect4_free(c, old_root);
    if (c->root == CONNECT4_NULL)
        c->root = connect4_alloc(c);
}

static int
connect4_playout(struct connect4 *c,
                 uint32_t node,
                 const uint64_t state[2],
                 int turn)
{
    if (node == CONNECT4_WIN0)
        return 0;
    else if (node == CONNECT4_WIN1)
        return 1;
    else if (node == CONNECT4_DRAW)
        return 2;
    assert(node != CONNECT4_NULL);

    struct connect4_node *n = c->nodes + node;
    int options[CONNECT4_WIDTH];
    int noptions = 0;
    uint64_t taken = state[0] | state[1];
    for (int i = 0; i < CONNECT4_WIDTH; i++)
        if (n->next[i] == CONNECT4_NULL && connect4_valid(taken, i))
            options[noptions++] = i;
    int play;
    if (noptions == 0) {
        /* UCB1 */
        uint32_t total = 0;
        for (int i = 0; i < CONNECT4_WIDTH; i++)
            if (connect4_valid(taken, i))
                total += n->playouts[i];
        float best_value = -INFINITY;
        float numerator = 2.0f * logf(total);
        int best[CONNECT4_WIDTH];
        int nbest = 0;
        for (int i = 0; i < CONNECT4_WIDTH; i++) {
            if (connect4_valid(taken, i)) {
                assert(n->playouts[i]);
                total += n->playouts[i];
                float mean = n->score[i] / n->playouts[i];
                float value = mean + sqrtf(numerator / n->playouts[i]);
                if (value > best_value) {
                    best_value = value;
                    nbest = 1;
                    best[0] = i;
                } else if (value == best_value) {
                    best[nbest++] = i;
                }
            }
        }
        play = nbest == 1 ? best[0] : best[xoroshiro128plus(c->rng) % nbest];
        int position = connect4_drop(state[0] | state[1], play);
        uint64_t place = UINT64_C(1) << position;
        uint64_t copy[2] = {state[0], state[1]};
        copy[turn] |= place;
        int winner = connect4_playout(c, n->next[play], copy, !turn);
        if (winner >= 0)
            n->playouts[play]++;
        if (winner == turn)
            n->score[play] += CONNECT4_SCORE_WIN;
        else if (winner == 2)
            n->score[play] += CONNECT4_SCORE_WIN;
        return winner;
    } else {
        /* Random unplayed. */
        if (noptions == 1)
            play = options[0];
        else
            play = options[xoroshiro128plus(c->rng) % noptions];
        int position = connect4_drop(state[0] | state[1], play);
        uint64_t place = UINT64_C(1) << position;
        uint64_t copy[2] = {state[0], state[1]};
        copy[turn] |= place;
        uint64_t dummy;
        switch (connect4_check(copy[turn], copy[!turn], position, &dummy)) {
            case CONNECT4_RESULT_DRAW:
                n->playouts[play]++;
                n->score[play] += CONNECT4_SCORE_DRAW;
                n->next[play] = CONNECT4_DRAW;
                return 2;
            case CONNECT4_RESULT_WIN:
                n->playouts[play]++;
                n->score[play] += CONNECT4_SCORE_WIN;
                n->next[play] = turn ? CONNECT4_WIN1 : CONNECT4_WIN0;
                return turn;
            case CONNECT4_RESULT_UNRESOLVED:
                n->next[play] = connect4_alloc(c);
                if (n->next[play] == CONNECT4_NULL)
                    return -1;
                n->playouts[play]++;
                break;
        }
        int original_play = play;
        int original_turn = turn;
        for (;;) {
            turn = !turn;
            int options[CONNECT4_WIDTH];
            int noptions = 0;
            uint64_t taken = copy[0] | copy[1];
            for (int i = 0; i < CONNECT4_WIDTH; i++)
                if (connect4_valid(taken, i))
                    options[noptions++] = i;
            int play;
            if (noptions == 1)
                play = options[0];
            else
                play = options[xoroshiro128plus(c->rng) % noptions];
            int position = connect4_drop(copy[0] | copy[1], play);
            uint64_t place = UINT64_C(1) << position;
            copy[turn] |= place;
            uint64_t x;
            switch (connect4_check(copy[turn], copy[!turn], position, &x)) {
                case CONNECT4_RESULT_UNRESOLVED:
                    break;
                case CONNECT4_RESULT_DRAW:
                    n->score[original_play] += CONNECT4_SCORE_DRAW;
                    return 2;
                case CONNECT4_RESULT_WIN:
                    if (turn == original_turn)
                        n->score[original_play] += CONNECT4_SCORE_WIN;
                    return turn;
            }
            turn = !turn;
        }
    }
}

static int
connect4_playout_many(struct connect4 *c, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        if (connect4_playout(c, c->root, c->state, c->turn) == -1)
            break;
    struct connect4_node *n = c->nodes + c->root;
    double best_ratio = -INFINITY;
    int best = -1;
    for (int i = 0; i < CONNECT4_WIDTH; i++)
        if (n->playouts[i]) {
            double ratio = n->score[i] / (double)n->playouts[i];
            if (ratio > best_ratio) {
                best_ratio = ratio;
                best = i;
            }
        }
    return best;
}

static void
connect4_display(uint64_t p0, uint64_t p1)
{
    putchar('\n');
    for (int w = 0; w < CONNECT4_WIDTH; w++)
        printf("%d ", w + 1);
    putchar('\n');
    for (int h = 0; h < CONNECT4_HEIGHT; h++) {
        for (int w = 0; w < CONNECT4_WIDTH; w++) {
            int s = h * CONNECT4_WIDTH + w;
            if ((p0 >> s) & 1)
                fputs("O ", stdout);
            else if ((p1 >> s) & 1)
                fputs("X ", stdout);
            else
                fputs(". ", stdout);
        }
        putchar('\n');
    }
}

int
main(void)
{
    /* Options */
    uint32_t max_playouts = 100000;
    size_t size = 16UL * 1024 * 1024;
    enum player_type {
        PLAYER_HUMAN,
        PLAYER_AI,
    } player_type[2] = {
        PLAYER_HUMAN, PLAYER_AI
    };

    /* Initialization */
    connect4_startup();
    void *buf = malloc(size);
    while (!buf) {
        size *= 0.8;
        buf = malloc(size);
    }
    connect4_init(buf, size);
    struct connect4 *connect4 = buf;
    printf("AI using %zuMB (%" PRIu32 " nodes)\n",
           size / 1024 / 1024, connect4->nodes_available);

    /* Game Loop */
    for (;;) {
        connect4_display(connect4->state[0], connect4->state[1]);
        uint64_t taken = connect4->state[0] | connect4->state[1];
        int play;
        switch (player_type[connect4->turn]) {
            case PLAYER_HUMAN:
                for (;;) {
                    fputs("\n> ", stdout);
                    fflush(stdout);
                    if (scanf(" %d", &play) != 1)
                        exit(-1);
                    play--;
                    if (connect4_valid(taken, play))
                        break;
                    printf("invalid move\n");
                }
                break;
            case PLAYER_AI:
                play = connect4_playout_many(buf, max_playouts);
                break;
        }
        int position = connect4_drop(taken, play);
        int turn = connect4->turn;
        connect4_advance(buf, play);
        uint64_t who = connect4->state[turn];
        uint64_t opponent = connect4->state[!turn];
        uint64_t how;
        switch (connect4_check(who, opponent, position, &how)) {
            case CONNECT4_RESULT_UNRESOLVED:
                break;
            case CONNECT4_RESULT_DRAW:
                connect4_display(connect4->state[0], connect4->state[1]);
                printf("draw\n");
                exit(0);
            case CONNECT4_RESULT_WIN:
                connect4_display(connect4->state[0], connect4->state[1]);
                printf("player %c wins\n", "OX"[turn]);
                exit(0);
        }
    }

    /* Cleanup */
    free(buf);
    return 0;
}
