/* Exercises the Dropbox backend against tests/test_dropbox.py's fake Dropbox.
 * Usage: test_dropbox <port> <ca.pem> <scratch dir> */
#include "camera_app/https.h"
#include "camera_app/upload_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static unsigned g_port;
static const char *g_ca;
static std::string g_dir;

static double now_s()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static std::string control(const char *query)
{
    char error[200], body[16384];
    ca_https_options o = {};
    o.ca_file = g_ca;
    ca_https *c = ca_https_connect("localhost", g_port, &o, error, sizeof(error));
    if (!c) return "";
    char path[256];
    snprintf(path, sizeof(path), "/__control?%s", query);
    ca_https_req q = {};
    q.method = "GET"; q.path = path; q.fd = -1;
    ca_https_response r = {};
    r.body = body; r.body_capacity = sizeof(body);
    ca_https_request(c, &q, &r, error, sizeof(error));
    ca_https_close(c);
    return body;
}

static void write_file(const std::string &path, const std::string &data)
{
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

/* An independent computation of Dropbox's content hash (python's hashlib). */
static std::string dbx_hash(const std::string &path)
{
    const std::string command =
        "python3 -c \"import hashlib,sys;d=open(sys.argv[1],'rb').read();B=4<<20;"
        "print(hashlib.sha256(b''.join(hashlib.sha256(d[i:i+B]).digest() for i in range(0,len(d),B))).hexdigest())\" '" + path + "'";
    FILE *p = popen(command.c_str(), "r");
    char hex[80] = "";
    if (p) { if (fscanf(p, "%64s", hex) != 1) hex[0] = 0; pclose(p); }
    return hex;
}

static ca_upload_target target(const std::atomic<bool> *stop, const char *credentials)
{
    ca_upload_target t = {};
    t.service = "dropbox";
    t.credentials_path = credentials;
    t.credentials_section = "dropbox";
    t.root_folder = "AP";
    t.ca_file = g_ca;
    t.stop = stop;
    t.api_host = "localhost";
    t.api_port = g_port;
    return t;
}

struct Seen { uint64_t last = 0; bool backwards = false; };
static void on_progress(void *user, uint64_t sent)
{
    Seen *s = (Seen *)user;
    if (sent < s->last) s->backwards = true;
    s->last = sent;
}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    g_port = (unsigned)atoi(argv[1]);
    g_ca = argv[2];
    g_dir = argv[3];
    char error[200] = "";
    std::atomic<bool> stop(false);

    const std::string credentials = g_dir + "/rclone.conf", bad_credentials = g_dir + "/bad.conf";
    write_file(credentials, "[gdrive]\nclient_id = google\n\n[dropbox]\ntype = dropbox\nclient_id = appkey\nclient_secret = app/secret+=\n"
               "token = {\"access_token\":\"old\",\"token_type\":\"bearer\",\"refresh_token\":\"refresh-tok\",\"expiry\":\"2020-01-01T00:00:00Z\"}\n");
    write_file(bad_credentials, "[dropbox]\ntype = dropbox\nclient_id = bad\nclient_secret = app/secret+=\n"
               "token = {\"refresh_token\":\"refresh-tok\"}\n");

    /* no credentials, no backend */
    ca_upload_target t = target(&stop, "/nonexistent/rclone.conf");
    CHECK(ca_upload_backend_create(&t, error, sizeof(error)) == NULL && strstr(error, "Dropbox credentials"));
    t = target(&stop, credentials.c_str());
    t.credentials_section = "other";
    CHECK(ca_upload_backend_create(&t, error, sizeof(error)) == NULL);
    t.credentials_section = "dropbox";
    APC_UploadBackend *dbx = ca_upload_backend_create(&t, error, sizeof(error));
    if (!dbx) { fprintf(stderr, "FAIL create: %s\n", error); return 1; }

    /* a new file, the same file again, then a changed one */
    const std::string photo = g_dir + "/a.jpg";
    write_file(photo, std::string(70000, 'a'));
    Seen seen;
    CHECK(dbx->put("photos", "a.jpg", photo.c_str(), on_progress, &seen, error, sizeof(error)) == CA_PUT_UPLOADED);
    CHECK(seen.last == 70000 && !seen.backwards);
    CHECK(control("state=1") == "/AP/photos/a.jpg|70000|" + dbx_hash(photo));
    CHECK(dbx->put("photos", "a.jpg", photo.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_SKIPPED);
    write_file(photo, std::string(5000, 'b'));
    CHECK(dbx->put("photos", "a.jpg", photo.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED);
    CHECK(control("state=1") == "/AP/photos/a.jpg|5000|" + dbx_hash(photo)); /* replaced, not duplicated */

    /* nested folders, names that need escaping (quotes, a non-ASCII letter), and an empty file */
    const std::string odd = g_dir + "/caf\xc3\xa9 \"x\".mp4";
    write_file(odd, "video");
    CHECK(dbx->put("recordings/2026/day 1", "caf\xc3\xa9 \"x\".mp4", odd.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED);
    CHECK(dbx->put("recordings/2026/day 1", "caf\xc3\xa9 \"x\".mp4", odd.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_SKIPPED);
    const std::string empty = g_dir + "/empty.txt";
    write_file(empty, "");
    CHECK(dbx->put("", "empty.txt", empty.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED);
    CHECK(dbx->put("", "empty.txt", empty.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_SKIPPED);
    const std::string listing = control("state=1");
    CHECK(listing.find("/AP/recordings/2026/day 1/caf\xc3\xa9 \"x\".mp4|5|") != std::string::npos);
    CHECK(listing.find("/AP/empty.txt|0|") != std::string::npos);

    /* a long token (the web page accepts 4096 characters) keeps its refresh token, which follows a long
     * access token; one too long to hold is refused rather than cut short */
    {
        const std::string long_conf = g_dir + "/long.conf", huge_conf = g_dir + "/huge.conf";
        write_file(long_conf, "[dropbox]\nclient_id = appkey\nclient_secret = app/secret+=\ntoken = {\"access_token\":\"" + std::string(3500, 'a') +
                   "\",\"token_type\":\"bearer\",\"refresh_token\":\"refresh-tok\"}\n");
        write_file(huge_conf, "[dropbox]\nclient_id = appkey\nclient_secret = app/secret+=\ntoken = {\"access_token\":\"" + std::string(5000, 'a') +
                   "\",\"refresh_token\":\"refresh-tok\"}\n");
        ca_upload_target lt = target(&stop, long_conf.c_str());
        APC_UploadBackend *longtoken = ca_upload_backend_create(&lt, error, sizeof(error));
        CHECK(longtoken != NULL);
        if (longtoken) {
            const std::string f = g_dir + "/longtoken.jpg";
            write_file(f, "long token works");
            CHECK(longtoken->put("photos", "longtoken.jpg", f.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED);
            delete longtoken;
        }
        ca_upload_target ht = target(&stop, huge_conf.c_str());
        CHECK(ca_upload_backend_create(&ht, error, sizeof(error)) == NULL);
    }

    /* a big file goes through an upload session in several pieces */
    const std::string big = g_dir + "/big.mp4";
    std::string data(9 * 1024 * 1024 + 123, 0);
    for (size_t i = 0; i < data.size(); i++) data[i] = (char)((i * 2654435761u) >> 13);
    write_file(big, data);
    seen = Seen();
    CHECK(dbx->put("recordings", "big.mp4", big.c_str(), on_progress, &seen, error, sizeof(error)) == CA_PUT_UPLOADED);
    CHECK(seen.last == data.size() && !seen.backwards);
    CHECK(control("state=1").find("/AP/recordings/big.mp4|" + std::to_string(data.size()) + "|" + dbx_hash(big)) != std::string::npos);

    /* a piece applied by Dropbox but never acknowledged: the retry is told where it is (incorrect_offset) */
    const std::string big2 = g_dir + "/big2.mp4";
    data[100] ^= 1;
    write_file(big2, data);
    control("drop_after=1");
    double start = now_s();
    CHECK(dbx->put("recordings", "big2.mp4", big2.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED);
    CHECK(now_s() - start < 15);
    CHECK(control("state=1").find("/AP/recordings/big2.mp4|" + std::to_string(data.size()) + "|" + dbx_hash(big2)) != std::string::npos);

    /* a piece lost on the way, so Dropbox has nothing of it */
    const std::string big3 = g_dir + "/big3.mp4";
    data[200] ^= 1;
    write_file(big3, data);
    control("drop_before=1");
    CHECK(dbx->put("recordings", "big3.mp4", big3.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED);
    CHECK(control("state=1").find("/AP/recordings/big3.mp4|" + std::to_string(data.size()) + "|" + dbx_hash(big3)) != std::string::npos);

    /* server errors and rate limits are retried, and an expired token is renewed */
    const std::string retry = g_dir + "/retry.jpg";
    write_file(retry, "retry me");
    control("error500=2");
    CHECK(dbx->put("photos", "retry.jpg", retry.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED);
    control("ratelimit=1");
    start = now_s();
    const std::string limited = g_dir + "/limited.jpg";
    write_file(limited, "rate limited");
    CHECK(dbx->put("photos", "limited.jpg", limited.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED);
    CHECK(now_s() - start >= 0.9 && now_s() - start < 10); /* it waited for Retry-After */
    control("expire_token=1");
    const std::string renewed = g_dir + "/renewed.jpg";
    write_file(renewed, "new token");
    CHECK(dbx->put("photos", "renewed.jpg", renewed.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED);

    /* a hash that does not match is a failure, so nothing would be deleted */
    const std::string corrupt = g_dir + "/corrupt.jpg";
    write_file(corrupt, "will be damaged");
    control("corrupt=1");
    CHECK(dbx->put("photos", "corrupt.jpg", corrupt.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_FAILED);
    CHECK(strstr(error, "did not match") != NULL);

    /* a token without a permission: Dropbox says which, and nothing is retried */
    control("noscope=1");
    start = now_s();
    const std::string scoped = g_dir + "/scoped.jpg";
    write_file(scoped, "needs a permission");
    CHECK(dbx->put("photos", "scoped.jpg", scoped.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_FAILED);
    CHECK(strstr(error, "files.metadata.read") != NULL && strstr(error, "Permissions tab") != NULL);
    CHECK(now_s() - start < 2);
    control("reset=1");

    /* an empty local file never replaces different content on Dropbox, and is not a failure */
    const std::string full = g_dir + "/full.jpg", truncated = g_dir + "/truncated.jpg";
    write_file(full, "some content");
    write_file(truncated, "");
    CHECK(dbx->put("photos", "full.jpg", full.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED);
    CHECK(dbx->put("photos", "full.jpg", truncated.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_LEFT);
    CHECK(control("state=1").find("/AP/photos/full.jpg|12|") != std::string::npos); /* untouched */

    /* a file that is not there */
    CHECK(dbx->put("photos", "gone.jpg", (g_dir + "/gone.jpg").c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_REJECTED);

    /* A name Dropbox cannot take is skipped, not a failure of the connection: put() says so, and the next file can go.
     * Dropbox refuses a name ending in a space or a dot, and the client refuses bad UTF-8 and a path that is too long. */
    CHECK(dbx->put("photos", "ends with a space ", full.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_REJECTED);
    CHECK(strstr(error, "cannot take") != NULL);
    CHECK(dbx->put("photos", "ends with a dot.", full.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_REJECTED);
    CHECK(dbx->put("photos", "\xff\xfe.jpg", full.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_REJECTED);
    CHECK(dbx->put("photos", std::string(800, 'n').c_str(), full.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_REJECTED);
    CHECK(strstr(error, "too long") != NULL);
    /* Under the byte limit, but the \uXXXX escapes of 300 accented letters (about 1800 bytes) do not fit in a header. */
    std::string accents;
    for (int i = 0; i < 300; i++) accents += "\xC3\xA9";
    CHECK(dbx->put("photos", accents.c_str(), full.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_REJECTED);
    CHECK(strstr(error, "too long") != NULL);
    CHECK(dbx->put("photos", "after.jpg", full.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED); /* the next one goes */

    /* stop interrupts a slow upload */
    dbx->set_bandwidth(8);
    std::thread stopper([&] { usleep(1500000); stop = true; });
    start = now_s();
    CHECK(dbx->put("recordings", "slow.mp4", big.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_STOPPED);
    CHECK(now_s() - start < 5);
    stopper.join();
    delete dbx;
    stop = false;

    /* credentials Dropbox refuses: a clear message, with no retry delay */
    t = target(&stop, bad_credentials.c_str());
    dbx = ca_upload_backend_create(&t, error, sizeof(error));
    CHECK(dbx != NULL);
    if (dbx) {
        start = now_s();
        CHECK(dbx->put("photos", "a.jpg", photo.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_FAILED);
        CHECK(strstr(error, "refused") != NULL && strstr(error, "Authorise again") != NULL);
        CHECK(now_s() - start < 2);
        delete dbx;
    }

    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("PASS Dropbox backend\n");
    return 0;
}
