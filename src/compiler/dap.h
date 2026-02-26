#ifndef DAP_H
#define DAP_H

/*
 * DAP (Debug Adapter Protocol) handler for Clay.
 *
 * Speaks DAP over stdin/stdout to VS Code or any DAP client.
 * Uses the existing bytecode VM stepping infrastructure.
 */

#include "clay.h"

#define DAP_MAX_BREAKPOINTS 256
#define DAP_MAX_SOURCES 16
#define DAP_MAX_VAR_REFS 256

typedef struct {
    int lines[DAP_MAX_BREAKPOINTS];
    int count;
} DapBreakpointSet;

typedef struct {
    char *file;            /* source file path */
    DapBreakpointSet bp;   /* breakpoints for this file */
} DapSource;

/* Variable reference table — maps variablesReference IDs to ClayValues.
 * IDs 1 and 2 are reserved for Globals and Locals scopes.
 * IDs 3+ are assigned to expandable objects/arrays. */
typedef struct {
    ClayValue values[DAP_MAX_VAR_REFS];
    int count;             /* next ID to assign (starts at 3) */
} DapVarRefs;

typedef struct {
    DapSource sources[DAP_MAX_SOURCES];
    int source_count;
    int seq;               /* outgoing sequence number */
    int configured;        /* configurationDone received */
    int launched;          /* launch completed */
    int stopped;           /* VM is paused */
    int terminate;         /* disconnect received */
    int step_mode;         /* 0=run, 1=next, 2=stepIn, 3=stepOut */
    int step_line;         /* line we stepped from (for "next") */
    int step_depth;        /* trace depth when step started */
    int last_stopped_line; /* line where we last stopped (avoid re-triggering) */
    const char *source;    /* source text */
    const char *source_file;
    void *bc;              /* BcCompiler* */
    void *vm;              /* clayvm_vm_t* */
    DapVarRefs var_refs;   /* expandable variable tracking */
} DapState;

/* Run the DAP adapter for the given source file. Does not return until disconnect. */
int dap_run(const char *filename);

#endif /* DAP_H */
