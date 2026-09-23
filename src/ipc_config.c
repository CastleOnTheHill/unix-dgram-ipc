/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * ipc_config.c -- 静态模块表的解析与查找。
 *
 * 规范细节以 examples/README.md 为准，本文件与它必须逐条一致。
 * 本文件是**纯逻辑**：不建 socket、不碰上下文，可以整体做白盒单测。
 */
#include "ipc_config.h"

#include <stdlib.h>
#include <string.h>

#include "ipc_log.h"
#include "ipc_util.h"

/* ------------------------------------------------------------------ */
/* 字符集判定                                                         */
/* ------------------------------------------------------------------ */

/*
 * 刻意不用 isalnum()：它受 locale 影响，在非 C locale 下可能把非 ASCII 的
 * 字母也判为「字母」，于是配置解析结果会随环境变量而变。这里按显式范围判断，
 * 结果与 locale 无关。
 */
static int32_t IsAsciiAlnum(char c)
{
    return ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
               ? 1
               : 0;
}

static int32_t IsNsChar(char c)
{
    return (IsAsciiAlnum(c) || c == '_' || c == '-') ? 1 : 0;
}

static int32_t IsModuleChar(char c)
{
    return (IsAsciiAlnum(c) || c == '_' || c == '-' || c == '.') ? 1 : 0;
}

static int32_t IsBlank(char c)
{
    return (c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r') ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* 行处理                                                             */
/* ------------------------------------------------------------------ */

size_t IpcConfigStripEol(char *line)
{
    size_t n;

    if (line == NULL) {
        return 0;
    }
    n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[--n] = '\0';
    }
    return n;
}

/*
 * uid：只接受纯十进制。刻意拒绝首字符为 '+' '-' 或空白，
 * 也拒绝尾随垃圾（"100 " 在切分阶段就已经被切成 "100"，这里只处理
 * 真正的越界值）。
 */
static int32_t ParseUid(const char *text, uid_t *out)
{
    char          *end = NULL;
    unsigned long  value;

    if (text == NULL || text[0] == '\0') {
        return IPC_ERR_CONFIG;
    }
    if (text[0] < '0' || text[0] > '9') {
        return IPC_ERR_CONFIG;
    }
    value = strtoul(text, &end, 10);
    if (end == text || *end != '\0') {
        return IPC_ERR_CONFIG;
    }
#ifdef UID_MAX
    if (value > (unsigned long)UID_MAX) {
        return IPC_ERR_CONFIG;
    }
#else
    if (value > 4294967294ul) {
        return IPC_ERR_CONFIG;
    }
#endif
    *out = (uid_t)value;
    return IPC_OK;
}

static int32_t CheckCharset(const char *text, int32_t (*predicate)(char),
                            const char *what, int32_t lineno)
{
    const char *p;

    for (p = text; *p != '\0'; p++) {
        if (!predicate(*p)) {
            IpcLogGlobalEmit(IPC_LOG_WARN, NULL,
                             "config line %d: illegal char '%c' in %s", lineno, *p,
                             what);
            return IPC_ERR_CONFIG;
        }
    }
    return IPC_OK;
}

int32_t IpcConfigParseLine(char *line, IpcConfigEntry *out, int32_t lineno)
{
    char  *token[4] = { NULL, NULL, NULL, NULL };
    char  *p;
    int32_t ntoken = 0;
    size_t  n;

    if (line == NULL || out == NULL) {
        return IPC_ERR_INVAL;
    }
    memset(out, 0, sizeof(*out));

    /* 按空白字符切分；这里是本函数唯一允许「就地写」的地方。 */
    p = line;
    while (*p != '\0') {
        while (*p != '\0' && IsBlank(*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (ntoken == 4) {
            IpcLogGlobalEmit(IPC_LOG_WARN, NULL, "config line %d: too many fields",
                             lineno);
            return IPC_ERR_CONFIG;
        }
        token[ntoken++] = p;
        while (*p != '\0' && !IsBlank(*p)) {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }

    if (ntoken != 4) {
        IpcLogGlobalEmit(IPC_LOG_WARN, NULL,
                         "config line %d: expected 4 fields, got %d", lineno, ntoken);
        return IPC_ERR_CONFIG;
    }

    /* 命名空间 */
    n = strlen(token[0]);
    if (n == 0 || n >= (size_t)IPC_NS_MAX) {
        IpcLogGlobalEmit(IPC_LOG_WARN, NULL,
                         "config line %d: namespace length %zu out of range (1..%d)",
                         lineno, n, IPC_NS_MAX - 1);
        return IPC_ERR_CONFIG;
    }
    if (CheckCharset(token[0], IsNsChar, "namespace", lineno) != IPC_OK) {
        return IPC_ERR_CONFIG;
    }
    (void)IpcStrlcpy(out->ns, token[0], sizeof(out->ns));

    /* 模块标识 */
    n = strlen(token[1]);
    if (n == 0 || n >= (size_t)IPC_NAME_MAX) {
        IpcLogGlobalEmit(
            IPC_LOG_WARN, NULL,
            "config line %d: module id length %zu out of range (1..%d)", lineno, n,
            IPC_NAME_MAX - 1);
        return IPC_ERR_CONFIG;
    }
    if (CheckCharset(token[1], IsModuleChar, "module id", lineno) != IPC_OK) {
        return IPC_ERR_CONFIG;
    }
    (void)IpcStrlcpy(out->moduleId, token[1], sizeof(out->moduleId));

    /* uid */
    if (ParseUid(token[2], &out->uid) != IPC_OK) {
        IpcLogGlobalEmit(IPC_LOG_WARN, NULL,
                         "config line %d: uid must be a plain decimal number, got '%s'",
                         lineno, token[2]);
        return IPC_ERR_CONFIG;
    }

    /* 端点路径 */
    if (token[3][0] != '/') {
        IpcLogGlobalEmit(IPC_LOG_WARN, NULL,
                         "config line %d: socket path must be absolute", lineno);
        return IPC_ERR_CONFIG;
    }
    n = strlen(token[3]);
    if (n >= (size_t)IPC_PATH_MAX) {
        IpcLogGlobalEmit(IPC_LOG_WARN, NULL,
                         "config line %d: socket path length %zu exceeds %d", lineno, n,
                         IPC_PATH_MAX - 1);
        return IPC_ERR_CONFIG;
    }
    if (n == 1) {
        IpcLogGlobalEmit(IPC_LOG_WARN, NULL,
                         "config line %d: '/' is a directory, not a socket path",
                         lineno);
        return IPC_ERR_CONFIG;
    }
    (void)IpcStrlcpy(out->path, token[3], sizeof(out->path));

    return IPC_OK;
}

/* ------------------------------------------------------------------ */
/* 表操作                                                             */
/* ------------------------------------------------------------------ */

static int32_t PushEntry(IpcConfig *config, const IpcConfigEntry *entry)
{
    if (config->count == config->cap) {
        int32_t         newCap = (config->cap > 0) ? (config->cap * 2) : 16;
        IpcConfigEntry *grown;

        grown = (IpcConfigEntry *)realloc(config->entries,
                                         (size_t)newCap * sizeof(*grown));
        if (grown == NULL) {
            return IPC_ERR_NOMEM;
        }
        config->entries = grown;
        config->cap     = newCap;
    }
    config->entries[config->count] = *entry;
    config->count++;
    return IPC_OK;
}

/*
 * 查重：`(ns, moduleId)` 与 path **各自**都必须唯一。
 * 注意 path 的唯一性是跨命名空间的 —— 两个命名空间不能共用同一个 socket
 * 路径，因为路径本身就是内核那个地址空间里的名字，跟本库的 ns 无关。
 */
static int32_t IsDuplicate(const IpcConfig *config, const IpcConfigEntry *entry)
{
    int32_t i;

    for (i = 0; i < config->count; i++) {
        const IpcConfigEntry *e = &config->entries[i];

        if (strcmp(e->ns, entry->ns) == 0 && strcmp(e->moduleId, entry->moduleId) == 0) {
            return 1;
        }
        if (strcmp(e->path, entry->path) == 0) {
            return 1;
        }
    }
    return 0;
}

int32_t IpcConfigParse(const char *text, IpcConfig **outConfig)
{
    IpcConfig *config;
    char      *copy;
    char      *cursor;
    int32_t    lineno = 0;
    int32_t    rc     = IPC_OK;

    if (text == NULL || outConfig == NULL) {
        return IPC_ERR_INVAL;
    }
    *outConfig = NULL;

    config = (IpcConfig *)calloc(1, sizeof(*config));
    if (config == NULL) {
        return IPC_ERR_NOMEM;
    }
    copy = (char *)malloc(strlen(text) + 1);
    if (copy == NULL) {
        free(config);
        return IPC_ERR_NOMEM;
    }
    (void)memcpy(copy, text, strlen(text) + 1);

    cursor = copy;
    while (*cursor != '\0') {
        char              *lineEnd = strchr(cursor, '\n');
        char              *hash;
        char              *p;
        IpcConfigEntry     entry;

        lineno++;
        if (lineEnd != NULL) {
            *lineEnd = '\0';
        }
        p = cursor;
        if (IpcConfigStripEol(p) == 0) {
            /* 空行（或只有 \r） */
            if (lineEnd == NULL) {
                break;
            }
            cursor = lineEnd + 1;
            continue;
        }

        /* `#` 到行尾都是注释，行内注释同样支持。 */
        hash = strchr(p, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
        while (*p != '\0' && IsBlank(*p)) {
            p++;
        }
        if (*p != '\0') {
            rc = IpcConfigParseLine(p, &entry, lineno);
            if (rc != IPC_OK) {
                break;
            }
            if (IsDuplicate(config, &entry)) {
                IpcLogGlobalEmit(IPC_LOG_WARN, NULL,
                                 "config line %d: duplicate (ns,module) or duplicate "
                                 "socket path",
                                 lineno);
                rc = IPC_ERR_CONFIG;
                break;
            }
            rc = PushEntry(config, &entry);
            if (rc != IPC_OK) {
                break;
            }
        }

        if (lineEnd == NULL) {
            break;
        }
        cursor = lineEnd + 1;
    }

    free(copy);
    if (rc != IPC_OK) {
        IpcConfigDestroy(config);
        return rc;
    }
    *outConfig = config;
    return IPC_OK;
}

int32_t IpcConfigLoad(const char *path, IpcConfig **outConfig)
{
    char   *text;
    int32_t err = IPC_OK;
    int32_t rc;

    if (path == NULL || outConfig == NULL) {
        return IPC_ERR_INVAL;
    }
    text = IpcReadFile(path, IPC_CONFIG_FILE_MAX, &err);
    if (text == NULL) {
        IpcLogGlobalEmit(IPC_LOG_WARN, NULL, "cannot read config '%s': %s", path,
                         IpcErrnoString(-err));
        return err;
    }
    rc = IpcConfigParse(text, outConfig);
    if (rc != IPC_OK) {
        IpcLogGlobalEmit(IPC_LOG_WARN, NULL, "config '%s' rejected (whole table)",
                         path);
    }
    free(text);
    return rc;
}

void IpcConfigDestroy(IpcConfig *config)
{
    if (config == NULL) {
        return;
    }
    free(config->entries);
    free(config);
}

int32_t IpcConfigGetCount(const IpcConfig *config)
{
    return (config != NULL) ? config->count : 0;
}

const IpcConfigEntry *IpcConfigGetEntry(const IpcConfig *config, int32_t index)
{
    if (config == NULL || index < 0 || index >= config->count) {
        return NULL;
    }
    return &config->entries[index];
}

const IpcConfigEntry *IpcConfigFindModule(const IpcConfig *config, const char *ns,
                                          const char *moduleId)
{
    int32_t i;

    if (config == NULL || ns == NULL || moduleId == NULL) {
        return NULL;
    }
    for (i = 0; i < config->count; i++) {
        const IpcConfigEntry *e = &config->entries[i];

        if (strcmp(e->ns, ns) == 0 && strcmp(e->moduleId, moduleId) == 0) {
            return e;
        }
    }
    return NULL;
}

const IpcConfigEntry *IpcConfigFindByPath(const IpcConfig *config, const char *path)
{
    int32_t i;

    if (config == NULL || path == NULL) {
        return NULL;
    }
    for (i = 0; i < config->count; i++) {
        const IpcConfigEntry *e = &config->entries[i];

        if (strcmp(e->path, path) == 0) {
            return e;
        }
    }
    return NULL;
}
