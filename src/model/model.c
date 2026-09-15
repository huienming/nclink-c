/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* NC-Link core - device data model implementation. */
#include "nclink/ncl_model.h"

#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_general.h"
#include "nclink/ncl_logger.h"

const char *ncl_node_type_name(ncl_node_type type)
{
    switch (type) {
    case NCL_NODE_BASE: return "base";
    case NCL_NODE_ROOT: return "root";
    case NCL_NODE_DEVICE: return "device";
    case NCL_NODE_COMPONENT: return "component";
    case NCL_NODE_DATA_ITEM: return "dataItem";
    case NCL_NODE_CONFIG: return "config";
    default: return "base";
    }
}

const char *ncl_upload_type_name(ncl_upload_type type)
{
    return type == NCL_UPLOAD_EVENT ? "Event" : "Normal";
}

bool ncl_upload_type_parse(const char *text, ncl_upload_type *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }
    if (strcmp(text, "Normal") == 0) {
        *out = NCL_UPLOAD_NORMAL;
        return true;
    }
    if (strcmp(text, "Event") == 0) {
        *out = NCL_UPLOAD_EVENT;
        return true;
    }
    return false;
}

/* ============================================================== helpers === */

static ncl_err ncl_node_replace_str(char **slot, const char *value)
{
    char *copy = value != NULL ? ncl_strdup(value) : NULL;
    if (value != NULL && copy == NULL) {
        return NCL_ERR_NOMEM;
    }
    free(*slot);
    *slot = copy;
    return NCL_OK;
}

static void ncl_ptrvec_own_free(void *p)
{
    ncl_node_free((ncl_node *)p);
}

static void ncl_sample_ref_dtor(void *p)
{
    ncl_sample_ref_free((ncl_sample_ref *)p);
}

/* ======================================================= sample channels === */

ncl_sample_params *ncl_sample_params_new(void)
{
    ncl_sample_params *p = (ncl_sample_params *)calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    ncl_strvec_init(&p->indexes);
    ncl_strvec_init(&p->keys);
    return p;
}

void ncl_sample_params_free(ncl_sample_params *p)
{
    if (p == NULL) {
        return;
    }
    ncl_strvec_free(&p->indexes);
    ncl_strvec_free(&p->keys);
    free(p);
}

ncl_sample_params *ncl_sample_params_clone(const ncl_sample_params *p)
{
    ncl_sample_params *copy;
    size_t i;
    if (p == NULL) {
        return NULL;
    }
    copy = ncl_sample_params_new();
    if (copy == NULL) {
        return NULL;
    }
    for (i = 0; i < ncl_strvec_len(&p->indexes); i++) {
        ncl_strvec_push(&copy->indexes, ncl_strvec_at(&p->indexes, i));
    }
    for (i = 0; i < ncl_strvec_len(&p->keys); i++) {
        ncl_strvec_push(&copy->keys, ncl_strvec_at(&p->keys, i));
    }
    copy->has_offset = p->has_offset;
    copy->offset = p->offset;
    copy->has_length = p->has_length;
    copy->length = p->length;
    return copy;
}

bool ncl_sample_params_is_valid(const ncl_sample_params *p)
{
    if (p == NULL) {
        return true;
    }
    /* Not both indexes and keys may be present. */
    return !(ncl_strvec_len(&p->indexes) > 0 && ncl_strvec_len(&p->keys) > 0);
}

ncl_json *ncl_sample_params_to_json(const ncl_sample_params *p)
{
    ncl_json *j;
    if (p == NULL) {
        return NULL;
    }
    j = ncl_json_new_object();
    if (j == NULL) {
        return NULL;
    }
    if (ncl_strvec_len(&p->indexes) > 0) {
        ncl_json_obj_set(j, "indexes", ncl_strvec_to_json(&p->indexes));
    }
    if (ncl_strvec_len(&p->keys) > 0) {
        ncl_json_obj_set(j, "keys", ncl_strvec_to_json(&p->keys));
    }
    if (p->has_offset) {
        ncl_json_obj_set_int(j, "offset", p->offset);
    }
    if (p->has_length) {
        ncl_json_obj_set_int(j, "length", p->length);
    }
    return j;
}

ncl_sample_params *ncl_sample_params_from_json(const ncl_json *j)
{
    ncl_sample_params *p;
    ncl_json *arr;
    size_t i;

    if (j == NULL || ncl_json_type_of(j) != NCL_JSON_OBJECT) {
        return NULL;
    }
    p = ncl_sample_params_new();
    if (p == NULL) {
        return NULL;
    }
    arr = ncl_json_obj_get(j, "indexes");
    for (i = 0; i < ncl_json_arr_len(arr); i++) {
        const char *s = ncl_json_as_string(ncl_json_arr_get(arr, i));
        if (s != NULL) {
            ncl_strvec_push(&p->indexes, s);
        }
    }
    arr = ncl_json_obj_get(j, "keys");
    for (i = 0; i < ncl_json_arr_len(arr); i++) {
        const char *s = ncl_json_as_string(ncl_json_arr_get(arr, i));
        if (s != NULL) {
            ncl_strvec_push(&p->keys, s);
        }
    }
    if (ncl_json_obj_has(j, "offset")) {
        p->has_offset = ncl_json_as_int(ncl_json_obj_get(j, "offset"), &p->offset);
    }
    if (ncl_json_obj_has(j, "length")) {
        p->has_length = ncl_json_as_int(ncl_json_obj_get(j, "length"), &p->length);
    }
    return p;
}

ncl_sample_ref *ncl_sample_ref_new(const char *id)
{
    ncl_sample_ref *ref = (ncl_sample_ref *)calloc(1, sizeof(*ref));
    if (ref == NULL) {
        return NULL;
    }
    if (id != NULL) {
        ref->id = ncl_strdup(id);
        if (ref->id == NULL) {
            free(ref);
            return NULL;
        }
    }
    return ref;
}

void ncl_sample_ref_free(ncl_sample_ref *ref)
{
    if (ref == NULL) {
        return;
    }
    free(ref->id);
    ncl_sample_params_free(ref->params);
    free(ref->path);
    free(ref);
}

bool ncl_sample_ref_is_valid(const ncl_sample_ref *ref)
{
    if (ref == NULL) {
        return false;
    }
    /* The id must be non-empty and the params valid when present. */
    if (ncl_str_is_empty(ref->id)) {
        return false;
    }
    return ref->params == NULL || ncl_sample_params_is_valid(ref->params);
}

/* A multi element list is rendered as "[1, 2]". */
static char *ncl_strvec_to_list_string(const ncl_strvec *v)
{
    ncl_strbuf sb;
    size_t i;
    ncl_strbuf_init(&sb);
    ncl_strbuf_putc(&sb, '[');
    for (i = 0; i < ncl_strvec_len(v); i++) {
        if (i > 0) {
            ncl_strbuf_puts(&sb, ", ");
        }
        ncl_strbuf_puts(&sb, ncl_strvec_at(v, i));
    }
    ncl_strbuf_putc(&sb, ']');
    return ncl_strbuf_detach(&sb);
}

char *ncl_sample_ref_path(ncl_sample_ref *ref)
{
    char *detail = NULL;
    char *result = NULL;

    if (ref == NULL) {
        return NULL;
    }
    if (ref->path != NULL) {
        return ncl_strdup(ref->path);
    }
    if (ref->node == NULL) {
        return NULL;
    }

    if (ref->node->data_type == NULL) {
        ref->path = ncl_strdup(ncl_node_path(ref->node));
        return ncl_strdup(ref->path);
    }

    if (ncl_streq_ignore_case(ref->node->data_type, NCL_DATA_TYPE_HASH)) {
        size_t count = ncl_strvec_len(ref->params != NULL ? &ref->params->keys : NULL);
        if (ref->params == NULL || count == 0) {
            return NULL;
        }
        if (count == 1) {
            detail = ncl_strdup(ncl_strvec_at(&ref->params->keys, 0));
        } else {
            detail = ncl_strvec_to_list_string(&ref->params->keys);
        }
        if (detail == NULL) {
            return NULL;
        }
        if (ncl_asprintf(&result, "%s%s%s%c%s", ncl_node_path(ref->node),
                         NCL_DATA_TYPE_SEPARATOR, NCL_DATA_TYPE_HASH,
                         NCL_DATA_CHILD_SEPARATOR[0], detail) != NCL_OK) {
            result = NULL;
        }
        free(detail);
        ref->path = result != NULL ? ncl_strdup(result) : NULL;
        return result;
    }

    if (ncl_streq_ignore_case(ref->node->data_type, NCL_DATA_TYPE_LIST)) {
        size_t count = ncl_strvec_len(ref->params != NULL ? &ref->params->indexes : NULL);
        if (ref->params == NULL || count == 0) {
            return NULL;
        }
        if (count == 1) {
            detail = ncl_strdup(ncl_strvec_at(&ref->params->indexes, 0));
        } else {
            detail = ncl_strvec_to_list_string(&ref->params->indexes);
        }
        if (detail == NULL) {
            return NULL;
        }
        if (ncl_asprintf(&result, "%s%s%s%c%s", ncl_node_path(ref->node),
                         NCL_DATA_TYPE_SEPARATOR, NCL_DATA_TYPE_LIST,
                         NCL_DATA_CHILD_SEPARATOR[0], detail) != NCL_OK) {
            result = NULL;
        }
        free(detail);
        ref->path = result != NULL ? ncl_strdup(result) : NULL;
        return result;
    }

    return NULL;
}

/* =============================================================== node ==== */

ncl_node *ncl_node_new(ncl_node_type type)
{
    ncl_node *node = (ncl_node *)calloc(1, sizeof(ncl_node));
    if (node == NULL) {
        return NULL;
    }
    node->type = type;
    ncl_ptrvec_init(&node->configs, ncl_ptrvec_own_free);
    ncl_ptrvec_init(&node->data_items, ncl_ptrvec_own_free);
    ncl_ptrvec_init(&node->components, ncl_ptrvec_own_free);
    ncl_ptrvec_init(&node->devices, ncl_ptrvec_own_free);
    ncl_ptrvec_init(&node->sample_items, ncl_sample_ref_dtor);
    return node;
}

void ncl_node_free(ncl_node *node)
{
    if (node == NULL) {
        return;
    }
    free(node->name);
    free(node->id);
    free(node->node_type_name);
    free(node->description);
    free(node->path);
    free(node->number);
    free(node->data_type);
    ncl_json_free(node->value);
    free(node->mapping);
    free(node->value_type);
    free(node->source);
    free(node->version);
    free(node->guid);
    free(node->unique_id);

    ncl_ptrvec_free(&node->configs);
    ncl_ptrvec_free(&node->data_items);
    ncl_ptrvec_free(&node->components);
    ncl_ptrvec_free(&node->devices);
    ncl_ptrvec_free(&node->sample_items);
    free(node);
}

ncl_node *ncl_node_clone(const ncl_node *node, bool shallow_children)
{
    ncl_node *copy;
    size_t i;

    if (node == NULL) {
        return NULL;
    }
    copy = ncl_node_new(node->type);
    if (copy == NULL) {
        return NULL;
    }

    ncl_node_set_name(copy, node->name);
    ncl_node_set_id(copy, node->id);
    ncl_node_set_type_name(copy, node->node_type_name);
    copy->has_settable = node->has_settable;
    copy->settable = node->settable;
    ncl_node_set_description(copy, node->description);
    ncl_node_set_number(copy, node->number);
    ncl_node_set_data_type(copy, node->data_type);
    if (node->value != NULL) {
        ncl_node_set_value(copy, ncl_json_clone(node->value));
    }
    ncl_node_set_mapping(copy, node->mapping);
    ncl_node_set_value_type(copy, node->value_type);
    ncl_node_set_source(copy, node->source);
    ncl_node_set_version(copy, node->version);
    ncl_node_set_guid(copy, node->guid);
    ncl_node_set_unique_id(copy, node->unique_id);
    if (node->path != NULL) {
        copy->path = ncl_strdup(node->path);
    }

    copy->has_sample_interval = node->has_sample_interval;
    copy->sample_interval = node->sample_interval;
    copy->has_upload_interval = node->has_upload_interval;
    copy->upload_interval = node->upload_interval;
    copy->has_upload_type = node->has_upload_type;
    copy->upload_type = node->upload_type;

    for (i = 0; i < ncl_node_sample_count(node); i++) {
        const ncl_sample_ref *src = ncl_node_sample_at(node, i);
        ncl_sample_ref *ref = ncl_sample_ref_new(src->id);
        if (ref == NULL) {
            ncl_node_free(copy);
            return NULL;
        }
        ref->params = ncl_sample_params_clone(src->params);
        ref->node = src->node;
        if (ncl_node_add_sample_item(copy, ref) != NCL_OK) {
            ncl_sample_ref_free(ref);
            ncl_node_free(copy);
            return NULL;
        }
    }

    if (!shallow_children) {
        for (i = 0; i < ncl_ptrvec_len(&node->configs); i++) {
            ncl_node *child = ncl_node_clone((const ncl_node *)
                                                 ncl_ptrvec_at(&node->configs, i), false);
            if (child == NULL || ncl_node_add_config(copy, child) != NCL_OK) {
                ncl_node_free(copy);
                return NULL;
            }
        }
        for (i = 0; i < ncl_ptrvec_len(&node->data_items); i++) {
            ncl_node *child = ncl_node_clone((const ncl_node *)
                                                 ncl_ptrvec_at(&node->data_items, i), false);
            if (child == NULL || ncl_node_add_data_item(copy, child) != NCL_OK) {
                ncl_node_free(copy);
                return NULL;
            }
        }
        for (i = 0; i < ncl_ptrvec_len(&node->components); i++) {
            ncl_node *child = ncl_node_clone((const ncl_node *)
                                                 ncl_ptrvec_at(&node->components, i), false);
            if (child == NULL || ncl_node_add_component(copy, child) != NCL_OK) {
                ncl_node_free(copy);
                return NULL;
            }
        }
        for (i = 0; i < ncl_ptrvec_len(&node->devices); i++) {
            ncl_node *child = ncl_node_clone((const ncl_node *)
                                                 ncl_ptrvec_at(&node->devices, i), false);
            if (child == NULL || ncl_node_add_device(copy, child) != NCL_OK) {
                ncl_node_free(copy);
                return NULL;
            }
        }
    }
    return copy;
}

#define NCL_DEF_SETTER(fn, field)                                             \
    ncl_err fn(ncl_node *node, const char *value)                             \
    {                                                                         \
        if (node == NULL) {                                                   \
            return NCL_ERR_INVALID_ARG;                                       \
        }                                                                     \
        return ncl_node_replace_str(&node->field, value);                     \
    }

NCL_DEF_SETTER(ncl_node_set_name, name)
NCL_DEF_SETTER(ncl_node_set_id, id)
NCL_DEF_SETTER(ncl_node_set_description, description)
NCL_DEF_SETTER(ncl_node_set_number, number)
NCL_DEF_SETTER(ncl_node_set_data_type, data_type)
NCL_DEF_SETTER(ncl_node_set_mapping, mapping)
NCL_DEF_SETTER(ncl_node_set_value_type, value_type)
NCL_DEF_SETTER(ncl_node_set_version, version)
NCL_DEF_SETTER(ncl_node_set_guid, guid)
NCL_DEF_SETTER(ncl_node_set_unique_id, unique_id)

/* The root path is derived from the type, so keep it in sync here. */
ncl_err ncl_node_set_type_name(ncl_node *node, const char *value)
{
    ncl_err rc;
    if (node == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_node_replace_str(&node->node_type_name, value);
    if (rc != NCL_OK) {
        return rc;
    }
    if (node->type == NCL_NODE_ROOT) {
        char *built = NULL;
        if (ncl_asprintf(&built, "%s%s", NCL_PATH_SEPARATOR,
                         node->node_type_name != NULL ? node->node_type_name : "")
            != NCL_OK) {
            return NCL_ERR_NOMEM;
        }
        free(node->path);
        node->path = built;
    }
    return NCL_OK;
}

ncl_err ncl_node_set_source(ncl_node *node, const char *value)
{
    char *trimmed = NULL;
    ncl_err rc;
    if (node == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (value != NULL) {
        trimmed = ncl_str_trim_dup(value);
        if (trimmed == NULL) {
            return NCL_ERR_NOMEM;
        }
        if (trimmed[0] == '\0') {
            free(trimmed);
            trimmed = NULL;
        }
    }
    rc = ncl_node_replace_str(&node->source, trimmed);
    free(trimmed);
    return rc;
}

ncl_err ncl_node_set_settable(ncl_node *node, bool value)
{
    if (node == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    node->settable = value;
    node->has_settable = true;
    return NCL_OK;
}

ncl_err ncl_node_set_value(ncl_node *node, ncl_json *value)
{
    if (node == NULL) {
        ncl_json_free(value);
        return NCL_ERR_INVALID_ARG;
    }
    ncl_json_free(node->value);
    node->value = value;
    return NCL_OK;
}

/* ----------------------------------------------------------- children ---- */

ncl_err ncl_node_add_config(ncl_node *parent, ncl_node *config)
{
    if (parent == NULL || config == NULL) {
        ncl_node_free(config);
        return NCL_ERR_INVALID_ARG;
    }
    config->parent = parent;
    return ncl_ptrvec_push_owned(&parent->configs, config);
}

ncl_err ncl_node_add_data_item(ncl_node *parent, ncl_node *item)
{
    if (parent == NULL || item == NULL) {
        ncl_node_free(item);
        return NCL_ERR_INVALID_ARG;
    }
    item->parent = parent;
    return ncl_ptrvec_push_owned(&parent->data_items, item);
}

ncl_err ncl_node_add_component(ncl_node *parent, ncl_node *component)
{
    if (parent == NULL || component == NULL) {
        ncl_node_free(component);
        return NCL_ERR_INVALID_ARG;
    }
    component->parent = parent;
    return ncl_ptrvec_push_owned(&parent->components, component);
}

ncl_err ncl_node_add_device(ncl_node *root, ncl_node *device)
{
    if (root == NULL || device == NULL) {
        ncl_node_free(device);
        return NCL_ERR_INVALID_ARG;
    }
    device->parent = root;
    return ncl_ptrvec_push_owned(&root->devices, device);
}

ncl_err ncl_node_add_sample_item(ncl_node *config, ncl_sample_ref *ref)
{
    if (config == NULL || ref == NULL) {
        ncl_sample_ref_free(ref);
        return NCL_ERR_INVALID_ARG;
    }
    return ncl_ptrvec_push_owned(&config->sample_items, ref);
}

ncl_err ncl_node_add_child(ncl_node *parent, ncl_node *child)
{
    if (parent == NULL || child == NULL) {
        ncl_node_free(child);
        return NCL_ERR_INVALID_ARG;
    }
    switch (child->type) {
    case NCL_NODE_CONFIG: return ncl_node_add_config(parent, child);
    case NCL_NODE_DATA_ITEM: return ncl_node_add_data_item(parent, child);
    case NCL_NODE_COMPONENT: return ncl_node_add_component(parent, child);
    case NCL_NODE_DEVICE: return ncl_node_add_device(parent, child);
    default:
        /* An unknown parent kind has nowhere to put the child; drop it. */
        ncl_node_free(child);
        return NCL_ERR_INVALID_TYPE;
    }
}

size_t ncl_node_child_count(const ncl_node *node)
{
    if (node == NULL) {
        return 0;
    }
    return ncl_ptrvec_len(&node->configs) + ncl_ptrvec_len(&node->data_items) +
           ncl_ptrvec_len(&node->components) + ncl_ptrvec_len(&node->devices);
}

ncl_node *ncl_node_config_at(const ncl_node *node, size_t index)
{
    return node == NULL ? NULL : (ncl_node *)ncl_ptrvec_at(&node->configs, index);
}

ncl_node *ncl_node_data_item_at(const ncl_node *node, size_t index)
{
    return node == NULL ? NULL : (ncl_node *)ncl_ptrvec_at(&node->data_items, index);
}

ncl_node *ncl_node_component_at(const ncl_node *node, size_t index)
{
    return node == NULL ? NULL
                        : (ncl_node *)ncl_ptrvec_at(&node->components, index);
}

ncl_node *ncl_node_device_at(const ncl_node *node, size_t index)
{
    return node == NULL ? NULL : (ncl_node *)ncl_ptrvec_at(&node->devices, index);
}

size_t ncl_node_sample_count(const ncl_node *node)
{
    return node == NULL ? 0 : ncl_ptrvec_len(&node->sample_items);
}

ncl_sample_ref *ncl_node_sample_at(const ncl_node *node, size_t index)
{
    return node == NULL ? NULL
                        : (ncl_sample_ref *)ncl_ptrvec_at(&node->sample_items, index);
}

ncl_node *ncl_node_find_by_id(const ncl_node *node, const char *id)
{
    size_t i;
    if (node == NULL || id == NULL) {
        return NULL;
    }
    for (i = 0; i < ncl_ptrvec_len(&node->configs); i++) {
        ncl_node *candidate = ncl_node_config_at(node, i);
        if (candidate->id != NULL && strcmp(candidate->id, id) == 0) {
            return candidate;
        }
    }
    for (i = 0; i < ncl_ptrvec_len(&node->data_items); i++) {
        ncl_node *candidate = ncl_node_data_item_at(node, i);
        if (candidate->id != NULL && strcmp(candidate->id, id) == 0) {
            return candidate;
        }
    }
    for (i = 0; i < ncl_ptrvec_len(&node->components); i++) {
        ncl_node *candidate = ncl_node_component_at(node, i);
        if (candidate->id != NULL && strcmp(candidate->id, id) == 0) {
            return candidate;
        }
        candidate = ncl_node_find_by_id(candidate, id);
        if (candidate != NULL) {
            return candidate;
        }
    }
    for (i = 0; i < ncl_ptrvec_len(&node->devices); i++) {
        ncl_node *candidate = ncl_node_device_at(node, i);
        if (candidate->id != NULL && strcmp(candidate->id, id) == 0) {
            return candidate;
        }
        candidate = ncl_node_find_by_id(candidate, id);
        if (candidate != NULL) {
            return candidate;
        }
    }
    return NULL;
}

bool ncl_node_is_sample_node(const ncl_node *node)
{
    return node != NULL && node->node_type_name != NULL &&
           strcmp(node->node_type_name, NCL_NODE_TYPE_SAMPLE_CHANNEL) == 0;
}

/* -------------------------------------------------------- paths/relations - */

const char *ncl_node_path(const ncl_node *node)
{
    if (node == NULL) {
        return NULL;
    }
    if (node->type == NCL_NODE_ROOT) {
        return node->path != NULL ? node->path : NCL_PATH_SEPARATOR;
    }
    return node->path;
}

static ncl_err ncl_node_build_path(ncl_node *node, const char *parent_path)
{
    const char *sep = NCL_PATH_SEPARATOR;
    char *built = NULL;

    if (node->number != NULL) {
        ncl_asprintf(&built, "%s%s%s%s%s", parent_path, sep,
                     node->node_type_name != NULL ? node->node_type_name : "",
                     NCL_PATH_TAG_SEPARATOR, node->number);
    } else {
        ncl_asprintf(&built, "%s%s%s", parent_path, sep,
                     node->node_type_name != NULL ? node->node_type_name : "");
    }
    if (built == NULL) {
        return NCL_ERR_NOMEM;
    }
    free(node->path);
    node->path = built;
    return NCL_OK;
}

ncl_err ncl_node_set_path(ncl_node *node, const char *parent_path)
{
    size_t i;
    const char *effective_parent;

    if (node == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (parent_path == NULL) {
        parent_path = "";
    }

    switch (node->type) {
    case NCL_NODE_ROOT:
        /* The root path is derived, nothing to store. */
        return NCL_OK;

    case NCL_NODE_DATA_ITEM:
    case NCL_NODE_CONFIG:
        /* setPath(): a device parent resets the prefix, and an
         * explicit source overrides the whole path. */
        effective_parent = parent_path;
        if (node->parent != NULL && node->parent->type == NCL_NODE_DEVICE) {
            effective_parent = "";
        }
        if (node->source != NULL && node->source[0] != '\0') {
            char *source_path;
            char *built = NULL;
            if (node->source[0] == NCL_PATH_SEPARATOR[0]) {
                source_path = ncl_strdup(node->source);
            } else {
                ncl_asprintf(&source_path, "%s%s", NCL_PATH_SEPARATOR, node->source);
            }
            if (source_path == NULL) {
                return NCL_ERR_NOMEM;
            }
            if (node->number != NULL && node->number[0] != '\0') {
                ncl_asprintf(&built, "%s%s%s%s%s", source_path, NCL_PATH_SEPARATOR,
                             node->node_type_name != NULL ? node->node_type_name : "",
                             NCL_PATH_TAG_SEPARATOR, node->number);
            } else {
                ncl_asprintf(&built, "%s%s%s", source_path, NCL_PATH_SEPARATOR,
                             node->node_type_name != NULL ? node->node_type_name : "");
            }
            free(source_path);
            if (built == NULL) {
                return NCL_ERR_NOMEM;
            }
            free(node->path);
            node->path = built;
            return NCL_OK;
        }
        return ncl_node_build_path(node, effective_parent);

    case NCL_NODE_COMPONENT:
    case NCL_NODE_DEVICE:
        effective_parent = parent_path;
        if (node->parent != NULL && node->parent->type == NCL_NODE_DEVICE) {
            effective_parent = "";
        }
        {
            ncl_err rc = ncl_node_build_path(node, effective_parent);
            if (rc != NCL_OK) {
                return rc;
            }
        }
        for (i = 0; i < ncl_ptrvec_len(&node->components); i++) {
            ncl_err rc = ncl_node_set_path(ncl_node_component_at(node, i),
                                           ncl_node_path(node));
            if (rc != NCL_OK) {
                return rc;
            }
        }
        for (i = 0; i < ncl_ptrvec_len(&node->configs); i++) {
            ncl_err rc = ncl_node_set_path(ncl_node_config_at(node, i),
                                           ncl_node_path(node));
            if (rc != NCL_OK) {
                return rc;
            }
        }
        for (i = 0; i < ncl_ptrvec_len(&node->data_items); i++) {
            ncl_err rc = ncl_node_set_path(ncl_node_data_item_at(node, i),
                                           ncl_node_path(node));
            if (rc != NCL_OK) {
                return rc;
            }
        }
        return NCL_OK;

    default:
        return ncl_node_build_path(node, parent_path);
    }
}

void ncl_node_build_relations(ncl_node *node)
{
    size_t i;
    if (node == NULL) {
        return;
    }
    for (i = 0; i < ncl_ptrvec_len(&node->configs); i++) {
        ncl_node_config_at(node, i)->parent = node;
    }
    for (i = 0; i < ncl_ptrvec_len(&node->data_items); i++) {
        ncl_node_data_item_at(node, i)->parent = node;
    }
    for (i = 0; i < ncl_ptrvec_len(&node->components); i++) {
        ncl_node *child = ncl_node_component_at(node, i);
        child->parent = node;
        ncl_node_build_relations(child);
    }
    for (i = 0; i < ncl_ptrvec_len(&node->devices); i++) {
        ncl_node *child = ncl_node_device_at(node, i);
        child->parent = node;
        ncl_node_build_relations(child);
    }
}

/* ----------------------------------------------------------- validation -- */

static bool ncl_config_is_valid_sample_channel(const ncl_node *node)
{
    size_t i;
    if (ncl_ptrvec_len(&node->sample_items) == 0 || !node->has_sample_interval ||
        !node->has_upload_interval) {
        return false;
    }
    for (i = 0; i < ncl_ptrvec_len(&node->sample_items); i++) {
        if (!ncl_sample_ref_is_valid(ncl_node_sample_at(node, i))) {
            return false;
        }
    }
    return true;
}

bool ncl_node_is_valid(const ncl_node *node)
{
    size_t i;
    if (node == NULL) {
        return false;
    }
    /* Rules common to every node kind */
    if (node->id == NULL || node->node_type_name == NULL) {
        return false;
    }

    switch (node->type) {
    case NCL_NODE_DATA_ITEM:
        return ncl_check_is_data_type_valid(node->data_type);

    case NCL_NODE_CONFIG:
        if (node->node_type_name != NULL &&
            strcmp(node->node_type_name, NCL_NODE_TYPE_SAMPLE_CHANNEL) == 0) {
            return ncl_config_is_valid_sample_channel(node);
        }
        return ncl_check_is_data_type_valid(node->data_type);

    case NCL_NODE_COMPONENT:
    case NCL_NODE_DEVICE:
        for (i = 0; i < ncl_ptrvec_len(&node->configs); i++) {
            if (!ncl_node_is_valid(ncl_node_config_at(node, i))) {
                return false;
            }
        }
        for (i = 0; i < ncl_ptrvec_len(&node->data_items); i++) {
            if (!ncl_node_is_valid(ncl_node_data_item_at(node, i))) {
                return false;
            }
        }
        for (i = 0; i < ncl_ptrvec_len(&node->components); i++) {
            if (!ncl_node_is_valid(ncl_node_component_at(node, i))) {
                return false;
            }
        }
        return true;

    case NCL_NODE_ROOT:
        if (ncl_ptrvec_len(&node->devices) < 1) {
            return false;
        }
        for (i = 0; i < ncl_ptrvec_len(&node->devices); i++) {
            if (!ncl_node_is_valid(ncl_node_device_at(node, i))) {
                return false;
            }
        }
        for (i = 0; i < ncl_ptrvec_len(&node->configs); i++) {
            if (!ncl_node_is_valid(ncl_node_config_at(node, i))) {
                return false;
            }
        }
        return true;

    default:
        return true;
    }
}

/* ========================================================= serialisation == */

/* Every setter below omits null valued fields: a NULL field and an absent flag are
 * not emitted at all. The emission order matches the property order fixed by the
 * specification:
 *   base    : name, id, type, settable, description
 *   dataItem: number, dataType, value, mapping, valueType, source
 *   component: number, configs, dataItems, components
 *   device  : version, guid
 *   root    : devices, configs, uniqueID
 *   config  : sampleInterval, ids, uploadInterval, uploadType
 */
static void ncl_node_write_base(const ncl_node *node, ncl_json *obj)
{
    if (node->name != NULL) {
        ncl_json_obj_set_string(obj, "name", node->name);
    }
    if (node->id != NULL) {
        ncl_json_obj_set_string(obj, "id", node->id);
    }
    if (node->node_type_name != NULL) {
        ncl_json_obj_set_string(obj, "type", node->node_type_name);
    }
    if (node->has_settable) {
        ncl_json_obj_set_bool(obj, "settable", node->settable);
    }
    if (node->description != NULL) {
        ncl_json_obj_set_string(obj, "description", node->description);
    }
}

static void ncl_node_write_data_item_fields(const ncl_node *node, ncl_json *obj)
{
    if (node->number != NULL) {
        ncl_json_obj_set_string(obj, "number", node->number);
    }
    if (node->data_type != NULL) {
        ncl_json_obj_set_string(obj, "dataType", node->data_type);
    }
    if (node->value != NULL) {
        ncl_json_obj_set(obj, "value", ncl_json_clone(node->value));
    }
    if (node->mapping != NULL) {
        ncl_json_obj_set_string(obj, "mapping", node->mapping);
    }
    if (node->value_type != NULL) {
        ncl_json_obj_set_string(obj, "valueType", node->value_type);
    }
    if (node->source != NULL) {
        ncl_json_obj_set_string(obj, "source", node->source);
    }
}

/* A component declares only "number" ahead of its child arrays. */
static void ncl_node_write_component_fields(const ncl_node *node, ncl_json *obj)
{
    if (node->number != NULL) {
        ncl_json_obj_set_string(obj, "number", node->number);
    }
}

static void ncl_node_write_children(ncl_json *obj, const char *key,
                                    const ncl_ptrvec *children)
{
    ncl_json *arr;
    size_t i;
    if (ncl_ptrvec_len(children) == 0) {
        return;
    }
    arr = ncl_json_new_array();
    if (arr == NULL) {
        return;
    }
    for (i = 0; i < ncl_ptrvec_len(children); i++) {
        ncl_json *child = ncl_node_to_json((const ncl_node *)ncl_ptrvec_at(children, i));
        if (child == NULL || ncl_json_arr_push(arr, child) != NCL_OK) {
            ncl_json_free(arr);
            return;
        }
    }
    ncl_json_obj_set(obj, key, arr);
}

ncl_json *ncl_node_to_json(const ncl_node *node)
{
    ncl_json *obj;

    if (node == NULL) {
        return ncl_json_new_null();
    }
    obj = ncl_json_new_object();
    if (obj == NULL) {
        return NULL;
    }
    ncl_node_write_base(node, obj);

    switch (node->type) {
    case NCL_NODE_ROOT:
        ncl_node_write_children(obj, "devices", &node->devices);
        ncl_node_write_children(obj, "configs", &node->configs);
        if (node->unique_id != NULL) {
            ncl_json_obj_set_string(obj, "uniqueID", node->unique_id);
        }
        break;

    case NCL_NODE_DEVICE:
        ncl_node_write_component_fields(node, obj);
        ncl_node_write_children(obj, "configs", &node->configs);
        ncl_node_write_children(obj, "dataItems", &node->data_items);
        ncl_node_write_children(obj, "components", &node->components);
        if (node->version != NULL) {
            ncl_json_obj_set_string(obj, "version", node->version);
        }
        if (node->guid != NULL) {
            ncl_json_obj_set_string(obj, "guid", node->guid);
        }
        break;

    case NCL_NODE_COMPONENT:
        ncl_node_write_component_fields(node, obj);
        ncl_node_write_children(obj, "configs", &node->configs);
        ncl_node_write_children(obj, "dataItems", &node->data_items);
        ncl_node_write_children(obj, "components", &node->components);
        break;

    case NCL_NODE_DATA_ITEM:
        ncl_node_write_data_item_fields(node, obj);
        break;

    case NCL_NODE_CONFIG:
        ncl_node_write_data_item_fields(node, obj);
        if (node->has_sample_interval) {
            ncl_json_obj_set_int(obj, "sampleInterval", node->sample_interval);
        }
        if (ncl_ptrvec_len(&node->sample_items) > 0) {
            ncl_json *arr = ncl_json_new_array();
            size_t i;
            if (arr != NULL) {
                for (i = 0; i < ncl_ptrvec_len(&node->sample_items); i++) {
                    const ncl_sample_ref *ref = ncl_node_sample_at(node, i);
                    ncl_json *entry = ncl_json_new_object();
                    if (entry == NULL) {
                        break;
                    }
                    ncl_json_obj_set_string(entry, "id", ref->id);
                    if (ref->params != NULL) {
                        ncl_json_obj_set(entry, "params",
                                         ncl_sample_params_to_json(ref->params));
                    }
                    if (ncl_json_arr_push(arr, entry) != NCL_OK) {
                        break;
                    }
                }
                ncl_json_obj_set(obj, "ids", arr);
            }
        }
        if (node->has_upload_interval) {
            ncl_json_obj_set_int(obj, "uploadInterval", node->upload_interval);
        }
        if (node->has_upload_type) {
            ncl_json_obj_set_string(obj, "uploadType",
                                    ncl_upload_type_name(node->upload_type));
        }
        break;

    default:
        break;
    }
    return obj;
}

char *ncl_node_write_string(const ncl_node *node)
{
    ncl_json *j = ncl_node_to_json(node);
    char *text;
    if (j == NULL) {
        return NULL;
    }
    text = ncl_json_write_string(j);
    ncl_json_free(j);
    return text;
}

/* ========================================================= deserialisation = */

ncl_node *ncl_node_from_json(const ncl_json *obj, ncl_node_type type);

static char *ncl_node_read_string(const ncl_json *obj, const char *key)
{
    const char *s = ncl_json_obj_get_string(obj, key);
    return s != NULL ? ncl_strdup(s) : NULL;
}

static ncl_err ncl_node_read_children(const ncl_json *obj, const char *key,
                                      ncl_node *parent, ncl_node_type child_type)
{
    ncl_json *arr = ncl_json_obj_get(obj, key);
    size_t i;
    for (i = 0; i < ncl_json_arr_len(arr); i++) {
        ncl_node *child = ncl_node_from_json(ncl_json_arr_get(arr, i), child_type);
        if (child == NULL) {
            /* A null element is skipped. */
            continue;
        }
        if (ncl_node_add_child(parent, child) != NCL_OK) {
            return NCL_ERR_NOMEM;
        }
    }
    return NCL_OK;
}

static ncl_node *ncl_config_from_json(const ncl_json *obj)
{
    ncl_node *node = ncl_node_new(NCL_NODE_CONFIG);
    ncl_json *ids;
    size_t i;
    const char *upload_type;

    if (node == NULL) {
        return NULL;
    }
    if (ncl_json_obj_has(obj, "sampleInterval")) {
        node->has_sample_interval =
            ncl_json_as_int(ncl_json_obj_get(obj, "sampleInterval"),
                            &node->sample_interval);
    }
    ids = ncl_json_obj_get(obj, "ids");
    for (i = 0; i < ncl_json_arr_len(ids); i++) {
        const ncl_json *entry = ncl_json_arr_get(ids, i);
        ncl_sample_ref *ref;
        const char *id = ncl_json_obj_get_string(entry, "id");
        if (id == NULL) {
            continue;
        }
        ref = ncl_sample_ref_new(id);
        if (ref == NULL) {
            ncl_node_free(node);
            return NULL;
        }
        ref->params = ncl_sample_params_from_json(ncl_json_obj_get(entry, "params"));
        if (ncl_node_add_sample_item(node, ref) != NCL_OK) {
            ncl_sample_ref_free(ref);
            ncl_node_free(node);
            return NULL;
        }
    }
    if (ncl_json_obj_has(obj, "uploadInterval")) {
        node->has_upload_interval =
            ncl_json_as_int(ncl_json_obj_get(obj, "uploadInterval"),
                            &node->upload_interval);
    }
    upload_type = ncl_json_obj_get_string(obj, "uploadType");
    if (upload_type != NULL) {
        node->has_upload_type = ncl_upload_type_parse(upload_type, &node->upload_type);
    }
    return node;
}

ncl_node *ncl_node_from_json(const ncl_json *obj, ncl_node_type type)
{
    ncl_node *node;

    if (obj == NULL || ncl_json_type_of(obj) != NCL_JSON_OBJECT) {
        return NULL;
    }

    if (type == NCL_NODE_CONFIG) {
        node = ncl_config_from_json(obj);
    } else {
        node = ncl_node_new(type);
    }
    if (node == NULL) {
        return NULL;
    }

    node->name = ncl_node_read_string(obj, "name");
    node->id = ncl_node_read_string(obj, "id");
    node->node_type_name = ncl_node_read_string(obj, "type");
    if (ncl_json_obj_has(obj, "settable")) {
        node->has_settable = ncl_json_as_bool(ncl_json_obj_get(obj, "settable"),
                                             &node->settable);
    }
    node->description = ncl_node_read_string(obj, "description");
    node->number = ncl_node_read_string(obj, "number");
    node->data_type = ncl_node_read_string(obj, "dataType");
    node->mapping = ncl_node_read_string(obj, "mapping");
    node->value_type = ncl_node_read_string(obj, "valueType");
    node->source = ncl_node_read_string(obj, "source");
    {
        ncl_json *value = ncl_json_obj_get(obj, "value");
        if (value != NULL) {
            node->value = ncl_json_clone(value);
        }
    }
    node->version = ncl_node_read_string(obj, "version");
    node->guid = ncl_node_read_string(obj, "guid");
    node->unique_id = ncl_node_read_string(obj, "uniqueID");

    if (type == NCL_NODE_ROOT) {
        ncl_node_set_type_name(node, node->node_type_name);
        if (ncl_node_read_children(obj, "devices", node, NCL_NODE_DEVICE) != NCL_OK ||
            ncl_node_read_children(obj, "configs", node, NCL_NODE_CONFIG) != NCL_OK) {
            ncl_node_free(node);
            return NULL;
        }
    } else if (type == NCL_NODE_COMPONENT || type == NCL_NODE_DEVICE) {
        if (ncl_node_read_children(obj, "configs", node, NCL_NODE_CONFIG) != NCL_OK ||
            ncl_node_read_children(obj, "dataItems", node, NCL_NODE_DATA_ITEM) != NCL_OK ||
            ncl_node_read_children(obj, "components", node, NCL_NODE_COMPONENT) != NCL_OK) {
            ncl_node_free(node);
            return NULL;
        }
    }
    return node;
}

/* ==================================================== post construction == */

static bool ncl_config_bind_sample_items(ncl_node *config, const ncl_node *root)
{
    ncl_node_map id_map;
    ncl_node_map path_map;
    size_t i;
    bool ok = true;

    if (!ncl_node_is_sample_node(config) || ncl_ptrvec_len(&config->sample_items) == 0) {
        return false;
    }
    ncl_node_map_init(&id_map);
    ncl_node_map_init(&path_map);
    if (ncl_root_node_id_map(root, &id_map) != NCL_OK ||
        ncl_root_node_path_map(root, &path_map) != NCL_OK) {
        ncl_node_map_free(&id_map);
        ncl_node_map_free(&path_map);
        return false;
    }

    for (i = 0; i < ncl_ptrvec_len(&config->sample_items); i++) {
        ncl_sample_ref *ref = ncl_node_sample_at(config, i);
        ncl_node *target;
        if (ref == NULL) {
            ok = false;
            break;
        }
        target = ncl_node_map_get(&id_map, ref->id);
        if (target == NULL) {
            target = ncl_node_map_get(&path_map, ref->id);
        }
        if (target != NULL &&
            (target->type == NCL_NODE_DATA_ITEM || target->type == NCL_NODE_CONFIG)) {
            ref->node = target;
            free(ref->path);
            ref->path = NULL;
        } else {
            ok = false;
            break;
        }
    }
    ncl_node_map_free(&id_map);
    ncl_node_map_free(&path_map);
    return ok;
}

ncl_node *ncl_root_node_post_construct(ncl_node *root)
{
    size_t i;
    size_t j;

    if (root == NULL || root->type != NCL_NODE_ROOT) {
        return NULL;
    }

    /* 1. wire relations and give the root its derived path */
    {
        char *path = NULL;
        if (ncl_asprintf(&path, "%s%s", NCL_PATH_SEPARATOR,
                         root->node_type_name != NULL ? root->node_type_name : "")
            != NCL_OK) {
            return NULL;
        }
        free(root->path);
        root->path = path;
    }
    for (i = 0; i < ncl_ptrvec_len(&root->devices); i++) {
        ncl_node *device = ncl_node_device_at(root, i);
        device->parent = root;
        ncl_node_build_relations(device);
    }

    /* 2. compute paths for devices ... */
    for (i = 0; i < ncl_ptrvec_len(&root->devices); i++) {
        if (ncl_node_set_path(ncl_node_device_at(root, i), ncl_node_path(root)) !=
            NCL_OK) {
            return NULL;
        }
    }

    /* 3. ... and for root level configs */
    for (i = 0; i < ncl_ptrvec_len(&root->configs); i++) {
        ncl_node *config = ncl_node_config_at(root, i);
        config->parent = root;
        if (ncl_node_set_path(config, ncl_node_path(root)) != NCL_OK) {
            return NULL;
        }
    }

    /* 4. sample channel defaults + binding */
    for (i = 0; i < ncl_ptrvec_len(&root->devices); i++) {
        ncl_node *device = ncl_node_device_at(root, i);
        device->parent = root;
        for (j = 0; j < ncl_ptrvec_len(&device->configs); j++) {
            ncl_node *config = ncl_node_config_at(device, j);
            if (config == NULL) {
                continue;
            }
            if (config->node_type_name != NULL &&
                strcmp(config->node_type_name, NCL_NODE_TYPE_SAMPLE_CHANNEL) == 0 &&
                !config->has_sample_interval) {
                config->sample_interval = config->upload_interval;
                config->has_sample_interval = true;
            }
            if (ncl_node_is_valid(config) && ncl_node_is_sample_node(config)) {
                if (!ncl_config_bind_sample_items(config, root)) {
                    return NULL;
                }
            }
        }
    }
    return root;
}

static ncl_node *ncl_root_node_default(void)
{
    ncl_node *root = ncl_node_new(NCL_NODE_ROOT);
    ncl_node *device;
    if (root == NULL) {
        return NULL;
    }
    ncl_node_set_type_name(root, NCL_NODE_TYPE_ROOT);
    ncl_node_set_id(root, "01");
    ncl_node_set_name(root, "nclink");

    device = ncl_node_new(NCL_NODE_DEVICE);
    if (device == NULL) {
        ncl_node_free(root);
        return NULL;
    }
    ncl_node_set_id(device, "02");
    ncl_node_set_type_name(device, "MACHINE");
    ncl_node_set_version(device, "2.0");
    if (ncl_node_add_device(root, device) != NCL_OK) {
        ncl_node_free(root);
        return NULL;
    }
    return ncl_root_node_post_construct(root);
}

ncl_node *ncl_root_node_from_json(const ncl_json *json)
{
    ncl_node *root = ncl_node_from_json(json, NCL_NODE_ROOT);
    if (root == NULL) {
        return NULL;
    }
    return ncl_root_node_post_construct(root);
}

ncl_node *ncl_root_node_parse(const char *text)
{
    ncl_json *json;
    ncl_node *root;

    if (text == NULL || text[0] == '\0') {
        return ncl_root_node_default();
    }

    json = ncl_json_parse_cstr(text, NULL);
    if (json == NULL) {
        ncl_log_error("模型文件JSON解析失败");
        return NULL;
    }
    root = ncl_node_from_json(json, NCL_NODE_ROOT);
    ncl_json_free(json);
    if (root == NULL) {
        return NULL;
    }
    /* A payload without a devices array yields null. */
    if (ncl_ptrvec_len(&root->devices) == 0) {
        ncl_node_free(root);
        return NULL;
    }
    return ncl_root_node_post_construct(root);
}

/* ================================================================== maps == */

void ncl_node_map_init(ncl_node_map *map)
{
    if (map != NULL) {
        map->keys = NULL;
        map->vals = NULL;
        map->len = 0;
        map->cap = 0;
    }
}

void ncl_node_map_free(ncl_node_map *map)
{
    size_t i;
    if (map == NULL) {
        return;
    }
    for (i = 0; i < map->len; i++) {
        free(map->keys[i]);
    }
    free(map->keys);
    free(map->vals);
    map->keys = NULL;
    map->vals = NULL;
    map->len = 0;
    map->cap = 0;
}

ncl_err ncl_node_map_put(ncl_node_map *map, const char *key, ncl_node *node)
{
    size_t i;
    char *copy;

    if (map == NULL || key == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    for (i = 0; i < map->len; i++) {
        if (strcmp(map->keys[i], key) == 0) {
            map->vals[i] = node; /* HashMap.put semantics: replace */
            return NCL_OK;
        }
    }
    if (map->len == map->cap) {
        size_t cap = map->cap == 0 ? 16 : map->cap * 2;
        char **keys = (char **)realloc(map->keys, cap * sizeof(char *));
        ncl_node **vals;
        if (keys == NULL) {
            return NCL_ERR_NOMEM;
        }
        map->keys = keys;
        vals = (ncl_node **)realloc(map->vals, cap * sizeof(ncl_node *));
        if (vals == NULL) {
            return NCL_ERR_NOMEM;
        }
        map->vals = vals;
        map->cap = cap;
    }
    copy = ncl_strdup(key);
    if (copy == NULL) {
        return NCL_ERR_NOMEM;
    }
    map->keys[map->len] = copy;
    map->vals[map->len] = node;
    map->len++;
    return NCL_OK;
}

ncl_node *ncl_node_map_get(const ncl_node_map *map, const char *key)
{
    size_t i;
    if (map == NULL || key == NULL) {
        return NULL;
    }
    for (i = 0; i < map->len; i++) {
        if (strcmp(map->keys[i], key) == 0) {
            return map->vals[i];
        }
    }
    return NULL;
}

size_t ncl_node_map_len(const ncl_node_map *map)
{
    return map == NULL ? 0 : map->len;
}

const char *ncl_node_map_key_at(const ncl_node_map *map, size_t index)
{
    if (map == NULL || index >= map->len) {
        return NULL;
    }
    return map->keys[index];
}

ncl_node *ncl_node_map_val_at(const ncl_node_map *map, size_t index)
{
    if (map == NULL || index >= map->len) {
        return NULL;
    }
    return map->vals[index];
}

/* Collect every path of the subtree into a map. */
static ncl_err ncl_node_collect_paths(const ncl_node *node, ncl_node_map *out)
{
    size_t i;

    for (i = 0; i < ncl_ptrvec_len(&node->components); i++) {
        const ncl_node *child = ncl_node_component_at(node, i);
        if (ncl_node_path(child) != NULL) {
            ncl_node_map_put(out, ncl_node_path(child), (ncl_node *)child);
        }
        ncl_node_collect_paths(child, out);
    }
    if (ncl_node_path(node) != NULL) {
        for (i = 0; i < ncl_ptrvec_len(&node->configs); i++) {
            const ncl_node *child = ncl_node_config_at(node, i);
            if (ncl_node_path(child) != NULL) {
                ncl_node_map_put(out, ncl_node_path(child), (ncl_node *)child);
            }
        }
        for (i = 0; i < ncl_ptrvec_len(&node->data_items); i++) {
            const ncl_node *child = ncl_node_data_item_at(node, i);
            if (ncl_node_path(child) != NULL) {
                ncl_node_map_put(out, ncl_node_path(child), (ncl_node *)child);
            }
        }
    }
    return NCL_OK;
}

ncl_err ncl_root_node_path_map(const ncl_node *root, ncl_node_map *out)
{
    size_t i;
    if (root == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    for (i = 0; i < ncl_ptrvec_len(&root->devices); i++) {
        const ncl_node *device = ncl_node_device_at(root, i);
        ncl_node_collect_paths(device, out);
        if (ncl_node_path(device) != NULL) {
            ncl_node_map_put(out, ncl_node_path(device), (ncl_node *)device);
        }
    }
    for (i = 0; i < ncl_ptrvec_len(&root->configs); i++) {
        const ncl_node *config = ncl_node_config_at(root, i);
        if (ncl_node_path(config) != NULL) {
            ncl_node_map_put(out, ncl_node_path(config), (ncl_node *)config);
        }
    }
    return NCL_OK;
}

ncl_err ncl_root_node_id_map(const ncl_node *root, ncl_node_map *out)
{
    ncl_node_map path_map;
    size_t i;

    if (root == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_node_map_init(&path_map);
    if (ncl_root_node_path_map(root, &path_map) != NCL_OK) {
        ncl_node_map_free(&path_map);
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < ncl_node_map_len(&path_map); i++) {
        ncl_node *node = ncl_node_map_val_at(&path_map, i);
        if (node->id != NULL) {
            ncl_node_map_put(out, node->id, node);
        }
    }
    ncl_node_map_free(&path_map);

    /* Configs that are not reachable through the path map are added last. */
    for (i = 0; i < ncl_ptrvec_len(&root->devices); i++) {
        const ncl_node *device = ncl_node_device_at(root, i);
        size_t j;
        for (j = 0; j < ncl_ptrvec_len(&device->configs); j++) {
            ncl_node *config = ncl_node_config_at(device, j);
            if (config->id != NULL && ncl_node_map_get(out, config->id) == NULL) {
                ncl_node_map_put(out, config->id, config);
            }
        }
    }
    return NCL_OK;
}
