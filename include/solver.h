/**
 * Simple solver plugin API for presimulation tasks.
 * One solver == one domain. Implementations should provide a presim entrypoint.
 */
#ifndef SOLVER_H
#define SOLVER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Run a presimulation for `case_name` using inputs found under `parent_input_dir`.
 * Output file must be written to `out_path` as JSON. Return 0 on success.
 */
int solver_presim_run(const char *case_name,
                      const char *parent_job_id,
                      const char *parent_input_dir,
                      const char *out_path);

/* Simulate running a solver given per-zone errors/sizes and fidelities.
 * Produces an estimated runtime in milliseconds and a final average error.
 */
int solver_simulate(const double *zone_errors, const double *zone_sizes, int zones,
                    const int *fidelities, unsigned int *est_runtime_ms, double *final_error_avg);

#ifdef __cplusplus
}
#endif

#endif /* SOLVER_H */
