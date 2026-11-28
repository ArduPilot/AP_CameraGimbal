/* Exercises the Google Drive backend against tests/test_gdrive.py's fake
 * Drive. Usage: test_gdrive <port> <ca.pem> <scratch dir> */
#include "camera_app/https.h"
#include "camera_app/upload_backend.h"

#include <fcntl.h>
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

/* The fake server's control channel; the answer to state is its file list. */
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

static std::string md5_hex(const std::string &path)
{
    std::string command = "md5sum '" + path + "'";
    FILE *p = popen(command.c_str(), "r");
    char hex[40] = "";
    if (p) { if (fscanf(p, "%32s", hex) != 1) hex[0] = 0; pclose(p); }
    return hex;
}

static ca_upload_target target(const std::atomic<bool> *stop, const char *credentials)
{
    ca_upload_target t = {};
    t.service = "gdrive";
    t.credentials_path = credentials;
    t.credentials_section = "gdrive";
    t.root_folder = "AP";
    t.ca_file = g_ca;
    t.stop = stop;
    t.api_host = t.auth_host = "localhost";
    t.api_port = t.auth_port = g_port;
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
    write_file(credentials, "[other]\nclient_id = x\n\n[gdrive]\ntype = drive\nclient_id = id\nclient_secret = secret/+=\n"
               "token = {\"access_token\":\"old\",\"token_type\":\"Bearer\",\"refresh_token\":\"1//refresh\",\"expiry\":\"2020-01-01T00:00:00Z\"}\n");
    write_file(bad_credentials, "[gdrive]\ntype = drive\nclient_id = bad\nclient_secret = secret/+=\n"
               "token = {\"access_token\":\"old\",\"refresh_token\":\"1//refresh\"}\n");

    /* no credentials, no backend */
    ca_upload_target t = target(&stop, "/nonexistent/rclone.conf");
    CHECK(ca_upload_backend_create(&t, error, sizeof(error)) == NULL && strstr(error, "credentials"));
    t = target(&stop, credentials.c_str());
    t.service = "nosuchservice";
    CHECK(ca_upload_backend_create(&t, error, sizeof(error)) == NULL);
    t.service = "gdrive";
    APC_UploadBackend *drive = ca_upload_backend_create(&t, error, sizeof(error));
    if (!drive) { fprintf(stderr, "FAIL create: %s\n", error); return 1; }

    /* a new file, then the same file again, then a changed one */
    const std::string photo = g_dir + "/a.jpg";
    write_file(photo, std::string(70000, 'a'));
    Seen seen;
    CHECK(drive->put("photos", "a.jpg", photo.c_str(), on_progress, &seen, error, sizeof(error)) == CA_PUT_UPLOADED);
    CHECK(seen.last == 70000 && !seen.backwards);
    CHECK(control("state=1") == "AP/photos/a.jpg|70000|" + md5_hex(photo));
    CHECK(drive->put("photos", "a.jpg", photo.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_SKIPPED);
    write_file(photo, std::string(5000, 'b'));
    CHECK(drive->put("photos", "a.jpg", photo.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED);
    CHECK(control("state=1") == "AP/photos/a.jpg|5000|" + md5_hex(photo)); /* replaced, not duplicated */

    /* nested folders, a name that needs escaping, an empty file */
    const std::string quote = g_dir + "/it's \"x\".mp4";
    write_file(quote, "video");
    CHECK(drive->put("recordings/2026/day 1", "it's \"x\".mp4", quote.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED);
    CHECK(drive->put("recordings/2026/day 1", "it's \"x\".mp4", quote.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_SKIPPED);
    const std::string empty = g_dir + "/empty.txt";
    write_file(empty, "");
    CHECK(drive->put("", "empty.txt", empty.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED);
    const std::string listing = control("state=1");
    CHECK(listing.find("AP/recordings/2026/day 1/it's \"x\".mp4|5|") != std::string::npos);
    CHECK(listing.find("AP/empty.txt|0|") != std::string::npos);

    /* a long token (the web page accepts 4096 characters) keeps its refresh token, which follows a long
     * access token; one too long to hold is refused rather than cut short */
    {
        const std::string long_conf = g_dir + "/long.conf", huge_conf = g_dir + "/huge.conf";
        write_file(long_conf, "[gdrive]\nclient_id = id\nclient_secret = secret/+=\ntoken = {\"access_token\":\"" + std::string(3500, 'a') +
                   "\",\"token_type\":\"Bearer\",\"refresh_token\":\"1//refresh\"}\n");
        write_file(huge_conf, "[gdrive]\nclient_id = id\nclient_secret = secret/+=\ntoken = {\"access_token\":\"" + std::string(5000, 'a') +
                   "\",\"refresh_token\":\"1//refresh\"}\n");
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

    /* a big file goes in several chunks */
    const std::string big = g_dir + "/big.mp4";
    std::string data(9 * 1024 * 1024 + 123, 0);
    for (size_t i = 0; i < data.size(); i++) data[i] = (char)((i * 2654435761u) >> 13);
    write_file(big, data);
    seen = Seen();
    CHECK(drive->put("recordings", "big.mp4", big.c_str(), on_progress, &seen, error, sizeof(error)) == CA_PUT_UPLOADED);
    CHECK(seen.last == data.size() && !seen.backwards);
    CHECK(control("state=1").find("AP/recordings/big.mp4|" + std::to_string(data.size()) + "|" + md5_hex(big)) != std::string::npos);

    /* a connection lost part way through a chunk resumes from what arrived */
    const std::string big2 = g_dir + "/big2.mp4";
    data[100] ^= 1;
    write_file(big2, data);
    control("drop_chunk=1");
    double start = now_s();
    CHECK(drive->put("recordings", "big2.mp4", big2.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED);
    CHECK(now_s() - start < 15);
    CHECK(control("state=1").find("AP/recordings/big2.mp4|" + std::to_string(data.size()) + "|" + md5_hex(big2)) != std::string::npos);

    /* server errors are retried, and an expired token is renewed */
    const std::string retry = g_dir + "/retry.jpg";
    write_file(retry, "retry me");
    control("error500=2");
    CHECK(drive->put("photos", "retry.jpg", retry.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED);
    control("expire_token=1");
    const std::string renewed = g_dir + "/renewed.jpg";
    write_file(renewed, "new token");
    CHECK(drive->put("photos", "renewed.jpg", renewed.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED);

    /* a checksum that does not match is a failure, so nothing would be deleted */
    const std::string corrupt = g_dir + "/corrupt.jpg";
    write_file(corrupt, "will be damaged");
    control("corrupt=1");
    CHECK(drive->put("photos", "corrupt.jpg", corrupt.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_FAILED);
    CHECK(strstr(error, "did not match") != NULL);

    /* a server that keeps nothing must not be fed the same chunk for ever */
    control("stall=1000");
    start = now_s();
    CHECK(drive->put("recordings", "stalled.mp4", big.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_FAILED);
    CHECK(now_s() - start < 20);
    control("stall=0");

    /* an empty local file never replaces different content on Drive, and is not a failure */
    const std::string truncated = g_dir + "/truncated.jpg";
    write_file(truncated, "");
    CHECK(drive->put("photos", "retry.jpg", truncated.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_LEFT);
    CHECK(control("state=1").find("AP/photos/retry.jpg|8|") != std::string::npos); /* what Drive has is untouched */

    /* a file that is not there */
    CHECK(drive->put("photos", "gone.jpg", (g_dir + "/gone.jpg").c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_REJECTED);

    /* A name Drive cannot take (too long for the request) is skipped, and the next file can go. */
    CHECK(drive->put("photos", std::string(1200, 'n').c_str(), truncated.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_REJECTED);
    CHECK(strstr(error, "too long") != NULL);
    /* Nor a folder name it cannot take: the file is skipped, not retried as if the service were down. */
    CHECK(drive->put(std::string(300, 'f').c_str(), "x.jpg", truncated.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_REJECTED);
    CHECK(strstr(error, "folder name too long") != NULL);
    CHECK(drive->put("photos", "after.jpg", truncated.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_UPLOADED);

    /* stop interrupts a slow upload */
    drive->set_bandwidth(8);
    std::thread stopper([&] { usleep(1500000); stop = true; });
    start = now_s();
    CHECK(drive->put("recordings", "slow.mp4", big.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_STOPPED);
    CHECK(now_s() - start < 5);
    stopper.join();
    delete drive;
    stop = false;

    /* credentials Google refuses: a clear message, with no retry delay */
    t = target(&stop, bad_credentials.c_str());
    drive = ca_upload_backend_create(&t, error, sizeof(error));
    CHECK(drive != NULL);
    if (drive) {
        start = now_s();
        CHECK(drive->put("photos", "a.jpg", photo.c_str(), NULL, NULL, error, sizeof(error)) == CA_PUT_FAILED);
        CHECK(strstr(error, "refused") != NULL && strstr(error, "revoked") != NULL && strstr(error, "Authorise again") != NULL);
        CHECK(now_s() - start < 2);
        delete drive;
    }

    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("PASS Google Drive backend\n");
    return 0;
}
