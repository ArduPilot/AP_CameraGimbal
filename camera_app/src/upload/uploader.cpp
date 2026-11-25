/* Copies photos and recordings to a cloud service from a worker thread. A
 * small state machine driven from the main loop: it never blocks, and every
 * failure ends in DISABLED or BACKOFF so video is unaffected. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/uploader.h"
#include "camera_app/log.h"

#include "camera_app/https.h"
#include "upload_util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <limits.h>
#include <map>
#include <mutex>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <unistd.h>
#include <vector>

#define MAX_DEPTH 8
#define MAX_CONSECUTIVE_FAILURES 3
/* A file changed more recently than this is not uploaded, and is looked at again soon: it may still be being
 * written (a recording that has just started exists, empty, before the uploader hears about it). The file the
 * camera says it is recording to is left alone however old it looks. */
#define RECENT_MS 5000U
#define RECENT_RETRY_MS 10000U
/* A failed upload is tried again after this long, however many times it has failed. */
#define RETRY_MS 60000U

enum job_kind { JOB_PHOTOS, JOB_RECORDINGS };

struct ca_uploader {
    struct ca_upload_settings settings;
    struct ca_upload_settings scan_dest; /* the settings when the scan started: where every job of the scan sends to */
    struct ca_upload_status status;
    ca_uploader_backend_factory factory;
    enum job_kind jobs[2];
    bool job_low[2]; /* free space was low when the scan started: see start_scan() */
    unsigned job_count, job_index;
    uint64_t next_scan, retry_at;
    bool blocked;
    bool forced;                /* "Upload now": the scan it asks for runs even while armed */
    bool recording;             /* the camera is recording... */
    bool was_recording;
    std::vector<std::string> recording_files; /* ...to these files, if it said which (a file changed just now is left alone anyway) */
    /* The same files, as they are now (device and inode): the worker looks again before it sends or deletes each
     * file, because a recording can start after the folders were listed. */
    std::mutex recording_lock;
    /* Files the service cannot take (or has something better for), by path, size and time: they are not tried,
     * counted or announced again until they change, or "Upload now" asks for another go. The worker reads and
     * writes it while a job runs; the main loop clears it only between scans. Not kept over a restart. */
    std::map<std::string, std::string> unsendable;
    std::vector<std::pair<dev_t, ino_t>> recording_now;
    std::vector<std::string> recording_names_now;
    /* The worker: it reads `job` and the stop flag, and writes the counters, its
     * error and `ok`, then sets `done` last. The main loop reads the rest only
     * after `done`, when the thread has been joined. */
    pthread_t thread;
    bool running;
    std::atomic<bool> stop, done;
    struct {
        struct ca_upload_settings settings;
        enum job_kind kind;
        bool recording;
        std::vector<std::string> recording_files;
        bool move;
        bool plan;                /* the first job of a scan also counts what the others will upload */
        unsigned other_count;
        enum job_kind others[2];
        bool others_move[2];      /* whether each of those deletes after uploading */
    } job;
    /* What the worker works out before it starts, so the owner can say how many files need uploading. */
    std::atomic<bool> plan_ready;
    std::atomic<unsigned> plan_files;
    bool plan_seen, series_open, failed_announced;
    ca_uploader_notify notify;
    void *notify_user;
    std::atomic<uint64_t> job_bytes, job_total;
    std::atomic<unsigned> job_files, job_files_total;
    char job_error[160];
    unsigned job_unreadable;  /* files this job's folder could not examine: see run_job() */
    unsigned scan_unreadable; /* the same, for the whole scan: it is reported when the scan ends */
    unsigned job_recent;      /* files left for later because they changed a moment ago */
    unsigned scan_recent;
    unsigned job_active;      /* files left because the camera is recording to them */
    unsigned scan_active;
    unsigned job_unfinished;  /* unfinished files left behind in this job's folder: see UNFINISHED_STALE_S */
    unsigned scan_unfinished;
    unsigned job_rejected;    /* files that cannot be sent (unreadable, or a name the service cannot take): skipped */
    unsigned scan_rejected;
    char job_rejected_error[160], scan_rejected_error[160]; /* what was wrong with the last of them */
    bool job_ok;
    /* Progress of finished jobs in this scan, plus the running one. */
    uint64_t base_bytes, base_total;
    unsigned base_files, base_files_total;
    uint64_t credentials_checked; /* when the config file was last searched for the section */
    bool credentials_found;
    std::string credentials_key; /* the file and section credentials_found is for */
};

static void update_progress(struct ca_uploader *u)
{
    u->status.bytes_done = u->base_bytes + u->job_bytes;
    u->status.bytes_total = u->base_total + u->job_total;
    u->status.files_done = u->base_files + u->job_files;
    u->status.files_total = u->base_files_total + u->job_files_total;
}

static void set_text(char *to, size_t size, const char *from)
{
    size_t length = strlen(from);
    if (length >= size) length = size - 1;
    memcpy(to, from, length);
    to[length] = '\0';
}

/* ---- settings ---- */

/* Whether `path` is a folder to look in. A folder that is not there is nothing to upload; one that cannot be
 * examined for another reason is counted in `unreadable`, so the scan does not end as a success. */
static bool is_directory(const char *path, unsigned &unreadable)
{
    struct stat info;
    if (!path[0]) return false;
    if (stat(path, &info) != 0) {
        if (errno != ENOENT && errno != ENOTDIR) {
            ca_log("upload: cannot examine %s: %s", path, strerror(errno));
            unreadable++;
        }
        return false;
    }
    return S_ISDIR(info.st_mode);
}

/* What the user knows the service as, for messages. */
static const char *service_label(const char *service)
{
    return service && !strcmp(service, "dropbox") ? "Dropbox" : "Google Drive";
}

static bool settings_usable(const struct ca_upload_settings *s, char *reason, size_t size)
{
    if (s->disabled_reason[0]) { set_text(reason, size, s->disabled_reason); return false; }
    if (!s->enable) { set_text(reason, size, "Disabled"); return false; }
    if (!s->config_path[0]) { set_text(reason, size, "No credentials file set"); return false; }
    if (!s->folder[0]) { set_text(reason, size, "Enter a folder name"); return false; }
    if (!s->ca_pem && (!s->ca_file[0] || access(s->ca_file, R_OK) != 0)) {
        snprintf(reason, size, "CA certificates not found at %.64s", s->ca_file);
        return false;
    }
    if (!s->what || (s->what & ~(CA_UPLOAD_PHOTOS | CA_UPLOAD_RECORDINGS))) {
        set_text(reason, size, "Nothing selected to upload");
        return false;
    }
    return true;
}

/* True if the credentials file has the section the settings name (always, if none is named).
 * The file is read at most once a second. */
static bool credentials_present(struct ca_uploader *u, uint64_t now)
{
    const struct ca_upload_settings *s = &u->settings;
    if (!s->credentials_section[0]) return true;
    /* What was looked up is part of the answer: another service or file starts afresh. */
    const std::string key = std::string(s->config_path) + "|" + s->credentials_section;
    if (u->credentials_checked && now - u->credentials_checked < 1000U && key == u->credentials_key) return u->credentials_found;
    u->credentials_key = key;
    u->credentials_checked = now ? now : 1;
    u->credentials_found = false;
    FILE *file = fopen(s->config_path, "re");
    if (!file) return false;
    char header[48], line[256];
    snprintf(header, sizeof(header), "[%s]", s->credentials_section);
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, header, strlen(header)) == 0) { u->credentials_found = true; break; }
    }
    fclose(file);
    return u->credentials_found;
}

static bool space_is_low(const char *path, unsigned percent)
{
    struct statvfs v;
    if (statvfs(path, &v) != 0 || v.f_blocks == 0) return false;
    return (uint64_t)v.f_bavail * 100U < (uint64_t)percent * v.f_blocks;
}

/* ---- the worker ---- */

static void announce(struct ca_uploader *u, enum ca_upload_event event, unsigned files)
{
    if (u->notify) u->notify(u->notify_user, event, files);
}

/* The worker has worked out how many files need uploading: announce the start of a batch of work,
 * once, however many attempts it takes. */
static void check_plan(struct ca_uploader *u)
{
    if (u->plan_seen || !u->plan_ready.load()) return;
    u->plan_seen = true;
    const unsigned files = u->plan_files.load();
    if (files > 0 && !u->series_open) {
        u->series_open = true;
        u->failed_announced = false;
        announce(u, CA_UPLOAD_EVENT_STARTED, files);
    }
}

static void set_disabled(struct ca_uploader *u, const char *reason)
{
    u->series_open = false;
    u->forced = false;
    if (u->status.state != CA_UPLOAD_DISABLED || strcmp(u->status.reason, reason) != 0) {
        if (strcmp(reason, "Disabled") != 0) ca_log("upload: %s", reason);
        set_text(u->status.reason, sizeof(u->status.reason), reason);
    }
    u->status.state = CA_UPLOAD_DISABLED;
}

static void reset_counts(struct ca_uploader *u)
{
    u->base_bytes = u->base_total = 0;
    u->base_files = u->base_files_total = 0;
    u->job_bytes = u->job_total = 0;
    u->job_files = u->job_files_total = 0;
}

static void fail_run(struct ca_uploader *u, uint64_t now, const char *error)
{
    if (u->series_open && !u->failed_announced) {
        u->failed_announced = true;
        announce(u, CA_UPLOAD_EVENT_FAILED, 0);
    }
    set_text(u->status.last_error, sizeof(u->status.last_error), error);
    u->forced = false;
    u->retry_at = now + RETRY_MS;
    reset_counts(u);
    update_progress(u);
    u->status.state = CA_UPLOAD_BACKOFF;
    ca_log("upload: %s; retrying in %u s", error, RETRY_MS / 1000U);
}

struct file_entry {
    std::string path;       /* where it is on this device */
    std::string remote_dir; /* folders below photos/ or recordings/, "" for the top */
    std::string name;
    uint64_t size;
    int64_t mtime_ns;
    dev_t dev;
    ino_t ino;
};

/* What has been uploaded and checked, kept in a file so a restart does not check it all again. A file
 * counts as uploaded while its size and modification time are what they were. The file starts with the
 * service and folder it is for: another service or folder starts afresh. Not shared between threads. */
class upload_cache {
public:
    void open(const char *path, const char *service, const char *folder, const char *account)
    {
        _entries.clear();
        _dirty = false;
        _last_save = monotonic_ms() / 1000U;
        _path = path ? path : "";
        /* The service, the folder and the account (a hash that cannot be turned back into the credentials)
         * the files were uploaded to. */
        _header = std::string("# ") + service + " " + account + " " + folder;
        if (_path.empty()) return;
        FILE *file = fopen(_path.c_str(), "re");
        if (!file) return;
        std::vector<char> line(PATH_MAX + 80);
        bool first = true;
        while (fgets(line.data(), (int)line.size(), file)) {
            line[strcspn(line.data(), "\n")] = '\0';
            if (first) {
                first = false;
                if (_header != line.data()) break; /* for another service or folder */
                continue;
            }
            unsigned long long size = 0;
            long long mtime = 0;
            int used = 0;
            if (sscanf(line.data(), "%llu %lld %n", &size, &mtime, &used) == 2 && line[(size_t)used])
                _entries[line.data() + used] = {size, mtime};
        }
        fclose(file);
    }
    bool current(const file_entry &f) const
    {
        const auto found = _entries.find(f.path);
        return found != _entries.end() && found->second.size == f.size && found->second.mtime_ns == f.mtime_ns;
    }
    void add(const file_entry &f)
    {
        if (_path.empty()) return;
        _entries[f.path] = {f.size, f.mtime_ns};
        _dirty = true;
    }
    /* Writes the file again. Along the way it is written at most every 30 s, and only at the end of a
     * job are the files that are gone from this device dropped (that looks at every entry). */
    /* `prune` (at the end of a job that ran to the end) drops the files that are no longer on the device. */
    void save(bool final, bool prune = true)
    {
        if (_path.empty() || !_dirty) return;
        const uint64_t now = monotonic_ms() / 1000U;
        if (!final && now - _last_save < 30U) return;
        _last_save = now;
        const std::string temporary = _path + ".tmp";
        FILE *file = fopen(temporary.c_str(), "we");
        if (!file) return;
        bool ok = fprintf(file, "%s\n", _header.c_str()) > 0;
        for (const auto &entry : _entries) {
            struct stat info;
            if (final && prune && lstat(entry.first.c_str(), &info) != 0) continue;
            ok = ok && fprintf(file, "%llu %lld %s\n", (unsigned long long)entry.second.size,
                               (long long)entry.second.mtime_ns, entry.first.c_str()) > 0;
        }
        ok = (fclose(file) == 0) && ok;
        if (ok && rename(temporary.c_str(), _path.c_str()) == 0) _dirty = false;
        else unlink(temporary.c_str());
    }
private:
    struct entry { uint64_t size; int64_t mtime_ns; };
    std::map<std::string, entry> _entries;
    std::string _path, _header;
    bool _dirty = false;
    uint64_t _last_save = 0;
};

static bool skipped_name(const char *name)
{
    const size_t length = strlen(name);
    return (length >= 5 && !strcmp(name + length - 5, ".part")) || (length >= 4 && !strcmp(name + length - 4, ".tmp"));
}

/* An unfinished file (*.part, *.tmp) not changed for this long was left behind by a crash or a power cut: it is
 * not sent, and the user is told, so they can delete it. */
#define UNFINISHED_STALE_S 600

/* Lists the regular files below `directory`, without following links. */
static void collect_files(const std::string &directory, const std::string &remote_dir, unsigned depth,
                          std::vector<file_entry> &out, unsigned &unreadable, unsigned &unfinished, const std::atomic<bool> &stop)
{
    DIR *dir = opendir(directory.c_str());
    if (!dir) {
        if (errno != ENOENT) {
            ca_log("upload: cannot read %s: %s", directory.c_str(), strerror(errno));
            unreadable++;
        }
        return;
    }
    for (;;) {
        errno = 0;
        const dirent *entry = readdir(dir);
        if (!entry) {
            /* No more entries, or an error that looks the same: a directory read that failed part way would
             * otherwise end as a scan that succeeded. */
            if (errno != 0) {
                ca_log("upload: cannot read %s: %s", directory.c_str(), strerror(errno));
                unreadable++;
            }
            break;
        }
        if (stop.load()) break; /* a large card takes a while to walk: arming or recording must not wait for it */
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        const std::string path = directory + "/" + entry->d_name;
        struct stat info;
        if (lstat(path.c_str(), &info) != 0) {
            /* A file that vanished since readdir is nothing to report; any other error (such as EOVERFLOW
             * for a file too large for a 32-bit off_t) means a file is being passed over. */
            if (errno != ENOENT) {
                ca_log("upload: cannot examine %s: %s", path.c_str(), strerror(errno));
                unreadable++;
            }
            continue;
        }
        if (S_ISDIR(info.st_mode)) {
            if (depth < MAX_DEPTH) collect_files(path, remote_dir.empty() ? entry->d_name : remote_dir + "/" + entry->d_name, depth + 1, out, unreadable, unfinished, stop);
        } else if (S_ISREG(info.st_mode) && skipped_name(entry->d_name)) {
            if (time(NULL) - info.st_mtim.tv_sec > UNFINISHED_STALE_S) {
                ca_log("upload: %s is an unfinished file left behind; it is not sent, and can be deleted", path.c_str());
                unfinished++;
            }
        } else if (S_ISREG(info.st_mode)) {
            out.push_back({path, remote_dir, entry->d_name, (uint64_t)info.st_size,
                           (int64_t)info.st_mtim.tv_sec * 1000000000LL + info.st_mtim.tv_nsec, info.st_dev, info.st_ino});
        }
    }
    closedir(dir);
}

/* Tells one account from another without keeping anything secret: a 64-bit hash of the client ID and the
 * token saved for the service. Replacing the credentials with another account's changes it. */
static std::string credentials_fingerprint(const char *path, const char *section)
{
    char id[256] = "", token[4352] = "";
    config_value(path, section, "client_id", id, sizeof(id));
    config_value(path, section, "token", token, sizeof(token));
    uint64_t hash = 1469598103934665603ULL;
    const char *parts[3] = {id, "\n", token};
    for (const char *p : parts)
        for (; *p; p++) hash = (hash ^ (unsigned char)*p) * 1099511628211ULL;
    secure_zero(token, sizeof(token));
    char hex[20];
    snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)hash);
    return hex;
}

struct progress_context {
    struct ca_uploader *u;
    uint64_t finished_bytes;
};

static void on_progress(void *user, uint64_t sent)
{
    progress_context *p = (progress_context *)user;
    p->u->job_bytes = p->finished_bytes + sent;
}

static std::string unsendable_key(const file_entry &f)
{
    return f.path + "\n" + std::to_string(f.size) + "\n" + std::to_string(f.mtime_ns);
}

/* A recording on FAT continues in <stem>_part0002<extension> and so on, which the camera does not report: such a
 * file is taken as being recorded to while the recording it continues is open. */
static bool continues_recording(const std::string &path, const std::vector<std::string> &recording)
{
    for (const std::string &name : recording) {
        const size_t slash = name.rfind('/');
        size_t dot = name.rfind('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) dot = name.size();
        const std::string stem = name.substr(0, dot) + "_part", extension = name.substr(dot);
        if (path.size() <= stem.size() + extension.size() || path.compare(0, stem.size(), stem) != 0 ||
            path.compare(path.size() - extension.size(), extension.size(), extension) != 0) continue;
        const std::string digits = path.substr(stem.size(), path.size() - stem.size() - extension.size());
        if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos) return true;
    }
    return false;
}

/* Whether the camera is recording to this file now, which may be after the folders were listed. */
static bool being_recorded_now(struct ca_uploader *u, const file_entry &f)
{
    std::lock_guard<std::mutex> guard(u->recording_lock);
    if (continues_recording(f.path, u->recording_names_now)) return true;
    for (const auto &id : u->recording_now) {
        if (f.dev == id.first && f.ino == id.second) return true;
    }
    return false;
}

static void run_job(struct ca_uploader *u)
{
    const struct ca_upload_settings &s = u->job.settings;
    const bool photos = u->job.kind == JOB_PHOTOS;
    char error[sizeof(u->job_error)] = "";
    struct ca_upload_target target = {};
    target.service = s.service[0] ? s.service : "gdrive";
    target.credentials_path = s.config_path;
    target.credentials_section = s.credentials_section[0] ? s.credentials_section : target.service;
    target.root_folder = s.folder;
    target.ca_file = s.ca_file;
    target.ca_pem = s.ca_pem;
    target.bandwidth_kib_s = s.bandwidth_kib_s;
    target.stop = &u->stop;
#ifdef CAMERA_APP_UPLOAD_TEST_HOOKS
    /* Only in the test build: it points the service at a local server (CAMERA_APP_UPLOAD_TEST_ENDPOINT=host:port,
     * with CAMERA_APP_UPLOAD_TEST_NAME as the name its certificate carries). The cameras' firmware has no such
     * way to send the credentials to another server. */
    char test_host[128];
    const char *endpoint = getenv("CAMERA_APP_UPLOAD_TEST_ENDPOINT");
    if (endpoint && strchr(endpoint, ':') && strlen(endpoint) < sizeof(test_host)) {
        snprintf(test_host, sizeof(test_host), "%s", endpoint);
        *strrchr(test_host, ':') = '\0';
        target.api_host = target.auth_host = test_host;
        target.api_port = target.auth_port = (unsigned)atoi(strrchr(endpoint, ':') + 1);
        target.server_name = getenv("CAMERA_APP_UPLOAD_TEST_NAME");
    }
#endif
    /* What needs uploading: not what the cache already has. Nothing to do means no network at all. */
    upload_cache cache;
    cache.open(s.cache_path, target.service, s.folder,
               credentials_fingerprint(s.config_path, target.credentials_section).c_str());
    /* Deleting after the upload has to look at every file, remembered or not: one uploaded while the
     * setting was "keep" is still on this device, and is deleted once it is checked again. */
    unsigned unreadable = 0, recent = 0, active = 0, unfinished = 0, known_rejected = 0;
    std::string known_reason;
    /* The files being recorded to (some cameras write two), which may be named in a way that looks different from
     * the listing's, so they are matched by device and inode. */
    std::vector<std::pair<dev_t, ino_t>> recording_ids;
    if (u->job.recording) {
        for (const std::string &name : u->job.recording_files) {
            struct stat info;
            if (stat(name.c_str(), &info) == 0) recording_ids.push_back(std::make_pair(info.st_dev, info.st_ino));
        }
    }
    auto todo_in = [&](const char *root, bool move) {
        std::vector<file_entry> all, todo;
        collect_files(root, "", 0, all, unreadable, unfinished, u->stop);
        struct timespec clock_now;
        clock_gettime(CLOCK_REALTIME, &clock_now);
        const int64_t now_ns = (int64_t)clock_now.tv_sec * 1000000000LL + clock_now.tv_nsec;
        for (const file_entry &f : all) {
            /* Not a file being recorded to: it is sent when the recording is over. */
            bool being_recorded = false;
            for (const auto &id : recording_ids) {
                if (f.dev == id.first && f.ino == id.second) being_recorded = true;
            }
            if (being_recorded || continues_recording(f.path, u->job.recording_files)) {
                active++;
                continue;
            }
            /* Nor a file that may still be being written: it would be sent part way, or deleted part way. */
            const int64_t age_ns = now_ns - f.mtime_ns;
            if (age_ns < (int64_t)RECENT_MS * 1000000LL && age_ns > -(int64_t)RECENT_MS * 1000000LL) {
                recent++;
                continue;
            }
            const auto known = u->unsendable.find(unsendable_key(f));
            if (known != u->unsendable.end()) {
                if (!known->second.empty()) { known_rejected++; known_reason = known->second; }
                continue;
            }
            if (move || !cache.current(f)) todo.push_back(f);
        }
        return todo;
    };
    std::vector<file_entry> files = todo_in(photos ? s.capture_root : s.record_root, u->job.move);
    const unsigned passed_over = unreadable; /* from this job's folder only, not those looked at for the plan */
    u->job_unreadable = passed_over;
    u->job_recent = recent;
    u->job_active = active;
    u->job_unfinished = unfinished;
    if (known_rejected) { /* still there, still not sent: it stays on the page, but is not tried or announced again */
        u->job_rejected += known_rejected;
        set_text(u->job_rejected_error, sizeof(u->job_rejected_error), known_reason.c_str());
    }
    uint64_t total = 0;
    for (const file_entry &f : files) total += f.size;
    u->job_total = total;
    u->job_files_total = (unsigned)files.size();
    if (u->job.plan) {
        unsigned all = (unsigned)files.size();
        for (unsigned i = 0; i < u->job.other_count; i++)
            all += (unsigned)todo_in(u->job.others[i] == JOB_PHOTOS ? s.capture_root : s.record_root,
                                     u->job.others_move[i]).size();
        u->plan_files = all;
        u->plan_ready = true;
    }
    if (files.empty()) {
        u->job_ok = true;
        return;
    }

    APC_UploadBackend *backend = (u->factory ? u->factory : ca_upload_backend_create)(&target, error, sizeof(error));
    if (!backend) {
        set_text(u->job_error, sizeof(u->job_error), error[0] ? error : "cannot start the upload service");
        return;
    }

    const char *kind_folder = photos ? "photos" : "recordings";
    progress_context progress = {u, 0};
    unsigned failures = 0, consecutive = 0;
    bool stopped = false;
    for (const file_entry &f : files) {
        if (u->stop) { stopped = true; break; }
        /* A recording may have started to this file since the folders were listed: it is sent when it is over. */
        if (being_recorded_now(u, f)) {
            u->job_active++;
            u->job_total -= f.size;
            u->job_files_total--;
            continue;
        }
        const std::string remote_dir = f.remote_dir.empty() ? kind_folder : std::string(kind_folder) + "/" + f.remote_dir;
        const ca_put_result result = backend->put(remote_dir.c_str(), f.name.c_str(), f.path.c_str(), on_progress, &progress,
                                                  error, sizeof(error));
        if (result == CA_PUT_STOPPED) { stopped = true; break; }
        if ((result == CA_PUT_REJECTED || result == CA_PUT_FAILED) && access(f.path.c_str(), F_OK) != 0) {
            u->job_files_total--; /* gone since the listing: nothing to send */
            u->job_total -= f.size;
            u->job_bytes = progress.finished_bytes; /* without what was sent of it before it went */
            continue;
        }
        if (result == CA_PUT_REJECTED) {
            /* Nothing wrong with the connection, only with this file: the others are still sent. */
            ca_log("upload: skipping %s: %s", f.path.c_str(), error[0] ? error : "it cannot be sent");
            u->job_rejected++;
            set_text(u->job_rejected_error, sizeof(u->job_rejected_error), error[0] ? error : "it cannot be sent");
            u->unsendable[unsendable_key(f)] = u->job_rejected_error;
            u->job_total -= f.size;
            u->job_files_total--;
            u->job_bytes = progress.finished_bytes;
            continue;
        }
        if (result == CA_PUT_FAILED) {
            failures++;
            set_text(u->job_error, sizeof(u->job_error), error[0] ? error : "upload failed");
            if (++consecutive >= MAX_CONSECUTIVE_FAILURES) break;
            continue;
        }
        consecutive = 0;
        progress.finished_bytes += f.size;
        u->job_bytes = progress.finished_bytes;
        u->job_files++;
        /* Not sent, and not verified: it is neither remembered as uploaded nor deleted. */
        if (result == CA_PUT_LEFT) { u->unsendable[unsendable_key(f)] = ""; continue; }
        if (!u->job.move) {
            cache.add(f);
            if (u->job_files % 10 == 0) cache.save(false);
        }
        /* Only a file that the service has, checked, and that is still the file that was sent, may be
         * deleted here: one that grew while it was being sent (a recording that has started) is kept. */
        if (u->job.move) {
            struct stat after;
            if (lstat(f.path.c_str(), &after) != 0) {
                if (errno != ENOENT) ca_log("upload: cannot examine %s: %s", f.path.c_str(), strerror(errno));
            } else if ((uint64_t)after.st_size != f.size ||
                       (int64_t)after.st_mtim.tv_sec * 1000000000LL + after.st_mtim.tv_nsec != f.mtime_ns) {
                ca_log("upload: %s changed while it was sent, so it is kept", f.path.c_str());
            } else if (being_recorded_now(u, f)) {
                ca_log("upload: %s is being recorded to, so it is kept", f.path.c_str());
            } else if (unlink(f.path.c_str()) != 0 && errno != ENOENT) {
                ca_log("upload: cannot delete %s: %s", f.path.c_str(), strerror(errno));
            }
        }
    }
    delete backend;
    cache.save(true, !stopped); /* arming or recording is waiting: do not look at every file again */
    u->job_ok = !stopped && failures == 0;
    if (stopped && !u->job_error[0]) set_text(u->job_error, sizeof(u->job_error), "stopped");
}

static void *worker_main(void *context)
{
    struct ca_uploader *u = (struct ca_uploader *)context;
#ifdef __linux__ /* Cygwin has neither: the worker just runs at the normal priority there */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
    syscall(SYS_ioprio_set, 1 /* thread */, 0, 3 << 13 /* idle class */);
#endif
    run_job(u);
    u->done.store(true);
    return NULL;
}

static bool start_job(struct ca_uploader *u, uint64_t now)
{
    const struct ca_upload_settings *s = &u->settings;
    const enum job_kind kind = u->jobs[u->job_index];
    /* Delete or keep follows the current setting, so changing it during a scan applies to the
     * jobs still to run. Only the free-space measurement was taken when the scan started. */
    u->job.settings = *s;
    /* Where the files go is fixed for the whole scan, so one scan does not send to two services or accounts. */
    memcpy(u->job.settings.service, u->scan_dest.service, sizeof(u->job.settings.service));
    memcpy(u->job.settings.config_path, u->scan_dest.config_path, sizeof(u->job.settings.config_path));
    memcpy(u->job.settings.credentials_section, u->scan_dest.credentials_section, sizeof(u->job.settings.credentials_section));
    memcpy(u->job.settings.folder, u->scan_dest.folder, sizeof(u->job.settings.folder));
    memcpy(u->job.settings.cache_path, u->scan_dest.cache_path, sizeof(u->job.settings.cache_path));
    u->job.settings.ca_pem = u->scan_dest.ca_pem;
    memcpy(u->job.settings.ca_file, u->scan_dest.ca_file, sizeof(u->job.settings.ca_file));
    u->job.kind = kind;
    u->job.plan = u->job_index == 0;
    u->job.other_count = 0;
    for (unsigned i = u->job_index + 1; i < u->job_count && u->job.other_count < 2; i++) {
        const unsigned wanted = s->what;
        if (u->jobs[i] == JOB_PHOTOS ? (wanted & CA_UPLOAD_PHOTOS) : (wanted & CA_UPLOAD_RECORDINGS)) {
            u->job.others_move[u->job.other_count] = s->after_upload == CA_UPLOAD_DELETE ||
                (s->after_upload == CA_UPLOAD_DELETE_WHEN_LOW && u->job_low[i]);
            u->job.others[u->job.other_count++] = u->jobs[i];
        }
    }
    if (u->job.plan) {
        u->plan_ready.store(false);
        u->plan_files.store(0);
        u->plan_seen = false;
    }
    u->job.move = s->after_upload == CA_UPLOAD_DELETE ||
                  (s->after_upload == CA_UPLOAD_DELETE_WHEN_LOW && u->job_low[u->job_index]);
    u->job_bytes = u->job_total = 0;
    u->job_files = u->job_files_total = 0;
    u->job_error[0] = '\0';
    u->job_unreadable = 0;
    u->job_recent = 0;
    u->job_active = 0;
    u->job_rejected = 0;
    u->job_unfinished = 0;
    u->job.recording = u->recording;
    u->job.recording_files = u->recording_files;
    u->job_ok = false;
    u->stop.store(false);
    u->done.store(false);
    if (pthread_create(&u->thread, NULL, worker_main, u) != 0) {
        fail_run(u, now, "cannot start the upload thread");
        return false;
    }
    u->running = true;
    u->status.state = CA_UPLOAD_RUNNING;
    update_progress(u);
    ca_log("upload: %s %s", u->job.move ? "moving" : "copying", kind == JOB_PHOTOS ? s->capture_root : s->record_root);
    return true;
}

/* Files and folders that could not be examined are skipped like files that cannot be sent. */
static void note_unreadable(struct ca_uploader *u)
{
    if (!u->scan_unreadable) return;
    u->scan_rejected += u->scan_unreadable;
    set_text(u->scan_rejected_error, sizeof(u->scan_rejected_error), "they could not be examined (see the log)");
    u->scan_unreadable = 0;
}

static void complete_scan(struct ca_uploader *u, uint64_t now)
{
    if (u->series_open) {
        u->series_open = false;
        u->failed_announced = false;
        announce(u, CA_UPLOAD_EVENT_COMPLETE, u->status.files_done);
    }
    unsigned minutes = u->settings.scan_minutes ? u->settings.scan_minutes : 1U;
    u->status.last_success = time(NULL);
    u->status.last_error[0] = '\0';
    /* A file that cannot be sent does not fail the scan, but the page says so. */
    if (u->scan_rejected)
        snprintf(u->status.last_error, sizeof(u->status.last_error), "%u file(s) could not be uploaded: %.100s",
                 u->scan_rejected, u->scan_rejected_error);
    u->scan_rejected = 0;
    /* Unfinished files are left alone, and the user may want to delete them. */
    if (u->scan_unfinished) {
        const size_t used = strlen(u->status.last_error);
        snprintf(u->status.last_error + used, sizeof(u->status.last_error) - used,
                 "%s%u unfinished file(s) (*.part, *.tmp) left in the folders: delete them", used ? "; " : "",
                 u->scan_unfinished);
    }
    u->scan_unfinished = 0;
    u->forced = false;
    u->next_scan = now + (uint64_t)minutes * 60000U;
    /* Files left because they had just changed, or because they were being recorded to, are looked at again
     * shortly, not after a whole interval. While recording, that is when the recording ends (see
     * ca_uploader_periodic), not every few seconds until then. */
    if ((u->scan_recent || u->scan_active) && !u->recording && now + RECENT_RETRY_MS < u->next_scan)
        u->next_scan = now + RECENT_RETRY_MS;
    u->scan_recent = 0;
    u->scan_active = 0;
    u->status.state = CA_UPLOAD_WAITING;
}

static void start_scan(struct ca_uploader *u, uint64_t now)
{
    const struct ca_upload_settings *s = &u->settings;
    /* A series left open by a scan that was stopped, or that failed, names the old service: if this one sends
     * somewhere else it starts a series of its own, so the status text does not name two services. */
    if (u->series_open && strcmp(u->scan_dest.service, s->service) != 0) {
        u->series_open = false;
        u->failed_announced = false;
    }
    if (u->forced || strcmp(u->scan_dest.service, s->service) != 0 || strcmp(u->scan_dest.folder, s->folder) != 0)
        u->unsendable.clear(); /* another service or folder, or "Upload now": all files are tried again */
    u->scan_dest = *s;
    u->job_count = u->job_index = 0;
    u->scan_unreadable = 0;
    u->scan_recent = 0;
    u->scan_active = 0;
    u->scan_rejected = 0;
    u->scan_unfinished = 0;
    /* Whether free space is low is measured now, when the scan starts: uploading the
     * photos must not change it for the recordings. */
    if ((s->what & CA_UPLOAD_PHOTOS) && is_directory(s->capture_root, u->scan_unreadable)) {
        u->job_low[u->job_count] = space_is_low(s->capture_root, s->low_space_percent);
        u->jobs[u->job_count++] = JOB_PHOTOS;
    }
    if ((s->what & CA_UPLOAD_RECORDINGS) && is_directory(s->record_root, u->scan_unreadable)) {
        u->job_low[u->job_count] = space_is_low(s->record_root, s->low_space_percent);
        u->jobs[u->job_count++] = JOB_RECORDINGS;
    }
    reset_counts(u);
    update_progress(u);
    if (u->job_count == 0) {
        /* Nothing to look at: a success, though a folder that was there but could not be examined is reported. */
        note_unreadable(u);
        complete_scan(u, now);
    } else {
        (void)start_job(u, now);
    }
}

/* The job list is made when a scan starts, so the settings may have changed
 * since: a job runs only if they still ask for its kind. */
static bool job_wanted(const struct ca_uploader *u)
{
    const unsigned wanted = u->settings.what;
    return (u->jobs[u->job_index] == JOB_PHOTOS ? (wanted & CA_UPLOAD_PHOTOS) : (wanted & CA_UPLOAD_RECORDINGS)) != 0;
}

/* The worker has finished. */
static void finish_job(struct ca_uploader *u, uint64_t now, bool may_continue)
{
    check_plan(u); /* a short job can be over before the plan was noticed */
    pthread_join(u->thread, NULL);
    u->running = false;
    if (u->status.state == CA_UPLOAD_STOPPING) {
        /* Asked to stop: not a failure. Look again when uploads may run. */
        reset_counts(u);
        update_progress(u);
        u->next_scan = now;
        u->status.state = CA_UPLOAD_WAITING;
        ca_log("upload: stopped");
        return;
    }
    if (u->job_ok) {
        u->scan_unreadable += u->job_unreadable;
        u->scan_recent += u->job_recent;
        u->scan_active += u->job_active;
        u->scan_unfinished += u->job_unfinished;
        if (u->job_rejected) {
            u->scan_rejected += u->job_rejected;
            set_text(u->scan_rejected_error, sizeof(u->scan_rejected_error), u->job_rejected_error);
        }
        u->base_bytes += u->job_bytes;
        u->base_total += u->job_total;
        u->base_files += u->job_files;
        u->base_files_total += u->job_files_total;
        u->job_bytes = u->job_total = 0;
        u->job_files = u->job_files_total = 0;
        update_progress(u);
        do u->job_index++;
        while (u->job_index < u->job_count && !job_wanted(u));
        if (u->job_index < u->job_count) {
            if (may_continue) (void)start_job(u, now);
            else {
                /* Uploads are not allowed now: the scan starts again, from the beginning, when they are. */
                reset_counts(u);
                update_progress(u);
                u->next_scan = now;
                u->status.state = CA_UPLOAD_WAITING;
            }
            return;
        }
        /* Files that could not be examined are skipped, and reported when the scan ends. */
        note_unreadable(u);
        complete_scan(u, now);
        ca_log("upload: finished, %u files", u->status.files_done);
        return;
    }
    fail_run(u, now, u->job_error[0] ? u->job_error : "upload failed");
}

static void service_job(struct ca_uploader *u, uint64_t now, bool want_stop, bool may_continue)
{
    update_progress(u);
    check_plan(u);
    if (u->done.load()) {
        finish_job(u, now, may_continue);
        return;
    }
    /* A stop already asked for runs its course, even if uploads may run again. */
    if (want_stop && u->status.state != CA_UPLOAD_STOPPING) {
        u->stop.store(true);
        u->status.state = CA_UPLOAD_STOPPING;
    }
}

/* ---- public interface ---- */

struct ca_uploader *ca_uploader_create(void)
{
    struct ca_uploader *u = new (std::nothrow) ca_uploader();
    if (!u) return NULL;
    u->status.state = CA_UPLOAD_DISABLED;
    set_text(u->status.reason, sizeof(u->status.reason), "Disabled");
    return u;
}

void ca_uploader_destroy(struct ca_uploader *u)
{
    if (!u) return;
    if (u->running) {
        u->stop.store(true);
        pthread_join(u->thread, NULL);
    }
    delete u;
}

void ca_uploader_set_notify(struct ca_uploader *u, ca_uploader_notify notify, void *user)
{
    if (!u) return;
    u->notify = notify;
    u->notify_user = user;
}

void ca_uploader_set_backend_factory(struct ca_uploader *u, ca_uploader_backend_factory factory)
{
    if (u) u->factory = factory;
}

/* The service the status names: the one a running job, or a scan between its jobs, is sending to, which does not
 * change when the setting does, or else the one that is set. */
static void sync_service(struct ca_uploader *u)
{
    const char *service = u->running ? u->job.settings.service : u->series_open ? u->scan_dest.service : u->settings.service;
    set_text(u->status.service, sizeof(u->status.service), service[0] ? service : "gdrive");
}

void ca_uploader_configure(struct ca_uploader *u, const struct ca_upload_settings *settings)
{
    if (!u || !settings) return;
    u->settings = *settings;
    sync_service(u);
}

void ca_uploader_set_recording(struct ca_uploader *u, bool recording, const char *const *files, unsigned count)
{
    if (!u) return;
    u->recording = recording;
    u->recording_files.clear();
    std::vector<std::pair<dev_t, ino_t>> now;
    for (unsigned i = 0; recording && files && i < count; i++) {
        if (!files[i] || !files[i][0]) continue;
        u->recording_files.push_back(files[i]);
        struct stat info;
        if (stat(files[i], &info) == 0) now.push_back(std::make_pair(info.st_dev, info.st_ino));
    }
    std::lock_guard<std::mutex> guard(u->recording_lock);
    u->recording_now.swap(now);
    u->recording_names_now = u->recording_files;
}

void ca_uploader_periodic(struct ca_uploader *u, uint64_t now, bool armed)
{
    if (!u) return;
    sync_service(u); /* before the job is looked at, so what it announces names the service it sent to */
    /* Recording does not stop an upload: the file being recorded to is left alone. A recording that has
     * ended has a file to send, which is looked for soon (the file has just been closed). */
    if (u->was_recording && !u->recording && u->next_scan > now + RECENT_RETRY_MS) u->next_scan = now + RECENT_RETRY_MS;
    u->was_recording = u->recording;
    /* "Upload now" runs even while armed; being armed otherwise stops uploads. */
    const bool allowed = !armed || u->forced;
    if (u->blocked && allowed) u->next_scan = now; /* disarmed: look for new files */
    u->blocked = !allowed;

    char reason[sizeof(u->status.reason)];
    bool usable = settings_usable(&u->settings, reason, sizeof(reason));
    if (usable && !credentials_present(u, now)) {
        snprintf(reason, sizeof(reason), "No %s credentials saved", service_label(u->settings.service));
        usable = false;
    }
    if (u->running) {
        service_job(u, now, !usable || !allowed, usable && allowed);
        if (u->running) return;
    }
    if (!usable) {
        set_disabled(u, reason);
        return;
    }
    if (u->status.state == CA_UPLOAD_DISABLED) {
        u->status.reason[0] = '\0';
        u->status.state = CA_UPLOAD_WAITING;
        u->next_scan = now;
    }
    if (!allowed) return;
    /* Certificates have dates, so nothing can be sent before the clock is set. */
    const bool clock_ok = ca_https_clock_plausible();
    if (!clock_ok && u->status.state == CA_UPLOAD_WAITING) {
        u->forced = false; /* "Upload now" was for now: it must not start a scan later, perhaps while armed */
        set_text(u->status.reason, sizeof(u->status.reason), "Waiting for the clock to be set");
        return;
    }
    if (clock_ok && u->status.state == CA_UPLOAD_WAITING) u->status.reason[0] = '\0';
    if ((u->status.state == CA_UPLOAD_WAITING && now >= u->next_scan) ||
        (u->status.state == CA_UPLOAD_BACKOFF && now >= u->retry_at))
        start_scan(u, now);
}

void ca_uploader_scan_now(struct ca_uploader *u, uint64_t now)
{
    if (!u) return;
    if (u->status.state == CA_UPLOAD_WAITING) {
        u->next_scan = now;
        u->forced = true;
    } else if (u->status.state == CA_UPLOAD_BACKOFF) {
        u->retry_at = now;
        u->forced = true;
    }
}

const struct ca_upload_status *ca_uploader_status(const struct ca_uploader *u)
{
    return &u->status;
}

size_t ca_uploader_format_status(const struct ca_upload_status *status, char *out, size_t size)
{
    static const char *const names[] = {"disabled", "waiting", "running", "stopping", "backoff"};
    int n = snprintf(out, size,
                     "state=%s\nservice=%s\nreason=%s\nlast_success=%lld\nfiles_done=%u\nfiles_total=%u\n"
                     "bytes_done=%llu\nbytes_total=%llu\nlast_error=%s\n",
                     names[status->state], status->service, status->reason, (long long)status->last_success,
                     status->files_done, status->files_total,
                     (unsigned long long)status->bytes_done, (unsigned long long)status->bytes_total,
                     status->last_error);
    return n > 0 && (size_t)n < size ? (size_t)n : 0;
}
