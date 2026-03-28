#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solver.h"

/* Simple presim runner tool. Usage:
 * presim_runner --parent <PARENT_ID> --case <domain> --out <out_path> --input-dir <parent_input_dir>
 * This program dispatches to the solver implementation for the requested domain.
 */

int main(int argc, char **argv)
{
    const char *parent = NULL, *case_name = NULL, *out = NULL, *input_dir = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--parent") == 0 && i+1 < argc) parent = argv[++i];
        else if (strcmp(argv[i], "--case") == 0 && i+1 < argc) case_name = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i+1 < argc) out = argv[++i];
        else if (strcmp(argv[i], "--input-dir") == 0 && i+1 < argc) input_dir = argv[++i];
    }
    if (!parent || !case_name || !out) {
        fprintf(stderr, "Usage: %s --parent <id> --case <domain> --out <out_path> [--input-dir <dir>]\n", argv[0]);
        return 2;
    }

    /* For now, only thermal is supported. Solver implementations are expected
     * to be linked into the binary and selected by name.
     */
    int rc = -1;
    if (strcmp(case_name, "thermal") == 0) {
        rc = solver_presim_run(case_name, parent, input_dir ? input_dir : "", out);
    } else {
        fprintf(stderr, "Unknown presim case: %s\n", case_name);
        return 3;
    }

    if (rc == 0) {
        printf("presim_runner: wrote %s\n", out);
        return 0;
    }
    fprintf(stderr, "presim_runner: failed to generate presim output\n");
    return 1;
}
