#ifndef DECISION_CORE_H
#define DECISION_CORE_H

#include <stdint.h>

typedef enum {
    DC_ACTION_NONE = 0,
    DC_ACTION_RUN_PRE_SIM,
    DC_ACTION_RUN_COARSE_SIM,
    DC_ACTION_RUN_FINE_SIM,
    DC_ACTION_REFINE,
    DC_ACTION_MIGRATE,
    DC_ACTION_DEFER
} dc_action_t;

typedef struct {
    const char *job_id;
    double local_error_estimate;
    uint32_t available_cpus;
    uint32_t available_mem_mb;
    /* Extend with other metrics as needed */
} dc_context_t;

typedef struct {
    dc_action_t action;
    uint32_t target_cores;
    const char *notes; /* optional human-readable note (may be NULL) */
    char allocation_json[2048]; /* JSON describing zones, priorities, cores */
} dc_result_t;

/* Initialize the decision core. `config_path` may be NULL. */
int decision_core_init(const char *config_path);

/* Make a decision given the context. Returns 0 on success. */
int decision_core_decide(const dc_context_t *ctx, dc_result_t *out);

/* Shutdown / cleanup */
void decision_core_shutdown(void);

#endif /* DECISION_CORE_H */
#ifndef DECISION_CORE_H
#define DECISION_CORE_H

#include <stdint.h>

typedef enum {
    DC_ACTION_NONE = 0,
    DC_ACTION_RUN_PRE_SIM,
    DC_ACTION_RUN_COARSE_SIM,
    DC_ACTION_RUN_FINE_SIM,
    DC_ACTION_REFINE,
    DC_ACTION_MIGRATE,
    DC_ACTION_DEFER
} dc_action_t;

typedef struct {
    const char *job_id;
    double local_error_estimate;
    uint32_t available_cpus;
    uint32_t available_mem_mb;
    /* add other metrics as needed */
} dc_context_t;

typedef struct {
    dc_action_t action;
    uint32_t target_cores;
    const char *notes; /* optional human message */
} dc_result_t;

int decision_core_init(const char *config_path);
int decision_core_decide(const dc_context_t *ctx, dc_result_t *out);
void decision_core_shutdown(void);

#endif /* DECISION_CORE_H */