/* The cloud upload supervisor against a fake backend injected through
 * ca_uploader_set_backend_factory(): no network or account is needed. Time is
 * passed in, so waiting periods are stepped over by changing "now"; only the
 * worker thread's own run time is real. */
#include "camera_app/uploader.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

static char g_dir[PATH_MAX];

/* ---- the fake backend ---- */

enum fake_mode {
    MODE_OK,
    MODE_FAIL,       /* every put fails */
    MODE_FAIL_ONE,   /* only bad.jpg fails */
    MODE_REJECT_ONE, /* only bad.jpg cannot be sent, and the service is fine */
    MODE_REJECT_GONE,/* bad.jpg is deleted by someone else, and then cannot be sent */
    MODE_GROW_ONE,   /* grow.mp4 gets more data while it is being sent */
    MODE_LEFT_ONE,   /* only keep.jpg is left alone: not sent, and not to be deleted */
    MODE_SKIPPED,    /* the file is already there */
    MODE_SLOW,       /* blocks until stopped or released */
    MODE_STUBBORN,   /* like slow, but takes 300 ms to notice a stop */
    MODE_NO_SERVICE  /* the backend cannot be made */
};

static std::atomic<int> g_mode(MODE_OK);
static std::atomic<bool> g_release(false);
static std::atomic<int> g_in_put(0);
static std::atomic<unsigned> g_creates(0);
static std::atomic<unsigned> g_bandwidth(0);
static std::mutex g_lock;
static std::vector<std::string> g_puts; /* "remote_dir|name" */
static std::string g_root, g_ca, g_service, g_section;
static const char *g_ca_pem = NULL;

static std::vector<std::string> puts_sorted()
{
    std::lock_guard<std::mutex> guard(g_lock);
    std::vector<std::string> copy = g_puts;
    std::sort(copy.begin(), copy.end());
    return copy;
}

static size_t put_count()
{
    std::lock_guard<std::mutex> guard(g_lock);
    return g_puts.size();
}

class Fake : public APC_UploadBackend {
public:
    explicit Fake(const std::atomic<bool> *stop) : _stop(stop) {}
    ca_put_result put(const char *remote_dir, const char *name, const char *local_path,
                      void (*progress)(void *, uint64_t), void *user, char *error, size_t error_size) override
    {
        struct stat info;
        assert(stat(local_path, &info) == 0);
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_puts.push_back(std::string(remote_dir) + "|" + name);
        }
        g_in_put++;
        ca_put_result result = CA_PUT_UPLOADED;
        switch (g_mode.load()) {
        case MODE_FAIL:
            snprintf(error, error_size, "Google Drive said: quota \"exceeded\"");
            result = CA_PUT_FAILED;
            break;
        case MODE_FAIL_ONE:
            if (!strcmp(name, "bad.jpg")) {
                snprintf(error, error_size, "bad file");
                result = CA_PUT_FAILED;
            }
            break;
        case MODE_REJECT_ONE:
            if (!strcmp(name, "bad.jpg")) {
                snprintf(error, error_size, "cannot read bad.jpg");
                result = CA_PUT_REJECTED;
            }
            break;
        case MODE_REJECT_GONE:
            if (!strcmp(name, "bad.jpg")) {
                unlink(local_path);
                snprintf(error, error_size, "cannot read bad.jpg");
                result = CA_PUT_REJECTED;
            }
            break;
        case MODE_GROW_ONE:
            if (!strcmp(name, "grow.mp4")) {
                FILE *f = fopen(local_path, "ab");
                assert(f);
                fputs("more", f);
                fclose(f);
            }
            break;
        case MODE_LEFT_ONE:
            if (!strcmp(name, "keep.jpg")) result = CA_PUT_LEFT;
            break;
        case MODE_SKIPPED:
            result = CA_PUT_SKIPPED;
            break;
        case MODE_SLOW:
        case MODE_STUBBORN: {
            progress(user, (uint64_t)info.st_size / 2);
            while (!g_release) {
                if (_stop->load()) {
                    if (g_mode == MODE_STUBBORN) usleep(300000);
                    result = CA_PUT_STOPPED;
                    break;
                }
                usleep(1000);
            }
            break;
        }
        default:
            break;
        }
        if (result == CA_PUT_UPLOADED || result == CA_PUT_SKIPPED) progress(user, (uint64_t)info.st_size);
        g_in_put--;
        return result;
    }
    void set_bandwidth(unsigned) override {}

private:
    const std::atomic<bool> *_stop;
};

static APC_UploadBackend *make_fake(const struct ca_upload_target *target, char *error, size_t error_size)
{
    g_creates++;
    if (g_mode == MODE_NO_SERVICE) {
        snprintf(error, error_size, "didn't find section in config file");
        return NULL;
    }
    g_bandwidth = target->bandwidth_kib_s;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_root = target->root_folder;
        g_ca = target->ca_file;
        g_service = target->service;
        g_ca_pem = target->ca_pem;
        g_section = target->credentials_section;
    }
    return new Fake(target->stop);
}

/* ---- files and settings ---- */

static void put_text(char *to, size_t size, const char *from)
{
    size_t length = strlen(from);
    if (length >= size) length = size - 1;
    memcpy(to, from, length);
    to[length] = '\0';
}

static void under(char *to, size_t size, const char *directory, const char *name)
{
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    put_text(to, size, path);
}

static std::string path_of(const char *name)
{
    return std::string(g_dir) + "/" + name;
}

static void make_directory(const char *name)
{
    assert(mkdir(path_of(name).c_str(), 0755) == 0 || errno == EEXIST);
}

static void write_file(const char *name, size_t size)
{
    FILE *f = fopen(path_of(name).c_str(), "wb");
    assert(f);
    const std::string data(size, 'x');
    assert(fwrite(data.data(), 1, size, f) == size);
    fclose(f);
    /* Older than the minimum age for deleting, as a finished capture is. A file written again gets a new time. */
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    const struct timespec times[2] = {{now.tv_sec - 60, now.tv_nsec}, {now.tv_sec - 60, now.tv_nsec}};
    assert(utimensat(AT_FDCWD, path_of(name).c_str(), times, 0) == 0);
}

/* A file written just now, as a recording that has just started is. */
static void write_file_now(const char *name, size_t size)
{
    FILE *f = fopen(path_of(name).c_str(), "wb");
    assert(f);
    const std::string data(size, 'x');
    assert(fwrite(data.data(), 1, size, f) == size);
    fclose(f);
}

static bool exists(const char *name)
{
    return access(path_of(name).c_str(), F_OK) == 0;
}

/* capture: a.jpg (1000), sub/b.jpg (500), and two unfinished files that are
 * skipped; record: c.mp4 (3000). */
static void populate()
{
    assert(system(("rm -rf '" + path_of("capture") + "' '" + path_of("record") + "'").c_str()) == 0);
    make_directory("capture");
    make_directory("capture/sub");
    make_directory("record");
    write_file("capture/a.jpg", 1000);
    write_file("capture/sub/b.jpg", 500);
    write_file("capture/c.part", 10);
    write_file("capture/sub/d.tmp", 10);
    write_file("record/c.mp4", 3000);
}

static struct ca_upload_settings good_settings(void)
{
    struct ca_upload_settings s = {};
    s.enable = true;
    under(s.config_path, sizeof(s.config_path), g_dir, "rclone.conf");
    under(s.ca_file, sizeof(s.ca_file), g_dir, "ca.pem");
    put_text(s.folder, sizeof(s.folder), "cam");
    under(s.capture_root, sizeof(s.capture_root), g_dir, "capture");
    under(s.record_root, sizeof(s.record_root), g_dir, "record");
    s.what = CA_UPLOAD_PHOTOS | CA_UPLOAD_RECORDINGS;
    s.after_upload = CA_UPLOAD_KEEP;
    s.low_space_percent = 20;
    s.scan_minutes = 5;
    return s;
}

/* What the uploader announced: S<files> for a start, C for complete, F for failed. */
static std::vector<std::pair<int, unsigned>> g_events;
static void on_event(void *, enum ca_upload_event event, unsigned files)
{
    g_events.push_back({(int)event, files});
}
static std::string events()
{
    std::string text;
    for (const auto &e : g_events) {
        if (!text.empty()) text += ",";
        text += e.first == CA_UPLOAD_EVENT_STARTED ? "S" + std::to_string(e.second)
                : e.first == CA_UPLOAD_EVENT_COMPLETE ? "C" : "F";
    }
    return text;
}

static bool has(const std::string &text, const char *part)
{
    return text.find(part) != std::string::npos;
}

static struct ca_uploader *start(const struct ca_upload_settings &settings, int mode)
{
    g_mode = mode;
    g_release = false;
    g_creates = 0;
    g_events.clear();
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_puts.clear();
    }
    populate();
    struct ca_uploader *u = ca_uploader_create();
    assert(u);
    ca_uploader_set_backend_factory(u, make_fake);
    ca_uploader_set_notify(u, on_event, NULL);
    ca_uploader_configure(u, &settings);
    return u;
}

/* Calls periodic until the state is reached; the worker takes a little real time. */
static void wait_for(struct ca_uploader *u, enum ca_upload_state state, uint64_t now,
                     bool armed = false)
{
    for (int i = 0; i < 2000; i++) {
        ca_uploader_periodic(u, now, armed);
        if (ca_uploader_status(u)->state == state) return;
        usleep(5000);
    }
    fprintf(stderr, "stuck in state %d, wanted %d\n", ca_uploader_status(u)->state, state);
    assert(false);
}

/* The worker is inside put(). */
static void wait_for_put(struct ca_uploader *u, uint64_t now)
{
    for (int i = 0; i < 2000; i++) {
        ca_uploader_periodic(u, now, false);
        if (g_in_put > 0) return;
        usleep(5000);
    }
    assert(false);
}

/* ---- the tests ---- */

static void unusable_settings(void)
{
    struct ca_upload_settings off = good_settings();
    off.enable = false;
    struct ca_upload_settings dropbox_nocreds = good_settings();
    put_text(dropbox_nocreds.service, sizeof(dropbox_nocreds.service), "dropbox");
    put_text(dropbox_nocreds.credentials_section, sizeof(dropbox_nocreds.credentials_section), "dropbox"); /* not in the file */
    struct ca_upload_settings nocert = good_settings(), empty = good_settings();
    struct ca_upload_settings nothing = good_settings(), config = good_settings(), nocreds = good_settings();
    under(nocert.ca_file, sizeof(nocert.ca_file), g_dir, "nonexistent.pem");
    empty.folder[0] = '\0';
    nothing.what = 0;
    config.config_path[0] = '\0';
    put_text(nocreds.credentials_section, sizeof(nocreds.credentials_section), "gdrive"); /* not in the file */
    const struct { const struct ca_upload_settings *settings; const char *reason; } cases[] = {
        {&off, "Disabled"}, {&nocert, "CA certificates not found"}, {&empty, "Enter a folder name"},
        {&nothing, "Nothing selected"}, {&config, "credentials file"}, {&nocreds, "No Google Drive credentials"},
        {&dropbox_nocreds, "No Dropbox credentials"},
    };
    for (const auto &c : cases) {
        struct ca_uploader *u = start(*c.settings, MODE_OK);
        ca_uploader_periodic(u, 1000, false);
        usleep(50000);
        ca_uploader_periodic(u, 1000, false);
        const struct ca_upload_status *status = ca_uploader_status(u);
        assert(status->state == CA_UPLOAD_DISABLED);
        assert(has(status->reason, c.reason));
        assert(g_creates == 0 && put_count() == 0);
        ca_uploader_destroy(u);
    }
}

static void full_scan(void)
{
    struct ca_upload_settings s = good_settings();
    s.bandwidth_kib_s = 100;
    struct ca_uploader *u = start(s, MODE_OK);
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    wait_for(u, CA_UPLOAD_WAITING, now);

    /* Folders keep their layout, unfinished files are left, and nothing is deleted. */
    const std::vector<std::string> expected = {"photos/sub|b.jpg", "photos|a.jpg", "recordings|c.mp4"};
    std::vector<std::string> want = expected;
    std::sort(want.begin(), want.end());
    assert(puts_sorted() == want);
    assert(g_creates == 2 && g_bandwidth == 100 && g_root == "cam" && has(g_ca, "ca.pem"));
    assert(exists("capture/a.jpg") && exists("capture/sub/b.jpg") && exists("record/c.mp4"));
    const struct ca_upload_status *status = ca_uploader_status(u);
    assert(status->last_success != 0 && status->last_error[0] == '\0');
    assert(status->files_done == 3 && status->files_total == 3);
    assert(status->bytes_done == 4500 && status->bytes_total == 4500);

    /* Nothing more until the next scan is due, five minutes later. */
    ca_uploader_periodic(u, now + 299999, false);
    assert(g_creates == 2 && ca_uploader_status(u)->state == CA_UPLOAD_WAITING);
    now += 300000;
    ca_uploader_periodic(u, now, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(g_creates == 4);
    ca_uploader_destroy(u);
}

/* The service chosen in the settings is the one the backend is made for, and its credentials section. */
static void chooses_the_service(void)
{
    struct ca_upload_settings s = good_settings();
    struct ca_uploader *u = start(s, MODE_OK);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(g_service == "gdrive" && g_section == "gdrive"); /* the default */
    assert(!strcmp(ca_uploader_status(u)->service, "gdrive"));
    ca_uploader_destroy(u);

    put_text(s.service, sizeof(s.service), "dropbox");
    put_text(s.credentials_section, sizeof(s.credentials_section), "dropbox");
    /* The credentials check looks for the section in the file. */
    FILE *f = fopen(path_of("rclone.conf").c_str(), "a");
    assert(f);
    fputs("[dropbox]\ntype = dropbox\n", f);
    fclose(f);
    u = start(s, MODE_OK);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(g_service == "dropbox" && g_section == "dropbox" && g_creates == 2);
    assert(!strcmp(ca_uploader_status(u)->service, "dropbox")); /* the status names the service in use */
    ca_uploader_destroy(u);
}

/* One message when a batch of work starts (how many files need uploading), one when it is done, and
 * one when it fails: the retries after a failure are silent. */
static void announces_work(void)
{
    struct ca_uploader *u = start(good_settings(), MODE_OK);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(events() == "S3,C"); /* three files: the two photos and the recording */
    ca_uploader_destroy(u);

    u = start(good_settings(), MODE_FAIL);
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_BACKOFF, now);
    assert(events() == "S3,F");
    now += 60000;
    ca_uploader_periodic(u, now, false); /* the retry fails too, and says nothing */
    wait_for(u, CA_UPLOAD_BACKOFF, now);
    now += 60000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_BACKOFF, now);
    assert(events() == "S3,F" && g_creates == 3);
    g_mode = MODE_OK;
    now += 60000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(events() == "S3,F,C"); /* and one message when it finally succeeds */
    ca_uploader_destroy(u);

    /* Stopping for arming is not a failure and starts nothing new: one batch, one message each way. */
    u = start(good_settings(), MODE_SLOW);
    ca_uploader_periodic(u, 1000, false);
    wait_for_put(u, 1000);
    ca_uploader_periodic(u, 1000, true);
    wait_for(u, CA_UPLOAD_WAITING, 1000, true);
    g_mode = MODE_OK;
    ca_uploader_periodic(u, 2000, false);
    wait_for(u, CA_UPLOAD_WAITING, 2000);
    assert(events() == "S3,C");
    ca_uploader_destroy(u);
}

/* Files that are uploaded and unchanged are not looked at again: no network, no message, and the
 * status does not pretend to be uploading. The memory survives a restart. */
static void remembers_uploaded_files(void)
{
    struct ca_upload_settings s = good_settings();
    under(s.cache_path, sizeof(s.cache_path), g_dir, "upload-cache.txt");
    unlink(s.cache_path);
    struct ca_uploader *u = start(s, MODE_OK);
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(events() == "S3,C" && g_creates == 2);

    now += 300000; /* the next scan: nothing is new */
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(g_creates == 2 && events() == "S3,C" && put_count() == 3);
    assert(ca_uploader_status(u)->files_total == 0 && ca_uploader_status(u)->last_error[0] == '\0');

    write_file("capture/a.jpg", 1234); /* a changed file is the only thing sent */
    now += 300000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(events() == "S3,C,S1,C" && put_count() == 4 && g_creates == 3);
    ca_uploader_destroy(u);

    /* A restart remembers it too. */
    g_creates = 0;
    g_events.clear();
    u = ca_uploader_create();
    ca_uploader_set_backend_factory(u, make_fake);
    ca_uploader_set_notify(u, on_event, NULL);
    ca_uploader_configure(u, &s);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(g_creates == 0 && events().empty());
    ca_uploader_destroy(u);

    /* Another folder in the cloud starts afresh. */
    struct ca_upload_settings other = s;
    put_text(other.folder, sizeof(other.folder), "other");
    u = ca_uploader_create();
    ca_uploader_set_backend_factory(u, make_fake);
    ca_uploader_set_notify(u, on_event, NULL);
    ca_uploader_configure(u, &other);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(events() == "S3,C" && g_root == "other");
    ca_uploader_destroy(u);
    unlink(s.cache_path);
}

/* Files uploaded while the setting was "keep" are still on the device. Switching to delete must verify
 * and delete them too, not skip them because they are remembered. */
static void delete_after_keep(void)
{
    struct ca_upload_settings s = good_settings();
    under(s.cache_path, sizeof(s.cache_path), g_dir, "upload-cache-delete.txt");
    unlink(s.cache_path);
    struct ca_uploader *u = start(s, MODE_OK);
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(exists("capture/a.jpg") && exists("capture/sub/b.jpg") && exists("record/c.mp4") && events() == "S3,C");

    s.after_upload = CA_UPLOAD_DELETE;
    ca_uploader_configure(u, &s);
    now += 300000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(!exists("capture/a.jpg") && !exists("capture/sub/b.jpg") && !exists("record/c.mp4"));
    assert(events() == "S3,C,S3,C"); /* all three were looked at again, so they are announced */
    ca_uploader_destroy(u);
    unlink(s.cache_path);
}

/* The memory belongs to the account: other credentials for the same service and folder start afresh. */
static void remembers_per_account(void)
{
    struct ca_upload_settings s = good_settings();
    under(s.cache_path, sizeof(s.cache_path), g_dir, "upload-cache-account.txt");
    under(s.config_path, sizeof(s.config_path), g_dir, "account.conf");
    unlink(s.cache_path);
    auto write_account = [&](const char *token) {
        FILE *f = fopen(path_of("account.conf").c_str(), "w");
        assert(f);
        fprintf(f, "[gdrive]\nclient_id = id\nclient_secret = s\ntoken = {\"refresh_token\":\"%s\"}\n", token);
        fclose(f);
    };
    write_account("one");
    struct ca_uploader *u = start(s, MODE_OK);
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    now += 300000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(events() == "S3,C"); /* the same account: nothing new */
    write_account("two");
    now += 300000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(events() == "S3,C,S3,C"); /* another account: everything is sent to it */
    ca_uploader_destroy(u);
    unlink(s.cache_path);
}

/* Cameras that build the CA certificates into the program have no CA file: the text is passed on instead. */
static void built_in_certificates(void)
{
    static const char roots[] = "-----BEGIN CERTIFICATE-----\nfake\n-----END CERTIFICATE-----\n";
    struct ca_upload_settings s = good_settings();
    s.ca_file[0] = '\0';
    s.ca_pem = roots;
    struct ca_uploader *u = start(s, MODE_OK);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(g_ca_pem == roots && g_creates == 2 && ca_uploader_status(u)->last_error[0] == '\0');
    ca_uploader_destroy(u);
    /* No file and no built-in text: the reason says so. */
    s.ca_pem = NULL;
    u = start(s, MODE_OK);
    ca_uploader_periodic(u, 1000, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_DISABLED && has(ca_uploader_status(u)->reason, "CA certificates"));
    ca_uploader_destroy(u);
}

/* "Upload now": the next scan starts without waiting for the interval. */
static void scan_now(void)
{
    struct ca_uploader *u = start(good_settings(), MODE_OK);
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(g_creates == 2);
    now += 1000; /* the next scan is minutes away */
    ca_uploader_periodic(u, now, false);
    assert(g_creates == 2 && ca_uploader_status(u)->state == CA_UPLOAD_WAITING);
    ca_uploader_scan_now(u, now);
    ca_uploader_periodic(u, now, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(g_creates == 4);
    ca_uploader_destroy(u);
}

/* "Upload now" runs while armed, and while recording; the scan it starts is not stopped by arming, and the
 * next one waits for the vehicle to be disarmed again. */
/* The camera says it is recording to these files (some cameras write two). */
static void record_to(struct ca_uploader *u, std::vector<std::string> names)
{
    std::vector<std::string> paths;
    std::vector<const char *> pointers;
    for (const std::string &name : names) paths.push_back(path_of(name.c_str()));
    for (const std::string &path : paths) pointers.push_back(path.c_str());
    ca_uploader_set_recording(u, true, pointers.data(), (unsigned)pointers.size());
}

static void scan_now_overrides_armed(void)
{
    struct ca_uploader *u = start(good_settings(), MODE_SLOW);
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, true); /* armed: nothing starts by itself */
    assert(ca_uploader_status(u)->state == CA_UPLOAD_WAITING && g_creates == 0);
    ca_uploader_set_recording(u, true, NULL, 0);
    ca_uploader_scan_now(u, now);
    ca_uploader_periodic(u, now, true);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    wait_for_put(u, now);
    ca_uploader_periodic(u, now + 10, true); /* still armed, and it keeps going */
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    g_release = true;
    wait_for(u, CA_UPLOAD_WAITING, now, true);
    assert(g_creates == 2 && ca_uploader_status(u)->last_success != 0);
    /* The override is used up: the next scan, due again, does not start while armed. */
    ca_uploader_periodic(u, now + 600000, true);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_WAITING && g_creates == 2);
    ca_uploader_destroy(u);
}

static void gate(void)
{
    struct ca_uploader *u = start(good_settings(), MODE_OK);
    uint64_t now = 1000;
    /* Armed: nothing runs. */
    ca_uploader_periodic(u, now, true);
    usleep(20000);
    ca_uploader_periodic(u, now, true);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_WAITING && g_creates == 0);
    wait_for(u, CA_UPLOAD_WAITING, now); /* gate open: the scan runs and ends */
    assert(g_creates == 2);

    /* The scan is not due for five minutes, but a recording ending looks again, after a moment. */
    ca_uploader_set_recording(u, true, NULL, 0);
    ca_uploader_periodic(u, now + 1000, false);
    ca_uploader_set_recording(u, false, NULL, 0);
    ca_uploader_periodic(u, now + 2000, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_WAITING);
    ca_uploader_periodic(u, now + 12000, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    wait_for(u, CA_UPLOAD_WAITING, now + 12000);
    assert(g_creates == 4);
    /* So does disarming. */
    ca_uploader_periodic(u, now + 3000, true);
    ca_uploader_periodic(u, now + 4000, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    wait_for(u, CA_UPLOAD_WAITING, now + 4000);
    assert(g_creates == 6);
    ca_uploader_destroy(u);
}

/* Changing the service while a job runs does not change what the status says it is sending to. */
static void service_named_is_the_running_one(void)
{
    struct ca_upload_settings s = good_settings();
    snprintf(s.service, sizeof(s.service), "gdrive");
    struct ca_uploader *u = start(s, MODE_SLOW);
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false);
    wait_for_put(u, now);
    assert(!strcmp(ca_uploader_status(u)->service, "gdrive"));
    snprintf(s.service, sizeof(s.service), "dropbox");
    ca_uploader_configure(u, &s);
    ca_uploader_periodic(u, now, false);
    assert(!strcmp(ca_uploader_status(u)->service, "gdrive")); /* the job is still sending to Drive */
    g_release = true;
    wait_for(u, CA_UPLOAD_WAITING, now);
    ca_uploader_periodic(u, now + 1, false);
    assert(!strcmp(ca_uploader_status(u)->service, "dropbox"));
    ca_uploader_destroy(u);
}

/* A scan sends everything to the service it started with: the second job does not go to a service set meanwhile. */
static void one_scan_one_service(void)
{
    struct ca_upload_settings s = good_settings();
    snprintf(s.service, sizeof(s.service), "gdrive");
    struct ca_uploader *u = start(s, MODE_SLOW);
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false);
    wait_for_put(u, now); /* the photos job */
    snprintf(s.service, sizeof(s.service), "dropbox");
    ca_uploader_configure(u, &s);
    g_release = true;
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(g_creates == 2); /* both jobs ran */
    {
        std::lock_guard<std::mutex> guard(g_lock);
        assert(g_service == "gdrive");
    }
    ca_uploader_destroy(u);
}

static void stops_when_armed(void)
{
    struct ca_uploader *u = start(good_settings(), MODE_SLOW);
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false);
    wait_for_put(u, now);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING && ca_uploader_status(u)->bytes_total > 0);

    ca_uploader_periodic(u, now, true); /* armed */
    const enum ca_upload_state state = ca_uploader_status(u)->state;
    assert(state == CA_UPLOAD_STOPPING || state == CA_UPLOAD_WAITING);
    wait_for(u, CA_UPLOAD_WAITING, now, true);
    /* Stopping on request is not a failure, and nothing restarts while armed. */
    assert(ca_uploader_status(u)->last_error[0] == '\0' && ca_uploader_status(u)->last_success == 0);
    assert(g_in_put == 0);
    ca_uploader_periodic(u, now + 600000, true);
    assert(g_creates == 1);

    /* Disarmed: the interrupted work starts again. */
    ca_uploader_periodic(u, now + 600001, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    wait_for_put(u, now + 600001);
    assert(g_creates == 2);
    ca_uploader_destroy(u);
}

/* Recording does not stop an upload that is running, or keep one from starting. */
static void recording_does_not_stop_it(void)
{
    struct ca_uploader *u = start(good_settings(), MODE_SLOW);
    uint64_t now = 1000;
    record_to(u, {"record/c.mp4"});
    ca_uploader_periodic(u, now, false); /* it starts while recording */
    wait_for_put(u, now);
    ca_uploader_set_recording(u, false, NULL, 0);
    ca_uploader_periodic(u, now, false);
    ca_uploader_set_recording(u, true, NULL, 0);
    ca_uploader_periodic(u, now, false); /* and a recording starting meanwhile does not stop it */
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    g_release = true;
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(ca_uploader_status(u)->last_error[0] == '\0' && ca_uploader_status(u)->last_success != 0);
    ca_uploader_destroy(u);
}

/* The file the camera is recording to is not sent, however old it looks; once the recording is over it is. */
static void skips_the_file_being_recorded(void)
{
    struct ca_uploader *u = start(good_settings(), MODE_OK);
    uint64_t now = 1000;
    record_to(u, {"record/c.mp4"});
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert((puts_sorted() == std::vector<std::string>{"photos/sub|b.jpg", "photos|a.jpg"}));
    /* Still recording: it is not looked at again until the next scan is due. */
    ca_uploader_periodic(u, now + 20000, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_WAITING && g_creates == 1);
    /* The recording ends: soon, not after the whole interval, the file is sent. */
    ca_uploader_set_recording(u, false, NULL, 0);
    ca_uploader_periodic(u, now + 21000, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_WAITING);
    ca_uploader_periodic(u, now + 31000, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    wait_for(u, CA_UPLOAD_WAITING, now + 31000);
    {
        const std::vector<std::string> sent = puts_sorted(); /* nothing is remembered here, so the photos go again */
        assert(std::find(sent.begin(), sent.end(), "recordings|c.mp4") != sent.end());
    }
    ca_uploader_destroy(u);
}

/* Some cameras record three files at once (the thermal view, and its raw pixels): none is sent while it is open,
 * even if one of them has been quiet for a while. */
static void skips_every_file_being_recorded(void)
{
    struct ca_uploader *u = start(good_settings(), MODE_OK);
    write_file("record/c_I.mp4", 2000);
    write_file("record/c.raw.mkv", 4000);
    uint64_t now = 1000;
    record_to(u, {"record/c.mp4", "record/c_I.mp4", "record/c.raw.mkv"});
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert((puts_sorted() == std::vector<std::string>{"photos/sub|b.jpg", "photos|a.jpg"}));
    ca_uploader_destroy(u);
}

/* A recording on FAT continues in <stem>_part0002<extension>: the camera does not report it, but it is still being
 * written, so it is left alone while the recording it continues is open (and sent when that is over). */
static void skips_the_continuation_of_a_recording(void)
{
    struct ca_upload_settings s = good_settings();
    s.after_upload = CA_UPLOAD_DELETE;
    struct ca_uploader *u = start(s, MODE_OK);
    write_file("record/c_part0002.mp4", 2000);
    write_file("record/cc_part0002.mp4", 1500); /* another recording's: sent */
    write_file("record/c_part.mp4", 1200);      /* not a continuation: sent */
    uint64_t now = 1000;
    record_to(u, {"record/c.mp4"});
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(exists("record/c.mp4") && exists("record/c_part0002.mp4")); /* kept: still being recorded */
    assert(!exists("record/cc_part0002.mp4") && !exists("record/c_part.mp4"));
    ca_uploader_set_recording(u, false, NULL, 0);
    ca_uploader_scan_now(u, now);
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(!exists("record/c.mp4") && !exists("record/c_part0002.mp4")); /* the recording is over: sent */
    ca_uploader_destroy(u);
}

/* A recording can start to a file after the folders were listed: the file is looked at again before it is sent, and
 * before it is deleted, and is left when it is being recorded to. */
static void skips_a_file_recorded_to_after_the_listing(void)
{
    struct ca_upload_settings s = good_settings();
    s.after_upload = CA_UPLOAD_DELETE;
    struct ca_uploader *u = start(s, MODE_SLOW);
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false);
    wait_for_put(u, now); /* the first photo is being sent: both were listed, neither was being recorded to */
    const bool a_first = puts_sorted().size() == 1 && puts_sorted()[0] == "photos|a.jpg";
    const char *other = a_first ? "capture/sub/b.jpg" : "capture/a.jpg";
    record_to(u, {other});
    g_release = true;
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(exists(other));                                             /* not sent, and not deleted */
    assert(!exists(a_first ? "capture/a.jpg" : "capture/sub/b.jpg"));  /* the one already being sent was */
    const std::vector<std::string> sent = puts_sorted();
    assert(std::count(sent.begin(), sent.end(), a_first ? "photos/sub|b.jpg" : "photos|a.jpg") == 0);
    ca_uploader_destroy(u);
}

/* A recording that starts to the file that is being sent: it is not deleted afterwards. */
static void keeps_a_file_recorded_to_while_it_is_sent(void)
{
    struct ca_upload_settings s = good_settings();
    s.after_upload = CA_UPLOAD_DELETE;
    struct ca_uploader *u = start(s, MODE_SLOW);
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false);
    wait_for_put(u, now);
    const bool a_first = puts_sorted().size() == 1 && puts_sorted()[0] == "photos|a.jpg";
    const char *sending = a_first ? "capture/a.jpg" : "capture/sub/b.jpg";
    record_to(u, {sending});
    g_release = true;
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(exists(sending));
    assert(!exists(a_first ? "capture/sub/b.jpg" : "capture/a.jpg")); /* the other photo was sent and deleted */
    ca_uploader_destroy(u);
}

/* A file changed a moment ago may still be being written, whatever the camera says: it is left, and looked at
 * again soon. */
static void skips_a_file_changed_just_now(void)
{
    struct ca_uploader *u = start(good_settings(), MODE_OK);
    write_file_now("record/d.mp4", 100);
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert((puts_sorted() == std::vector<std::string>{"photos/sub|b.jpg", "photos|a.jpg", "recordings|c.mp4"}));
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    const struct timespec old[2] = {{t.tv_sec - 60, 0}, {t.tv_sec - 60, 0}};
    assert(utimensat(AT_FDCWD, path_of("record/d.mp4").c_str(), old, 0) == 0);
    ca_uploader_periodic(u, now + 9000, false);
    assert(g_creates == 2); /* not yet: ten seconds */
    ca_uploader_periodic(u, now + 10000, false);
    wait_for(u, CA_UPLOAD_WAITING, now + 10000);
    {
        const std::vector<std::string> sent = puts_sorted();
        assert(std::find(sent.begin(), sent.end(), "recordings|d.mp4") != sent.end());
    }
    ca_uploader_destroy(u);
}

/* A worker that is slow to notice the stop keeps the state at STOPPING, and the
 * gate reopening meanwhile does not cancel the stop. */
static void waits_for_a_slow_stop(void)
{
    struct ca_uploader *u = start(good_settings(), MODE_STUBBORN);
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false);
    wait_for_put(u, now);
    ca_uploader_periodic(u, now, true); /* armed */
    assert(ca_uploader_status(u)->state == CA_UPLOAD_STOPPING);
    ca_uploader_periodic(u, now + 100, false); /* disarmed again, while it is still stopping */
    assert(ca_uploader_status(u)->state == CA_UPLOAD_STOPPING);
    g_mode = MODE_OK; /* the gate is open, so the scan starts again at once: let it finish */
    wait_for(u, CA_UPLOAD_WAITING, now + 200); /* the stop ended with no failure */
    assert(ca_uploader_status(u)->last_error[0] == '\0' && ca_uploader_status(u)->last_success != 0);
    ca_uploader_destroy(u);
}

static void backoff(void)
{
    struct ca_uploader *u = start(good_settings(), MODE_FAIL);
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_BACKOFF, now);
    assert(g_creates == 1);
    assert(has(ca_uploader_status(u)->last_error, "quota \"exceeded\""));
    assert(ca_uploader_status(u)->last_success == 0);
    assert(exists("capture/a.jpg")); /* nothing is deleted without a confirmed upload */

    /* A minute, every time. */
    ca_uploader_periodic(u, now + 59999, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_BACKOFF && g_creates == 1);
    now += 60000;
    ca_uploader_periodic(u, now, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    wait_for(u, CA_UPLOAD_BACKOFF, now);
    assert(g_creates == 2);
    ca_uploader_periodic(u, now + 59999, false);
    assert(g_creates == 2);
    now += 60000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_BACKOFF, now);
    ca_uploader_periodic(u, now + 59999, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_BACKOFF && g_creates == 3); /* a minute, however often */
    now += 60000;

    /* A success clears the error and the wait. */
    g_mode = MODE_OK;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(ca_uploader_status(u)->last_error[0] == '\0' && ca_uploader_status(u)->last_success != 0);
    const unsigned before = g_creates;
    g_mode = MODE_FAIL;
    now += 300000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_BACKOFF, now);
    ca_uploader_periodic(u, now + 60000, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    wait_for(u, CA_UPLOAD_BACKOFF, now + 60000);
    assert(g_creates == before + 2);
    ca_uploader_destroy(u);
}

/* One bad file does not stop the others, but the scan still counts as failed. */
static void one_bad_file(void)
{
    struct ca_upload_settings s = good_settings();
    s.after_upload = CA_UPLOAD_DELETE;
    struct ca_uploader *u = start(s, MODE_FAIL_ONE);
    write_file("capture/bad.jpg", 700);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_BACKOFF, 1000);
    assert(has(ca_uploader_status(u)->last_error, "bad file"));
    assert(exists("capture/bad.jpg"));                             /* kept: it was not uploaded */
    assert(!exists("capture/a.jpg") && !exists("capture/sub/b.jpg")); /* the others were */
    ca_uploader_destroy(u);
}

/* A file that cannot be sent is skipped: the rest of the photos and the recordings are still sent, the scan
 * completes, and the status says what was skipped. */
static void rejected_file_is_skipped(void)
{
    struct ca_upload_settings s = good_settings();
    s.after_upload = CA_UPLOAD_DELETE;
    struct ca_uploader *u = start(s, MODE_REJECT_ONE);
    write_file("capture/bad.jpg", 700);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(ca_uploader_status(u)->last_success != 0);
    assert(has(ca_uploader_status(u)->last_error, "1 file(s) could not be uploaded: cannot read bad.jpg"));
    assert(exists("capture/bad.jpg"));                                 /* kept */
    assert(!exists("capture/a.jpg") && !exists("capture/sub/b.jpg"));  /* the others were sent */
    assert(!exists("record/c.mp4"));                                   /* and the recordings job ran */
    assert(ca_uploader_status(u)->files_done == 3);
    ca_uploader_destroy(u);
}

/* A file that was deleted after the listing is not a problem with the file: it is not reported. */
static void a_deleted_file_is_not_reported(void)
{
    struct ca_uploader *u = start(good_settings(), MODE_REJECT_GONE);
    write_file("capture/bad.jpg", 700);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(ca_uploader_status(u)->last_success != 0 && ca_uploader_status(u)->last_error[0] == '\0');
    assert(ca_uploader_status(u)->files_done == 3 && !exists("capture/bad.jpg"));
    ca_uploader_destroy(u);
}

/* A scan that failed leaves its series open; if the next one goes to another service it announces its own start,
 * so the ground station does not read "started" for one service and "complete" for another. */
static void a_new_service_starts_a_new_series(void)
{
    struct ca_upload_settings s = good_settings();
    struct ca_uploader *u = start(s, MODE_FAIL);
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_BACKOFF, now);
    assert(events() == "S3,F" && !strcmp(ca_uploader_status(u)->service, "gdrive"));
    put_text(s.service, sizeof(s.service), "dropbox");
    ca_uploader_configure(u, &s);
    g_mode = MODE_OK;
    now += 60000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(events() == "S3,F,S3,C" && !strcmp(ca_uploader_status(u)->service, "dropbox"));
    ca_uploader_destroy(u);
}

/* A file that can never be sent is not announced again and again: the ground station hears about a scan only when
 * there is something to upload. The page keeps saying it is there, until it is gone or changed, or "Upload now". */
static void a_file_that_cannot_be_sent_is_announced_once(void)
{
    struct ca_upload_settings s = good_settings();
    under(s.cache_path, sizeof(s.cache_path), g_dir, "upload-cache-unsendable.txt"); /* what was sent is remembered */
    unlink(s.cache_path);
    struct ca_uploader *u = start(s, MODE_REJECT_ONE);
    write_file("capture/bad.jpg", 700);
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(events() == "S4,C" && g_creates == 2); /* a job for the photos, and one for the recordings */
    assert(has(ca_uploader_status(u)->last_error, "1 file(s) could not be uploaded: cannot read bad.jpg"));
    now += 6 * 60000; /* the next scan: nothing new, so nothing is said, and the page still shows the problem */
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(events() == "S4,C"); /* and nothing more is announced */
    assert(has(ca_uploader_status(u)->last_error, "1 file(s) could not be uploaded: cannot read bad.jpg"));
    assert(ca_uploader_status(u)->files_total == 0);
    ca_uploader_scan_now(u, now); /* "Upload now" tries it again */
    ca_uploader_periodic(u, now, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(events() == "S4,C,S1,C");
    ca_uploader_destroy(u);
    unlink(s.cache_path);
}

/* An unfinished file that has not changed for a long time was left behind by a crash: it is not sent, the scan
 * still completes, and the status asks the user to delete it. A recent one is only being written. */
static void warns_about_old_unfinished_files(void)
{
    struct ca_uploader *u = start(good_settings(), MODE_OK);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(ca_uploader_status(u)->last_error[0] == '\0'); /* the 60 s old capture/c.part is not old enough */
    ca_uploader_destroy(u);

    u = start(good_settings(), MODE_OK);
    write_file("capture/old.part", 10);
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    const struct timespec hour_ago[2] = {{t.tv_sec - 3600, 0}, {t.tv_sec - 3600, 0}};
    assert(utimensat(AT_FDCWD, path_of("capture/old.part").c_str(), hour_ago, 0) == 0);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(ca_uploader_status(u)->last_success != 0);
    assert(has(ca_uploader_status(u)->last_error, "1 unfinished file(s)") && has(ca_uploader_status(u)->last_error, "delete them"));
    const std::vector<std::string> sent = puts_sorted();
    assert(std::find(sent.begin(), sent.end(), "photos|old.part") == sent.end()); /* never sent */
    ca_uploader_destroy(u);
}

/* A file the backend leaves alone is not deleted after the upload, and is not a failure. */
static void left_alone(void)
{
    struct ca_upload_settings s = good_settings();
    s.after_upload = CA_UPLOAD_DELETE;
    struct ca_uploader *u = start(s, MODE_LEFT_ONE);
    write_file("capture/keep.jpg", 0);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(!ca_uploader_status(u)->last_error[0]);
    assert(exists("capture/keep.jpg"));                              /* kept */
    assert(!exists("capture/a.jpg") && !exists("capture/sub/b.jpg")); /* the others were deleted */
    ca_uploader_destroy(u);
}

/* Deleting never takes a file that has just been created or changed: a recording that has started exists,
 * empty, a moment before the uploader hears about it. */
static void delete_spares_new_files(void)
{
    struct ca_upload_settings s = good_settings();
    s.after_upload = CA_UPLOAD_DELETE;
    s.what = CA_UPLOAD_PHOTOS;
    struct ca_uploader *u = start(s, MODE_OK);
    write_file_now("capture/new.jpg", 0);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(!ca_uploader_status(u)->last_error[0]);
    assert(exists("capture/new.jpg"));  /* neither uploaded nor deleted */
    assert(!exists("capture/a.jpg")); /* the finished ones were */
    ca_uploader_destroy(u);
}

/* A file that grows while it is sent is kept even though it was sent and checked. */
static void delete_spares_growing_file(void)
{
    struct ca_upload_settings s = good_settings();
    s.after_upload = CA_UPLOAD_DELETE;
    struct ca_uploader *u = start(s, MODE_GROW_ONE);
    write_file("capture/grow.mp4", 100);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(exists("capture/grow.mp4"));
    assert(!exists("capture/a.jpg") && !exists("capture/sub/b.jpg"));
    ca_uploader_destroy(u);
}

/* A folder that cannot be examined is skipped, and reported, not a reason to fail the scan. */
static void unreadable_root_is_reported(void)
{
    if (geteuid() == 0) return; /* root can read it anyway */
    struct ca_upload_settings s = good_settings();
    struct ca_uploader *u = start(s, MODE_OK);
    const std::string locked = std::string(g_dir) + "/locked";
    assert(mkdir(locked.c_str(), 0755) == 0 || errno == EEXIST);
    assert(mkdir((locked + "/capture").c_str(), 0755) == 0 || errno == EEXIST);
    snprintf(s.capture_root, sizeof(s.capture_root), "%s/capture", locked.c_str());
    ca_uploader_configure(u, &s);
    assert(chmod(locked.c_str(), 0) == 0);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(has(ca_uploader_status(u)->last_error, "could not be uploaded: they could not be examined"));
    assert(ca_uploader_status(u)->last_success != 0);
    assert(chmod(locked.c_str(), 0755) == 0);
    ca_uploader_destroy(u);
}

static void status_text(void)
{
    struct ca_upload_status status = {};
    status.state = CA_UPLOAD_RUNNING;
    snprintf(status.service, sizeof(status.service), "dropbox");
    status.last_success = 1730000000;
    status.files_done = 1;
    status.files_total = 4;
    status.bytes_done = 2048;
    status.bytes_total = 8192;
    snprintf(status.last_error, sizeof(status.last_error), "quota exceeded");
    char text[512];
    assert(ca_uploader_format_status(&status, text, sizeof(text)) > 0);
    assert(!strcmp(text, "state=running\nservice=dropbox\nreason=\nlast_success=1730000000\nfiles_done=1\nfiles_total=4\n"
                         "bytes_done=2048\nbytes_total=8192\nlast_error=quota exceeded\n"));
    assert(ca_uploader_format_status(&status, text, 20) == 0); /* too small */
}

static void backend_cannot_start(void)
{
    /* A backend that cannot be made (for example, unusable credentials) is a failure, not a crash. */
    struct ca_uploader *u = start(good_settings(), MODE_NO_SERVICE);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_BACKOFF, 1000);
    assert(has(ca_uploader_status(u)->last_error, "didn't find section in config file"));
    ca_uploader_destroy(u);
}

static void source_folders(void)
{
    struct ca_upload_settings s = good_settings();

    /* A folder that does not exist is skipped, not an error. */
    struct ca_uploader *u = start(s, MODE_OK);
    assert(system(("rm -rf '" + path_of("record") + "'").c_str()) == 0);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(g_creates == 1 && puts_sorted().size() == 2 && has(puts_sorted()[0], "photos"));
    assert(ca_uploader_status(u)->last_error[0] == '\0');
    ca_uploader_destroy(u);

    u = start(s, MODE_OK);
    assert(system(("rm -rf '" + path_of("capture") + "' '" + path_of("record") + "'").c_str()) == 0);
    ca_uploader_periodic(u, 1000, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_WAITING && ca_uploader_status(u)->last_success != 0);
    assert(g_creates == 0);
    ca_uploader_destroy(u);

    /* The choice of what to upload. */
    s.what = CA_UPLOAD_RECORDINGS;
    u = start(s, MODE_OK);
    ca_uploader_periodic(u, 1000, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(g_creates == 1 && puts_sorted() == std::vector<std::string>{"recordings|c.mp4"});
    ca_uploader_destroy(u);
}

/* Settings saved while a scan runs: a queued job whose kind is no longer wanted
 * must not start (with delete_after it would move that whole folder). */
static void settings_change_during_a_scan(void)
{
    struct ca_upload_settings s = good_settings();
    s.after_upload = CA_UPLOAD_DELETE;
    struct ca_uploader *u = start(s, MODE_SLOW);
    ca_uploader_periodic(u, 1000, false); /* the photos job starts, and waits in put() */
    wait_for_put(u, 1000);
    s.what = CA_UPLOAD_PHOTOS; /* recordings no longer wanted */
    ca_uploader_configure(u, &s);
    g_release = true;
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(g_creates == 1);
    assert(!exists("capture/a.jpg") && exists("record/c.mp4"));
    ca_uploader_destroy(u);

    /* Delete changed to keep while the photos job runs: the queued recordings job keeps its files. */
    s = good_settings();
    s.after_upload = CA_UPLOAD_DELETE;
    u = start(s, MODE_SLOW);
    ca_uploader_periodic(u, 1000, false);
    wait_for_put(u, 1000);
    s.after_upload = CA_UPLOAD_KEEP;
    ca_uploader_configure(u, &s);
    g_release = true;
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(g_creates == 2 && !exists("capture/a.jpg") && exists("record/c.mp4"));
    ca_uploader_destroy(u);

    /* The other way round: the queued recordings job is still wanted and runs. */
    s = good_settings();
    u = start(s, MODE_SLOW);
    ca_uploader_periodic(u, 1000, false);
    wait_for_put(u, 1000);
    s.what = CA_UPLOAD_RECORDINGS;
    ca_uploader_configure(u, &s);
    g_release = true;
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(g_creates == 2 && puts_sorted().back() == "recordings|c.mp4");
    ca_uploader_destroy(u);
}

static void after_upload(void)
{
    const struct { enum ca_upload_after mode; unsigned percent; int mode_of_fake; bool deleted; } cases[] = {
        {CA_UPLOAD_KEEP, 20, MODE_OK, false},
        {CA_UPLOAD_DELETE, 20, MODE_OK, true},
        {CA_UPLOAD_DELETE, 20, MODE_SKIPPED, true}, /* already there with the same content: safe to delete */
        {CA_UPLOAD_DELETE, 20, MODE_FAIL, false},   /* never deleted without a confirmed upload */
        {CA_UPLOAD_DELETE_WHEN_LOW, 100, MODE_OK, true}, /* free space is below 100% */
        {CA_UPLOAD_DELETE_WHEN_LOW, 0, MODE_OK, false},  /* never below 0% */
    };
    for (const auto &c : cases) {
        struct ca_upload_settings s = good_settings();
        s.after_upload = c.mode;
        s.low_space_percent = c.percent;
        struct ca_uploader *u = start(s, c.mode_of_fake);
        ca_uploader_periodic(u, 1000, false);
        wait_for(u, c.mode_of_fake == MODE_FAIL ? CA_UPLOAD_BACKOFF : CA_UPLOAD_WAITING, 1000);
        assert(exists("capture/a.jpg") != c.deleted);
        assert(exists("capture/sub/b.jpg") != c.deleted);
        assert(c.mode_of_fake == MODE_FAIL || exists("record/c.mp4") != c.deleted);
        assert(exists("capture/c.part") && exists("capture/sub/d.tmp")); /* unfinished files stay */
        ca_uploader_destroy(u);
    }
}

static void destroy_and_disable_stop_the_worker(void)
{
    struct ca_uploader *u = start(good_settings(), MODE_SLOW);
    ca_uploader_periodic(u, 1000, false);
    wait_for_put(u, 1000);
    ca_uploader_destroy(u); /* joins the worker, which sees the stop */
    assert(g_in_put == 0);

    u = start(good_settings(), MODE_SLOW);
    ca_uploader_periodic(u, 1000, false);
    wait_for_put(u, 1000);
    struct ca_upload_settings off = good_settings();
    off.enable = false;
    ca_uploader_configure(u, &off);
    wait_for(u, CA_UPLOAD_DISABLED, 1000);
    assert(g_in_put == 0);
    /* Turned on again, it starts at once. */
    struct ca_upload_settings on = good_settings();
    ca_uploader_configure(u, &on);
    ca_uploader_periodic(u, 2000, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    ca_uploader_destroy(u);
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    char pattern[PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s/uploader-test-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    assert(mkdtemp(pattern));
    put_text(g_dir, sizeof(g_dir), pattern);
    /* The credentials file has a [gdrive] section only for the case that asks for it to be checked. */
    FILE *f = fopen(path_of("ca.pem").c_str(), "w");
    assert(f);
    fputs("not really a certificate\n", f);
    fclose(f);
    f = fopen(path_of("rclone.conf").c_str(), "w");
    assert(f);
    fputs("[other]\ntype = drive\n", f);
    fclose(f);

    unusable_settings();
    full_scan();
    chooses_the_service();
    built_in_certificates();
    announces_work();
    remembers_uploaded_files();
    delete_after_keep();
    remembers_per_account();
    scan_now();
    gate();
    stops_when_armed();
    recording_does_not_stop_it();
    skips_the_file_being_recorded();
    skips_every_file_being_recorded();
    skips_the_continuation_of_a_recording();
    skips_a_file_recorded_to_after_the_listing();
    keeps_a_file_recorded_to_while_it_is_sent();
    a_deleted_file_is_not_reported();
    a_file_that_cannot_be_sent_is_announced_once();
    a_new_service_starts_a_new_series();
    skips_a_file_changed_just_now();
    scan_now_overrides_armed();
    service_named_is_the_running_one();
    one_scan_one_service();
    waits_for_a_slow_stop();
    backoff();
    one_bad_file();
    left_alone();
    delete_spares_new_files();
    delete_spares_growing_file();
    unreadable_root_is_reported();
    rejected_file_is_skipped();
    warns_about_old_unfinished_files();
    status_text();
    backend_cannot_start();
    source_folders();
    settings_change_during_a_scan();
    after_upload();
    destroy_and_disable_stop_the_worker();

    char command[PATH_MAX + 32];
    snprintf(command, sizeof(command), "rm -rf '%s'", g_dir);
    if (system(command) != 0) return 1;
    printf("PASS uploader: gate, stop, backoff, source folders, copy/move and cleanup\n");
    return 0;
}
