#include "shell.h"
#include "term.h"
#include "string.h"
#include "timer.h"
#include <stdint.h>
#include <stddef.h>

#define SHELL_PROMPT_COLOR 0x0000FF00 // green
#define MAX_ARGS 8
#define LINE_BUF_SIZE 256

typedef void (*shell_handler_t)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *description;
    shell_handler_t handler;
} shell_command_t;

static void cmd_help(int argc, char **argv);
static void cmd_clear(int argc, char **argv);
static void cmd_echo(int argc, char **argv);
static void cmd_uptime(int argc, char **argv);
static void cmd_eqfetch(int argc, char **argv);
static void cmd_ver(int argc, char **argv);

// Add new builtins here — nothing else needs to change to register one.
static const shell_command_t commands[] = {
    { "help",    "List available commands",        cmd_help },
    { "clear",   "Clear the terminal screen",       cmd_clear },
    { "echo",    "Print text back to the terminal", cmd_echo },
    { "uptime",  "Show time since boot",            cmd_uptime },
    { "eqfetch", "Show the system info banner",     cmd_eqfetch },
    { "ver",     "Show OS version",                 cmd_ver },
};

#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

// Splits line in place on spaces using the libc strtok. Returns argc
// and fills argv with pointers into line (no allocation/copying).
static int tokenize(char *line, char **argv, int max_args) {
    int argc = 0;
    char *token = strtok(line, " ");
    while (token && argc < max_args) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    return argc;
}

static void print_prompt(void) {
    uint32_t prev = term_get_color();
    term_set_color(SHELL_PROMPT_COLOR);
    term_print("EquantOS> ");
    term_set_color(prev);
}

void shell_init(void) {
    print_prompt();
}

void shell_execute(const char *cmd_line_in) {
    char line[LINE_BUF_SIZE];
    size_t len = strlen(cmd_line_in);
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    for (size_t i = 0; i < len; i++) line[i] = cmd_line_in[i];
    line[len] = '\0';

    char *argv[MAX_ARGS];
    int argc = tokenize(line, argv, MAX_ARGS);

    if (argc > 0) {
        int found = 0;
        for (size_t i = 0; i < NUM_COMMANDS; i++) {
            if (strcmp(argv[0], commands[i].name) == 0) {
                commands[i].handler(argc, argv);
                found = 1;
                break;
            }
        }
        if (!found) {
            term_print("Command not found: ");
            term_print(argv[0]);
            term_print("\n");
        }
    }

    print_prompt();
}

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("Available commands:\n");
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        term_print("  ");
        term_print(commands[i].name);
        term_print(" - ");
        term_print(commands[i].description);
        term_print("\n");
    }
}

static void cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    term_clear();
}

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        term_print(argv[i]);
        if (i + 1 < argc) term_print(" ");
    }
    term_print("\n");
}

static void cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t seconds = tick / 100; // PIT frequency is 100 Hz

    char num[24];
    itoa((int64_t)seconds, 10, num);

    term_print("System uptime: ");
    term_print(num);
    term_print("s\n");
}

static void cmd_eqfetch(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("    _/_/_/_/   | Equant OS\n");
    term_print("   _/             \n");
    term_print("  _/_/_/        | v0.0.1 Alpha\n");
    term_print(" _/\n");
    term_print("    _/_/_/_/   | Nothing to see here for now.\n");
}

static void cmd_ver(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("Equant OS v0.0.1 Alpha\n");
}
