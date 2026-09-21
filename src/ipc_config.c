#include "ipc_config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "ipc_util.h"

/* ------------------------------------------------------------------ */
/* line parser                                                         */
/* ------------------------------------------------------------------ */

static int parse_uid(const char *s, uid_t *out)
{
    char          *end = NULL;
    unsigned long  v;

    if (s == NULL || *s == '\0') {
        return IPC_ERR_CONFIG;
    }
    if (!isdigit((unsigned char)s[0])) {
        return IPC_ERR_CONFIG; /* reject leading '+', '-', space */
    }
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return IPC_ERR_CONFIG;
    }
#ifdef UID_MAX
    if (v > (unsigned long)UID_MAX) {
        return IPC_ERR_CONFIG;
    }
#else
    if (v > 4294967294ul) {
        return IPC_ERR_CONFIG;
    }
#endif
    *out = (uid_t)v;
    return IPC_OK;
}

int ipc_config_parse_line(char *line, ipc_config_entry_t *out, int lineno)
{
    char *tok[5] = { NULL, NULL, NULL, NULL, NULL };
    int   ntok = 0;
    char *p;
    size_t n;

    if (line == NULL || out == NULL) {
        return IPC_ERR_INVAL;
    }
    memset(out, 0, sizeof(*out));

    /* tokenise on runs of whitespace */
    p = line;
    while (*p != '\0') {
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (ntok == 5) {
            IPC_LOGE("config line %d: too many fields", lineno);
            return IPC_ERR_CONFIG;
        }
        tok[ntok++] = p;
        while (*p != '\0' && !isspace((unsigned char)*p)) {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }

    if (ntok == 0) {
        return IPC_ERR_CONFIG; /* caller should have skipped blanks */
    }
    if (ntok != 4) {
        IPC_LOGE("config line %d: expected 4 fields, got %d", lineno, ntok);
        return IPC_ERR_CONFIG;
    }

    /* namespace */
    n = strlen(tok[0]);
    if (n == 0 || n >= IPC_NS_MAX) {
        IPC_LOGE("config line %d: namespace too long (max %d)", lineno,
                 IPC_NS_MAX - 1);
        return IPC_ERR_CONFIG;
    }
    ipc_strlcpy(out->ns, tok[0], sizeof(out->ns));

    /* module */
    n = strlen(tok[1]);
    if (n == 0 || n >= IPC_NAME_MAX) {
        IPC_LOGE("config line %d: module name too long (max %d)", lineno,
                 IPC_NAME_MAX - 1);
        return IPC_ERR_CONFIG;
    }
    ipc_strlcpy(out->module, tok[1], sizeof(out->module));

    /* uid */
    if (parse_uid(tok[2], &out->uid) != IPC_OK) {
        IPC_LOGE("config line %d: bad uid '%s'", lineno, tok[2]);
        return IPC_ERR_CONFIG;
    }

    /* socket path */
    if (tok[3][0] != '/') {
        IPC_LOGE("config line %d: socket path must be absolute", lineno);
        return IPC_ERR_CONFIG;
    }
    n = strlen(tok[3]);
    if (n == 0 || n >= IPC_SUN_PATH_MAX) {
        IPC_LOGE("config line %d: socket path too long (max %d)", lineno,
                 IPC_SUN_PATH_MAX - 1);
        return IPC_ERR_CONFIG;
    }
    if (strcmp(tok[3], "/") == 0) {
        IPC_LOGE("config line %d: '/' is not a socket path", lineno);
        return IPC_ERR_CONFIG;
    }
    ipc_strlcpy(out->path, tok[3], sizeof(out->path));

    /* module ids must not contain characters that would be ambiguous in the
     * journal / logs we produce in tests.  Keep them conservative. */
    {
        const char *q;
        for (q = out->module; *q != '\0'; q++) {
            if (!isalnum((unsigned char)*q) && *q != '_' && *q != '-' &&
                *q != '.') {
                IPC_LOGE("config line %d: illegal char '%c' in module name",
                         lineno, *q);
                return IPC_ERR_CONFIG;
            }
        }
        for (q = out->ns; *q != '\0'; q++) {
            if (!isalnum((unsigned char)*q) && *q != '_' && *q != '-') {
                IPC_LOGE("config line %d: illegal char '%c' in namespace",
                         lineno, *q);
                return IPC_ERR_CONFIG;
            }
        }
    }

    return IPC_OK;
}

/* ------------------------------------------------------------------ */
/* table                                                               */
/* ------------------------------------------------------------------ */

static int push(ipc_config_t *c, const ipc_config_entry_t *e)
{
    if (c->count == c->cap) {
        int                 ncap = c->cap ? c->cap * 2 : 16;
        ipc_config_entry_t *ne =
            realloc(c->entries, (size_t)ncap * sizeof(*ne));
        if (ne == NULL) {
            return IPC_ERR_NOMEM;
        }
        c->entries = ne;
        c->cap     = ncap;
    }
    c->entries[c->count++] = *e;
    return IPC_OK;
}

static int find_dup(const ipc_config_t *c, const ipc_config_entry_t *e)
{
    int i;

    for (i = 0; i < c->count; i++) {
        if (strcmp(c->entries[i].ns, e->ns) == 0 &&
            strcmp(c->entries[i].module, e->module) == 0) {
            return 1; /* duplicate module inside a namespace */
        }
        if (strcmp(c->entries[i].path, e->path) == 0) {
            return 1; /* two modules sharing a socket path */
        }
    }
    return 0;
}

int ipc_config_parse(const char *text, ipc_config_t **out)
{
    ipc_config_t *c;
    char         *copy;
    char         *line;
    char         *save = NULL;
    int           lineno = 0;
    int           rc     = IPC_OK;

    if (text == NULL || out == NULL) {
        return IPC_ERR_INVAL;
    }
    *out = NULL;

    c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return IPC_ERR_NOMEM;
    }
    copy = strdup(text);
    if (copy == NULL) {
        free(c);
        return IPC_ERR_NOMEM;
    }

    for (line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        ipc_config_entry_t e;
        char              *p;

        lineno++;
        p = strchr(line, '#');
        if (p != NULL) {
            *p = '\0';
        }
        p = line;
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            continue;
        }
        if (ipc_config_parse_line(p, &e, lineno) != IPC_OK) {
            rc = IPC_ERR_CONFIG;
            break;
        }
        if (find_dup(c, &e)) {
            IPC_LOGE("config line %d: duplicate namespace/module or path", lineno);
            rc = IPC_ERR_CONFIG;
            break;
        }
        rc = push(c, &e);
        if (rc != IPC_OK) {
            break;
        }
    }

    free(copy);
    if (rc != IPC_OK) {
        ipc_config_free(c);
        return rc;
    }
    *out = c;
    return IPC_OK;
}

int ipc_config_load(const char *path, ipc_config_t **out)
{
    char *text;
    int   err = IPC_OK;
    int   rc;

    if (path == NULL || out == NULL) {
        return IPC_ERR_INVAL;
    }
    text = ipc_read_file(path, 1u << 20, &err);
    if (text == NULL) {
        IPC_LOGE("cannot read config %s: %s", path, ipc_strerror(err));
        return err;
    }
    rc = ipc_config_parse(text, out);
    free(text);
    if (rc != IPC_OK) {
        IPC_LOGE("config %s rejected (%s)", path, ipc_strerror(rc));
    }
    return rc;
}

void ipc_config_free(ipc_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    free(cfg->entries);
    free(cfg);
}

int ipc_config_count(const ipc_config_t *cfg)
{
    return cfg ? cfg->count : 0;
}

const ipc_config_entry_t *ipc_config_at(const ipc_config_t *cfg, int idx)
{
    if (cfg == NULL || idx < 0 || idx >= cfg->count) {
        return NULL;
    }
    return &cfg->entries[idx];
}

const ipc_config_entry_t *ipc_config_lookup(const ipc_config_t *cfg,
                                            const char *ns, const char *module)
{
    int i;

    if (cfg == NULL || ns == NULL || module == NULL) {
        return NULL;
    }
    for (i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].ns, ns) == 0 &&
            strcmp(cfg->entries[i].module, module) == 0) {
            return &cfg->entries[i];
        }
    }
    return NULL;
}

const ipc_config_entry_t *ipc_config_lookup_path(const ipc_config_t *cfg,
                                                 const char *path)
{
    int i;

    if (cfg == NULL || path == NULL) {
        return NULL;
    }
    for (i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].path, path) == 0) {
            return &cfg->entries[i];
        }
    }
    return NULL;
}
