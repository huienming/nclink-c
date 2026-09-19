/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the MTConnect XML documents (see ncl_mtconnect.h).
 *
 * A scanner rather than a DOM: /probe is a flat list of DataItem elements and
 * /current is a flat list of value elements, so finding elements by name and
 * reading their attributes is all the parsing this protocol needs.
 *
 * Namespaces are ignored on purpose (§6.4: the URN differs between agent
 * versions, so matching on the local name keeps this working across them).
 */

#include "nclink_adapter/ncl_mtconnect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =============================================================== helpers == */

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/** True when @p at names @p name exactly (no longer name sharing the prefix). */
static bool name_matches(const char *at, const char *end, const char *name)
{
    size_t len = strlen(name);

    if ((size_t)(end - at) < len || strncmp(at, name, len) != 0) {
        return false;
    }
    return at + len == end || is_space(at[len]) || at[len] == '>' ||
           at[len] == '/' || at[len] == ':';
}

/**
 * The element whose opening tag ends at @p open_end: find its matching closing
 * tag, hand back the text between them and where the element ends.
 */
static bool element_content(const char *text, size_t len, size_t open_end,
                            const char *name, ncl_mt_slice *content,
                            size_t *after)
{
    const char *scan = text + open_end;

    while ((size_t)(scan - text) < len) {
        const char *lt = memchr(scan, '<', len - (size_t)(scan - text));

        if (lt == NULL) {
            break;
        }
        if (lt[1] == '/') {
            const char *gt = memchr(lt, '>', len - (size_t)(lt - text));
            const char *local = lt + 2;

            if (gt != NULL) {
                const char *space = memchr(local, ' ', (size_t)(gt - local));
                const char *limit = space != NULL ? space : gt;
                const char *colon = memchr(local, ':',
                                           (size_t)(limit - local));

                if (colon != NULL) {
                    local = colon + 1;
                }
                if (name_matches(local, gt, name)) {
                    if (content != NULL) {
                        content->text = text + open_end;
                        content->len = (size_t)(lt - (text + open_end));
                    }
                    if (after != NULL) {
                        *after = (size_t)(gt - text) + 1;
                    }
                    return true;
                }
            }
        }
        scan = lt + 1;
    }
    return false;
}

/** Decode the XML entities of @p text into @p out; returns the length used. */
static size_t decode_entities(const char *text, size_t len, char *out,
                              size_t out_len)
{
    size_t read_at = 0;
    size_t written = 0;

    while (read_at < len && written + 1 < out_len) {
        if (text[read_at] != '&') {
            out[written++] = text[read_at++];
            continue;
        }
        {
            const char *semi = memchr(text + read_at, ';', len - read_at);
            size_t entity_len;

            if (semi == NULL || (entity_len = (size_t)(semi - text) - read_at - 1u) >
                                    sizeof("apos") - 1u) {
                out[written++] = text[read_at++];
                continue;
            }
            if (strncmp(text + read_at + 1, "amp", entity_len) == 0 &&
                entity_len == 3) {
                out[written++] = '&';
            } else if (strncmp(text + read_at + 1, "lt", entity_len) == 0 &&
                       entity_len == 2) {
                out[written++] = '<';
            } else if (strncmp(text + read_at + 1, "gt", entity_len) == 0 &&
                       entity_len == 2) {
                out[written++] = '>';
            } else if (strncmp(text + read_at + 1, "quot", entity_len) == 0 &&
                       entity_len == 4) {
                out[written++] = '"';
            } else if (strncmp(text + read_at + 1, "apos", entity_len) == 0 &&
                       entity_len == 4) {
                out[written++] = '\'';
            } else if (entity_len >= 2 && text[read_at + 1] == '#') {
                long code = 0;
                size_t k = read_at + 2;
                bool hex = text[k] == 'x' || text[k] == 'X';

                if (hex) {
                    k++;
                }
                for (; k < (size_t)(semi - text); k++) {
                    char c = text[k];
                    int digit = c >= '0' && c <= '9'   ? c - '0'
                                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                       : -1;

                    if (digit < 0) {
                        break;
                    }
                    code = code * (hex ? 16 : 10) + digit;
                }
                if (code > 0 && code < 0x80) {
                    out[written++] = (char)code;
                }
            } else {
                out[written++] = text[read_at]; /* leave it as it came */
            }
            read_at = (size_t)(semi - text) + 1;
        }
    }
    out[written] = '\0';
    return written;
}

/* ============================================================== XML scan == */

bool ncl_mt_next_element(const char *text, size_t len, const char *name,
                         const ncl_mt_slice *from, ncl_mt_slice *tag,
                         ncl_mt_slice *content, size_t *next)
{
    size_t at = from != NULL ? from->len : 0;

    if (text == NULL || name == NULL) {
        return false;
    }
    while (at + 1 < len) {
        const char *open = memchr(text + at, '<', len - at);
        const char *gt;
        const char *local;
        bool self_closing;

        if (open == NULL) {
            return false;
        }
        at = (size_t)(open - text) + 1;
        if (text[at] == '/' || text[at] == '?' || text[at] == '!') {
            continue; /* a closing tag, a declaration or a comment */
        }
        gt = memchr(text + at, '>', len - at);
        if (gt == NULL) {
            return false;
        }
        /* "prefix:name" is the same element as "name" (§6.4). */
        local = text + at;
        {
            /* A namespace prefix sits in the element name, so stop at the
             * first blank: a ':' inside an attribute value (a timestamp, say)
             * is not one. */
            const char *space = memchr(local, ' ', (size_t)(gt - local));
            const char *limit = space != NULL ? space : gt;
            const char *colon = memchr(local, ':', (size_t)(limit - local));

            if (colon != NULL) {
                local = colon + 1;
            }
        }
        if (!name_matches(local, gt, name)) {
            at = (size_t)(gt - text) + 1;
            continue;
        }
        self_closing = gt[-1] == '/';
        if (tag != NULL) {
            tag->text = text + at;
            tag->len = (size_t)(gt - local) + (size_t)(local - (text + at)) -
                       (self_closing ? 1u : 0u);
        }
        if (content != NULL) {
            content->text = "";
            content->len = 0;
        }
        {
            size_t after = (size_t)(gt - text) + 1;

            if (!self_closing) {
                /* The value runs up to the matching closing tag. */
                ncl_mt_slice inner;

                if (element_content(text, len, (size_t)(gt - text) + 1, name,
                                    &inner, &after) &&
                    content != NULL) {
                    *content = inner;
                }
            }
            if (next != NULL) {
                *next = after;
            }
        }
        return true;
    }
    return false;
}

/** Value of the attribute @p name of @p tag, decoded. */
static bool tag_attribute(const ncl_mt_slice *tag, const char *name, char *out,
                          size_t out_len)
{
    size_t name_len;
    const char *at;
    const char *end;

    if (tag == NULL || tag->text == NULL || name == NULL || out == NULL) {
        return false;
    }
    name_len = strlen(name);
    at = tag->text;
    end = tag->text + tag->len;
    out[0] = '\0';
    while (at < end) {
        const char *eq;
        char quote;
        const char *value;
        size_t value_len;

        while (at < end && (is_space(*at) || *at == '/')) {
            at++;
        }
        if ((size_t)(end - at) <= name_len || strncmp(at, name, name_len) != 0 ||
            (at[name_len] != '=' && !is_space(at[name_len]))) {
            const char *space = memchr(at, ' ', (size_t)(end - at));

            if (space == NULL) {
                return false;
            }
            at = space + 1;
            continue;
        }
        eq = at + name_len;
        while (eq < end && is_space(*eq)) {
            eq++;
        }
        if (eq >= end || *eq != '=') {
            at = eq + 1;
            continue;
        }
        eq++;
        while (eq < end && is_space(*eq)) {
            eq++;
        }
        if (eq >= end) {
            return false;
        }
        quote = *eq;
        if (quote != '"' && quote != '\'') {
            return false;
        }
        eq++;
        value = eq;
        while (eq < end && *eq != quote) {
            eq++;
        }
        if (eq >= end) {
            return false;
        }
        value_len = (size_t)(eq - value);
        (void)decode_entities(value, value_len, out, out_len);
        return true;
    }
    return false;
}

bool ncl_mt_attr(const ncl_mt_slice *tag, const char *name, char *out,
                 size_t out_len)
{
    return tag_attribute(tag, name, out, out_len);
}

char *ncl_mt_text(const ncl_mt_slice *content)
{
    char buffer[2048];
    size_t len;
    size_t start = 0;
    size_t end;

    if (content == NULL || content->text == NULL) {
        return ncl_strdup("");
    }
    len = content->len < sizeof(buffer) ? content->len : sizeof(buffer) - 1;
    /* Blanks around the value are formatting, not data. */
    while (start < len && is_space(content->text[start])) {
        start++;
    }
    end = len;
    while (end > start && is_space(content->text[end - 1])) {
        end--;
    }
    decode_entities(content->text + start, end - start, buffer, sizeof(buffer));
    return ncl_strdup(buffer);
}

/* ================================================================ /probe == */

static char *tag_attr_dup(const ncl_mt_slice *tag, const char *name)
{
    char buffer[512];

    if (!tag_attribute(tag, name, buffer, sizeof(buffer))) {
        return NULL;
    }
    return ncl_strdup(buffer);
}

static ncl_mt_data_item *probe_push(ncl_mt_probe *probe)
{
    ncl_mt_data_item *item;

    if (probe->count == probe->capacity) {
        size_t capacity = probe->capacity == 0 ? 16 : probe->capacity * 2;
        ncl_mt_data_item *grown = (ncl_mt_data_item *)ncl_mem_realloc(
            probe->items, capacity * sizeof(*grown));

        if (grown == NULL) {
            return NULL;
        }
        probe->items = grown;
        probe->capacity = capacity;
    }
    item = &probe->items[probe->count];
    memset(item, 0, sizeof(*item));
    return item;
}

void ncl_mt_probe_init(ncl_mt_probe *probe)
{
    if (probe != NULL) {
        memset(probe, 0, sizeof(*probe));
    }
}

void ncl_mt_probe_free(ncl_mt_probe *probe)
{
    size_t i;

    if (probe == NULL) {
        return;
    }
    for (i = 0; i < probe->count; i++) {
        ncl_free_safe(probe->items[i].id);
        ncl_free_safe(probe->items[i].type);
        ncl_free_safe(probe->items[i].sub_type);
        ncl_free_safe(probe->items[i].category);
        ncl_free_safe(probe->items[i].units);
        ncl_free_safe(probe->items[i].name);
    }
    ncl_free_safe(probe->items);
    ncl_free_safe(probe->device_name);
    ncl_free_safe(probe->device_uuid);
    ncl_free_safe(probe->model);
    memset(probe, 0, sizeof(*probe));
}

ncl_err ncl_mt_probe_parse(const char *xml, size_t len, ncl_mt_probe *out,
                           char *err, size_t err_len)
{
    ncl_mt_slice from;
    ncl_mt_slice tag;
    ncl_mt_slice content;
    size_t next = 0;
    size_t found = 0;

    if (xml == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_mt_probe_init(out);
    if (len == 0) {
        len = strlen(xml);
    }
    from.text = xml;
    from.len = 0;
    if (ncl_mt_next_element(xml, len, "Device", &from, &tag, &content, &next)) {
        out->device_name = tag_attr_dup(&tag, "name");
        out->device_uuid = tag_attr_dup(&tag, "uuid");
    }
    from.len = 0;
    if (ncl_mt_next_element(xml, len, "Description", &from, &tag, &content,
                            &next)) {
        out->model = ncl_mt_text(&content);
    }
    from.len = 0;
    while (ncl_mt_next_element(xml, len, "DataItem", &from, &tag, &content,
                               &next)) {
        ncl_mt_data_item *item;

        from.len = next;
        item = probe_push(out);
        if (item == NULL) {
            ncl_mt_probe_free(out);
            return NCL_ERR_NOMEM;
        }
        item->id = tag_attr_dup(&tag, "id");
        item->type = tag_attr_dup(&tag, "type");
        item->sub_type = tag_attr_dup(&tag, "subType");
        item->category = tag_attr_dup(&tag, "category");
        item->units = tag_attr_dup(&tag, "units");
        item->name = tag_attr_dup(&tag, "name");
        out->count++;
        if (item->id != NULL) {
            found++;
        }
    }
    if (found == 0) {
        if (err != NULL) {
            snprintf(err, err_len, "the probe document carries no DataItem");
        }
        ncl_mt_probe_free(out);
        return NCL_DRV_ERR_PROTOCOL(0x70);
    }
    return NCL_OK;
}

const ncl_mt_data_item *ncl_mt_probe_find(const ncl_mt_probe *probe,
                                          const char *id)
{
    size_t i;

    if (probe == NULL || id == NULL) {
        return NULL;
    }
    for (i = 0; i < probe->count; i++) {
        if (probe->items[i].id != NULL && strcmp(probe->items[i].id, id) == 0) {
            return &probe->items[i];
        }
    }
    return NULL;
}

ncl_json *ncl_mt_probe_to_json(const ncl_mt_probe *probe)
{
    ncl_json *root;
    ncl_json *items;
    size_t i;

    if (probe == NULL) {
        return NULL;
    }
    root = ncl_json_new_object();
    items = ncl_json_new_array();
    if (root == NULL || items == NULL) {
        ncl_json_free(root);
        ncl_json_free(items);
        return NULL;
    }
    if (probe->device_name != NULL) {
        (void)ncl_json_obj_set_string(root, "device", probe->device_name);
    }
    if (probe->model != NULL) {
        (void)ncl_json_obj_set_string(root, "model", probe->model);
    }
    for (i = 0; i < probe->count; i++) {
        const ncl_mt_data_item *item = &probe->items[i];
        ncl_json *entry = ncl_json_new_object();

        if (entry == NULL) {
            continue;
        }
        (void)ncl_json_obj_set_string(entry, "id", item->id);
        if (item->type != NULL) {
            (void)ncl_json_obj_set_string(entry, "type", item->type);
        }
        if (item->sub_type != NULL) {
            (void)ncl_json_obj_set_string(entry, "subType", item->sub_type);
        }
        if (item->category != NULL) {
            (void)ncl_json_obj_set_string(entry, "category", item->category);
        }
        if (item->units != NULL) {
            (void)ncl_json_obj_set_string(entry, "units", item->units);
        }
        (void)ncl_json_arr_push(items, entry);
    }
    (void)ncl_json_obj_set(root, "dataItems", items);
    return root;
}

/* ============================================================== /current == */

/* The elements /current may carry, with the category they belong to. The probe
 * is authoritative when it names the item; this list is the fallback. */
static const struct {
    const char *name;
    const char *category;
} kValueElements[] = {
    {"Position", "SAMPLE"},       {"PathFeedrate", "SAMPLE"},
    {"RotaryVelocity", "SAMPLE"}, {"Load", "SAMPLE"},
    {"Temperature", "SAMPLE"},    {"PartCount", "SAMPLE"},
    {"Execution", "EVENT"},       {"ControllerMode", "EVENT"},
    {"Program", "EVENT"},         {"Block", "EVENT"},
    {"ToolNumber", "EVENT"},      {"ToolId", "EVENT"},
    {"Availability", "EVENT"},    {"Message", "EVENT"},
    {"Alarm", "CONDITION"},       {"Warning", "CONDITION"},
    {"Fault", "CONDITION"},       {"Condition", "CONDITION"},
    {"Normal", "CONDITION"},      {"Unavailable", "CONDITION"},
};

/** Container elements: they hold readings rather than being one. */
static bool is_container(const char *name)
{
    return strcmp(name, "Header") == 0 || strcmp(name, "Streams") == 0 ||
           strcmp(name, "DeviceStream") == 0 ||
           strcmp(name, "ComponentStream") == 0 ||
           strcmp(name, "Samples") == 0 || strcmp(name, "Events") == 0;
    /* "Condition" is both a block and a value element, so it is not listed:
     * the presence of a dataItemId decides, which current_add() checks. */
}

static const char *category_of_element(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(kValueElements) / sizeof(kValueElements[0]); i++) {
        if (strcmp(kValueElements[i].name, name) == 0) {
            return kValueElements[i].category;
        }
    }
    return "SAMPLE";
}

void ncl_mt_current_init(ncl_mt_current *current)
{
    if (current != NULL) {
        memset(current, 0, sizeof(*current));
    }
}

void ncl_mt_current_free(ncl_mt_current *current)
{
    size_t i;

    if (current == NULL) {
        return;
    }
    for (i = 0; i < current->count; i++) {
        ncl_free_safe(current->readings[i].item_id);
        ncl_free_safe(current->readings[i].type);
        ncl_free_safe(current->readings[i].category);
        ncl_free_safe(current->readings[i].value);
        ncl_free_safe(current->readings[i].timestamp);
        ncl_free_safe(current->readings[i].native_code);
        ncl_free_safe(current->readings[i].severity);
    }
    ncl_free_safe(current->readings);
    memset(current, 0, sizeof(*current));
}

static ncl_mt_reading *current_push(ncl_mt_current *current)
{
    ncl_mt_reading *reading;

    if (current->count == current->capacity) {
        size_t capacity = current->capacity == 0 ? 32 : current->capacity * 2;
        ncl_mt_reading *grown = (ncl_mt_reading *)ncl_mem_realloc(
            current->readings, capacity * sizeof(*grown));

        if (grown == NULL) {
            return NULL;
        }
        current->readings = grown;
        current->capacity = capacity;
    }
    reading = &current->readings[current->count];
    memset(reading, 0, sizeof(*reading));
    return reading;
}

/** Add one reading of the element @p name whose tag is @p tag. */
static ncl_err current_add(const char *id, const char *name,
                           const ncl_mt_slice *tag, const ncl_mt_slice *content,
                           const ncl_mt_probe *probe, ncl_mt_current *out)
{
    char text[64];
    ncl_mt_reading *reading;
    const ncl_mt_data_item *known;
    long long sequence = 0;

    reading = current_push(out);
    if (reading == NULL) {
        return NCL_ERR_NOMEM;
    }
    reading->item_id = ncl_strdup(id);
    reading->type = ncl_strdup(name);
    reading->value = ncl_mt_text(content);
    reading->timestamp = tag_attr_dup(tag, "timestamp");
    reading->native_code = tag_attr_dup(tag, "nativeCode");
    reading->severity = tag_attr_dup(tag, "severity");
    if (tag_attribute(tag, "sequence", text, sizeof(text))) {
        sequence = strtoll(text, NULL, 10);
        reading->sequence = sequence;
        if (out->first_sequence == 0 || sequence < out->first_sequence) {
            out->first_sequence = sequence;
        }
        if (sequence > out->last_sequence) {
            out->last_sequence = sequence;
        }
    }
    /* The probe knows the category; the element name is the fallback. */
    known = probe != NULL ? ncl_mt_probe_find(probe, id) : NULL;
    reading->category = ncl_strdup(known != NULL && known->category != NULL
                                       ? known->category
                                       : category_of_element(name));
    out->count++;
    return NCL_OK;
}

ncl_err ncl_mt_current_parse(const char *xml, size_t len,
                             const ncl_mt_probe *probe, ncl_mt_current *out,
                             char *err, size_t err_len)
{
    size_t at = 0;

    if (xml == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_mt_current_init(out);
    if (len == 0) {
        len = strlen(xml);
    }
    /* Every element that carries a dataItemId is one reading, wherever it sits
     * in the tree (§3.2): walking the document flat is enough and immune to
     * the component nesting an agent chooses. */
    while (at + 1 < len) {
        const char *open = memchr(xml + at, '<', len - at);
        const char *gt;
        const char *local;
        char name[64];
        size_t name_len = 0;
        ncl_mt_slice tag;
        ncl_mt_slice content;
        size_t next = 0;
        bool self_closing;
        char id[512];

        if (open == NULL) {
            break;
        }
        at = (size_t)(open - xml) + 1;
        if (xml[at] == '/' || xml[at] == '?' || xml[at] == '!') {
            continue;
        }
        gt = memchr(xml + at, '>', len - at);
        if (gt == NULL) {
            break;
        }
        local = xml + at;
        {
            /* The prefix lives in the name itself: stop at the first blank so
             * a ':' in an attribute value cannot be taken for one. */
            const char *space = memchr(local, ' ', (size_t)(gt - local));
            const char *limit = space != NULL ? space : gt;
            const char *colon = memchr(local, ':', (size_t)(limit - local));

            if (colon != NULL) {
                local = colon + 1;
            }
        }
        while (local + name_len < gt && name_len + 1 < sizeof(name) &&
               !is_space(local[name_len]) && local[name_len] != '/' &&
               local[name_len] != '>') {
            name[name_len] = local[name_len];
            name_len++;
        }
        name[name_len] = '\0';
        self_closing = gt[-1] == '/';
        tag.text = xml + at;
        tag.len = (size_t)(gt - (xml + at)) - (self_closing ? 1u : 0u);
        content.text = "";
        content.len = 0;
        next = (size_t)(gt - xml) + 1;
        /* A container, or an element we do not know: step into it and let the
         * readings inside be found on their own. */
        if (is_container(name) ||
            !tag_attribute(&tag, "dataItemId", id, sizeof(id))) {
            at = next;
            continue;
        }
        if (!self_closing) {
            /* The element's value: its own closing tag, not the next element
             * that happens to share the name. */
            (void)element_content(xml, len, next, name, &content, &next);
        }
        {
            ncl_err result = current_add(id, name, &tag, &content, probe, out);

            if (result != NCL_OK) {
                ncl_mt_current_free(out);
                return result;
            }
        }
        at = next;
    }
    if (out->count == 0 && err != NULL) {
        snprintf(err, err_len, "the current document carries no reading");
    }
    return NCL_OK;
}

const ncl_mt_reading *ncl_mt_current_find(const ncl_mt_current *current,
                                          const char *id)
{
    size_t i;

    if (current == NULL || id == NULL) {
        return NULL;
    }
    for (i = 0; i < current->count; i++) {
        if (current->readings[i].item_id != NULL &&
            strcmp(current->readings[i].item_id, id) == 0) {
            return &current->readings[i];
        }
    }
    return NULL;
}

bool ncl_mt_value_is_unavailable(const char *value)
{
    return value == NULL || value[0] == '\0' ||
           ncl_streq_ignore_case(value, "UNAVAILABLE") ||
           ncl_streq_ignore_case(value, "UNAVAILABLE_VALUE");
}
