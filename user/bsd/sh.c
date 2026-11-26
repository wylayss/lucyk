#include "bsd_user.h"

#define SH_LINE_MAX      256
#define SH_ARG_MAX       16
#define SH_DIRENT_SLOTS  16

static char line[SH_LINE_MAX];
static char *args[SH_ARG_MAX + 1];
static bsd_dirent_t dirents[SH_DIRENT_SLOTS];
static char pathbuf[BSD_PATH_MAX];
static char cwdbuf[BSD_PATH_MAX];
static char iobuf[512];

static bool dirent_is(const bsd_dirent_t *entry, const char *name)
{
    size_t length = text_len(name);

    return entry->d_namlen == length &&
           str_eq_len(entry->d_name, name, length);
}

static void print_error(const char *command, const char *what)
{
    write_text(1, "sh: ");
    write_text(1, command);
    write_text(1, ": ");
    write_text(1, what);
    write_all(1, "\n", 1);
}

static int split_line(char *text, char **out, int max)
{
    int count = 0;
    char *cursor = text;

    while (*cursor != '\0' && count < max) {
        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        out[count++] = cursor;

        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
            cursor++;
        }
        if (*cursor != '\0') {
            *cursor = '\0';
            cursor++;
        }
    }

    out[count] = NULL;
    return count;
}

static void builtin_cd(int argc, char **argv)
{
    const char *directory = argc > 1 ? argv[1] : "/";

    if (bsd_chdir(directory) != 0) {
        print_error("cd", "cannot change directory");
    }
}

static void builtin_pwd(void)
{
    if (bsd_getcwd(cwdbuf, sizeof(cwdbuf)) < 0) {
        print_error("pwd", "cannot read current directory");
        return;
    }

    write_text(1, cwdbuf);
    write_all(1, "\n", 1);
}

static void builtin_ls(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : ".";
    int64_t fd = bsd_open(path, BSD_O_RDONLY, 0);

    if (fd < 0) {
        print_error("ls", "cannot open");
        return;
    }

    for (;;) {
        int64_t bytes = bsd_getdents((int32_t)fd, dirents, sizeof(dirents));

        if (bytes == 0) {
            break;
        }
        if (bytes < 0) {
            print_error("ls", "not a directory");
            break;
        }

        size_t count = (size_t)bytes / sizeof(bsd_dirent_t);

        for (size_t i = 0; i < count; i++) {
            bsd_dirent_t *entry = &dirents[i];

            if (entry->d_reclen != sizeof(bsd_dirent_t)) {
                continue;
            }
            if (dirent_is(entry, ".") || dirent_is(entry, "..")) {
                continue;
            }

            write_all(1, entry->d_name, entry->d_namlen);
            write_all(1, "\n", 1);
        }
    }

    (void)bsd_close((int32_t)fd);
}

static void builtin_cat(int argc, char **argv)
{
    if (argc < 2) {
        write_text(1, "usage: cat <file>\n");
        return;
    }

    int64_t fd = bsd_open(argv[1], BSD_O_RDONLY, 0);

    if (fd < 0) {
        print_error("cat", "cannot open");
        return;
    }

    for (;;) {
        int64_t bytes = bsd_read((int32_t)fd, iobuf, sizeof(iobuf));

        if (bytes <= 0) {
            break;
        }
        write_all(1, iobuf, (size_t)bytes);
    }

    (void)bsd_close((int32_t)fd);
}

static void builtin_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            write_all(1, " ", 1);
        }
        if (argv[i] != NULL) {
            write_text(1, argv[i]);
        }
    }
    write_all(1, "\n", 1);
}

static int parse_signal(const char *text)
{
    if (text == NULL || text[0] != '-') {
        return -1;
    }

    text++;

    int64_t value = 0;
    if (parse_int(text, &value)) {
        return (value > 0 && value < 32) ? (int)value : -1;
    }

    if (str_eq(text, "HUP")) {
        return BSD_SIGHUP;
    }
    if (str_eq(text, "INT")) {
        return BSD_SIGINT;
    }
    if (str_eq(text, "KILL")) {
        return BSD_SIGKILL;
    }
    if (str_eq(text, "TERM")) {
        return BSD_SIGTERM;
    }

    return -1;
}

static void builtin_kill(int argc, char **argv)
{
    int signal = BSD_SIGTERM;
    int64_t pid = 0;

    if (argc == 2) {
        if (!parse_int(argv[1], &pid)) {
            write_text(1, "usage: kill [-sig] <pid>\n");
            return;
        }
    } else if (argc == 3) {
        signal = parse_signal(argv[1]);
        if (signal <= 0 || !parse_int(argv[2], &pid)) {
            write_text(1, "usage: kill [-sig] <pid>\n");
            return;
        }
    } else {
        write_text(1, "usage: kill [-sig] <pid>\n");
        return;
    }

    if (bsd_kill((int32_t)pid, (uint32_t)signal) < 0) {
        print_error("kill", "failed");
    }
}

static void builtin_help(void)
{
    write_text(1,
               "built-ins: cd [dir], pwd, ls [dir], cat <file>, echo [args...],\n"
               "           kill [-sig] <pid>, help, exit [code]\n"
               "external: /bin/<command> or an explicit path\n");
}

static void builtin_exit(int argc, char **argv)
{
    int64_t code = 0;

    if (argc > 1) {
        (void)parse_int(argv[1], &code);
    }

    user_exit((uint64_t)code);
}

static void run_external(char **argv)
{
    const char *path = argv[0];
    bool has_slash = false;

    for (const char *cursor = argv[0]; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            has_slash = true;
            break;
        }
    }

    if (!has_slash) {
        size_t length = text_len(argv[0]);

        if (length + 6 >= sizeof(pathbuf)) {
            print_error(argv[0], "path too long");
            return;
        }

        pathbuf[0] = '/';
        pathbuf[1] = 'b';
        pathbuf[2] = 'i';
        pathbuf[3] = 'n';
        pathbuf[4] = '/';
        for (size_t i = 0; i <= length; i++) {
            pathbuf[5 + i] = argv[0][i];
        }
        path = pathbuf;
    }

    int64_t pid = bsd_fork();

    if (pid < 0) {
        print_error(argv[0], "fork failed");
        return;
    }

    if (pid == 0) {
        (void)bsd_execve(path, argv, NULL);
        print_error(argv[0], "not found");
        user_exit(127);
    }

    int32_t status = 0;

    for (;;) {
        int64_t result = bsd_wait4((int32_t)pid, &status, 0);

        if (result == pid || result < 0) {
            break;
        }
    }
}

void user_main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    write_text(1, "\nLucyK BSD shell (Phase 3). Type 'help' for commands.\n");

    for (;;) {
        write_text(1, "lucyk# ");

        int64_t bytes = bsd_read(0, line, sizeof(line) - 1);

        if (bytes <= 0) {
            raw_sleep_ms(10);
            continue;
        }

        line[bytes] = '\0';

        while (bytes > 0 &&
               (line[bytes - 1] == '\n' || line[bytes - 1] == '\r')) {
            line[--bytes] = '\0';
        }

        int count = split_line(line, args, SH_ARG_MAX);

        if (count == 0) {
            continue;
        }

        if (str_eq(args[0], "cd")) {
            builtin_cd(count, args);
        } else if (str_eq(args[0], "pwd")) {
            builtin_pwd();
        } else if (str_eq(args[0], "ls")) {
            builtin_ls(count, args);
        } else if (str_eq(args[0], "cat")) {
            builtin_cat(count, args);
        } else if (str_eq(args[0], "echo")) {
            builtin_echo(count, args);
        } else if (str_eq(args[0], "kill")) {
            builtin_kill(count, args);
        } else if (str_eq(args[0], "help")) {
            builtin_help();
        } else if (str_eq(args[0], "exit")) {
            builtin_exit(count, args);
        } else {
            run_external(args);
        }
    }
}
