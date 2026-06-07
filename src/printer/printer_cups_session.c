/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Per-session CUPS instance management.
 *
 * Each xrdp session runs its own private cupsd so that:
 *  - Users only see their own redirected printers
 *  - System printers are forwarded as "remote" printers
 *  - No cross-session printer visibility
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pwd.h>
#include <grp.h>

#include "printer_cups.h"

#define LOG_PREFIX "CUPS_SESSION: "
#define CONF_TEMPLATE_DIR "/usr/share/xrdp"

/* Per-session state */
static pid_t g_cupsd_pid = -1;
static char g_state_dir[128] = {0};
static char g_socket_path[160] = {0};
static int g_session_cups_active = 0;

/**
 * Determine the base directory for per-session CUPS state.
 * Uses XDG_RUNTIME_DIR if available (e.g. /run/user/1000/),
 * otherwise falls back to /tmp.
 */
static void get_session_base_dir(char *buf, size_t buf_size, int session_id)
{
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir != NULL && runtime_dir[0] != '\0')
    {
        snprintf(buf, buf_size, "%s/xrdp-cups-%d", runtime_dir, session_id);
    }
    else
    {
        snprintf(buf, buf_size, "/tmp/xrdp-cups-%d-%d",
                 (int)getuid(), session_id);
    }
}

/**
 * Create the per-session directory structure for cupsd.
 */
static int create_session_dirs(const char *state_dir)
{
    char path[512];
    const char *subdirs[] = {"state", "cache", "spool", "tmp", "log", "ppd", NULL};

    if (mkdir(state_dir, 0700) != 0 && errno != EEXIST)
    {
        fprintf(stderr, LOG_PREFIX "Failed to create %s: %s\n",
                state_dir, strerror(errno));
        return -1;
    }

    for (int i = 0; subdirs[i] != NULL; i++)
    {
        snprintf(path, sizeof(path), "%s/%s", state_dir, subdirs[i]);
        if (mkdir(path, 0700) != 0 && errno != EEXIST)
        {
            fprintf(stderr, LOG_PREFIX "Failed to create %s: %s\n",
                    path, strerror(errno));
            return -1;
        }
    }

    return 0;
}

/**
 * Replace first occurrence of 'placeholder' in 'buf' with 'value'.
 * Returns 1 if replacement was made, 0 if not found.
 */
static int replace_placeholder(char *buf, size_t buf_size,
                               const char *placeholder, const char *value)
{
    char *p = strstr(buf, placeholder);
    if (p == NULL)
    {
        return 0;
    }

    size_t prefix_len = (size_t)(p - buf);
    size_t placeholder_len = strlen(placeholder);
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(p + placeholder_len);

    if (prefix_len + value_len + suffix_len >= buf_size)
    {
        return 0; /* would overflow */
    }

    memmove(p + value_len, p + placeholder_len, suffix_len + 1);
    memcpy(p, value, value_len);
    return 1;
}

/**
 * Generate a config file from template, substituting placeholders.
 */
static int generate_config(const char *template_path, const char *output_path,
                           const char *socket_path, const char *state_dir,
                           const char *user, const char *group)
{
    FILE *in = fopen(template_path, "r");
    if (in == NULL)
    {
        fprintf(stderr, LOG_PREFIX "Cannot open template %s: %s\n",
                template_path, strerror(errno));
        return -1;
    }

    FILE *out = fopen(output_path, "w");
    if (out == NULL)
    {
        fprintf(stderr, LOG_PREFIX "Cannot create config %s: %s\n",
                output_path, strerror(errno));
        fclose(in);
        return -1;
    }

    char line[1024];
    while (fgets(line, sizeof(line), in) != NULL)
    {
        char expanded[1024];
        strncpy(expanded, line, sizeof(expanded) - 1);
        expanded[sizeof(expanded) - 1] = '\0';

        while (replace_placeholder(expanded, sizeof(expanded),
                                   "@SOCKET_PATH@", socket_path)) {}
        while (replace_placeholder(expanded, sizeof(expanded),
                                   "@STATE_DIR@", state_dir)) {}
        while (replace_placeholder(expanded, sizeof(expanded),
                                   "@USER@", user)) {}
        while (replace_placeholder(expanded, sizeof(expanded),
                                   "@GROUP@", group)) {}

        fputs(expanded, out);
    }

    fclose(in);
    fclose(out);
    return 0;
}

/**
 * Create a minimal printers.conf that forwards system printers.
 * We use a helper script that queries the system CUPS and creates
 * relay entries in the private instance.
 */
#if 0 /* Disabled pending investigation into performance impact */
static int forward_system_printers(int display)
{
    char cmd[512];

    /*
     * Use lpadmin (targeting our private instance via CUPS_SERVER)
     * to create relay printers for each system printer.
     * The system CUPS listens on /run/cups/cups.sock.
     */
    snprintf(cmd, sizeof(cmd),
             "CUPS_SERVER='%s' /usr/share/xrdp/xrdp-cups-forward-printers "
             "/run/cups/cups.sock %d 2>/dev/null",
             g_socket_path, display);

    int ret = system(cmd);
    if (ret != 0)
    {
        fprintf(stderr, LOG_PREFIX "System printer forwarding returned %d "
                "(non-critical)\n", ret);
    }
    return 0; /* non-fatal */
}
#endif

/**
 * Start the per-session cupsd instance.
 *
 * @param session_id  The X display number for this session
 * @return 0 on success, -1 on failure
 */
int printer_cups_session_start(int session_id)
{
    char cupsd_conf[512];
    char cups_files_conf[512];
    const char *user;
    const char *group = "lpadmin";
    struct passwd *pw;

    if (g_session_cups_active)
    {
        return 0; /* already running */
    }

    /* Determine paths using user-writable directory */
    get_session_base_dir(g_state_dir, sizeof(g_state_dir), session_id);
    snprintf(g_socket_path, sizeof(g_socket_path),
             "%s/sock", g_state_dir);

    /* Get session user */
    pw = getpwuid(getuid());
    user = (pw != NULL) ? pw->pw_name : "lp";

    fprintf(stderr, LOG_PREFIX "Starting per-session cupsd for display %d "
            "(user=%s)\n", session_id, user);

    /* Create directory structure */
    if (create_session_dirs(g_state_dir) != 0)
    {
        return -1;
    }

    /* Generate config files from templates */
    snprintf(cupsd_conf, sizeof(cupsd_conf), "%s/cupsd.conf", g_state_dir);
    snprintf(cups_files_conf, sizeof(cups_files_conf),
             "%s/cups-files.conf", g_state_dir);

    if (generate_config(CONF_TEMPLATE_DIR "/xrdp-cupsd.conf.in",
                        cupsd_conf, g_socket_path, g_state_dir,
                        user, group) != 0)
    {
        return -1;
    }

    if (generate_config(CONF_TEMPLATE_DIR "/xrdp-cups-files.conf.in",
                        cups_files_conf, g_socket_path, g_state_dir,
                        user, group) != 0)
    {
        return -1;
    }

    /* Remove stale socket */
    unlink(g_socket_path);

    /* Fork and exec cupsd */
    g_cupsd_pid = fork();
    if (g_cupsd_pid < 0)
    {
        fprintf(stderr, LOG_PREFIX "fork() failed: %s\n", strerror(errno));
        return -1;
    }

    if (g_cupsd_pid == 0)
    {
        /* Child: close all inherited fds from chansrv (channels, pipes, etc.)
         * to prevent cupsd/backends from holding them open and corrupting
         * xrdp's internal socket/pipe state. Keep only stdin/stdout/stderr. */
        int maxfd = (int)sysconf(_SC_OPEN_MAX);
        if (maxfd < 0)
        {
            maxfd = 1024;
        }
        for (int fd = 3; fd < maxfd; fd++)
        {
            close(fd);
        }

        /* exec cupsd in foreground mode with our config */
        execl("/usr/sbin/cupsd", "cupsd", "-f",
              "-c", cupsd_conf,
              "-s", cups_files_conf,
              (char *)NULL);
        /* If exec fails */
        fprintf(stderr, LOG_PREFIX "execl(cupsd) failed: %s\n",
                strerror(errno));
        _exit(1);
    }

    /* Parent: wait briefly for socket to appear */
    for (int i = 0; i < 50; i++) /* up to 5 seconds */
    {
        usleep(100000);
        if (access(g_socket_path, F_OK) == 0)
        {
            break;
        }
        /* Check if child died */
        int status;
        pid_t p = waitpid(g_cupsd_pid, &status, WNOHANG);
        if (p > 0)
        {
            fprintf(stderr, LOG_PREFIX "cupsd exited immediately "
                    "(status=%d)\n", WEXITSTATUS(status));
            g_cupsd_pid = -1;
            return -1;
        }
    }

    if (access(g_socket_path, F_OK) != 0)
    {
        fprintf(stderr, LOG_PREFIX "cupsd socket did not appear at %s\n",
                g_socket_path);
        kill(g_cupsd_pid, SIGTERM);
        waitpid(g_cupsd_pid, NULL, 0);
        g_cupsd_pid = -1;
        return -1;
    }

    /* Make socket accessible to all (backend runs as lp) */
    chmod(g_socket_path, 0666);

    /* Brief pause to let cupsd finish initialization after creating socket */
    usleep(200000);

    /* Verify cupsd is actually accepting connections (max ~1s) */
    {
        char test_cmd[256];
        snprintf(test_cmd, sizeof(test_cmd),
                 "CUPS_SERVER='%s' lpstat -r >/dev/null 2>&1", g_socket_path);
        int tries = 0;
        while (tries < 5)
        {
            if (system(test_cmd) == 0)
            {
                break;
            }
            usleep(200000);
            tries++;
        }
        if (tries >= 5)
        {
            fprintf(stderr, LOG_PREFIX "cupsd not responding on %s\n",
                    g_socket_path);
            kill(g_cupsd_pid, SIGTERM);
            waitpid(g_cupsd_pid, NULL, 0);
            g_cupsd_pid = -1;
            return -1;
        }
    }

    /* Set CUPS_SERVER so all subsequent CUPS commands use our instance */
    setenv("CUPS_SERVER", g_socket_path, 1);

    g_session_cups_active = 1;
    fprintf(stderr, LOG_PREFIX "Per-session cupsd running (pid=%d, "
            "socket=%s)\n", (int)g_cupsd_pid, g_socket_path);

    /*
     * TODO: Forward system printers into our instance.
     * Disabled for now -- needs investigation into whether the IPP
     * relay printers cause performance issues.
     * forward_system_printers(session_id);
     */

    return 0;
}

/**
 * Stop the per-session cupsd and clean up.
 */
void printer_cups_session_stop(void)
{
    if (!g_session_cups_active)
    {
        return;
    }

    fprintf(stderr, LOG_PREFIX "Stopping per-session cupsd (pid=%d)\n",
            (int)g_cupsd_pid);

    if (g_cupsd_pid > 0)
    {
        kill(g_cupsd_pid, SIGTERM);

        /* Wait up to 3 seconds for graceful exit */
        for (int i = 0; i < 30; i++)
        {
            int status;
            pid_t p = waitpid(g_cupsd_pid, &status, WNOHANG);
            if (p > 0)
            {
                break;
            }
            usleep(100000);
        }

        /* Force kill if still running */
        if (waitpid(g_cupsd_pid, NULL, WNOHANG) == 0)
        {
            kill(g_cupsd_pid, SIGKILL);
            waitpid(g_cupsd_pid, NULL, 0);
        }

        g_cupsd_pid = -1;
    }

    /* Clean up socket and state */
    unlink(g_socket_path);

    /* Remove state directory contents */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_state_dir);
    if (system(cmd) != 0) { /* non-critical */ }

    unsetenv("CUPS_SERVER");
    g_session_cups_active = 0;

    fprintf(stderr, LOG_PREFIX "Per-session cupsd stopped\n");
}

/**
 * Get the per-session CUPS socket path.
 * Returns NULL if session CUPS is not active.
 */
const char *printer_cups_session_get_socket(void)
{
    return g_session_cups_active ? g_socket_path : NULL;
}
