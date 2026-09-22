/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - device data model.
 *
 * The seven node kinds of NC-Link (root, PLC device, component, data item,
 * config, sample channel) share one discriminated struct (ncl_node), so callers
 * can walk the tree without casts:
 *
 *   root -- device -- component -- component -- data item
 *                              \-- data item
 *                              \-- config -- (sample channel)
 *
 * Field names, JSON property names, JSON key order, path computation and
 * validation rules follow GB/T 41970-2022.
 */
#ifndef NCL_MODEL_H
#define NCL_MODEL_H

#include <stdbool.h>

#include "nclink/ncl_common.h"
#include "nclink/ncl_general.h"
#include "nclink/ncl_json.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Kind of a model node (the "type" property of the model file). */
typedef enum {
    NCL_NODE_BASE = 0,
    NCL_NODE_ROOT,
    NCL_NODE_DEVICE,
    NCL_NODE_COMPONENT,
    NCL_NODE_DATA_ITEM,
    NCL_NODE_CONFIG
} ncl_node_type;

/** Upload mode of a config node: periodic reporting or event driven. */
typedef enum {
    NCL_UPLOAD_NORMAL = 0,
    NCL_UPLOAD_EVENT
} ncl_upload_type;

const char *ncl_node_type_name(ncl_node_type type);
const char *ncl_upload_type_name(ncl_upload_type type);
bool ncl_upload_type_parse(const char *text, ncl_upload_type *out);

/** Well known type strings used by the protocol. */
#define NCL_NODE_TYPE_ROOT "NC_LINK_ROOT"
#define NCL_NODE_TYPE_SAMPLE_CHANNEL "SAMPLE_CHANNEL"

typedef struct ncl_node ncl_node;

/* -------------------------------------------------------- sample channels -- */

/** Sample parameters (property order indexes/keys/offset/length). */
typedef struct {
    ncl_strvec indexes;
    ncl_strvec keys;
    bool       has_offset;
    long long  offset;
    bool       has_length;
    long long  length;
} ncl_sample_params;

ncl_sample_params *ncl_sample_params_new(void);
ncl_sample_params *ncl_sample_params_clone(const ncl_sample_params *p);
void               ncl_sample_params_free(ncl_sample_params *p);
bool               ncl_sample_params_is_valid(const ncl_sample_params *p);
ncl_json          *ncl_sample_params_to_json(const ncl_sample_params *p);
ncl_sample_params *ncl_sample_params_from_json(const ncl_json *j);

/**
 * A sampling channel item: a reference from a SAMPLE_CHANNEL config node to a
 * data item, optionally narrowed to a hash key or list index range.
 */
typedef struct {
    char              *id;
    ncl_sample_params *params;
    ncl_node          *node; /**< resolved during post-construction, not owned */
    char              *path; /**< lazily computed cache */
} ncl_sample_ref;

ncl_sample_ref *ncl_sample_ref_new(const char *id);
void            ncl_sample_ref_free(ncl_sample_ref *ref);
bool            ncl_sample_ref_is_valid(const ncl_sample_ref *ref);

/** Path of the referenced data item, including the LIST/HASH suffixes. */
char *ncl_sample_ref_path(ncl_sample_ref *ref);

/* ------------------------------------------------------------------ node -- */

struct ncl_node {
    ncl_node_type type;   /**< kind discriminated tag */

    /* Common to every kind */
    char      *name;
    char      *id;
    char      *node_type_name; /**< "type", e.g. "NC_LINK_DATA_ITEM" */
    bool       settable;
    bool       has_settable;
    char      *description;
    char      *path;           /**< @JsonIgnore, computed by ncl_node_set_path */
    ncl_node  *parent;         /**< @JsonIgnore, not owned */

    /* Data item */
    char      *number;
    char      *data_type;
    ncl_json  *value;          /**< owned */
    char      *mapping;
    char      *value_type;
    char      *source;

    /* Children of a component or device (all owned) */
    ncl_ptrvec configs;        /**< ncl_node* of type NCL_NODE_CONFIG */
    ncl_ptrvec data_items;     /**< ncl_node* of type NCL_NODE_DATA_ITEM */
    ncl_ptrvec components;     /**< ncl_node* of type NCL_NODE_COMPONENT */

    /* Device */
    char      *version;
    char      *guid;

    /* Root */
    ncl_ptrvec devices;        /**< ncl_node* of type NCL_NODE_DEVICE */
    char      *unique_id;

    /* Config */
    bool            has_sample_interval;
    long long       sample_interval;
    bool            has_upload_interval;
    long long       upload_interval;
    bool            has_upload_type;
    ncl_upload_type upload_type;
    ncl_ptrvec      sample_items; /**< ncl_sample_ref* */
};

/** Create an empty node of the given kind (fields NULL / vectors empty). */
ncl_node *ncl_node_new(ncl_node_type type);
void      ncl_node_free(ncl_node *node);
/** Deep copy. When @p shallow_children is true the copy shares the children. */
ncl_node *ncl_node_clone(const ncl_node *node, bool shallow_children);

/* Scalar setters: every string setter copies. Passing NULL clears the field. */
ncl_err ncl_node_set_name(ncl_node *node, const char *value);
ncl_err ncl_node_set_id(ncl_node *node, const char *value);
ncl_err ncl_node_set_type_name(ncl_node *node, const char *value);
ncl_err ncl_node_set_description(ncl_node *node, const char *value);
ncl_err ncl_node_set_number(ncl_node *node, const char *value);
ncl_err ncl_node_set_data_type(ncl_node *node, const char *value);
ncl_err ncl_node_set_mapping(ncl_node *node, const char *value);
ncl_err ncl_node_set_value_type(ncl_node *node, const char *value);
ncl_err ncl_node_set_source(ncl_node *node, const char *value);
ncl_err ncl_node_set_version(ncl_node *node, const char *value);
ncl_err ncl_node_set_guid(ncl_node *node, const char *value);
ncl_err ncl_node_set_unique_id(ncl_node *node, const char *value);
ncl_err ncl_node_set_settable(ncl_node *node, bool value);
/** Takes ownership of @p value. */
ncl_err ncl_node_set_value(ncl_node *node, ncl_json *value);

/* Child management, dispatching on the parent kind. */
ncl_err ncl_node_add_config(ncl_node *parent, ncl_node *config);
ncl_err ncl_node_add_data_item(ncl_node *parent, ncl_node *item);
ncl_err ncl_node_add_component(ncl_node *parent, ncl_node *component);
ncl_err ncl_node_add_device(ncl_node *root, ncl_node *device);
ncl_err ncl_node_add_sample_item(ncl_node *config, ncl_sample_ref *ref);

/**
 * Generic child insertion: the parent kind decides which slot @p child goes
 * into. Takes ownership of @p child.
 */
ncl_err ncl_node_add_child(ncl_node *parent, ncl_node *child);

size_t    ncl_node_child_count(const ncl_node *node);
ncl_node *ncl_node_config_at(const ncl_node *node, size_t index);
ncl_node *ncl_node_data_item_at(const ncl_node *node, size_t index);
ncl_node *ncl_node_component_at(const ncl_node *node, size_t index);
ncl_node *ncl_node_device_at(const ncl_node *node, size_t index);
size_t    ncl_node_sample_count(const ncl_node *node);
ncl_sample_ref *ncl_node_sample_at(const ncl_node *node, size_t index);

/** Depth first lookup by id below @p node. */
ncl_node *ncl_node_find_by_id(const ncl_node *node, const char *id);

/**
 * Depth first lookup by the node's "type" (node_type_name) below @p node,
 * @p node included. This is how a client finds the reserved items the protocol
 * names by type rather than by id, e.g. NCL_METHODS_NODE_TYPE.
 */
ncl_node *ncl_node_find_by_type(const ncl_node *node, const char *type_name);

/** True when the node type string equals NCL_NODE_TYPE_SAMPLE_CHANNEL. */
bool ncl_node_is_sample_node(const ncl_node *node);

/* Paths and relations ---------------------------------------------------- */

/**
 * Recompute the path of @p node and of its subtree, applying the "parent of a
 * data item is a device" rule.
 * @p parent_path is the path of the parent node.
 */
ncl_err ncl_node_set_path(ncl_node *node, const char *parent_path);

/** Effective path: the root derives it from its type, others return the stored
 *  path. */
const char *ncl_node_path(const ncl_node *node);

/** Wire the parent pointers through the subtree rooted at @p node. */
void ncl_node_build_relations(ncl_node *node);

/** True when the node and its subtree satisfy the model rules. */
bool ncl_node_is_valid(const ncl_node *node);

/* Root node -------------------------------------------------------------- */

/**
 * Parse @p text into a root node and run ncl_root_node_post_construct().
 * When @p text is NULL or empty the built in default model is returned
 * (id "01", name "nclink", one PLC device id "02" version "2.0").
 * Returns NULL when the payload is malformed or post-construction fails.
 */
ncl_node *ncl_root_node_parse(const char *text);
ncl_node *ncl_root_node_from_json(const ncl_json *json);

/** Build a node of the requested kind from its JSON representation. */
ncl_node *ncl_node_from_json(const ncl_json *json, ncl_node_type type);

/** Fill in parents, paths, sample channel defaults and path/id maps. */
ncl_node *ncl_root_node_post_construct(ncl_node *root);

/** Serialise a node (and its subtree) using the property order of the
 *  specification. */
ncl_json *ncl_node_to_json(const ncl_node *node);
char     *ncl_node_write_string(const ncl_node *node);

/* Maps ------------------------------------------------------------------- */

/** path -> node (paths are borrowed from the nodes). */
typedef struct {
    char     **keys;
    ncl_node **vals;
    size_t     len;
    size_t     cap;
} ncl_node_map;

void       ncl_node_map_init(ncl_node_map *map);
void       ncl_node_map_free(ncl_node_map *map);
ncl_err    ncl_node_map_put(ncl_node_map *map, const char *key, ncl_node *node);
ncl_node  *ncl_node_map_get(const ncl_node_map *map, const char *key);
size_t     ncl_node_map_len(const ncl_node_map *map);
const char *ncl_node_map_key_at(const ncl_node_map *map, size_t index);
ncl_node  *ncl_node_map_val_at(const ncl_node_map *map, size_t index);

/** Map every path in the subtree to its node. */
ncl_err ncl_root_node_path_map(const ncl_node *root, ncl_node_map *out);
/** Map every id in the subtree to its node. */
ncl_err ncl_root_node_id_map(const ncl_node *root, ncl_node_map *out);

#ifdef __cplusplus
}
#endif

#endif /* NCL_MODEL_H */
