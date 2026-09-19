/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Licensing hygiene: the LICENSE file ships with the library and every source
 * file carries an SPDX identifier, so a single file copied out of the tree
 * keeps its licence information. Run as part of the normal test suite, so a
 * new file that forgets the header fails the build.
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "ncl_test.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

/* The CMake build passes the source root; build-linux.sh runs the binary with
 * the repository root as the working directory. */
#ifdef NCL_TEST_SOURCE_ROOT
#define SOURCE_ROOT NCL_TEST_SOURCE_ROOT
#else
#define SOURCE_ROOT "."
#endif

#define SPDX_LINE "SPDX-License-Identifier: MIT"
#define COPYRIGHT_LINE "Copyright (c) 2026 huienming"

static const char *const kDirs[] = {"include", "src", "tests", "examples",
                                    "tools",   "adapters"};
static const char *const kRootFiles[] = {"build.ps1", "build-linux.sh"};
static const char *const kExtensions[] = {".c",  ".h",   ".py", ".java",
                                          ".mjs", ".ps1", ".sh"};

static size_t g_scanned;
static size_t g_missing;

static bool has_source_extension(const char *path)
{
    size_t i;
    size_t len = strlen(path);

    for (i = 0; i < sizeof(kExtensions) / sizeof(kExtensions[0]); i++) {
        size_t ext_len = strlen(kExtensions[i]);
        if (len > ext_len && strcmp(path + len - ext_len, kExtensions[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* The identifier has to be in one of the first three lines: line 1 normally,
 * line 2 when the file starts with a shebang. */
static void check_source_file(const char *path)
{
    char line[512];
    int read = 0;
    bool found = false;
    FILE *fp;

    if (!has_source_extension(path)) {
        return;
    }
    g_scanned++;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        printf("    unreadable: %s\n", path);
        g_missing++;
        return;
    }
    while (read < 3 && fgets(line, sizeof(line), fp) != NULL) {
        read++;
        if (strstr(line, SPDX_LINE) != NULL) {
            found = true;
            break;
        }
    }
    fclose(fp);

    if (!found) {
        printf("    no \"%s\" header: %s\n", SPDX_LINE, path);
        g_missing++;
    }
}

static void walk(const char *dir)
{
    char path[1024];

#ifdef _WIN32
    WIN32_FIND_DATAA entry;
    char pattern[1024];
    HANDLE handle;

    if (snprintf(pattern, sizeof(pattern), "%s\\*", dir) <= 0) {
        return;
    }
    handle = FindFirstFileA(pattern, &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (strcmp(entry.cFileName, ".") == 0 ||
            strcmp(entry.cFileName, "..") == 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, entry.cFileName);
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            walk(path);
        } else {
            check_source_file(path);
        }
    } while (FindNextFileA(handle, &entry));
    FindClose(handle);
#else
    DIR *dirp = opendir(dir);
    struct dirent *entry;

    if (dirp == NULL) {
        return;
    }
    while ((entry = readdir(dirp)) != NULL) {
        DIR *sub;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        sub = opendir(path);
        if (sub != NULL) {
            closedir(sub);
            walk(path);
        } else {
            check_source_file(path);
        }
    }
    closedir(dirp);
#endif
}

static bool file_contains(const char *path, const char *needle)
{
    char line[1024];
    bool found = false;
    FILE *fp = fopen(path, "rb");

    if (fp == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, needle) != NULL) {
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

NCL_TEST_MAIN_BEGIN()
{
    char path[1024];
    size_t i;

    NCL_TEST_CASE("LICENSE ships with the library");
    snprintf(path, sizeof(path), "%s/LICENSE", SOURCE_ROOT);
    NCL_CHECK(file_contains(path, "MIT License"));
    NCL_CHECK(file_contains(path, COPYRIGHT_LINE));
    NCL_CHECK(file_contains(path, "WITHOUT WARRANTY OF ANY KIND"));

    NCL_TEST_CASE("every source file carries the SPDX header");
    for (i = 0; i < sizeof(kDirs) / sizeof(kDirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", SOURCE_ROOT, kDirs[i]);
        walk(path);
    }
    for (i = 0; i < sizeof(kRootFiles) / sizeof(kRootFiles[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", SOURCE_ROOT, kRootFiles[i]);
        check_source_file(path);
    }
    printf("    scanned %u files, %u without the header\n",
           (unsigned)g_scanned, (unsigned)g_missing);
    NCL_CHECK(g_scanned > 50);
    NCL_CHECK_EQ_INT(g_missing, 0);
}
NCL_TEST_MAIN_END()
