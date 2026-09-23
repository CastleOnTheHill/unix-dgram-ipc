/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * lab.c -- 测试用临时实验目录的实现。**仅供测试，不随交付物发布**。
 */
#include "lab.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ipc/ipc.h"

int32_t IpcLabCreate(char *outDir, size_t cap)
{
    char   template[256];
    char  *made;
    size_t need;

    if (outDir == NULL || cap == 0) {
        return -1;
    }
    (void)snprintf(template, sizeof(template), "%s/%sXXXXXX", IPC_LAB_ROOT,
                   IPC_LAB_PREFIX);
    made = mkdtemp(template);
    if (made == NULL) {
        return -1;
    }
    need = strlen(made);
    if (need + 1 > cap) {
        (void)rmdir(made);
        return -1;
    }
    (void)memcpy(outDir, made, need + 1);
    return 0;
}

int32_t IpcLabWriteFile(const char *path, const char *text)
{
    FILE *fp;

    if (path == NULL || text == NULL) {
        return -1;
    }
    fp = fopen(path, "wb"); /* 二进制模式：换行不做翻译，内容逐字节可控 */
    if (fp == NULL) {
        return -1;
    }
    if (fwrite(text, 1, strlen(text), fp) != strlen(text)) {
        (void)fclose(fp);
        return -1;
    }
    if (fclose(fp) != 0) {
        return -1;
    }
    return 0;
}

void IpcLabConfPath(const char *dir, char *out, size_t cap)
{
    if (dir == NULL || out == NULL || cap == 0) {
        return;
    }
    (void)snprintf(out, cap, "%s/modules.conf", dir);
}

void IpcLabSockPath(const char *dir, const char *module, char *out, size_t cap)
{
    if (dir == NULL || module == NULL || out == NULL || cap == 0) {
        return;
    }
    (void)snprintf(out, cap, "%s/%s.sock", dir, module);
}

size_t IpcLabBuildConf(const char *dir, const char *ns, const char *const *modules,
                       int32_t count, char *out, size_t cap)
{
    size_t  used = 0;
    int32_t i;

    if (dir == NULL || ns == NULL || modules == NULL || out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    for (i = 0; i < count; i++) {
        char   line[512];
        char   path[256];
        int    written;

        IpcLabSockPath(dir, modules[i], path, sizeof(path));
        written = snprintf(line, sizeof(line), "%s %s %lu %s\n", ns, modules[i],
                           (unsigned long)getuid(), path);
        if (written <= 0 || (size_t)written >= sizeof(line)) {
            return 0;
        }
        if (used + (size_t)written + 1 > cap) {
            return 0;
        }
        (void)memcpy(out + used, line, (size_t)written);
        used += (size_t)written;
        out[used] = '\0';
    }
    return used;
}

/* 目录名是否是我们自己建的那种。 */
static int32_t IsOwnLabDir(const char *dir)
{
    const char *base;
    size_t      rootLen = strlen(IPC_LAB_ROOT);

    if (dir == NULL) {
        return 0;
    }
    if (strncmp(dir, IPC_LAB_ROOT "/", rootLen + 1) != 0) {
        return 0;
    }
    base = strrchr(dir, '/');
    if (base == NULL) {
        return 0;
    }
    base++;
    return (strncmp(base, IPC_LAB_PREFIX, strlen(IPC_LAB_PREFIX)) == 0) ? 1 : 0;
}

void IpcLabRemove(const char *dir)
{
    DIR           *handle;
    struct dirent *entry;

    if (!IsOwnLabDir(dir)) {
        fprintf(stderr, "!! IpcLabRemove 拒绝处理 '%s'：不是本测试建的实验目录\n",
                (dir != NULL) ? dir : "(null)");
        return;
    }
    handle = opendir(dir);
    if (handle != NULL) {
        while ((entry = readdir(handle)) != NULL) {
            char        full[512];
            struct stat st;

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            if ((size_t)snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name) >=
                sizeof(full)) {
                continue;
            }
            /* 只删文件/socket/符号链接；遇到子目录就跳过，不做递归 ——
             * 需要递归的删除不该出现在测试辅助里。 */
            if (lstat(full, &st) != 0) {
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                continue;
            }
            (void)unlink(full);
        }
        (void)closedir(handle);
    }
    (void)rmdir(dir);
}

/* ------------------------------------------------------------------ */
/* 日志捕获                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t level;
    char    module[64];
    char    text[512];
} LabLogRecord;

static LabLogRecord g_logRing[IPC_LAB_CAPTURE_MAX];
static int32_t      g_logCount;
static int32_t      g_logDropped;
static int32_t      g_logCapturing;

static void LabCaptureFunc(IpcLogLevel level, const char *moduleId, const char *format,
                           va_list args, void *user)
{
    LabLogRecord *record;

    (void)user;
    if (g_logCapturing == 0) {
        return;
    }
    if (g_logCount >= IPC_LAB_CAPTURE_MAX) {
        g_logDropped++;
        return;
    }
    record        = &g_logRing[g_logCount];
    record->level = (int32_t)level;
    (void)snprintf(record->module, sizeof(record->module), "%s",
                   (moduleId != NULL) ? moduleId : "(null)");
    (void)vsnprintf(record->text, sizeof(record->text), format, args);
    g_logCount++;
}

static void LabDiscardFunc(IpcLogLevel level, const char *moduleId, const char *format,
                           va_list args, void *user)
{
    (void)level;
    (void)moduleId;
    (void)format; /* 刻意不展开参数：被丢弃的日志不该在测试里产生任何副作用 */
    (void)args;
    (void)user;
}

void IpcLabCaptureBegin(void)
{
    int32_t i;

    for (i = 0; i < IPC_LAB_CAPTURE_MAX; i++) {
        g_logRing[i].level     = 0;
        g_logRing[i].module[0] = '\0';
        g_logRing[i].text[0]   = '\0';
    }
    g_logCount     = 0;
    g_logDropped   = 0;
    g_logCapturing = 1;
    IpcSetLogFunc(LabCaptureFunc, NULL);
    IpcSetLogLevel(IPC_LOG_DEBUG); /* 抓全部：断言挑哪一条是测试自己的事 */
}

void IpcLabCaptureEnd(void)
{
    g_logCapturing = 0;
    IpcSetLogFunc(NULL, NULL);
    IpcSetLogLevel(0);
}

void IpcLabSilenceLog(void)
{
    g_logCapturing = 0;
    IpcSetLogFunc(LabDiscardFunc, NULL);
    IpcSetLogLevel(IPC_LOG_ERROR);
}

int32_t IpcLabCaptureCount(void)
{
    return g_logCount;
}

int32_t IpcLabCaptureDropped(void)
{
    return g_logDropped;
}

int32_t IpcLabCaptureLevel(int32_t index)
{
    if (index < 0 || index >= g_logCount) {
        return 0;
    }
    return g_logRing[index].level;
}

const char *IpcLabCaptureModule(int32_t index)
{
    if (index < 0 || index >= g_logCount) {
        return "";
    }
    return g_logRing[index].module;
}

const char *IpcLabCaptureText(int32_t index)
{
    if (index < 0 || index >= g_logCount) {
        return "";
    }
    return g_logRing[index].text;
}

int32_t IpcLabCaptureContains(const char *needleA, const char *needleB)
{
    int32_t i;

    if (needleA == NULL) {
        return 0;
    }
    for (i = 0; i < g_logCount; i++) {
        if (strstr(g_logRing[i].text, needleA) == NULL) {
            continue;
        }
        if (needleB != NULL && strstr(g_logRing[i].text, needleB) == NULL) {
            continue;
        }
        return 1;
    }
    return 0;
}
