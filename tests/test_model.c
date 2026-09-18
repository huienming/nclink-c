/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Model tests driven by the sample model file
 * (conf/model/nclink.json). Re-serialising it must reproduce the original bytes
 * exactly, which validates both the parser and the property order.
 */
#include "ncl_test.h"

#include "nclink/ncl_env.h"
#include "nclink/ncl_model.h"

#ifndef NCL_TEST_DATA_DIR
#define NCL_TEST_DATA_DIR "."
#endif

static char *read_model_file(void)
{
    char path[1024];
    char *content = NULL;
    snprintf(path, sizeof(path), "%s/model_nclink.json", NCL_TEST_DATA_DIR);
    if (ncl_file_read_all(path, &content, NULL) != NCL_OK) {
        printf("    cannot read %s\n", path);
    }
    return content;
}

static void test_round_trip(void)
{
    char *source = read_model_file();
    ncl_node *root;
    char *written;

    NCL_TEST_CASE("the sample model parses and re-serialises byte for byte");
    NCL_CHECK(source != NULL);
    if (source == NULL) {
        return;
    }

    root = ncl_root_node_parse(source);
    NCL_CHECK(root != NULL);
    if (root == NULL) {
        ncl_free_safe(source);
        return;
    }

    written = ncl_node_write_string(root);
    NCL_CHECK_EQ_STR(written, source);
    ncl_free_safe(written);

    NCL_TEST_CASE("the parsed model is valid");
    NCL_CHECK(ncl_node_is_valid(root));

    NCL_TEST_CASE("root fields");
    NCL_CHECK_EQ_STR(ncl_node_path(root), "/NC_LINK_ROOT");
    NCL_CHECK_EQ_STR(root->name, "nclink");
    NCL_CHECK_EQ_STR(root->id, "01");
    NCL_CHECK_EQ_STR(root->unique_id, "nclinkSampleTool");

    NCL_TEST_CASE("device path follows root path + type");
    {
        ncl_node *device = ncl_node_device_at(root, 0);
        NCL_CHECK(device != NULL);
        NCL_CHECK_EQ_STR(ncl_node_path(device), "/NC_LINK_ROOT/PLC");
        NCL_CHECK_EQ_STR(device->version, "2.0");
        NCL_CHECK_EQ_INT(ncl_ptrvec_len(&device->configs), 2);
        NCL_CHECK_EQ_INT(ncl_ptrvec_len(&device->data_items), 3);
    }

    NCL_TEST_CASE("config paths: source overrides, otherwise /<type>");
    {
        ncl_node *device = ncl_node_device_at(root, 0);
        ncl_node *file_config = ncl_node_config_at(device, 0);
        ncl_node *sample_config = ncl_node_config_at(device, 1);

        NCL_CHECK_EQ_STR(file_config->source, "CONTROLLER");
        NCL_CHECK_EQ_STR(ncl_node_path(file_config), "/CONTROLLER/FILE");
        NCL_CHECK_EQ_STR(ncl_node_path(sample_config), "/SAMPLE_CHANNEL");
        NCL_CHECK(ncl_node_is_sample_node(sample_config));
        NCL_CHECK(sample_config->has_sample_interval);
        NCL_CHECK_EQ_INT(sample_config->sample_interval, 1000);
        NCL_CHECK_EQ_INT(sample_config->upload_interval, 1000);
    }

    NCL_TEST_CASE("data item paths honour 'source'");
    {
        ncl_node *device = ncl_node_device_at(root, 0);
        ncl_node *status = ncl_node_data_item_at(device, 0);
        ncl_node *warning = ncl_node_data_item_at(device, 2);

        NCL_CHECK_EQ_STR(ncl_node_path(status), "/STATUS");
        NCL_CHECK_EQ_STR(ncl_node_path(warning), "/CONTROLLER/WARNNING");
    }

    NCL_TEST_CASE("sample channel references resolve during post-construction");
    {
        ncl_node *device = ncl_node_device_at(root, 0);
        ncl_node *sample_config = ncl_node_config_at(device, 1);
        size_t i;

        NCL_CHECK_EQ_INT(ncl_node_sample_count(sample_config), 3);
        for (i = 0; i < ncl_node_sample_count(sample_config); i++) {
            ncl_sample_ref *ref = ncl_node_sample_at(sample_config, i);
            NCL_CHECK(ref->node != NULL);
        }
        if (ncl_node_sample_count(sample_config) == 3) {
            char *path = ncl_sample_ref_path(ncl_node_sample_at(sample_config, 0));
            NCL_CHECK_EQ_STR(path, "/STATUS");
            ncl_free_safe(path);
            path = ncl_sample_ref_path(ncl_node_sample_at(sample_config, 2));
            NCL_CHECK_EQ_STR(path, "/CONTROLLER/WARNNING");
            ncl_free_safe(path);
        }
    }

    NCL_TEST_CASE("path and id maps are populated");
    {
        ncl_node_map paths;
        ncl_node_map ids;
        ncl_node_map_init(&paths);
        ncl_node_map_init(&ids);
        NCL_CHECK_EQ_INT(ncl_root_node_path_map(root, &paths), NCL_OK);
        NCL_CHECK_EQ_INT(ncl_root_node_id_map(root, &ids), NCL_OK);
        /* 1 device + 3 data items + 2 configs */
        NCL_CHECK_EQ_INT(ncl_node_map_len(&paths), 6);
        NCL_CHECK(ncl_node_map_get(&paths, "/NC_LINK_ROOT/PLC") != NULL);
        NCL_CHECK(ncl_node_map_get(&paths, "/STATUS") != NULL);
        NCL_CHECK(ncl_node_map_get(&ids, "030001") != NULL);
        NCL_CHECK(ncl_node_map_get(&ids, "sample_channel2") != NULL);
        ncl_node_map_free(&paths);
        ncl_node_map_free(&ids);
    }

    NCL_TEST_CASE("find by id walks into configs, data items and components");
    NCL_CHECK(ncl_node_find_by_id(root, "020001") != NULL);
    NCL_CHECK(ncl_node_find_by_id(root, "030003") != NULL);
    NCL_CHECK(ncl_node_find_by_id(root, "does-not-exist") == NULL);

    ncl_node_free(root);
    ncl_free_safe(source);
}

static void test_default_model(void)
{
    ncl_node *root = ncl_root_node_parse("");
    ncl_node *root2 = ncl_root_node_parse(NULL);

    NCL_TEST_CASE("empty input yields the built in default model");
    NCL_CHECK(root != NULL);
    NCL_CHECK_EQ_STR(root->id, "01");
    NCL_CHECK_EQ_STR(root->name, "nclink");
    NCL_CHECK_EQ_INT(ncl_ptrvec_len(&root->devices), 1);
    NCL_CHECK_EQ_STR(ncl_node_device_at(root, 0)->id, "02");
    NCL_CHECK_EQ_STR(ncl_node_device_at(root, 0)->node_type_name, "MACHINE");
    NCL_CHECK_EQ_STR(ncl_node_device_at(root, 0)->version, "2.0");
    NCL_CHECK(ncl_node_is_valid(root));
    ncl_node_free(root);
    ncl_node_free(root2);
}

static void test_invalid_payload(void)
{
    NCL_TEST_CASE("malformed JSON and missing devices yield NULL");
    NCL_CHECK(ncl_root_node_parse("{not json") == NULL);
    NCL_CHECK(ncl_root_node_parse("{}") == NULL);
    NCL_CHECK(ncl_root_node_parse("{\"devices\":[]}") == NULL);
}

static void test_builder_api(void)
{
    ncl_node *root = ncl_node_new(NCL_NODE_ROOT);
    ncl_node *device = ncl_node_new(NCL_NODE_DEVICE);
    ncl_node *item = ncl_node_new(NCL_NODE_DATA_ITEM);
    ncl_node *config = ncl_node_new(NCL_NODE_CONFIG);
    char *text;

    NCL_TEST_CASE("nodes can be assembled programmatically");
    ncl_node_set_type_name(root, NCL_NODE_TYPE_ROOT);
    ncl_node_set_id(root, "01");
    ncl_node_set_id(device, "02");
    ncl_node_set_type_name(device, "MACHINE");
    ncl_node_set_version(device, "2.0");
    ncl_node_set_id(item, "030001");
    ncl_node_set_type_name(item, "STATUS");
    ncl_node_set_settable(item, false);
    ncl_node_set_id(config, "020001");
    ncl_node_set_type_name(config, "FILE");
    ncl_node_set_data_type(config, "HASH");

    NCL_CHECK_EQ_INT(ncl_node_add_data_item(device, item), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_node_add_config(device, config), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_node_add_device(root, device), NCL_OK);
    NCL_CHECK(ncl_root_node_post_construct(root) == root);
    NCL_CHECK(ncl_node_is_valid(root));

    text = ncl_node_write_string(root);
    NCL_CHECK_EQ_STR(text,
        "{\"id\":\"01\",\"type\":\"NC_LINK_ROOT\",\"devices\":["
        "{\"id\":\"02\",\"type\":\"MACHINE\",\"configs\":["
        "{\"id\":\"020001\",\"type\":\"FILE\",\"dataType\":\"HASH\"}],"
        "\"dataItems\":[{\"id\":\"030001\",\"type\":\"STATUS\",\"settable\":false}],"
        "\"version\":\"2.0\"}]}");
    ncl_free_safe(text);
    ncl_node_free(root);
}

/*
 * 组件（带 number）下的数据项路径，以及采样通道引用它们时形成的“表头”。
 *
 * 组件/数据项的编号用 "@" 分隔，所以组件路径为 "/AXIS@0"，
 * 其下数据项为 "/AXIS@0/POSITION"；采样项再按数据类型加后缀：
 *   LIST -> "/AXIS@0/TRACE$LIST-0"   HASH -> "/AXIS@0/PARAM$HASH-speed"
 * 多个索引/键时形如 "$LIST-[0, 1]"。
 */
static const char *kComponentModel =
    "{\"name\":\"nclink\",\"id\":\"01\",\"type\":\"NC_LINK_ROOT\",\"devices\":["
    "{\"id\":\"02\",\"type\":\"PLC\",\"configs\":[{"
    "\"id\":\"ch1\",\"type\":\"SAMPLE_CHANNEL\","
    "\"sampleInterval\":1000,\"uploadInterval\":2000,\"ids\":["
    "{\"id\":\"030001\"},"
    "{\"id\":\"030002\",\"params\":{\"indexes\":[\"0\"]}},"
    "{\"id\":\"030003\",\"params\":{\"keys\":[\"speed\"]}}]}],"
    "\"components\":[{\"id\":\"0300\",\"type\":\"AXIS\",\"number\":\"0\","
    "\"configs\":[],\"dataItems\":["
    "{\"id\":\"030001\",\"type\":\"POSITION\"},"
    "{\"id\":\"030002\",\"type\":\"TRACE\",\"dataType\":\"LIST\"},"
    "{\"id\":\"030003\",\"type\":\"PARAM\",\"dataType\":\"HASH\"}]}],"
    "\"dataItems\":[{\"id\":\"030004\",\"type\":\"STATUS\"}],"
    "\"version\":\"2.0\"}]}";

static void test_component_paths_and_sample_header(void)
{
    ncl_node *root = ncl_root_node_parse(kComponentModel);
    ncl_node *device;
    ncl_node *axis;
    ncl_node *config;
    ncl_sample_ref *ref;
    char *path;
    ncl_node_map paths;

    NCL_TEST_CASE("组件路径用 '@' 分隔 number");
    NCL_CHECK(root != NULL);
    if (root == NULL) {
        return;
    }
    device = ncl_node_device_at(root, 0);
    axis = ncl_node_component_at(device, 0);
    NCL_CHECK_EQ_STR(ncl_node_path(axis), "/AXIS@0");
    NCL_CHECK_EQ_STR(ncl_node_path(ncl_node_data_item_at(axis, 0)),
                     "/AXIS@0/POSITION");
    NCL_CHECK_EQ_STR(ncl_node_path(ncl_node_data_item_at(axis, 1)),
                     "/AXIS@0/TRACE");
    NCL_CHECK_EQ_STR(ncl_node_path(ncl_node_data_item_at(axis, 2)),
                     "/AXIS@0/PARAM");
    /* 设备下的数据项仍走“父为设备则前缀清空”的规则 */
    NCL_CHECK_EQ_STR(ncl_node_path(ncl_node_data_item_at(device, 0)),
                     "/STATUS");

    NCL_TEST_CASE("路径可以反查到节点");
    ncl_node_map_init(&paths);
    NCL_CHECK_EQ_INT(ncl_root_node_path_map(root, &paths), NCL_OK);
    NCL_CHECK(ncl_node_map_get(&paths, "/AXIS@0/POSITION") != NULL);
    NCL_CHECK(ncl_node_map_get(&paths, "/AXIS@0") != NULL);
    ncl_node_map_free(&paths);

    NCL_TEST_CASE("采样表头：采样项路径按数据类型加后缀");
    config = ncl_node_config_at(device, 0);
    NCL_CHECK(ncl_node_is_sample_node(config));
    NCL_CHECK_EQ_INT(ncl_node_sample_count(config), 3);

    ref = ncl_node_sample_at(config, 0);
    path = ncl_sample_ref_path(ref);
    NCL_CHECK_EQ_STR(path, "/AXIS@0/POSITION");
    ncl_free_safe(path);

    ref = ncl_node_sample_at(config, 1);
    path = ncl_sample_ref_path(ref);
    NCL_CHECK_EQ_STR(path, "/AXIS@0/TRACE$LIST-0");
    ncl_free_safe(path);

    ref = ncl_node_sample_at(config, 2);
    path = ncl_sample_ref_path(ref);
    NCL_CHECK_EQ_STR(path, "/AXIS@0/PARAM$HASH-speed");
    ncl_free_safe(path);

    NCL_TEST_CASE("多个索引时形如 $LIST-[0, 1]");
    {
        ncl_sample_ref *multi = ncl_sample_ref_new("030002");
        ncl_sample_params *params = ncl_sample_params_new();
        ncl_strvec_push(&params->indexes, "0");
        ncl_strvec_push(&params->indexes, "1");
        multi->params = params;
        multi->node = ncl_node_data_item_at(axis, 1); /* 借用 */
        path = ncl_sample_ref_path(multi);
        NCL_CHECK_EQ_STR(path, "/AXIS@0/TRACE$LIST-[0, 1]");
        ncl_free_safe(path);
        multi->node = NULL;
        ncl_sample_ref_free(multi);
    }

    ncl_node_free(root);
}


/*
 * 数据项自己也能带 number：一个部件挂多路同类传感器时，就是"同 type、不同 number"
 * 的几个数据项，路径变成 /<父路径>/<类型>@<number>（主轴两路功率：/AXIS@S/POWER@1、
 * /AXIS@S/POWER@2）。没有 number 的项就是单路，路径不带后缀。
 */
static const char *kSensorNumberModel =
    "{\"id\":\"01\",\"type\":\"NC_LINK_ROOT\",\"devices\":["
    "{\"id\":\"02\",\"type\":\"MACHINE\",\"configs\":[{"
    "\"id\":\"ch1\",\"type\":\"SAMPLE_CHANNEL\",\"sampleInterval\":1000,"
    "\"uploadInterval\":1000,\"ids\":[{\"id\":\"030010\"},{\"id\":\"030011\"}]}],"
    "\"components\":[{\"id\":\"0300\",\"type\":\"AXIS\",\"number\":\"S\","
    "\"configs\":[],\"dataItems\":["
    "{\"id\":\"030010\",\"type\":\"POWER\",\"number\":\"1\"},"
    "{\"id\":\"030011\",\"type\":\"POWER\",\"number\":\"2\"},"
    "{\"id\":\"030012\",\"type\":\"SPEED\"}]}],"
    "\"dataItems\":[{\"id\":\"030020\",\"type\":\"STATUS\",\"number\":\"1\"}],"
    "\"version\":\"2.0\"}]}";

static void test_data_item_number_paths(void)
{
    ncl_node *root = ncl_root_node_parse(kSensorNumberModel);
    ncl_node *device;
    ncl_node *axis;
    ncl_node *config;
    ncl_node *item;
    ncl_sample_ref *ref;
    ncl_node_map paths;
    char *path;

    NCL_TEST_CASE("数据项带 number 时路径为 /<类型>@<number>");
    NCL_CHECK(root != NULL);
    if (root == NULL) {
        return;
    }
    device = ncl_node_device_at(root, 0);
    axis = ncl_node_component_at(device, 0);
    NCL_CHECK_EQ_STR(ncl_node_path(ncl_node_data_item_at(axis, 0)),
                     "/AXIS@S/POWER@1");
    NCL_CHECK_EQ_STR(ncl_node_path(ncl_node_data_item_at(axis, 1)),
                     "/AXIS@S/POWER@2");
    /* 没有 number 的项（单路）路径不带后缀 */
    NCL_CHECK_EQ_STR(ncl_node_path(ncl_node_data_item_at(axis, 2)),
                     "/AXIS@S/SPEED");
    /* 设备下的数据项：父前缀清空，自己带的 number 仍然进路径 */
    NCL_CHECK_EQ_STR(ncl_node_path(ncl_node_data_item_at(device, 0)),
                     "/STATUS@1");

    NCL_TEST_CASE("带 number 的数据项也能按路径反查");
    ncl_node_map_init(&paths);
    NCL_CHECK_EQ_INT(ncl_root_node_path_map(root, &paths), NCL_OK);
    NCL_CHECK(ncl_node_map_get(&paths, "/AXIS@S/POWER@1") != NULL);
    NCL_CHECK(ncl_node_map_get(&paths, "/AXIS@S/POWER@2") != NULL);
    NCL_CHECK(ncl_node_map_get(&paths, "/AXIS@S/SPEED") != NULL);
    ncl_node_map_free(&paths);

    NCL_TEST_CASE("同一通道里两路传感器各自成为一列表头");
    config = ncl_node_config_at(device, 0);
    NCL_CHECK(ncl_node_is_sample_node(config));
    NCL_CHECK_EQ_INT(ncl_node_sample_count(config), 2);
    ref = ncl_node_sample_at(config, 0);
    path = ncl_sample_ref_path(ref);
    NCL_CHECK_EQ_STR(path, "/AXIS@S/POWER@1");
    ncl_free_safe(path);
    ref = ncl_node_sample_at(config, 1);
    path = ncl_sample_ref_path(ref);
    NCL_CHECK_EQ_STR(path, "/AXIS@S/POWER@2");
    ncl_free_safe(path);

    ncl_node_free(root);
}

static void test_data_item_number_serialisation(void)
{
    ncl_node *root = ncl_node_new(NCL_NODE_ROOT);
    ncl_node *device = ncl_node_new(NCL_NODE_DEVICE);
    ncl_node *item = ncl_node_new(NCL_NODE_DATA_ITEM);
    char *text;

    ncl_node_set_id(root, "01");
    ncl_node_set_type_name(root, "NC_LINK_ROOT");   /* 与解析出来的根节点一致 */
    ncl_node_set_id(device, "02");
    ncl_node_set_type_name(device, "MACHINE");
    ncl_node_set_version(device, "2.0");
    ncl_node_set_id(item, "030001");
    ncl_node_set_type_name(item, "TRACE");
    ncl_node_set_number(item, "2");
    ncl_node_set_data_type(item, "LIST");
    NCL_CHECK_EQ_INT(ncl_node_add_data_item(device, item), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_node_add_device(root, device), NCL_OK);
    NCL_CHECK(ncl_root_node_post_construct(root) == root);

    NCL_TEST_CASE("数据项的 number 序列化在 dataType 之前");
    text = ncl_node_write_string(root);
    NCL_CHECK_EQ_STR(text,
        "{\"id\":\"01\",\"type\":\"NC_LINK_ROOT\",\"devices\":["
        "{\"id\":\"02\",\"type\":\"MACHINE\",\"dataItems\":["
        "{\"id\":\"030001\",\"type\":\"TRACE\",\"number\":\"2\","
        "\"dataType\":\"LIST\"}],\"version\":\"2.0\"}]}");
    ncl_free_safe(text);

    NCL_TEST_CASE("带 number 的模型文本能原样往返");
    {
        ncl_node *parsed = ncl_root_node_parse(kSensorNumberModel);
        char *written = parsed != NULL ? ncl_node_write_string(parsed) : NULL;

        NCL_CHECK(parsed != NULL);
        /* 解析 → 序列化后，两路传感器与单路项的路径都还在 */
        NCL_CHECK(written != NULL &&
                  strstr(written, "\"type\":\"POWER\",\"number\":\"1\"") != NULL);
        NCL_CHECK(written != NULL &&
                  strstr(written, "\"type\":\"POWER\",\"number\":\"2\"") != NULL);
        ncl_free_safe(written);
        ncl_node_free(parsed);
    }

    ncl_node_free(root);
}

NCL_TEST_MAIN_BEGIN()
    test_component_paths_and_sample_header();
    test_data_item_number_paths();
    test_data_item_number_serialisation();
    test_round_trip();
    test_default_model();
    test_invalid_payload();
    test_builder_api();
NCL_TEST_MAIN_END()
