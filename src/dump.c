/* The MIT License (MIT)
* 
* Copyright (c) 2014 Quinlan Pfiffer, Kyle Terry
* 
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
* 
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
* 
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
*/
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include "oleg.h"
#include "logging.h"
#include "dump.h"
#include "errhandle.h"


static inline int _ol_write_bucket(const ol_bucket *bucket, FILE *fd) {
    do {
        fwrite(&bucket->key, sizeof(char), KEY_SIZE, fd);
        fwrite(&bucket->data_size, sizeof(size_t), 1, fd);
        fwrite(bucket->data_ptr, sizeof(char), bucket->data_size, fd);
        bucket = bucket->next;
    } while (bucket != NULL);
    return 0;
}

static inline int _ol_store_bin_object(ol_database *db, FILE *fd) {
    char *tmp_key;
    unsigned char *tmp_value;
    size_t value_size;

    tmp_key = malloc(KEY_SIZE);
    check_mem(tmp_key);

    fread(tmp_key, sizeof(char), KEY_SIZE, fd);
    fread(&value_size, sizeof(size_t), 1, fd);

    tmp_value = calloc(1, value_size);
    check_mem(tmp_value);

    fread(tmp_value, sizeof(char), value_size, fd);
    ol_jar(db, tmp_key, tmp_value, value_size);

    free(tmp_key);
    free(tmp_value);
    return 0;

error:
    free(tmp_key);
    free(tmp_value);
    return -1;
}

int ol_background_save(ol_database *db) {
    int pid;

    pid = fork();
    if (pid == 0) {
        int ret;
        ret = ol_save_db(db);
        if(ret != 0)
            log_err("Could not save DB to disk."); exit(ret);
    } else {
        check(pid > 0, "Could not background dump.");
        ol_log_msg(LOG_INFO, "Backgrounding ol_dump. PID: %d", pid);
        return 0;
    }
    return 0;

error:
    return -1;
}

int ol_save_db(ol_database *db) {
    FILE *fd;
    struct dump_header header;
    char tmpfile[512];
    sprintf(tmpfile, "%s-tmp", db->dump_file);

    debug("Opening file %s", tmpfile);
    fd = fopen(tmpfile, "w+");
    check(fd, "Failed to open file: %s", tmpfile);

    /* Write the header to the file */
    snprintf(header.sig, sizeof(DUMP_SIG), "%s", DUMP_SIG);
    snprintf(header.version, sizeof(header.version)+1, "%04i", DUMP_VERSION);
    header.rcrd_cnt = db->rcrd_cnt;
    check(fwrite(&header, sizeof(header), 1, fd) == 1, "Write failed.");

    /* Start serializing the struct and write to the file */
    int i;
    int bucket_max = ol_ht_bucket_max(db->cur_ht_size);
    ol_bucket *item;
    for (i = 0; i < bucket_max; i++) {
        item = db->hashes[i];
        if (item != NULL)
            check(_ol_write_bucket(item, fd) == 0, "Recording bucket failed.");
    }

    fflush(fd);
    fclose(fd);

    debug("Renaming file: %s -> %s.", tmpfile, db->dump_file);
    check(rename(tmpfile, db->dump_file) == 0, "Could not rename.")

    return 0;

error:
    unlink(tmpfile);
    return -1;
}

int ol_load_db(ol_database *db, char *filename) {
    /* Pass in a filename so we can load in dumpfiles other than what
     * the db thinks we should have. For instance, a new or alternate
     * dataset.
     */
    FILE *fd;
    int i, dump_version;
    struct dump_header header;

    debug("Opening file %s", filename);
    fd = fopen(filename, "r");
    check(fd, "Failed to open file: %s", filename);

    fread(&header, sizeof(header), 1, fd);
    if (memcmp(header.sig, DUMP_SIG, 4) != 0) {
        fclose(fd);
        printf("Error: Not a valid oleg dump\n");
        return -1;
    }

    dump_version = atoi(header.version);
    if (dump_version != DUMP_VERSION) {
        fclose(fd);
        printf("Error: Cannot parse this version\n");
        return -1;
    }

    /* Load up the database */
    for (i = 0; i < header.rcrd_cnt; i++) {
        check(_ol_store_bin_object(db, fd) == 0, "Could not read item.");
    }

    fclose(fd);

    return 0;

error:
    return -1;
}