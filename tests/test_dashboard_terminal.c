#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "../src/demo_dashboard.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static int fail(const char *message)
{
    fprintf(stderr, "test_dashboard_terminal: %s\n", message);
    return 1;
}

static int make_pty(int *master, int *slave)
{
    *master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (*master < 0 || grantpt(*master) != 0 || unlockpt(*master) != 0) return -1;
    char *name = ptsname(*master);
    if (name == NULL) return -1;
    *slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    return *slave < 0 ? -1 : 0;
}

static void discard_pty_output(int master, char *buffer, size_t size)
{
    buffer[0] = '\0';
    int flags = fcntl(master, F_GETFL);
    if (flags >= 0) (void)fcntl(master, F_SETFL, flags | O_NONBLOCK);
    for (;;) {
        ssize_t length = read(master, buffer, size - 1);
        if (length <= 0) break;
        buffer[length] = '\0';
    }
}

static volatile sig_atomic_t caught_signal;

static void catch_signal(int signo)
{
    caught_signal = signo;
}

int main(void)
{
    int master = -1, slave = -1;
    if (make_pty(&master, &slave) != 0) return fail("could not create pseudo-terminal");
    int saved_stdin = dup(STDIN_FILENO), saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdin < 0 || saved_stdout < 0 ||
        dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0)
        return fail("could not attach pseudo-terminal");
    struct winsize dimensions = {.ws_row = 60, .ws_col = 120};
    if (ioctl(slave, TIOCSWINSZ, &dimensions) != 0)
        return fail("could not set initial terminal dimensions");
    struct termios original, active, restored;
    if (tcgetattr(slave, &original) != 0) return fail("could not read initial terminal settings");
    int tty_mode = 0;
    if (chrono_dashboard_start(&tty_mode) != 0 || !tty_mode ||
        tcgetattr(slave, &active) != 0 ||
        (active.c_lflag & (ICANON | ECHO)) != 0)
        return fail("dashboard did not enter no-echo cbreak input mode");

    if (write(master, "cc", 2) != 2) return fail("could not provide repeated key input");
    char key = '\0';
    if (chrono_dashboard_read_key(1, &key) != 1 || key != 'c')
        return fail("first key was not recognized");
    if (chrono_dashboard_read_key(1, &key) != 0)
        return fail("immediately repeated key was not suppressed");
    if (write(master, "c", 1) != 1 ||
        chrono_dashboard_read_key(1, &key) != 1 || key != 'c')
        return fail("key was not re-armed after an idle interval");

    chrono_dashboard_view view = {
        .phase = "TERMINAL TEST",
        .criu_version = "CRIU test fixture",
        .criu_status = "preflight not run",
        .criu_mode = "test only",
        .supervisor_status = "fixture",
        .managed_status = "not launched"
    };
    chrono_dashboard_render(&view, 1);
    char output[32768] = {0};
    discard_pty_output(master, output, sizeof(output));
    dimensions.ws_row = 18;
    dimensions.ws_col = 40;
    if (ioctl(slave, TIOCSWINSZ, &dimensions) != 0)
        return fail("could not resize terminal");
    chrono_dashboard_render(&view, 1);
    discard_pty_output(master, output, sizeof(output));
    if (strstr(output, "PROCESS CONTROL CENTER") == NULL ||
        strstr(output, "MANAGED WORKLOAD") != NULL)
        return fail("dashboard did not redraw using resized terminal dimensions");

    struct sigaction action = {0}, previous;
    action.sa_handler = catch_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &previous) != 0 || kill(getpid(), SIGINT) != 0 ||
        caught_signal != SIGINT)
        return fail("test SIGINT handler did not run");
    chrono_dashboard_stop(1);
    if (tcgetattr(slave, &restored) != 0 ||
        (restored.c_lflag & (ICANON | ECHO)) != (original.c_lflag & (ICANON | ECHO)))
        return fail("terminal attributes were not restored after interruption");
    (void)sigaction(SIGINT, &previous, NULL);

    int input_pipe[2];
    if (pipe(input_pipe) != 0) return fail("could not create EOF fixture");
    int prior_stdin = dup(STDIN_FILENO);
    if (prior_stdin < 0 || dup2(input_pipe[0], STDIN_FILENO) < 0)
        return fail("could not attach EOF fixture");
    close(input_pipe[0]);
    close(input_pipe[1]);
    if (chrono_dashboard_read_key(0, &key) != -1)
        return fail("non-interactive EOF was not reported");
    (void)dup2(prior_stdin, STDIN_FILENO);
    close(prior_stdin);

    (void)dup2(saved_stdin, STDIN_FILENO);
    (void)dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    close(slave);
    close(master);
    puts("dashboard cbreak input, repeat suppression, resize redraw, EOF, SIGINT, and terminal restoration tests passed");
    return 0;
}
