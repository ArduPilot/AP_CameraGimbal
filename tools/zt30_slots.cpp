#include "apcam/zt30_slots.h"
#include <sys/file.h>
int main(int argc, char **argv)
{
    std::string root = "/customer/camera-app", error, selected;
    int at = 1;
    if (argc > 3 && !strcmp(argv[at], "--root")) { root = argv[at + 1]; at += 2; }
    if (argc <= at || argc > at + 2) {
        fprintf(stderr, "Usage: zt30-slot [--root DIR] resolve | activate ID | migrate ID\n"); return 2;
    }
    bool ok = false;
    if (!strcmp(argv[at], "resolve") && argc == at + 1) {
        ok = apcam_zt30::resolve(root, selected, error);
        if (ok) puts(selected.c_str());
    } else if ((!strcmp(argv[at], "activate") || !strcmp(argv[at], "migrate")) && argc == at + 2) {
        if (!apcam_zt30::canonical_root(root, error)) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
        int fd = open((root + "/.slot-update.lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) < 0) {
            if (fd >= 0) close(fd);
            fprintf(stderr, "Slot update is locked or unavailable\n"); return 1;
        }
        ok = !strcmp(argv[at], "activate") ? apcam_zt30::activate(root, argv[at + 1], error) :
            apcam_zt30::migrate(root, argv[at + 1], error);
        close(fd);
    } else { fprintf(stderr, "Invalid slot operation\n"); return 2; }
    if (!ok) fprintf(stderr, "Slot operation failed (inspect active link if publication began): %s\n", error.c_str());
    return ok ? 0 : 1;
}
