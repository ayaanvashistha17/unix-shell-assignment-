#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>   // waitpid
#include <fcntl.h>      // open flags
#include <string.h>     // strcmp
#include <errno.h>

#include "parser.h"
#include "jobs.h"       // add_job(), print_jobs(), reap_finished_jobs()
#include "shell.h"      // extern char g_last_cmdline[256]

/**
 * @brief Executes a single, simple command in another process
 * @param cmd   program name (argv[0])
 * @param args  argv vector (NULL-terminated)
 * @param in    fd to use as STDIN (or STDIN_FILENO)
 * @param out   fd to use as STDOUT (or STDOUT_FILENO)
 * @param bg    run in background if non-zero
 */
int execute_command(char* cmd, char** args, int in, int out, int bg) {
    if (!cmd || !args || !args[0]) return EXIT_SUCCESS;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        if (in  != STDIN_FILENO)  close(in);
        if (out != STDOUT_FILENO) close(out);
        return EXIT_FAILURE;
    }

    if (pid == 0) {
        if (in  != STDIN_FILENO)  {
            if (dup2(in,  STDIN_FILENO)  < 0) { perror("dup2 in");  _exit(126); }
            close(in);
        }
        if (out != STDOUT_FILENO) {
            if (dup2(out, STDOUT_FILENO) < 0) { perror("dup2 out"); _exit(126); }
            close(out);
        }
        execvp(cmd, args);
        perror("execvp");
        _exit(127);
    }

    if (in  != STDIN_FILENO)  close(in);
    if (out != STDOUT_FILENO) close(out);

    if (!bg) {
        int status;
        (void)waitpid(pid, &status, 0);
    } else {
        add_job(pid, g_last_cmdline);
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Executes a command line (simple or pipeline)
 * @param l parsed command line
 * @return 0 on success, non-zero on error
 */
int execute(struct cmdline *l) {
    if (!l) return EXIT_SUCCESS;

    reap_finished_jobs();

    // Count commands in the pipeline
    int ncmds = 0;
    while (l->seq[ncmds] != NULL) ncmds++;
    if (ncmds == 0) return EXIT_SUCCESS;

    // Single-command path (unchanged)
    if (ncmds == 1) {
        char **argv = l->seq[0];
        if (argv == NULL || argv[0] == NULL) return EXIT_SUCCESS;

        // Builtin: jobs
        if (strcmp(argv[0], "jobs") == 0) {
            print_jobs();
            return EXIT_SUCCESS;
        }

        int in_fd  = STDIN_FILENO;
        int out_fd = STDOUT_FILENO;

        if (l->in) {
            in_fd = open(l->in, O_RDONLY);
            if (in_fd < 0) { perror(l->in); return EXIT_FAILURE; }
        }
        if (l->out) {
            out_fd = open(l->out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out_fd < 0) {
                perror(l->out);
                if (in_fd != STDIN_FILENO) close(in_fd);
                return EXIT_FAILURE;
            }
        }

        return execute_command(argv[0], argv, in_fd, out_fd, l->bg ? 1 : 0);
    }

    /* ---------- N-stage pipeline (ncmds >= 2) ---------- */

    const int npipes = ncmds - 1;
    int (*pipes)[2] = NULL;
    if (npipes > 0) {
        pipes = malloc(sizeof(int[2]) * npipes);
        if (!pipes) { fprintf(stderr, "malloc pipes: %s\n", strerror(errno)); return EXIT_FAILURE; }
    }

    // Create all pipes
    for (int i = 0; i < npipes; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            for (int k = 0; k < i; k++) { close(pipes[k][0]); close(pipes[k][1]); }
            free(pipes);
            return EXIT_FAILURE;
        }
    }

    // Redirection endpoints (opened once)
    int in_fd_first  = STDIN_FILENO;
    int out_fd_last  = STDOUT_FILENO;

    if (l->in) {
        in_fd_first = open(l->in, O_RDONLY);
        if (in_fd_first < 0) {
            perror(l->in);
            for (int k = 0; k < npipes; k++) { close(pipes[k][0]); close(pipes[k][1]); }
            free(pipes);
            return EXIT_FAILURE;
        }
    }
    if (l->out) {
        out_fd_last = open(l->out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd_last < 0) {
            perror(l->out);
            if (in_fd_first != STDIN_FILENO) close(in_fd_first);
            for (int k = 0; k < npipes; k++) { close(pipes[k][0]); close(pipes[k][1]); }
            free(pipes);
            return EXIT_FAILURE;
        }
    }

    pid_t *pids = malloc(sizeof(pid_t) * ncmds);
    if (!pids) {
        fprintf(stderr, "malloc pids: %s\n", strerror(errno));
        if (in_fd_first  != STDIN_FILENO)  close(in_fd_first);
        if (out_fd_last  != STDOUT_FILENO) close(out_fd_last);
        for (int k = 0; k < npipes; k++) { close(pipes[k][0]); close(pipes[k][1]); }
        free(pipes);
        return EXIT_FAILURE;
    }

    for (int i = 0; i < ncmds; i++) {
        char **argv = l->seq[i];
        if (!argv || !argv[0]) {
            fprintf(stderr, "empty command in pipeline at stage %d\n", i);
            goto PIPE_FAIL;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            goto PIPE_FAIL;
        }

        if (pid == 0) {
            // Child: choose fds
            int in_fd  = (i == 0)           ? in_fd_first      : pipes[i-1][0];
            int out_fd = (i == ncmds - 1)   ? out_fd_last      : pipes[i][1];

            // Wire stdin/stdout
            if (in_fd != STDIN_FILENO)  { if (dup2(in_fd,  STDIN_FILENO)  < 0) { perror("dup2 in");  _exit(126); } }
            if (out_fd != STDOUT_FILENO){ if (dup2(out_fd, STDOUT_FILENO) < 0) { perror("dup2 out"); _exit(126); } }

            // Close all pipe fds in child
            for (int k = 0; k < npipes; k++) {
                close(pipes[k][0]);
                close(pipes[k][1]);
            }
            // Close redirection fds if not stdin/stdout for this child
            if (l->in  && in_fd_first  != STDIN_FILENO  && in_fd  != STDIN_FILENO)  close(in_fd_first);
            if (l->out && out_fd_last  != STDOUT_FILENO && out_fd != STDOUT_FILENO) close(out_fd_last);

            execvp(argv[0], argv);
            perror("execvp");
            _exit(127);
        }

        // Parent
        pids[i] = pid;

        // After forking stage i:
        if (i == 0 && in_fd_first != STDIN_FILENO) {
            close(in_fd_first);
            in_fd_first = STDIN_FILENO;
        }
        if (i > 0) {
        
            close(pipes[i-1][1]);
        }
    }

    // Parent
    for (int k = 0; k < npipes; k++) {
        close(pipes[k][0]);
        close(pipes[k][1]);
    }
    if (out_fd_last != STDOUT_FILENO) close(out_fd_last);

    if (!l->bg) {
        int status;
        for (int t = 0; t < ncmds; t++) (void)waitpid(pids[t], &status, 0);
    } else {
        // Track pipeline as a background job via first PID (simple & sufficient here)
        add_job(pids[0], g_last_cmdline);
    }

    free(pipes);
    free(pids);
    return EXIT_SUCCESS;

PIPE_FAIL:
    // Cleanup on error during pipeline setup/forking
    {
        int saved = errno;
        for (int k = 0; k < npipes; k++) { close(pipes[k][0]); close(pipes[k][1]); }
        if (in_fd_first  != STDIN_FILENO)  close(in_fd_first);
        if (out_fd_last  != STDOUT_FILENO) close(out_fd_last);
        // Best-effort reap any already-forked children
        for (int t = 0; t < ncmds; t++) if (pids && pids[t] > 0) (void)waitpid(pids[t], NULL, 0);
        free(pipes);
        free(pids);
        errno = saved;
        return EXIT_FAILURE;
    }
}
