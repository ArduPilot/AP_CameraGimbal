/* Exercises camera_app/https.h against tests/test_https.py's loopback TLS
 * server. Usage: test_https <port> <ca.pem> <other-ca.pem> */
#include "camera_app/https.h"

#include <fcntl.h>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static double now_s()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int get(ca_https *c, const char *path, ca_https_response &r, char *body, size_t cap, char *error, size_t error_size)
{
    ca_https_req q = {};
    q.method = "GET";
    q.path = path;
    q.fd = -1;
    r = ca_https_response {};
    r.body = body;
    r.body_capacity = cap;
    return ca_https_request(c, &q, &r, error, error_size);
}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    const unsigned port = (unsigned)atoi(argv[1]);
    const char *ca = argv[2], *other_ca = argv[3];
    char error[200] = "", body[8192];
    ca_https_response r;
    std::atomic<bool> stop(false);
    ca_https_options options = {};
    options.ca_file = ca;
    options.stop = &stop;

    CHECK(ca_https_clock_plausible());

    /* a certificate for the wrong name, or from the wrong CA, is refused */
    options.server_name = "wrong.example";
    CHECK(ca_https_connect("127.0.0.1", port, &options, error, sizeof(error)) == NULL);
    CHECK(strstr(error, "certificate") != NULL);
    options.server_name = NULL;
    ca_https_options wrong = options;
    wrong.ca_file = other_ca;
    CHECK(ca_https_connect("localhost", port, &wrong, error, sizeof(error)) == NULL);
    CHECK(strstr(error, "certificate") != NULL);
    wrong.ca_file = "/nonexistent.pem";
    CHECK(ca_https_connect("localhost", port, &wrong, error, sizeof(error)) == NULL);
    CHECK(ca_https_connect("localhost", 1, &options, error, sizeof(error)) == NULL); /* nothing listens */

    /* the same roots given as text, as the cameras that build them into the program do, and the wrong ones refused */
    auto read_text = [](const char *path) {
        std::string text;
        FILE *f = fopen(path, "rb");
        char chunk[4096];
        size_t n;
        while (f && (n = fread(chunk, 1, sizeof(chunk), f)) > 0) text.append(chunk, n);
        if (f) fclose(f);
        return text;
    };
    const std::string pem = read_text(ca), other_pem = read_text(other_ca);
    ca_https_options text_options = options;
    text_options.ca_file = NULL;
    text_options.ca_pem = pem.c_str();
    ca_https *from_text = ca_https_connect("localhost", port, &text_options, error, sizeof(error));
    CHECK(from_text != NULL);
    if (from_text) {
        char reply[64];
        ca_https_response rt;
        CHECK(get(from_text, "/json", rt, reply, sizeof(reply), error, sizeof(error)) == CA_HTTPS_OK && rt.status == 200);
        ca_https_close(from_text);
    }
    text_options.ca_pem = other_pem.c_str();
    CHECK(ca_https_connect("localhost", port, &text_options, error, sizeof(error)) == NULL && strstr(error, "certificate") != NULL);
    text_options.ca_pem = "not a certificate\n";
    CHECK(ca_https_connect("localhost", port, &text_options, error, sizeof(error)) == NULL);

    ca_https *c = ca_https_connect("localhost", port, &options, error, sizeof(error));
    if (!c) { fprintf(stderr, "FAIL connect: %s\n", error); return 1; }

    /* content-length and chunked bodies on one kept-alive connection */
    CHECK(get(c, "/json", r, body, sizeof(body), error, sizeof(error)) == CA_HTTPS_OK);
    CHECK(r.status == 200 && !strcmp(body, "{\"ok\":true}"));
    CHECK(get(c, "/chunked", r, body, sizeof(body), error, sizeof(error)) == CA_HTTPS_OK);
    CHECK(r.status == 200 && !strcmp(body, "hello world"));
    CHECK(get(c, "/count", r, body, sizeof(body), error, sizeof(error)) == CA_HTTPS_OK);
    CHECK(!strcmp(body, "2")); /* two TLS connections so far: the one made from PEM text, and this one */

    /* statuses and headers */
    CHECK(get(c, "/nobody", r, body, sizeof(body), error, sizeof(error)) == CA_HTTPS_OK);
    CHECK(r.status == 204 && r.body_length == 0);
    CHECK(get(c, "/missing", r, body, sizeof(body), error, sizeof(error)) == CA_HTTPS_OK);
    CHECK(r.status == 404 && strstr(body, "not found"));
    CHECK(get(c, "/resume", r, body, sizeof(body), error, sizeof(error)) == CA_HTTPS_OK);
    CHECK(r.status == 308 && !strcmp(r.range, "bytes=0-99") && !strcmp(r.location, "https://example.test/next") &&
          !strcmp(r.retry_after, "7"));

    /* a body longer than the buffer is read and cut */
    char small[100];
    CHECK(get(c, "/big", r, small, sizeof(small), error, sizeof(error)) == CA_HTTPS_OK);
    CHECK(r.truncated && r.body_length == 99 && strlen(small) == 99);
    CHECK(get(c, "/json", r, body, sizeof(body), error, sizeof(error)) == CA_HTTPS_OK && !strcmp(body, "{\"ok\":true}"));

    /* request bodies: from memory, and from a slice of a file */
    unsigned char data[30000];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (unsigned char)(i % 251);
    auto sum = [&](size_t from, size_t count) { unsigned long s = 0; for (size_t i = 0; i < count; i++) s += data[from + i]; return s; };
    char expected[64];
    ca_https_req put = {};
    put.method = "PUT";
    put.path = "/echo";
    put.fd = -1;
    put.body = data;
    put.length = 10000;
    r = ca_https_response {};
    r.body = body; r.body_capacity = sizeof(body);
    CHECK(ca_https_request(c, &put, &r, error, sizeof(error)) == CA_HTTPS_OK);
    snprintf(expected, sizeof(expected), "10000:%lu", sum(0, 10000));
    CHECK(r.status == 200 && !strcmp(body, expected));

    char name[] = "/tmp/test_https_XXXXXX";
    const int fd = mkstemp(name);
    CHECK(fd >= 0 && write(fd, data, sizeof(data)) == (ssize_t)sizeof(data));
    unlink(name);
    put.body = NULL; put.fd = fd; put.offset = 1234; put.length = 20000;
    r = ca_https_response {};
    r.body = body; r.body_capacity = sizeof(body);
    CHECK(ca_https_request(c, &put, &r, error, sizeof(error)) == CA_HTTPS_OK);
    snprintf(expected, sizeof(expected), "20000:%lu", sum(1234, 20000));
    CHECK(r.status == 200 && !strcmp(body, expected));
    put.length = 0; put.fd = -1; /* an empty PUT still sends Content-Length: 0 */
    r = ca_https_response {};
    r.body = body; r.body_capacity = sizeof(body);
    CHECK(ca_https_request(c, &put, &r, error, sizeof(error)) == CA_HTTPS_OK && !strcmp(body, "0:0"));

    /* a server that closes after a response, with or without saying so */
    CHECK(get(c, "/close", r, body, sizeof(body), error, sizeof(error)) == CA_HTTPS_OK && r.status == 200);
    CHECK(get(c, "/json", r, body, sizeof(body), error, sizeof(error)) == CA_HTTPS_OK && r.status == 200);
    CHECK(get(c, "/silentclose", r, body, sizeof(body), error, sizeof(error)) == CA_HTTPS_OK && r.status == 200);
    CHECK(get(c, "/json", r, body, sizeof(body), error, sizeof(error)) == CA_HTTPS_OK && r.status == 200);
    CHECK(get(c, "/eof", r, body, sizeof(body), error, sizeof(error)) == CA_HTTPS_OK && !strcmp(body, "until close"));
    CHECK(get(c, "/count", r, body, sizeof(body), error, sizeof(error)) == CA_HTTPS_OK);
    CHECK(!strcmp(body, "5")); /* those two, then new ones after close, silentclose and eof */

    /* the bandwidth limit applies to sent data */
    ca_https_set_bandwidth(c, 16);
    put.body = data; put.length = 24576;
    double start = now_s();
    r = ca_https_response {};
    r.body = body; r.body_capacity = sizeof(body);
    CHECK(ca_https_request(c, &put, &r, error, sizeof(error)) == CA_HTTPS_OK);
    const double took = now_s() - start;
    CHECK(took >= 1.3 && took < 4.0);
    ca_https_set_bandwidth(c, 0);

    /* a body that arrives together with the headers, when the length is the end of the connection */
    CHECK(get(c, "/eofsame", r, body, sizeof(body), error, sizeof(error)) == CA_HTTPS_OK && !strcmp(body, "arrived with the headers"));

    /* stop ends a wait quickly */
    std::thread stopper([&] { usleep(400000); stop = true; });
    start = now_s();
    CHECK(get(c, "/slow", r, body, sizeof(body), error, sizeof(error)) == CA_HTTPS_ERR_STOPPED);
    CHECK(now_s() - start < 1.5);
    stopper.join();
    ca_https_close(c);
    stop = false;

    /* so does the idle timeout */
    options.timeout_ms = 600;
    c = ca_https_connect("localhost", port, &options, error, sizeof(error));
    CHECK(c != NULL);
    if (c) {
        start = now_s();
        CHECK(get(c, "/slow", r, body, sizeof(body), error, sizeof(error)) == CA_HTTPS_ERR_IO);
        CHECK(now_s() - start < 2.0);
        ca_https_close(c);
    }

    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("PASS https client\n");
    return 0;
}
