#ifndef CHRONO_DEMO_DASHBOARD_H
#define CHRONO_DEMO_DASHBOARD_H

#include "chrono/checkpoint.h"
#include "chrono/process.h"

#include <stddef.h>

#define CHRONO_DASHBOARD_COMMAND_MAX 256
#define CHRONO_DASHBOARD_USER_MAX 32

typedef struct {
    pid_t pid;
    pid_t parent_pid;
    uid_t owner;
    uint64_t start_time;
    char state;
    char user[CHRONO_DASHBOARD_USER_MAX];
    char command[CHRONO_DASHBOARD_COMMAND_MAX];
    char executable[CHRONO_DASHBOARD_COMMAND_MAX];
} chrono_dashboard_process;

typedef struct {
    const char *timestamp;
    const char *message;
} chrono_dashboard_event;

typedef enum {
    CHRONO_DASHBOARD_SCREEN_DEFAULT,
    CHRONO_DASHBOARD_SCREEN_MENU,
    CHRONO_DASHBOARD_SCREEN_PROGRAM,
    CHRONO_DASHBOARD_SCREEN_DETAILS,
    CHRONO_DASHBOARD_SCREEN_MONITOR,
    CHRONO_DASHBOARD_SCREEN_CHECKPOINT,
    CHRONO_DASHBOARD_SCREEN_TERMINATION,
    CHRONO_DASHBOARD_SCREEN_CHECKPOINT_BROWSER,
    CHRONO_DASHBOARD_SCREEN_RESTORE,
    CHRONO_DASHBOARD_SCREEN_VERIFY,
    CHRONO_DASHBOARD_SCREEN_REPORT,
    CHRONO_DASHBOARD_SCREEN_MESSAGE
} chrono_dashboard_screen;

typedef struct {
    chrono_checkpoint_metadata metadata;
    int artifacts_valid;
    int restored;
    uid_t original_uid;
    dev_t executable_device;
    ino_t executable_inode;
    unsigned long long saved_counter;
} chrono_dashboard_checkpoint_row;

typedef struct {
    chrono_dashboard_screen screen;
    const char *phase;
    const char *program_name;
    const char *working_directory;
    const char *launch_arguments;
    const char *operation_step;
    const char *screen_message;
    const char *selection;
    const char *criu_path;
    const char *criu_version;
    const char *criu_status;
    const char *criu_mode;
    const char *supervisor_status;
    const char *feedback;
    const char *input_acknowledgement;
    const char *controls;
    const char *checkpoint_directory;
    const char *restore_status;
    const char *counter_status;
    const char *session_log;
    const char *managed_status;
    size_t process_limit;
    const chrono_dashboard_process *processes;
    size_t process_count;
    size_t process_page;
    size_t process_page_size;
    size_t inaccessible_processes;
    size_t terminal_width;
    size_t terminal_height;
    const chrono_dashboard_process *monitored_process;
    const char *monitored_status;
    int process_browser;
    int process_help;
    const char *criu_diagnostic;
    const chrono_process_info *managed_process;
    int counter_available;
    unsigned long long counter;
    int display_counter_available;
    unsigned long long display_counter;
    const chrono_checkpoint_metadata *checkpoint;
    int checkpoint_artifact_valid;
    const chrono_process_info *restored_process;
    const chrono_dashboard_event *events;
    size_t event_count;
    const chrono_dashboard_checkpoint_row *checkpoint_rows;
    size_t checkpoint_row_count;
    size_t selected_checkpoint_index;
    int has_selected_checkpoint;
    int recovery_verified;
} chrono_dashboard_view;

int chrono_dashboard_start(int *tty_mode);
void chrono_dashboard_stop(int tty_mode);
int chrono_dashboard_read_key(int tty_mode, char *key);
int chrono_dashboard_read_input_char(int tty_mode, char *key);
int chrono_dashboard_read_counter(pid_t pid, unsigned long long *counter);
int chrono_dashboard_process_snapshot(chrono_dashboard_process **processes,
                                      size_t *count, size_t *inaccessible);
int chrono_dashboard_process_refresh(pid_t pid, uint64_t expected_start_time,
                                     chrono_dashboard_process *process,
                                     char *error, size_t error_size);
void chrono_dashboard_process_snapshot_free(chrono_dashboard_process *processes);
void chrono_dashboard_render(const chrono_dashboard_view *view, int tty_mode);
int chrono_dashboard_input_feedback(char key, char *message, size_t message_size);

#endif