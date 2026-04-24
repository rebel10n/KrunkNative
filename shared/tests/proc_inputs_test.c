#include <shared.h>
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EPSILON 1e-4
#define MAX_DIFFS 16

typedef struct {
    int seq;
    double delta;
    int y_dir;
    int x_dir;
    int move_dir;
    int shoot;
    int scope;
    int jump;
    int reload;
    int crouch;
    int weapon_scroll;
    int weapon_swap;
    int move_lock;

    double x, y, z;
    double x_vel, y_vel, z_vel;
    int on_ground;
    int on_wall;
    double jump_cooldown;
    int was_stop_crouch;
    double accel;
} TestStep;

typedef struct {
    const char *field;
    char actual[64];
    char expected[64];
    char diff[64];
    int bad;
} Diff;

static double get_num(const cJSON *arr, int idx) {
    return cJSON_GetNumberValue(cJSON_GetArrayItem(arr, idx));
}

static int parse_step(const cJSON *record, TestStep *out) {
    if (!cJSON_IsArray(record) || cJSON_GetArraySize(record) < 12) return -1;
    const cJSON *arr = cJSON_GetArrayItem(record, 0);
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < 13) return -1;

    out->seq = (int) get_num(arr, 0);
    out->delta = get_num(arr, 1);
    out->y_dir = (int) get_num(arr, 2);
    out->x_dir = (int) get_num(arr, 3);
    out->move_dir = (int) get_num(arr, 4);
    out->shoot = (int) get_num(arr, 5);
    out->scope = (int) get_num(arr, 6);
    out->jump = (int) get_num(arr, 7);
    out->reload = (int) get_num(arr, 8);
    out->crouch = (int) get_num(arr, 9);
    out->weapon_scroll = (int) get_num(arr, 10);
    out->weapon_swap = (int) get_num(arr, 11);
    out->move_lock = (int) get_num(arr, 12);

    out->x = get_num(record, 1);
    out->y = get_num(record, 2);
    out->z = get_num(record, 3);
    out->x_vel = get_num(record, 4);
    out->y_vel = get_num(record, 5);
    out->z_vel = get_num(record, 6);
    out->on_ground = cJSON_IsTrue(cJSON_GetArrayItem(record, 7));
    out->on_wall = (int) get_num(record, 8);
    out->jump_cooldown = get_num(record, 9);
    out->was_stop_crouch = cJSON_IsTrue(cJSON_GetArrayItem(record, 10));
    out->accel = get_num(record, 11);

    return 0;
}

static int load_steps(const char *path, TestStep **steps_out, size_t *count_out, cJSON **root_out) {
    unsigned char *buf = NULL;
    size_t len = 0;
    if (read_file(path, &buf, &len) < 0) {
        fprintf(stderr, "failed to read %s\n", path);
        return -1;
    }

    cJSON *root = cJSON_Parse((const char *) buf);
    free(buf);

    if (!cJSON_IsArray(root)) {
        fprintf(stderr, "%s: not a JSON array\n", path);
        cJSON_Delete(root);
        return -1;
    }

    const int n = cJSON_GetArraySize(root);
    TestStep *steps = calloc((size_t) n, sizeof(TestStep));
    if (!steps) {
        cJSON_Delete(root);
        return -1;
    }

    for (int i = 0; i < n; i++) {
        const cJSON *rec = cJSON_GetArrayItem(root, i);
        if (parse_step(rec, &steps[i]) < 0) {
            fprintf(stderr, "%s: failed to parse step %d\n", path, i);
            free(steps);
            cJSON_Delete(root);
            return -1;
        }
    }

    *steps_out = steps;
    *count_out = (size_t) n;
    *root_out = root;
    return 0;
}

static void cmp_float(Diff *d, const char *name, double actual, double expected) {
    const double diff = fabs(actual - expected);
    const int pass = diff <= EPSILON;

    d->field = name;
    snprintf(d->actual, sizeof(d->actual), "%.10g", actual);
    snprintf(d->expected, sizeof(d->expected), "%.10g", expected);

    if (pass) d->diff[0] = 0;
    else snprintf(d->diff, sizeof(d->diff), "%.4g", diff);

    d->bad = !pass;
}

static void cmp_int(Diff *d, const char *name, int actual, int expected) {
    d->field = name;
    snprintf(d->actual, sizeof(d->actual), "%d", actual);
    snprintf(d->expected, sizeof(d->expected), "%d", expected);
    d->diff[0] = 0;
    d->bad = actual != expected;
}

static void print_row(const Diff *d) {
    const char *red = d->bad ? "\x1b[31m" : "";
    const char *grn = d->bad ? "\x1b[32m" : "";
    const char *ylw = d->bad ? "\x1b[33m" : "";
    const char *rst = d->bad ? "\x1b[0m" : "";

    printf("%-20s | %s%-30s%s | %s%-30s%s | %s%-10s%s\n",
           d->field,
           red, d->actual, rst,
           grn, d->expected, rst,
           ylw, d->diff, rst);
}

static int run_file(const char *path, const char *map_json) {
    TestStep *steps = NULL;
    size_t count = 0;
    cJSON *root = NULL;

    if (load_steps(path, &steps, &count, &root) < 0) return -1;
    if (count == 0) {
        free(steps);
        cJSON_Delete(root);
        return 0;
    }

    Game game;
    memset(&game, 0, sizeof(game));
    game.map = map_init(map_json);
    if (!game.map) {
        fprintf(stderr, "failed to init map\n");
        free(steps);
        cJSON_Delete(root);
        return -1;
    }

    Player *player = player_init(&game);
    if (!player) {
        map_fini(game.map);
        free(game.map);
        free(steps);
        cJSON_Delete(root);
        return -1;
    }

    player->position.x = (float) steps[0].x;
    player->position.y = (float) steps[0].y;
    player->position.z = (float) steps[0].z;
    player->on_ground = steps[0].on_ground;

    int first_fail = -1;

    for (size_t i = 0; i < count; i++) {
        const TestStep *s = &steps[i];

        Input input = {0};
        input.seq = s->seq;
        input.delta = (float) s->delta / 1000.0 ;
        input.x_dir = (float) (s->x_dir / 1000.0);
        input.y_dir = (float) (s->y_dir / 1000.0);
        input.move_dir = s->move_dir;
        input.shoot = s->shoot != 0;
        input.jump = s->jump != 0;
        input.crouch = s->crouch != 0;
        input.reload = s->reload != 0;

        player_proc_input(player, &input, 0, 0);

        Diff diffs[MAX_DIFFS];
        int nd = 0;
        cmp_int(&diffs[nd++], "move_dir", input.move_dir, s->move_dir);
        cmp_float(&diffs[nd++], "x", player->position.x, s->x);
        cmp_float(&diffs[nd++], "y", player->position.y, s->y);
        cmp_float(&diffs[nd++], "z", player->position.z, s->z);
        cmp_float(&diffs[nd++], "x_vel", player->velocity.x, s->x_vel);
        cmp_float(&diffs[nd++], "y_vel", player->velocity.y, s->y_vel);
        cmp_float(&diffs[nd++], "z_vel", player->velocity.z, s->z_vel);
        cmp_float(&diffs[nd++], "accel", player->last_tick_accel, s->accel);

        cmp_int(&diffs[nd++], "on_ground", player->on_ground, s->on_ground);

        int any_bad = 0;
        for (int j = 0; j < nd; j++) any_bad |= diffs[j].bad;

        if (any_bad) {
            printf("\nStep %zu failed:\nTick: %d\n\n", i, s->seq);
            printf("%-20s | %-30s | %-30s | %-10s\n", "Field", "Actual", "Expected", "Diff");
            printf("---------------------+--------------------------------+--------------------------------+-----------\n");
            for (int j = 0; j < nd; j++) print_row(&diffs[j]);
            first_fail = (int) i;
            break;
        }
    }

    map_fini(game.map);
    free(game.map);
    free(player);
    free(steps);
    cJSON_Delete(root);

    return first_fail < 0 ? 0 : -1;
}

int main(int argc, char **argv) {
    static const char *test_files[] = {
        "shared/tests/inputs/complex.json",
        "shared/tests/inputs/jump.json",
        "shared/tests/inputs/jump_forwards.json",
        "shared/tests/inputs/ramp_walk_slide.json",
        "shared/tests/inputs/simple_slidehop.json",
        "shared/tests/inputs/walk_crouch_scope.json",
        "shared/tests/inputs/walk_turn_collide.json",
    };
    const size_t n_tests = sizeof(test_files) / sizeof(test_files[0]);

    unsigned char *map_buf = NULL;
    size_t map_len = 0;
    if (read_file("assets/maps/sandstorm.json", &map_buf, &map_len) < 0) {
        fprintf(stderr, "failed to read assets/maps/sandstorm.json (run from project root)\n");
        return 1;
    }

    int failures = 0;
    int ran = 0;

    for (size_t i = 0; i < n_tests; i++) {
        const char *p = test_files[i];

        if (argc > 1) {
            int match = 0;
            for (int j = 1; j < argc; j++) {
                if (strstr(p, argv[j])) {
                    match = 1;
                    break;
                }
            }
            if (!match) continue;
        }

        printf("=== %s ===\n", p);
        fflush(stdout);

        const int r = run_file(p, (const char *) map_buf);
        ran++;

        if (r == 0) printf("PASS\n\n");
        else {
            printf("FAIL\n\n");
            failures++;
        }
    }

    free(map_buf);

    if (ran == 0) {
        fprintf(stderr, "no matching test files\n");
        return 1;
    }

    printf("summary: %d passed, %d failed, %d total\n", ran - failures, failures, ran);
    return failures ? 1 : 0;
}
