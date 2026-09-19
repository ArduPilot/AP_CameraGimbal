#include "../APC_StringBuffer.h"
#include <cassert>

int main()
{
    APC_StringBuffer response;
    assert(response.append_html("<&\"'"));
    assert(strcmp(response.data(), "&lt;&amp;&quot;&#39;") == 0);
    assert(response.appendf(" %u", 123U));
    const size_t length = response.size();
    volatile size_t oversized = SIZE_MAX;
    assert(!response.append_n("", oversized));
    assert(!response.append_html_n("", oversized));
    assert(response.size() == length);
    char *body = response.release();
    assert(response.data() == nullptr && response.size() == 0);
    response.reset();
    assert(strcmp(body, "&lt;&amp;&quot;&#39; 123") == 0);
    free(body);
    assert(response.append_url("space &?"));
    assert(strcmp(response.data(), "space%20%26%3F") == 0);
    response.reset();
    assert(response.append_log_html("\033[31mred<&\r\n", 12));
    assert(strcmp(response.data(), "red&lt;&amp;\n") == 0);
    // Exercise reallocation and termination after release/reset reuse.
    for (unsigned i = 0; i < 10000; i++) assert(response.append("x"));
    assert(response.size() == strlen(response.data()));
    return 0;
}
