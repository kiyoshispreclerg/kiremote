/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Runs a libFuzzer target over files, for builds without -fsanitize=fuzzer
 * (make test runs every target over its corpus, under ASan in make asan). */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int run_file(const char *path)
{
    size_t len;
    char *data = kr_read_file(path, &len);
    if (!data) {
        perror(path);
        return 1;
    }
    LLVMFuzzerTestOneInput((const uint8_t *)data, len);
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    int files = 0, bad = 0;
    for (int i = 1; i < argc; i++) {
        DIR *d = opendir(argv[i]);
        if (!d) {
            bad |= run_file(argv[i]);
            files++;
            continue;
        }
        struct dirent *de;
        while ((de = readdir(d))) {
            char path[KR_PATH_MAX];
            if (de->d_name[0] == '.' || !kr_pathf(path, sizeof path, "%s/%s", argv[i], de->d_name))
                continue;
            bad |= run_file(path);
            files++;
        }
        closedir(d);
    }
    fprintf(stderr, "%s: %d inputs\n", argv[0], files);
    return bad;
}
