#include "bsd_user.h"

void user_main(int argc, char **argv, char **envp)
{
    (void)envp;

    if (argv != NULL && argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) {
                write_all(1, " ", 1);
            }
            if (argv[i] != NULL) {
                write_text(1, argv[i]);
            }
        }
    }

    write_all(1, "\n", 1);
    user_exit(0);
}
