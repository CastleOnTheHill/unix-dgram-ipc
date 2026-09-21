/*
 * ipc_config.h -- static module table.
 *
 * Format (whitespace separated, '#' comments, blank lines ignored):
 *
 *   <namespace> <module> <uid> <socket-path>
 *
 * The file is written by an administrator and is read-only for services.
 * It is loaded once at registration time and kept in memory; it is never
 * re-read per send.  Duplicate (namespace, module) pairs and duplicate socket
 * paths are configuration errors, not last-one-wins.
 */
#ifndef IPC_CONFIG_H
#define IPC_CONFIG_H

#include "ipc/ipc.h"

struct ipc_config {
    ipc_config_entry_t *entries;
    int                 count;
    int                 cap;
};

/* Parse "<ns> <module> <uid> <path>" from an already-split single line.
 * Exposed so the unit tests can drive the validation rules directly.  The
 * public entry points (ipc_config_load/parse/free/lookup) are declared in
 * ipc/ipc.h. */
int ipc_config_parse_line(char *line, ipc_config_entry_t *out, int lineno);

#endif /* IPC_CONFIG_H */
