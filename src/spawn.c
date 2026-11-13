/* spawn.c — see spawn.h */
#define _GNU_SOURCE
#include "spawn.h"
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define MP_MAX_CHILDREN 8
static struct { FILE *fp; pid_t pid; } g_kids[MP_MAX_CHILDREN];

FILE *mp_popen(const char *cmd)
{
    int slot = -1;
    for (int i = 0; i < MP_MAX_CHILDREN; i++) if (!g_kids[i].fp) { slot = i; break; }
    if (slot < 0) return NULL;

    int p[2];
    if (pipe(p) != 0) return NULL;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_addclose(&fa, p[0]);
    posix_spawn_file_actions_addclose(&fa, p[1]);

    int n = 0;
    while (environ[n]) n++;
    char **envp = malloc((size_t)(n + 1) * sizeof(char *));
    int k = 0;
    if (envp) {
        for (int i = 0; i < n; i++)
            if (strncmp(environ[i], "LD_PRELOAD=", 11) != 0) envp[k++] = environ[i];
        envp[k] = NULL;
    }
    char *argv[] = { "sh", "-c", (char *)cmd, NULL };
    pid_t pid;
    int rc = posix_spawn(&pid, "/bin/sh", &fa, NULL, argv, envp ? envp : environ);
    posix_spawn_file_actions_destroy(&fa);
    free(envp);
    close(p[1]);
    if (rc != 0) { close(p[0]); return NULL; }
    FILE *fp = fdopen(p[0], "r");
    if (!fp) { close(p[0]); waitpid(pid, NULL, 0); return NULL; }
    g_kids[slot].fp = fp;
    g_kids[slot].pid = pid;
    return fp;
}

int mp_pclose(FILE *fp)
{
    pid_t pid = -1;
    for (int i = 0; i < MP_MAX_CHILDREN; i++)
        if (g_kids[i].fp == fp) { pid = g_kids[i].pid; g_kids[i].fp = NULL; break; }
    fclose(fp);
    if (pid < 0) return -1;
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {}
    return st;
}
