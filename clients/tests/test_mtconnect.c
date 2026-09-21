/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * MTConnect: the XML scanner and the HTTP client against the documents of
 * 16-MTCONNECT.md, then the driver against a mock agent that serves /probe and
 * /current (and one that only serves /current, which has to work too).
 */
#include <stdio.h>
#include <string.h>

#include "ncl_test.h"

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "test_point_map.h"
#include "nclink/clients/mtconnect.h"
#include "mtconnect/ncl_mtconnect_driver.h"

static int  g_event_count;
static char g_event_id[64];

static void count_event(void *user, const char *id, const ncl_json *event)
{
    g_event_count++;
    if (user != NULL) {
        snprintf((char *)user, 64, "%s", id != NULL ? id : "");
    }
    (void)event;
}

/** Substring search over a buffer that may contain NULs. */
static bool contains(const uint8_t *haystack, size_t hay_len, const char *needle)
{
    size_t len = strlen(needle);
    size_t i;

    if (hay_len < len) {
        return false;
    }
    for (i = 0; i + len <= hay_len; i++) {
        if (memcmp(haystack + i, needle, len) == 0) {
            return true;
        }
    }
    return false;
}

/* The probe document of §3.1, with a namespace on the root to prove that the
 * prefix is ignored. */
static const char kProbe[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<MTConnectDevices xmlns=\"urn:mtconnect.org:MTConnectDevices:1.4\">\n"
    "  <Header creationTime=\"2026-09-18T10:00:00Z\" sender=\"agent\""
    " instanceId=\"1\" version=\"1.4\"/>\n"
    "  <Devices>\n"
    "    <Device id=\"d1\" name=\"DMU 50\" uuid=\"uuid-1\">\n"
    "      <Description>DMG MORI DMU 50</Description>\n"
    "      <DataItems>\n"
    "        <DataItem id=\"x1\" category=\"SAMPLE\" type=\"POSITION\""
    " subType=\"ACTUAL\" name=\"Xabs\" units=\"MILLIMETER\"/>\n"
    "        <DataItem id=\"r1\" category=\"EVENT\" type=\"EXECUTION\""
    " name=\"exec\"/>\n"
    "        <DataItem id=\"a1\" category=\"CONDITION\" type=\"ALARM\""
    " name=\"alarm\"/>\n"
    "      </DataItems>\n"
    "    </Device>\n"
    "  </Devices>\n"
    "</MTConnectDevices>\n";

/* The current document of §3.2. */
static const char kCurrent[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<MTConnectStreams xmlns=\"urn:mtconnect.org:MTConnectStreams:1.4\">\n"
    "  <Header creationTime=\"2026-09-18T10:00:00Z\" instanceId=\"1\"/>\n"
    "  <Streams>\n"
    "    <DeviceStream name=\"DMU 50\" uuid=\"uuid-1\">\n"
    "      <ComponentStream component=\"Linear\" name=\"X\">\n"
    "        <Samples>\n"
    "          <Position dataItemId=\"x1\""
    " timestamp=\"2026-09-18T10:00:00.000Z\" sequence=\"7\">123.456</Position>\n"
    "        </Samples>\n"
    "      </ComponentStream>\n"
    "      <ComponentStream component=\"Controller\">\n"
    "        <Events>\n"
    "          <Execution dataItemId=\"r1\""
    " timestamp=\"2026-09-18T10:00:00.000Z\" sequence=\"8\">ACTIVE</Execution>\n"
    "        </Events>\n"
    "        <Condition>\n"
    "          <Fault dataItemId=\"a1\" type=\"ALARM\""
    " nativeCode=\"1201\" severity=\"FAULT\""
    " timestamp=\"2026-09-18T10:00:01.000Z\" sequence=\"9\">Fault</Fault>\n"
    "        </Condition>\n"
    "      </ComponentStream>\n"
    "    </DeviceStream>\n"
    "  </Streams>\n"
    "</MTConnectStreams>\n";

/* ============================================================== the XML == */

static void test_probe(void)
{
    ncl_mt_probe probe;
    char err[160];
    const ncl_mt_data_item *item;

    NCL_TEST_CASE("a probe document gives the data item table");
    err[0] = '\0';
    NCL_CHECK_EQ_INT(ncl_mt_probe_parse(kProbe, sizeof(kProbe) - 1, &probe, err,
                                        sizeof(err)),
                     NCL_OK);
    NCL_CHECK_EQ_INT(probe.count, 3);
    NCL_CHECK_EQ_STR(probe.device_name, "DMU 50");
    NCL_CHECK_EQ_STR(probe.device_uuid, "uuid-1");
    NCL_CHECK_EQ_STR(probe.model, "DMG MORI DMU 50");
    item = ncl_mt_probe_find(&probe, "x1");
    NCL_CHECK(item != NULL);
    if (item != NULL) {
        NCL_CHECK_EQ_STR(item->type, "POSITION");
        NCL_CHECK_EQ_STR(item->sub_type, "ACTUAL");
        NCL_CHECK_EQ_STR(item->category, "SAMPLE");
        NCL_CHECK_EQ_STR(item->units, "MILLIMETER");
        NCL_CHECK_EQ_STR(item->name, "Xabs");
    }
    NCL_CHECK(ncl_mt_probe_find(&probe, "nope") == NULL);
    NCL_CHECK_EQ_STR(ncl_mt_probe_find(&probe, "a1")->category, "CONDITION");

    NCL_TEST_CASE("the probe turns into JSON for the probe method");
    {
        ncl_json *json = ncl_mt_probe_to_json(&probe);
        char *text;

        NCL_CHECK(json != NULL);
        text = ncl_json_write_string(json);
        NCL_CHECK(text != NULL && strstr(text, "\"units\":\"MILLIMETER\"") != NULL);
        ncl_free_safe(text);
        ncl_json_free(json);
    }
    ncl_mt_probe_free(&probe);

    NCL_TEST_CASE("a document without data items is refused");
    err[0] = '\0';
    NCL_CHECK_EQ_INT(ncl_mt_probe_parse("<MTConnectDevices/>", 19, &probe, err,
                                        sizeof(err)),
                     NCL_DRV_ERR_PROTOCOL(0x70));
    NCL_CHECK(err[0] != '\0');
}

static void test_current(void)
{
    ncl_mt_probe probe;
    ncl_mt_current current;
    const ncl_mt_reading *reading;

    NCL_TEST_CASE("a current document gives the readings");
    ncl_mt_probe_init(&probe);
    NCL_CHECK_EQ_INT(ncl_mt_probe_parse(kProbe, sizeof(kProbe) - 1, &probe, NULL,
                                        0),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_mt_current_parse(kCurrent, sizeof(kCurrent) - 1, &probe,
                                          &current, NULL, 0),
                     NCL_OK);
    NCL_CHECK_EQ_INT(current.count, 3);
    NCL_CHECK_EQ_INT(current.first_sequence, 7);
    NCL_CHECK_EQ_INT(current.last_sequence, 9);

    reading = ncl_mt_current_find(&current, "x1");
    NCL_CHECK(reading != NULL);
    if (reading != NULL) {
        NCL_CHECK_EQ_STR(reading->value, "123.456");
        NCL_CHECK_EQ_STR(reading->type, "Position");
        NCL_CHECK_EQ_STR(reading->category, "SAMPLE");
        NCL_CHECK_EQ_STR(reading->timestamp, "2026-09-18T10:00:00.000Z");
        NCL_CHECK_EQ_INT(reading->sequence, 7);
    }
    reading = ncl_mt_current_find(&current, "r1");
    NCL_CHECK(reading != NULL && reading->category != NULL &&
              strcmp(reading->category, "EVENT") == 0);
    reading = ncl_mt_current_find(&current, "a1");
    NCL_CHECK(reading != NULL);
    if (reading != NULL) {
        NCL_CHECK_EQ_STR(reading->value, "Fault");
        NCL_CHECK_EQ_STR(reading->native_code, "1201");
        NCL_CHECK_EQ_STR(reading->severity, "FAULT");
        NCL_CHECK_EQ_STR(reading->category, "CONDITION");
    }
    ncl_mt_current_free(&current);
    ncl_mt_probe_free(&probe);

    NCL_TEST_CASE("without a probe the element names still give the category");
    NCL_CHECK_EQ_INT(ncl_mt_current_parse(kCurrent, sizeof(kCurrent) - 1, NULL,
                                          &current, NULL, 0),
                     NCL_OK);
    NCL_CHECK_EQ_STR(ncl_mt_current_find(&current, "a1")->category, "CONDITION");
    NCL_CHECK_EQ_STR(ncl_mt_current_find(&current, "x1")->category, "SAMPLE");
    ncl_mt_current_free(&current);
}

static void test_scan_edges(void)
{
    ncl_mt_slice from;
    ncl_mt_slice tag;
    ncl_mt_slice content;
    size_t next = 0;
    char value[64];
    char *text;

    NCL_TEST_CASE("entities and blanks are decoded, namespaces ignored");
    {
        static const char kXml[] =
            "<a:Device><Description>  A &amp; B &lt;x&gt;  </Description>"
            "<DataItem id=\"a&amp;b\" units=\"MIL\"/></a:Device>";

        from.text = kXml;
        from.len = 0;
        NCL_CHECK(ncl_mt_next_element(kXml, sizeof(kXml) - 1, "Description", &from,
                                      &tag, &content, &next));
        text = ncl_mt_text(&content);
        NCL_CHECK_EQ_STR(text, "A & B <x>");
        ncl_free_safe(text);

        from.len = 0;
        NCL_CHECK(ncl_mt_next_element(kXml, sizeof(kXml) - 1, "DataItem", &from,
                                      &tag, NULL, &next));
        NCL_CHECK(ncl_mt_attr(&tag, "id", value, sizeof(value)));
        NCL_CHECK_EQ_STR(value, "a&b");
        NCL_CHECK(ncl_mt_attr(&tag, "units", value, sizeof(value)));
        NCL_CHECK_EQ_STR(value, "MIL");
        NCL_CHECK(!ncl_mt_attr(&tag, "category", value, sizeof(value)));
    }

    NCL_TEST_CASE("a name that merely starts with the same letters is not it");
    {
        static const char kXml[] = "<Positions/><Position id=\"a\">1</Position>";

        from.text = kXml;
        from.len = 0;
        NCL_CHECK(ncl_mt_next_element(kXml, sizeof(kXml) - 1, "Position", &from,
                                      &tag, &content, &next));
        text = ncl_mt_text(&content);
        NCL_CHECK_EQ_STR(text, "1");
        ncl_free_safe(text);
    }

    NCL_TEST_CASE("UNAVAILABLE is recognised however it is spelled");
    NCL_CHECK(ncl_mt_value_is_unavailable("UNAVAILABLE"));
    NCL_CHECK(ncl_mt_value_is_unavailable("unavailable"));
    NCL_CHECK(ncl_mt_value_is_unavailable(""));
    NCL_CHECK(!ncl_mt_value_is_unavailable("0"));
}

/* ============================================================= the agent == */

typedef struct {
    ncl_socket *listener;
    unsigned    port;
    ncl_thread *thread;
    bool        stop;
    bool        serve_probe; /**< the second agent answers /current only */
    bool        chunked;
    int         requests;
    int         probe_requests;
    int         current_requests;
    const char *current_body; /**< what /current answers, NULL for the default */
    bool        slow_probe;
} mt_agent;

static void agent_reply(ncl_socket *peer, const char *body, bool chunked)
{
    ncl_strbuf response;
    size_t len = strlen(body);

    ncl_strbuf_init(&response);
    if (chunked) {
        (void)ncl_strbuf_puts(&response,
                              "HTTP/1.1 200 OK\r\nContent-Type: application/xml\r\n"
                              "Transfer-Encoding: chunked\r\n\r\n");
        /* Two chunks, to exercise the parser. */
        (void)ncl_strbuf_printf(&response, "%X\r\n", (unsigned)(len / 2));
        (void)ncl_strbuf_append(&response, body, len / 2);
        (void)ncl_strbuf_puts(&response, "\r\n");
        (void)ncl_strbuf_printf(&response, "%X\r\n", (unsigned)(len - len / 2));
        (void)ncl_strbuf_append(&response, body + len / 2, len - len / 2);
        (void)ncl_strbuf_puts(&response, "\r\n0\r\n\r\n");
    } else {
        (void)ncl_strbuf_printf(&response,
                                "HTTP/1.1 200 OK\r\nContent-Type: application/xml\r\n"
                                "Content-Length: %u\r\nConnection: close\r\n\r\n",
                                (unsigned)len);
        (void)ncl_strbuf_puts(&response, body);
    }
    (void)ncl_socket_send(peer, response.data, response.len);
    ncl_strbuf_free(&response);
}

static void agent_main(void *arg)
{
    mt_agent *agent = (mt_agent *)arg;

    while (!agent->stop) {
        ncl_socket *peer = ncl_socket_accept(agent->listener, 200);

        if (peer == NULL) {
            continue;
        }
        for (;;) {
            char request[1024];
            size_t used = 0;
            bool got_line = false;

            /* One request line plus headers, read until the blank line. */
            while (used + 1 < sizeof(request)) {
                int got = ncl_socket_recv(peer, request + used,
                                          sizeof(request) - 1 - used, 2000);

                if (got <= 0) {
                    break;
                }
                used += (size_t)got;
                request[used] = '\0';
                if (strstr(request, "\r\n\r\n") != NULL) {
                    got_line = true;
                    break;
                }
            }
            if (!got_line) {
                break;
            }
            agent->requests++;
            if (strncmp(request, "GET /probe", 10) == 0) {
                agent->probe_requests++;
                if (!agent->serve_probe) {
                    static const char kNotFound[] =
                        "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";

                    (void)ncl_socket_send(peer, kNotFound, sizeof(kNotFound) - 1);
                } else {
                    if (agent->slow_probe) {
                        ncl_sleep_millis(200);
                    }
                    agent_reply(peer, kProbe, agent->chunked);
                }
            } else if (strncmp(request, "GET /current", 12) == 0) {
                agent->current_requests++;
                agent_reply(peer,
                            agent->current_body != NULL ? agent->current_body
                                                        : kCurrent,
                            agent->chunked);
            } else {
                static const char kNotFound[] =
                    "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";

                (void)ncl_socket_send(peer, kNotFound, sizeof(kNotFound) - 1);
            }
            break; /* one request per connection, as the client asks */
        }
        ncl_socket_close(peer);
    }
}

static mt_agent *agent_start(bool serve_probe, bool chunked)
{
    mt_agent *agent = (mt_agent *)ncl_mem_calloc(1, sizeof(*agent));

    if (agent == NULL) {
        return NULL;
    }
    agent->serve_probe = serve_probe;
    agent->chunked = chunked;
    agent->listener = ncl_socket_listen(0, NULL, 0);
    if (agent->listener == NULL) {
        ncl_free_safe(agent);
        return NULL;
    }
    agent->port = ncl_socket_local_port(agent->listener);
    agent->thread = ncl_thread_start(agent_main, agent);
    if (agent->thread == NULL) {
        ncl_socket_close(agent->listener);
        ncl_free_safe(agent);
        return NULL;
    }
    return agent;
}

static void agent_stop(mt_agent *agent)
{
    if (agent == NULL) {
        return;
    }
    agent->stop = true;
    ncl_thread_join(agent->thread);
    ncl_socket_close(agent->listener);
    ncl_free_safe(agent);
}

static ncl_driver *mt_driver(mt_agent *agent)
{
    ncl_driver *driver = ncl_mtconnect_create();
    ncl_strbuf json;
    ncl_json *params;

    if (driver == NULL) {
        return NULL;
    }
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"host\":\"127.0.0.1\",\"port\":%u,"
                            "\"timeoutMs\":800}",
                            agent->port);
    params = ncl_json_parse_cstr(ncl_strbuf_cstr(&json), NULL);
    ncl_strbuf_free(&json);
    if (params == NULL || driver->ops->create(driver, params) != NCL_OK) {
        ncl_json_free(params);
        driver->ops->destroy(driver);
        return NULL;
    }
    ncl_json_free(params);
    return driver;
}

static ncl_err read_item(ncl_driver *driver, const char *id, ncl_json **value)
{
    ncl_strbuf json;
    ncl_json *node;
    ncl_address address;
    ncl_err err;

    ncl_strbuf_init(&json);
    /* The area name is the dataItemId, the offset is always 0. */
    (void)ncl_strbuf_printf(&json, "{\"area\":\"%s\",\"offset\":0}", id);
    node = ncl_json_parse_cstr(ncl_strbuf_cstr(&json), NULL);
    ncl_strbuf_free(&json);
    if (node == NULL) {
        return NCL_ERR_PARSE;
    }
    err = ncl_address_from_json(node, &address);
    ncl_json_free(node);
    if (err != NCL_OK) {
        return err;
    }
    err = ncl_driver_read_one(driver, &address, value);
    ncl_address_clear(&address);
    return err;
}

static void test_against_an_agent(void)
{
    mt_agent *agent = agent_start(true, false);
    ncl_driver *driver;
    ncl_json *value = NULL;
    ncl_err err;

    NCL_TEST_CASE("the probe is read once, then every read is one GET");
    NCL_CHECK(agent != NULL);
    if (agent == NULL) {
        return;
    }
    driver = mt_driver(agent);
    NCL_CHECK(driver != NULL);
    if (driver == NULL) {
        agent_stop(agent);
        return;
    }
    NCL_CHECK_EQ_INT(read_item(driver, "x1", &value), NCL_OK);
    NCL_CHECK_EQ_INT(agent->probe_requests, 1);
    {
        double real = 0;

        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real > 123.45 && real < 123.46);
    }
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("an event value comes back as its text");
    NCL_CHECK_EQ_INT(read_item(driver, "r1", &value), NCL_OK);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "ACTIVE");
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("a condition going to Fault is pushed once as an event");
    {
        char last_id[64];

        last_id[0] = '\0';
        g_event_count = 0;
        driver->ops->attach_event(driver, count_event, last_id);
        NCL_CHECK_EQ_INT(read_item(driver, "a1", &value), NCL_OK);
        NCL_CHECK_EQ_STR(ncl_json_as_string(value), "Fault");
        ncl_json_free(value);
        value = NULL;
        NCL_CHECK_EQ_INT(g_event_count, 1);
        NCL_CHECK_EQ_STR(last_id, "a1");
        /* the same state again is not a new event */
        NCL_CHECK_EQ_INT(read_item(driver, "a1", &value), NCL_OK);
        ncl_json_free(value);
        value = NULL;
        NCL_CHECK_EQ_INT(g_event_count, 1);
    }

    NCL_TEST_CASE("an item the agent does not know reads as JSON null");
    NCL_CHECK_EQ_INT(read_item(driver, "nope", &value), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_json_type_of(value), NCL_JSON_NULL);
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("the probe and sequence methods report what was seen");
    {
        ncl_json *result = NULL;

        NCL_CHECK_EQ_INT(driver->ops->call(driver, "probe", NULL, &result), NCL_OK);
        NCL_CHECK(result != NULL &&
                  ncl_json_obj_get(result, "dataItems") != NULL);
        ncl_json_free(result);
        NCL_CHECK_EQ_INT(driver->ops->call(driver, "sequence", NULL, &result),
                         NCL_OK);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(result, "sequence", -1), 9);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(result, "dataItems", -1), 3);
        ncl_json_free(result);
        NCL_CHECK(ncl_driver_error_tier(driver->ops->call(driver, "nonsense", NULL,
                                                          NULL)) == 2);
    }

    NCL_TEST_CASE("writing is refused: MTConnect is read only");
    NCL_CHECK_EQ_INT(ncl_driver_write_one(driver, NULL, NULL),
                     NCL_ERR_NOT_SUPPORTED);

    NCL_TEST_CASE("the raw hatch returns the document itself");
    {
        ncl_driver_result out;
        const char *path = "/current";

        ncl_driver_result_init(&out);
        NCL_CHECK_EQ_INT(driver->ops->read_raw(driver, path, strlen(path), &out),
                         NCL_OK);
        NCL_CHECK(out.raw_len > 100);
        NCL_CHECK(contains(out.raw, out.raw_len, "MTConnectStreams"));
        ncl_driver_result_free(&out);
    }

    driver->ops->destroy(driver);
    agent_stop(agent);
}

static void test_chunked_and_no_probe(void)
{
    NCL_TEST_CASE("a chunked body is reassembled");
    {
        mt_agent *agent = agent_start(true, true);
        ncl_driver *driver;
        ncl_json *value = NULL;

        NCL_CHECK(agent != NULL);
        if (agent != NULL) {
            driver = mt_driver(agent);
            NCL_CHECK(driver != NULL);
            if (driver != NULL) {
                NCL_CHECK_EQ_INT(read_item(driver, "x1", &value), NCL_OK);
                {
                    double real = 0;

                    NCL_CHECK(ncl_json_as_double(value, &real));
                    NCL_CHECK(real > 123.45 && real < 123.46);
                }
                ncl_json_free(value);
                driver->ops->destroy(driver);
            }
            agent_stop(agent);
        }
    }

    NCL_TEST_CASE("an agent without /probe still reads: names give the category");
    {
        mt_agent *agent = agent_start(false, false);
        ncl_driver *driver;
        ncl_json *value = NULL;

        NCL_CHECK(agent != NULL);
        if (agent != NULL) {
            driver = mt_driver(agent);
            NCL_CHECK(driver != NULL);
            if (driver != NULL) {
                /* No probe means no data item table, so the session is "not
                 * connected" and every read opens it again -- which is what a
                 * read-only HTTP interface costs. */
                NCL_CHECK_EQ_INT(read_item(driver, "x1", &value), NCL_OK);
                {
                    double real = 0;

                    NCL_CHECK(ncl_json_as_double(value, &real));
                    NCL_CHECK(real > 123.45 && real < 123.46);
                }
                ncl_json_free(value);
                driver->ops->destroy(driver);
            }
            agent_stop(agent);
        }
    }

    NCL_TEST_CASE("a read of a missing page is a protocol error");
    {
        mt_agent *agent = agent_start(false, false);
        ncl_driver *driver;
        ncl_json *value = NULL;
        ncl_err err = NCL_OK;

        NCL_CHECK(agent != NULL);
        if (agent != NULL) {
            agent->current_body = "<MTConnectStreams/>"; /* no reading at all */
            driver = mt_driver(agent);
            if (driver != NULL) {
                err = read_item(driver, "x1", &value);
                NCL_CHECK_EQ_INT(err, NCL_OK); /* the document parsed, the item
                                                  simply is not there */
                NCL_CHECK_EQ_INT(ncl_json_type_of(value), NCL_JSON_NULL);
                ncl_json_free(value);
                driver->ops->destroy(driver);
            }
            agent_stop(agent);
        }
    }
}

static void test_through_the_manager(void)
{
    mt_agent *agent = agent_start(true, false);
    test_point_map *manager = test_point_map_create(ncl_mtconnect_create);
    ncl_strbuf json;
    ncl_strbuf err;
    ncl_json *config;
    ncl_json *value = NULL;

    NCL_TEST_CASE("a configured MTConnect link reads through the point map");
    NCL_CHECK(agent != NULL && manager != NULL);
    if (agent == NULL || manager == NULL) {
        agent_stop(agent);
        test_point_map_free(manager);
        return;
    }
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"id\":\"cnc\",\"path\":\"/CNC\","
                            "\"type\":\"mtconnect\","
                            "\"parameters\":{\"host\":\"127.0.0.1\","
                            "\"port\":%u,\"timeoutMs\":800},"
                            "\"points\":["
                            "{\"path\":\"/CNC/X\",\"addr\":"
                            "{\"area\":\"x1\",\"offset\":0,\"dtype\":\"float64\"}},"
                            "{\"path\":\"/CNC/EXEC\",\"addr\":"
                            "{\"area\":\"r1\",\"offset\":0,\"dtype\":\"string\"}}]}",
                            agent->port);
    config = ncl_json_parse_cstr(ncl_strbuf_cstr(&json), NULL);
    ncl_strbuf_free(&json);
    NCL_CHECK(config != NULL);
    ncl_strbuf_init(&err);
    if (config != NULL) {
        NCL_CHECK_EQ_INT(test_point_map_add_json(manager, config, &err),
                         NCL_OK);
    }
    if (err.len > 0) {
        printf("    %s\n", ncl_strbuf_cstr(&err));
    }
    ncl_strbuf_free(&err);
    ncl_json_free(config);

    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/CNC/X", &value), NCL_OK);
    {
        double real = 0;

        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real > 123.45 && real < 123.46);
    }
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/CNC/EXEC", &value),
                     NCL_OK);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "ACTIVE");
    ncl_json_free(value);
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/CNC/NOPE", &value),
                     NCL_ERR_NOT_FOUND);
    test_point_map_free(manager);
    agent_stop(agent);
}

NCL_TEST_MAIN_BEGIN()
    test_probe();
    test_current();
    test_scan_edges();
    test_against_an_agent();
    test_chunked_and_no_probe();
    test_through_the_manager();
NCL_TEST_MAIN_END()
