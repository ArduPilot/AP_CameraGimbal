#pragma once
#include "zt30_bundle.h"
#include "live_video_probe.h"

namespace apcam_zt30 {
struct Trial {
    std::string previous, candidate, boot, state;
    uint64_t deadline = 0, attempts = 0;
    bool pending() const { return state == "starting" || state == "ready" || state == "recovering"; }
};
inline std::string trial_text(const Trial &t)
{
    return "{\"previous\":\""+t.previous+"\",\"candidate\":\""+t.candidate+"\",\"boot\":\""+t.boot+
        "\",\"deadline\":"+std::to_string(t.deadline)+",\"attempts\":"+std::to_string(t.attempts)+",\"state\":\""+t.state+"\"}";
}
inline bool read_trial(const std::string &root, Trial &t, std::string &error)
{
    std::string path = root+"/trial.json"; struct stat st{};
    if (lstat(path.c_str(), &st)) return errno == ENOENT || fail(error, "Cannot inspect application trial");
    if (!regular(path) || st.st_size <= 0 || st.st_size > 1024) return fail(error, "Unsafe application trial record");
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC); std::string text(size_t(st.st_size), '\0');
    bool ok = fd >= 0 && read_exact(fd, &text[0], text.size()); if (fd >= 0) close(fd);
    BundleJSON j(text);
    ok = ok && j.take("{\"previous\":") && j.string(t.previous) && slot_id_valid(t.previous.c_str()) &&
        j.take(",\"candidate\":") && j.string(t.candidate) && slot_id_valid(t.candidate.c_str()) && t.previous != t.candidate &&
        j.take(",\"boot\":") && j.string(t.boot) && token(t.boot, 80) && j.take(",\"deadline\":") &&
        j.number(t.deadline, UINT64_MAX/2) && t.deadline && j.take(",\"attempts\":") && j.number(t.attempts, 3) &&
        j.take(",\"state\":") && j.string(t.state) && j.take("}") && j.done() &&
        (t.pending() || t.state == "confirmed" || t.state == "rolled-back" || t.state == "abandoned");
    return ok || fail(error, "Invalid application trial record");
}
inline bool write_trial(const std::string &root, const Trial &t, std::string &error)
{
    std::string temporary = root+"/.trial.XXXXXX", text = trial_text(t);
    int fd = mkstemp(&temporary[0]); if (fd < 0) return fail(error, "Cannot reserve application trial");
    bool ok = fchmod(fd, 0600) == 0 && write_exact(fd, text.data(), text.size()) && fsync(fd) == 0;
    close(fd);
    if (ok) ok = rename(temporary.c_str(), (root+"/trial.json").c_str()) == 0;
    if (!ok) {unlink(temporary.c_str()); return fail(error, "Cannot publish application trial");}
    return sync_directory(root, error);
}
inline std::string boot_identity()
{
    char text[81]{}; int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd < 0 ? -1 : read(fd, text, sizeof(text)-1); if (fd >= 0) close(fd);
    if (n <= 0) return {};
    text[strcspn(text, "\r\n")] = 0; return text;
}
inline uint64_t trial_now() {return uint64_t(apcam::probe_milliseconds());}
} // namespace apcam_zt30
