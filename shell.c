#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <errno.h>
#include <sys/wait.h>

#define MAX_COMMAND_LINE_LEN 1024
#define MAX_COMMAND_LINE_ARGS 128

char prompt[] = "> ";
char delimiters[] = " \t\r\n";
extern char **environ;

/* expand $VAR tokens via getenv into dst */
static void expand_env(char *dst, size_t n, const char *src) {
    if (src[0] == '$') {
        const char *name = src + 1;
        const char *val  = getenv(name);
        snprintf(dst, n, "%s", val ? val : "");
    } else {
        snprintf(dst, n, "%s", src);
    }
}

/* tokenize by whitespace, expand $VAR, return argc */
static int tokenize(char *line, char **argv, int max_args, const char *delims) {
    int argc = 0;
    static char bufstore[MAX_COMMAND_LINE_ARGS][PATH_MAX];

    for (char *tok = strtok(line, delims);
         tok && argc < max_args - 1;
         tok = strtok(NULL, delims)) {

        expand_env(bufstore[argc], sizeof(bufstore[argc]), tok);
        argv[argc++] = bufstore[argc-1];
    }
    argv[argc] = NULL;
    return argc;
}

/* prompt */
static void print_prompt(void) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        // format: <path/to/dir> >
        printf("%s> ", cwd);
    } else {
        printf("> ");
    }
    fflush(stdout);
}

static volatile sig_atomic_t fg_child = -1; // pid of foreground child or -1

static void sigint_ignore(int sig) {
    (void)sig;
    // ignore in the shell so Ctrl-C doesn’t kill the shell itself
    write(STDOUT_FILENO, "\n", 1);
}

static void sigalrm_kill_child(int sig) {
    (void)sig;
    if (fg_child > 0) {
        kill(fg_child, SIGKILL);
    }
}

static void install_parent_handlers(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_ignore;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigalrm_kill_child;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);
}

static void reset_child_signals(void) {
    signal(SIGINT, SIG_DFL);
    signal(SIGALRM, SIG_DFL);
}

int main(void) {
    // Stores the string typed into the command line.
    char command_line[MAX_COMMAND_LINE_LEN];
    char cmd_bak[MAX_COMMAND_LINE_LEN];

    // Stores the tokenized command line input.
    char *arguments[MAX_COMMAND_LINE_ARGS];

    install_parent_handlers();

    while (true) {
        do {
            // Print the shell prompt with current working directory.
            print_prompt();   // already does printf + fflush(stdout)

            // Read input from stdin and store it in command_line...
            if ((fgets(command_line, MAX_COMMAND_LINE_LEN, stdin) == NULL) &&
                ferror(stdin)) {
                fprintf(stderr, "fgets error");
                exit(0);
            }
        } while (command_line[0] == 0x0A);  // while just ENTER pressed

        command_line[strlen(command_line) - 1] = '\0';

        // If the user input was EOF (ctrl+d), exit the shell.
        if (feof(stdin)) {
            printf("\n");
            fflush(stdout);
            fflush(stderr);
            return 0;
        }

        // 1. Tokenize the command line input (split it on whitespace)
        int argc = tokenize(command_line, arguments,
                            MAX_COMMAND_LINE_ARGS, delimiters);
        if (argc == 0) continue; // empty

        // ===== Extra Credit: simple output redirection ">" =====
        int redirect_out = 0;
        char *out_file = NULL;
        for (int i = 0; i < argc; i++) {
            if (strcmp(arguments[i], ">") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr,
                            "usage: command ... > filename\n");
                    argc = 0;
                    break;
                }
                redirect_out = 1;
                out_file = arguments[i + 1];
                arguments[i] = NULL; // end of argv for execvp
                argc = i;            // logical argc
                break;
            }
        }
        if (argc == 0) continue;

        // 2. Implement Built-In Commands
        if (strcmp(arguments[0], "exit") == 0) {
            puts("");
            fflush(stdout);
            fflush(stderr);
            break;
        }

        if (strcmp(arguments[0], "pwd") == 0) {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd)))
                puts(cwd);
            else
                perror("pwd");
            continue;
        }

        if (strcmp(arguments[0], "cd") == 0) {
            const char *target = (argc > 1) ? arguments[1] : getenv("HOME");
            if (!target) target = ".";
            if (chdir(target) != 0) perror("cd");
            continue;
        }

        if (strcmp(arguments[0], "echo") == 0) {
            for (int i = 1; i < argc; i++) {
                if (i > 1) putchar(' ');
                fputs(arguments[i], stdout);
            }
            putchar('\n');
            continue;
        }

        if (strcmp(arguments[0], "env") == 0) {
            if (argc == 1) {
                for (char **e = environ; *e; ++e) puts(*e);
            } else {
                for (int i = 1; i < argc; ++i) {
                    const char *v = getenv(arguments[i]);
                    if (v) puts(v);
                }
            }
            continue;
        }

        if (strcmp(arguments[0], "setenv") == 0) {
            if (argc < 2) {
                fprintf(stderr, "usage: setenv NAME=VALUE\n");
                continue;
            }
            char *eq = strchr(arguments[1], '=');
            if (!eq) {
                fprintf(stderr, "usage: setenv NAME=VALUE\n");
                continue;
            }
            *eq = '\0';
            const char *name = arguments[1];
            const char *val  = eq + 1;
            if (setenv(name, val, 1) != 0)
                perror("setenv");
            continue;
        }

        // 3. Background job handling: look for '&' at end
        int background = 0;
        if (argc > 0 && strcmp(arguments[argc - 1], "&") == 0) {
            background = 1;
            arguments[argc - 1] = NULL;
            argc--;
            if (argc == 0) continue; // only '&'
        }

        // 4. The parent process should wait for the child to complete
        //    unless it's a background process
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            continue;
        }

        if (pid == 0) {
            // ---- child ----
            reset_child_signals();

            // apply output redirection if requested
            if (redirect_out) {
                int fd = open(out_file,
                              O_WRONLY | O_CREAT | O_TRUNC,
                              0666);
                if (fd < 0) {
                    perror("open");
                    _exit(127);
                }
                if (dup2(fd, STDOUT_FILENO) < 0) {
                    perror("dup2");
                    close(fd);
                    _exit(127);
                }
                close(fd);
            }

            execvp(arguments[0], arguments);
            // if exec failed:
            perror("execvp");
            _exit(127);
        }

        // ---- parent ----
        if (!background) {
            fg_child = pid;
            alarm(10);                 // Task 5: kill child after 10s if still running
            int status;
            if (waitpid(pid, &status, 0) < 0) {
                perror("waitpid");
            } else if (WIFEXITED(status) &&
                       WEXITSTATUS(status) == 127) {
                fprintf(stderr, "An error occurred.\n");
            }
            alarm(0);                  // cancel timeout
            fg_child = -1;
        } else {
            printf("[bg] started pid %d\n", pid);
        }
    }

    return -1; // should never be reached
}
