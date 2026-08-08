#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#  define PATH_SEPARATOR "\\"
#else
#  include <unistd.h>
#  define PATH_SEPARATOR "/"
#endif

typedef struct SyntheticCase {
    long index;
    unsigned long seed;
    int min_sleep;
    int max_sleep;
} SyntheticCase;

static void usage(const char *program)
{
    fprintf(stderr,
        "Usage: %s --index N --seed N [--min-sleep N] [--max-sleep N]\n",
        program);
}

static int parse_long(const char *text, long min, long max, long *value)
{
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno || !end || *end || parsed < min || parsed > max) return -1;
    *value = parsed;
    return 0;
}

static uint32_t case_random(unsigned long seed, long index)
{
    uint32_t value = (uint32_t)seed ^ ((uint32_t)(index + 1) * UINT32_C(0x9e3779b9));
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return value;
}

static void sleep_seconds(int seconds)
{
#ifdef _WIN32
    Sleep((DWORD)seconds * 1000U);
#else
    while (seconds > 0) seconds = sleep((unsigned int)seconds);
#endif
}

static int parse_arguments(int argc, char **argv, SyntheticCase *test_case)
{
    int has_index = 0;
    int has_seed = 0;
    test_case->min_sleep = 1;
    test_case->max_sleep = 60;

    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc) return -1;
        long parsed = 0;
        if (strcmp(argv[i], "--index") == 0) {
            if (parse_long(argv[++i], 0, 100000000L, &parsed) != 0) return -1;
            test_case->index = parsed;
            has_index = 1;
        } else if (strcmp(argv[i], "--seed") == 0) {
            if (parse_long(argv[++i], 0, 2147483647L, &parsed) != 0) return -1;
            test_case->seed = (unsigned long)parsed;
            has_seed = 1;
        } else if (strcmp(argv[i], "--min-sleep") == 0) {
            if (parse_long(argv[++i], 0, 3600, &parsed) != 0) return -1;
            test_case->min_sleep = (int)parsed;
        } else if (strcmp(argv[i], "--max-sleep") == 0) {
            if (parse_long(argv[++i], 0, 3600, &parsed) != 0) return -1;
            test_case->max_sleep = (int)parsed;
        } else {
            return -1;
        }
    }
    return has_index && has_seed && test_case->min_sleep <= test_case->max_sleep ? 0 : -1;
}

int main(int argc, char **argv)
{
    SyntheticCase test_case = {0};
    if (parse_arguments(argc, argv, &test_case) != 0) {
        usage(argv[0]);
        return 2;
    }

    const char *job_id = getenv("ORCH_JOB_ID");
    const char *worker_id = getenv("ORCH_WORKER_ID");
    const char *output_dir = getenv("ORCH_OUTPUT_DIR");
    if (!job_id || !job_id[0] || !worker_id || !worker_id[0] ||
        !output_dir || !output_dir[0]) {
        fprintf(stderr, "Missing ORCH_JOB_ID, ORCH_WORKER_ID, or ORCH_OUTPUT_DIR\n");
        return 3;
    }

    uint32_t random_value = case_random(test_case.seed, test_case.index);
    int sleep_duration = test_case.min_sleep +
        (int)(random_value % (uint32_t)(test_case.max_sleep - test_case.min_sleep + 1));
    long long operand_a = (long long)test_case.index + 3;
    long long operand_b = (long long)(random_value % 1000U) + 1;
    long long multiplier = (long long)(test_case.index % 11) + 1;
    long long result = (operand_a + operand_b) * multiplier;
    time_t started_at = time(NULL);

    printf("START job=%s worker=%s index=%ld sleep_seconds=%d\n",
        job_id, worker_id, test_case.index, sleep_duration);
    fflush(stdout);
    fprintf(stderr, "TRACE seed=%lu a=%lld b=%lld multiplier=%lld\n",
        test_case.seed, operand_a, operand_b, multiplier);
    fflush(stderr);

    sleep_seconds(sleep_duration);
    time_t ended_at = time(NULL);

    char result_path[1024];
    int path_length = snprintf(result_path, sizeof(result_path), "%s%sresult.json",
        output_dir, PATH_SEPARATOR);
    if (path_length < 0 || path_length >= (int)sizeof(result_path)) {
        fprintf(stderr, "Output path is too long\n");
        return 4;
    }

    FILE *result_file = fopen(result_path, "wb");
    if (!result_file) {
        fprintf(stderr, "Cannot write %s: %s\n", result_path, strerror(errno));
        return 5;
    }
    fprintf(result_file,
        "{\n"
        "  \"job_id\": \"%s\",\n"
        "  \"worker_id\": \"%s\",\n"
        "  \"index\": %ld,\n"
        "  \"seed\": %lu,\n"
        "  \"sleep_seconds\": %d,\n"
        "  \"operand_a\": %lld,\n"
        "  \"operand_b\": %lld,\n"
        "  \"multiplier\": %lld,\n"
        "  \"result\": %lld,\n"
        "  \"started_at\": %lld,\n"
        "  \"ended_at\": %lld\n"
        "}\n",
        job_id, worker_id, test_case.index, test_case.seed, sleep_duration,
        operand_a, operand_b, multiplier, result,
        (long long)started_at, (long long)ended_at);
    if (fclose(result_file) != 0) {
        fprintf(stderr, "Cannot close %s\n", result_path);
        return 6;
    }

    printf("RESULT job=%s index=%ld value=%lld elapsed_seconds=%lld\n",
        job_id, test_case.index, result, (long long)(ended_at - started_at));
    return 0;
}
