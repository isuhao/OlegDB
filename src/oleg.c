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

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "oleg.h"
#include "aol.h"
#include "logging.h"
#include "dump.h"
#include "murmur3.h"
#include "errhandle.h"


inline int ol_ht_bucket_max(size_t ht_size) {
    return (ht_size/sizeof(ol_bucket *));
}

void _ol_get_file_name(ol_database *db, const char *p, char *o_file) {
    sprintf(o_file, "%s/%s.%s", db->path, db->name, p);
}

void _ol_enable(int feature, int *feature_set) {
    *feature_set |= feature;
}

void _ol_disable(int feature, int *feature_set) {
    *feature_set &= ~feature;
}

bool _ol_is_enabled(int feature, int *feature_set) {
    return (*feature_set & feature);
}

ol_database *ol_open(char *path, char *name, int features){
    debug("Opening \"%s\" database", name);
    ol_database *new_db = malloc(sizeof(struct ol_database));

    size_t to_alloc = HASH_MALLOC;
    new_db->hashes = calloc(1, to_alloc);
    new_db->cur_ht_size = to_alloc;

    /* NULL everything */
    int i;
    for (i = 0; i < ol_ht_bucket_max(to_alloc); i++){
        new_db->hashes[i] = NULL;
    }

    time_t created;
    time(&created);
    new_db->created = created;
    new_db->rcrd_cnt = 0;
    new_db->key_collisions = 0;
    new_db->aolfd = 0;

    /* Function pointers for feature flags */
    new_db->enable = &_ol_enable;
    new_db->disable = &_ol_disable;
    new_db->is_enabled = &_ol_is_enabled;

    /* Function pointer building file paths based on db name */
    new_db->get_db_file_name = &_ol_get_file_name;

    memset(new_db->name, '\0', DB_NAME_SIZE);
    strncpy(new_db->name, name, DB_NAME_SIZE);
    memset(new_db->path, '\0', PATH_LENGTH);
    strncpy(new_db->path, path, PATH_LENGTH);

    /* Make sure that the directory the database is in exists */
    struct stat st = {0};
    if (stat(path, &st) == -1) /* Check to see if the DB exists */
        mkdir(path, 0755);

    new_db->dump_file = calloc(1, 512);
    check_mem(new_db->dump_file);
    new_db->get_db_file_name(new_db, "dump", new_db->dump_file);

    new_db->aol_file = calloc(1, 512);
    check_mem(new_db->aol_file);
    new_db->get_db_file_name(new_db, "aol", new_db->aol_file);
    new_db->feature_set = features;
    new_db->state = OL_S_STARTUP;
    if (new_db->is_enabled(OL_F_APPENDONLY, &new_db->feature_set)) {
        ol_aol_init(new_db);
        check(ol_aol_restore(new_db) == 0, "Error restoring from AOL file");
    }
    new_db->state = OL_S_AOKAY;

    return new_db;

error:
    return NULL;
}

static inline void _ol_free_bucket(ol_bucket *ptr) {
    free(ptr->content_type);
    free(ptr->data_ptr);
    free(ptr);
}
int _ol_close(ol_database *db){
    int iterations = ol_ht_bucket_max(db->cur_ht_size);
    int i;
    int rcrd_cnt = db->rcrd_cnt;
    int freed = 0;
    debug("Freeing %d records.", rcrd_cnt);
    debug("Hash table iterations: %d.", iterations);
    for (i = 0; i < iterations; i++) { /* 8=======D */
        if (db->hashes[i] != NULL) {
            ol_bucket *ptr;
            ol_bucket *next;
            for (ptr = db->hashes[i]; NULL != ptr; ptr = next) {
                next = ptr->next;
                _ol_free_bucket(ptr);
                freed++;
            }
        }
    }

    debug("Force flushing files");

    if (db->aolfd) {
        fflush(db->aolfd);
        fclose(db->aolfd);
    }

    debug("Files flushed to disk");

    free(db->hashes);
    free(db->dump_file);
    free(db->aol_file);
    db->feature_set = 0;
    free(db);
    if (freed != rcrd_cnt) {
        ol_log_msg(LOG_INFO, "Error: Couldn't free all records.");
        ol_log_msg(LOG_INFO, "Records freed: %i\n", freed);
        return 1;
    }
    return 0;
}

int ol_close_save(ol_database *db) {
    debug("Saving and closing \"%s\" database.", db->name);
    check(ol_save_db(db) == 0, "Could not save DB.");
    check(_ol_close(db) == 0, "Could not close DB.");

    return 0;

error:
    return 1;
}

int ol_close(ol_database *db) {
    debug("Closing \"%s\" database.", db->name);
    check(_ol_close(db) == 0, "Could not close DB.");

    return 0;

error:
    return 1;
}

int _ol_calc_idx(const size_t ht_size, const uint32_t hash) {
    int index;
    /* Powers of two, baby! */
    index = hash & (ol_ht_bucket_max(ht_size) - 1);
    return index;
}

/* TODO: Refactor this to not allocate a new str, but to fill out a passed in
 * str. This keeps memory management to the parent function. */
static inline char *_ol_trunc(const char *key, size_t klen) {
    /* Silently truncate because #yolo */
    size_t real_key_len = klen > KEY_SIZE ? KEY_SIZE : klen;
    char *_key = malloc(real_key_len+1);
    strncpy(_key, key, real_key_len);
    _key[real_key_len] = '\0';
    debug("New key: %s Klen: %zu", _key, strnlen(_key, KEY_SIZE));
    return _key;
}

ol_bucket *_ol_get_last_bucket_in_slot(ol_bucket *bucket) {
    ol_bucket *tmp_bucket = bucket;
    int depth = 0;
    while (tmp_bucket->next != NULL) {
        tmp_bucket = tmp_bucket->next;
        depth++;
        if (depth > 100)
            ol_log_msg(LOG_WARN, "Depth of bucket stack is crazy, help");
    }
    return tmp_bucket;
}

ol_bucket *_ol_get_bucket(const ol_database *db, const uint32_t hash, const char *key, size_t klen) {
    int index = _ol_calc_idx(db->cur_ht_size, hash);
    size_t larger_key = 0;
    if (db->hashes[index] != NULL) {
        ol_bucket *tmp_bucket = db->hashes[index];
        larger_key = tmp_bucket->klen > klen ? tmp_bucket->klen : klen;
        if (strncmp(tmp_bucket->key, key, larger_key) == 0) {
            return tmp_bucket;
        } else if (tmp_bucket->next != NULL) {
            do {
                tmp_bucket = tmp_bucket->next;
                larger_key = tmp_bucket->klen > klen ? tmp_bucket->klen : klen;
                if (strncmp(tmp_bucket->key, key, larger_key) == 0)
                    return tmp_bucket;
            } while (tmp_bucket->next != NULL);
        }
    }
    return NULL;
}

int _ol_set_bucket(ol_database *db, ol_bucket *bucket) {
    /* TODO: error codes? */
    int index = _ol_calc_idx(db->cur_ht_size, bucket->hash);
    if (db->hashes[index] != NULL) {
        db->key_collisions++;
        ol_bucket *tmp_bucket = db->hashes[index];
        tmp_bucket = _ol_get_last_bucket_in_slot(tmp_bucket);
        tmp_bucket->next = bucket;
    } else {
        db->hashes[index] = bucket;
    }
    db->rcrd_cnt++;
    return 0;
}

static inline void _ol_rehash_insert_bucket(
        ol_bucket **tmp_hashes, const size_t to_alloc, ol_bucket *bucket) {
    int new_index;
    new_index = _ol_calc_idx(to_alloc, bucket->hash);
    if (tmp_hashes[new_index] != NULL) {
        /* Enforce that this is the last bucket, KILL THE ORPHANS */
        ol_bucket *last_bucket = _ol_get_last_bucket_in_slot(
                tmp_hashes[new_index]);
        last_bucket->next = bucket;
    } else {
        tmp_hashes[new_index] = bucket;
    }
}

int _ol_grow_and_rehash_db(ol_database *db) {
    int i;
    ol_bucket *bucket;
    ol_bucket **tmp_hashes = NULL;

    size_t to_alloc = db->cur_ht_size * 2;
    debug("Growing DB to %zu bytes.", to_alloc);
    tmp_hashes = calloc(1, to_alloc);
    check_mem(tmp_hashes);

    int iterations = ol_ht_bucket_max(db->cur_ht_size);
    for (i = 0; i < iterations; i++) {
        bucket = db->hashes[i];
        if (bucket != NULL) {
            /* Rehash the bucket itself. */
            _ol_rehash_insert_bucket(tmp_hashes, to_alloc, bucket);
        }
    }
    free(db->hashes);
    db->hashes = tmp_hashes;
    db->cur_ht_size = to_alloc;
    debug("Current hash table size is now: %zu bytes.", to_alloc);
    return 0;

error:
    return -1;
}

ol_val ol_unjar(ol_database *db, const char *key, size_t klen) {
    return ol_unjar_ds(db, key, klen, NULL);
}

ol_val ol_unjar_ds(ol_database *db, const char *key, size_t klen, size_t *dsize) {
    uint32_t hash;

    char *_key = _ol_trunc(key, klen);
    size_t _klen = strnlen(_key, KEY_SIZE);
    MurmurHash3_x86_32(_key, _klen, DEVILS_SEED, &hash);
    ol_bucket *bucket = _ol_get_bucket(db, hash, _key, _klen);

    if (bucket != NULL) {
        if (dsize != NULL)
            memcpy(dsize, &bucket->data_size, sizeof(size_t));
        free(_key);
        return bucket->data_ptr;
    }

    free(_key);
    return NULL;
}

int _ol_jar(ol_database *db, const char *key, size_t klen, unsigned char *value,
        size_t vsize, const char *ct, const size_t ctsize) {
    int ret;
    uint32_t hash;

    /* Free the _key as soon as possible */
    char *_key = _ol_trunc(key, klen);
    size_t _klen = strnlen(_key, KEY_SIZE);
    MurmurHash3_x86_32(_key, _klen, DEVILS_SEED, &hash);
    ol_bucket *bucket = _ol_get_bucket(db, hash, _key, _klen);

    /* Check to see if we have an existing entry with that key */
    if (bucket) {
        free(_key);
        unsigned char *data = realloc(bucket->data_ptr, vsize);
        if (memcpy(data, value, vsize) != data)
            return 4;

        char *ct_real = realloc(bucket->content_type, ctsize+1);
        if (memcpy(ct_real, ct, ctsize) != ct_real)
            return 5;
        ct_real[ctsize] = '\0';

        bucket->klen = _klen;
        bucket->ctype_size = ctsize;
        bucket->content_type = ct_real;
        bucket->data_size = vsize;
        bucket->data_ptr = data;

        if(db->is_enabled(OL_F_APPENDONLY, &db->feature_set) &&
                db->state != OL_S_STARTUP) {
            ol_aol_write_cmd(db, "JAR", bucket);
        }

        return 0;
    }
    /* Looks like we don't have an old hash */
    ol_bucket *new_bucket = malloc(sizeof(ol_bucket));
    if (new_bucket == NULL)
        return 1;

    if (strncpy(new_bucket->key, _key, KEY_SIZE) != new_bucket->key) {
        free(_key);
        return 2;
    }
    free(_key);
    new_bucket->klen = _klen;

    new_bucket->next = NULL;

    new_bucket->data_size = vsize;
    unsigned char *data = calloc(1, vsize);
    if (memcpy(data, value, vsize) != data)
        return 3;
    new_bucket->data_ptr = data;
    new_bucket->hash = hash;

    new_bucket->ctype_size = ctsize;
    char *ct_real = calloc(1, ctsize+1);
    if (strncpy(ct_real, ct, ctsize) != ct_real)
        return 7;
    ct_real[ctsize] = '\0';
    new_bucket->content_type = ct_real;

    int bucket_max = ol_ht_bucket_max(db->cur_ht_size);
    /* TODO: rehash this shit at 80% */
    if (db->rcrd_cnt > 0 && db->rcrd_cnt == bucket_max) {
        debug("Record count is now %i; growing hash table.", db->rcrd_cnt);
        ret = _ol_grow_and_rehash_db(db);
        if (ret > 0) {
            ol_log_msg(LOG_ERR, "Problem rehashing DB. Error code: %i", ret);
            return 4;
        }
    }

    ret = _ol_set_bucket(db, new_bucket);

    if(ret > 0)
        ol_log_msg(LOG_ERR, "Problem inserting item: Error code: %i", ret);

    if(db->is_enabled(OL_F_APPENDONLY, &db->feature_set) &&
            db->state != OL_S_STARTUP) {
        ol_aol_write_cmd(db, "JAR", new_bucket);
    }


    return 0;
}

int ol_jar(ol_database *db, const char *key, size_t klen, unsigned char *value,
        size_t vsize) {
    return _ol_jar(db, key, klen, value, vsize, "application/octet-stream", 24);
}

int ol_jar_ct(ol_database *db, const char *key, size_t klen, unsigned char *value,
        size_t vsize, const char *content_type, const size_t content_type_size) {
    return _ol_jar(db, key, klen, value, vsize, content_type, content_type_size);
}

int ol_set_expire(ol_database *db, const char *key, size_t klen, const time_t time) {
    return 0;
}

int ol_scoop(ol_database *db, const char *key, size_t klen) {
    /* you know... like scoop some data from the jar and eat it? All gone. */
    uint32_t hash;
    char *_key = _ol_trunc(key, klen);
    size_t _klen = strnlen(_key, KEY_SIZE);

    MurmurHash3_x86_32(_key, _klen, DEVILS_SEED, &hash);
    int index = _ol_calc_idx(db->cur_ht_size, hash);

    if (index < 0) {
        free(_key);
        return 1;
    }

    if (db->hashes[index] != NULL) {
        ol_bucket *bucket = db->hashes[index];

        if (strncmp(bucket->key, _key, _klen) == 0){
            if (bucket->next != NULL) {
                db->hashes[index] = bucket->next;
            } else {
                db->hashes[index] = NULL;
            }
            if(db->is_enabled(OL_F_APPENDONLY, &db->feature_set) &&
                    db->state != OL_S_STARTUP) {
                ol_aol_write_cmd(db, "SCOOP", bucket);
            }
            _ol_free_bucket(bucket);
            free(_key);
            db->rcrd_cnt -= 1;
            return 0;
        } else if (bucket->next != NULL) {
            ol_bucket *last;
            do {
                last = bucket;
                bucket = bucket->next;
                if (strncmp(bucket->key, key, klen) == 0) {
                    if (bucket->next != NULL)
                        last->next = bucket->next;
                    _ol_free_bucket(bucket);
                    db->rcrd_cnt -= 1;
                    free(_key);
                    return 0;
                }
            } while (bucket->next != NULL);
        }
    }
    free(_key);
    return 2;
}

char *ol_content_type(ol_database *db, const char *key, size_t klen) {
    uint32_t hash;
    char *_key = _ol_trunc(key, klen);
    size_t _klen = strnlen(_key, KEY_SIZE);
    MurmurHash3_x86_32(_key, _klen, DEVILS_SEED, &hash);
    ol_bucket *bucket = _ol_get_bucket(db, hash, _key, _klen);
    free(_key);

    if (bucket != NULL)
        return bucket->content_type;

    return NULL;
}

int ol_uptime(ol_database *db) {
    /* Make uptime */
    time_t now;
    double diff;
    time(&now);
    diff = difftime(now, db->created);
    return diff;
}
