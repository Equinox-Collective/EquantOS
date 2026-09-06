#ifndef SHELL_H
#define SHELL_H

// Prints the very first prompt. Call once after term_init().
void shell_init(void);

// Parses one line of input and runs the matching builtin, if any.
// Always ends by printing the next prompt.
void shell_execute(const char *cmd_line);
void shell_execute_diag(const char *cmd_line);

#endif // SHELL_H
