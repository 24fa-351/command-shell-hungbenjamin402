#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

#define MAX_COMMAND_LENGTH 1024
#define MAX_ARGS 64
#define MAX_ENV_VARS 100
#define MAX_VAR_NAME 64
#define MAX_VAR_VALUE 256
#define MAX_PATH_LENGTH 4096
#define MAX_COMMANDS 10

// Structures
typedef struct {
    char name[MAX_VAR_NAME];
    char value[MAX_VAR_VALUE];
} EnvVar;

typedef struct {
    char *args[MAX_ARGS];
    int arg_count;
    char *input_file;
    char *output_file;
    int background;
} Command;

// Global variables
EnvVar env_vars[MAX_ENV_VARS];
int env_var_count = 0;
char *path_dirs[MAX_ARGS];
int path_count = 0;

// Function to initialize PATH
void initialize_path() {
    char *path = getenv("PATH");
    if (path) {
        char *path_copy = strdup(path);
        char *dir = strtok(path_copy, ":");
        while (dir && path_count < MAX_ARGS) {
            path_dirs[path_count++] = strdup(dir);
            dir = strtok(NULL, ":");
        }
        free(path_copy);
    }
}

// Environment variable functions
void set_env_var(const char *name, const char *value) {
    int index = -1;
    for (int i = 0; i < env_var_count; i++) {
        if (strcmp(env_vars[i].name, name) == 0) {
            index = i;
            break;
        }
    }
    if (index == -1) {
        if (env_var_count < MAX_ENV_VARS) {
            index = env_var_count++;
        } else {
            printf("Maximum environment variables reached\n");
            return;
        }
    }
    strncpy(env_vars[index].name, name, MAX_VAR_NAME - 1);
    strncpy(env_vars[index].value, value, MAX_VAR_VALUE - 1);
    env_vars[index].name[MAX_VAR_NAME - 1] = '\0';
    env_vars[index].value[MAX_VAR_VALUE - 1] = '\0';
}

void unset_env_var(const char *name) {
    for (int i = 0; i < env_var_count; i++) {
        if (strcmp(env_vars[i].name, name) == 0) {
            for (int j = i; j < env_var_count - 1; j++) {
                strcpy(env_vars[j].name, env_vars[j + 1].name);
                strcpy(env_vars[j].value, env_vars[j + 1].value);
            }
            env_var_count--;
            return;
        }
    }
}

char *get_env_var(const char *name) {
    for (int i = 0; i < env_var_count; i++) {
        if (strcmp(env_vars[i].name, name) == 0) {
            return env_vars[i].value;
        }
    }
    return NULL;
}

// Function to replace environment variables in command
void replace_env_vars(char *command) {
    char result[MAX_COMMAND_LENGTH] = "";
    char *curr = command;
    char *dollar;

    while ((dollar = strchr(curr, '$')) != NULL) {
        strncat(result, curr, dollar - curr);
        char var_name[MAX_VAR_NAME] = "";
        char *var_end = dollar + 1;
        while (*var_end && (isalnum(*var_end) || *var_end == '_')) {
            var_end++;
        }
        strncpy(var_name, dollar + 1, var_end - (dollar + 1));
        var_name[var_end - (dollar + 1)] = '\0';
        char *value = get_env_var(var_name);
        if (value) {
            strcat(result, value);
        }
        curr = var_end;
    }
    strcat(result, curr);
    strcpy(command, result);
}

// Function to find executable in PATH
char *find_executable(const char *command) {
    if (strchr(command, '/')) {
        if (access(command, X_OK) == 0) {
            return strdup(command);
        }
        return NULL;
    }

    char path[MAX_PATH_LENGTH];
    for (int i = 0; i < path_count; i++) {
        snprintf(path, sizeof(path), "%s/%s", path_dirs[i], command);
        if (access(path, X_OK) == 0) {
            return strdup(path);
        }
    }
    return NULL;
}

// Parse a single command
void parse_command(char *cmd_str, Command *cmd) {
    char *token;
    int in_redirect = 0, out_redirect = 0;

    while (*cmd_str == ' ') cmd_str++;
    token = strtok(cmd_str, " ");

    while (token && cmd->arg_count < MAX_ARGS - 1) {
        if (strcmp(token, "<") == 0) {
            token = strtok(NULL, " ");
            if (token) cmd->input_file = strdup(token);
        } else if (strcmp(token, ">") == 0) {
            token = strtok(NULL, " ");
            if (token) cmd->output_file = strdup(token);
        } else if (strcmp(token, "&") == 0) {
            cmd->background = 1;
            break;
        } else {
            cmd->args[cmd->arg_count++] = strdup(token);
        }
        token = strtok(NULL, " ");
    }
    cmd->args[cmd->arg_count] = NULL;
}

// Execute a single command
void execute_single_command(Command *cmd) {
    pid_t pid = fork();
    if (pid == 0) {  // Child process
        // Handle input redirection
        if (cmd->input_file) {
            int fd = open(cmd->input_file, O_RDONLY);
            if (fd != -1) {
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
        }

        // Handle output redirection
        if (cmd->output_file) {
            int fd = open(cmd->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd != -1) {
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
        }

        char *executable = find_executable(cmd->args[0]);
        if (executable) {
            execv(executable, cmd->args);
            free(executable);
        }
        exit(1);
    } else if (pid > 0 && !cmd->background) {  // Parent process
        waitpid(pid, NULL, 0);
    }
}

// Function to cleanup command resources
void cleanup_command(Command *cmd) {
    for (int i = 0; i < cmd->arg_count; i++) {
        free(cmd->args[i]);
    }
    if (cmd->input_file) free(cmd->input_file);
    if (cmd->output_file) free(cmd->output_file);
}

// Execute pipeline of commands
void execute_pipeline(Command *commands, int cmd_count) {
    if (cmd_count == 1) {
        execute_single_command(&commands[0]);
        return;
    }

    int pipes[MAX_COMMANDS-1][2];
    pid_t pids[MAX_COMMANDS];

    // Create pipes
    for (int i = 0; i < cmd_count - 1; i++) {
        pipe(pipes[i]);
    }

    // Launch processes
    for (int i = 0; i < cmd_count; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            // Setup pipes for child
            if (i > 0) {
                dup2(pipes[i-1][0], STDIN_FILENO);
            }
            if (i < cmd_count - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            }

            // Close all pipe fds
            for (int j = 0; j < cmd_count - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            // Execute command
            char *executable = find_executable(commands[i].args[0]);
            if (executable) {
                execv(executable, commands[i].args);
                free(executable);
            }
            exit(1);
        }
    }

    // Close all pipe fds in parent
    for (int i = 0; i < cmd_count - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    // Wait for completion unless background execution
    if (!commands[cmd_count-1].background) {
        for (int i = 0; i < cmd_count; i++) {
            waitpid(pids[i], NULL, 0);
        }
    }
}

// Main command processing function
void process_command(char *command) {
    Command commands[MAX_COMMANDS];
    memset(commands, 0, sizeof(commands));
    int cmd_count = 0;

    // Handle empty command
    char *tmp = command;
    while (*tmp && isspace(*tmp)) tmp++;
    if (!*tmp) return;

    // Replace environment variables
    replace_env_vars(command);

    // Split by pipes and parse each command
    char *cmd_str = strtok(command, "|");
    while (cmd_str && cmd_count < MAX_COMMANDS) {
        parse_command(cmd_str, &commands[cmd_count++]);
        cmd_str = strtok(NULL, "|");
    }

    // Handle built-in commands
    if (cmd_count == 1) {
        if (strcmp(commands[0].args[0], "cd") == 0) {
            if (commands[0].arg_count > 1) {
                chdir(commands[0].args[1]);
            }
            cleanup_command(&commands[0]);
            return;
        }
        if (strcmp(commands[0].args[0], "pwd") == 0) {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd)) != NULL) {
                printf("%s\n", cwd);
            }
            cleanup_command(&commands[0]);
            return;
        }
        if (strcmp(commands[0].args[0], "set") == 0) {
            if (commands[0].arg_count >= 3) {
                set_env_var(commands[0].args[1], commands[0].args[2]);
            }
            cleanup_command(&commands[0]);
            return;
        }
        if (strcmp(commands[0].args[0], "unset") == 0) {
            if (commands[0].arg_count >= 2) {
                unset_env_var(commands[0].args[1]);
            }
            cleanup_command(&commands[0]);
            return;
        }
    }

    // Execute commands
    execute_pipeline(commands, cmd_count);

    // Cleanup
    for (int i = 0; i < cmd_count; i++) {
        cleanup_command(&commands[i]);
    }
}

int main() {
    char command[MAX_COMMAND_LENGTH];
    initialize_path();

    while (1) {
        printf("xsh# ");
        fflush(stdout);

        if (fgets(command, MAX_COMMAND_LENGTH, stdin) == NULL) {
            break;
        }

        command[strcspn(command, "\n")] = '\0';
        if (strcmp(command, "exit") == 0 || strcmp(command, "quit") == 0) {
            break;
        }

        process_command(command);
    }

    // Cleanup PATH
    for (int i = 0; i < path_count; i++) {
        free(path_dirs[i]);
    }

    return 0;
}