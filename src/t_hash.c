/*
 * Copyright (c) 2009-2012, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "server.h"
#include <math.h>
#include <stdbool.h>

/*-----------------------------------------------------------------------------
 * Hash Entry API
 *----------------------------------------------------------------------------*/

/* The hashTypeEntry pointer is the field sds. We encode the entry layout type
 * in the field SDS header. Field type SDS_TYPE_5 doesn't have any spare bits to
 * encode this so we use it only for the first layout type.
 *
 * Entry with embedded value, used for small sizes. The value is stored as
 * SDS_TYPE_8. The field can use any SDS type.
 *
 *     +--------------+---------------+
 *     | field        | value         |
 *     | hdr "foo" \0 | hdr8 "bar" \0 |
 *     +------^-------+---------------+
 *            |
 *            |
 *          entry pointer = field sds
 *
 * Entry with value pointer, used for larger fields and values. The field is SDS
 * type 8 or higher.
 *
 *     +-------+--------------+
 *     | value | field        |
 *     | ptr   | hdr "foo" \0 |
 *     +-------+------^-------+
 *                    |
 *                    |
 *                 entry pointer = field sds
 */

/* The maximum allocation size we want to use for entries with embedded
 * values. */
#define EMBED_VALUE_MAX_ALLOC_SIZE 128

/* SDS aux flag. If set, it indicates that the entry has an embedded value
 * pointer located in memory before the embedded field. If unset, the entry
 * instead has an embedded value located after the embedded field. */
#define FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR 0

static inline bool entryHasValuePtr(const hashTypeEntry *entry) {
    return sdsGetAuxBit(entry, FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR);
}

/* Returns the location of a pointer to a separately allocated value. Only for
 * an entry without an embedded value. */
static sds *hashTypeEntryGetValueRef(const hashTypeEntry *entry) {
    serverAssert(entryHasValuePtr(entry));
    char *field_data = sdsAllocPtr(entry);
    field_data -= sizeof(sds *);
    return (sds *)field_data;
}

/* takes ownership of value, does not take ownership of field */
hashTypeEntry *hashTypeCreateEntry(sds field, sds value) {
    size_t field_len = sdslen(field);
    int field_sds_type = sdsReqType(field_len);
    size_t field_size = sdsReqSize(field_len, field_sds_type);
    size_t value_len = sdslen(value);
    size_t value_size = sdsReqSize(value_len, SDS_TYPE_8);
    sds embedded_field_sds;
    if (field_size + value_size <= EMBED_VALUE_MAX_ALLOC_SIZE) {
        /* Embed field and value. Value is fixed to SDS_TYPE_8. Unused
         * allocation space is recorded in the embedded value's SDS header.
         *
         *     +--------------+---------------+
         *     | field        | value         |
         *     | hdr "foo" \0 | hdr8 "bar" \0 |
         *     +--------------+---------------+
         */
        size_t min_size = field_size + value_size;
        size_t buf_size;
        char *buf = zmalloc_usable(min_size, &buf_size);
        embedded_field_sds = sdswrite(buf, field_size, field_sds_type, field, field_len);
        sdswrite(buf + field_size, buf_size - field_size, SDS_TYPE_8, value, value_len);
        /* Field sds aux bits are zero, which we use for this entry encoding. */
        sdsSetAuxBit(embedded_field_sds, FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR, 0);
        serverAssert(!entryHasValuePtr(embedded_field_sds));
        sdsfree(value);
    } else {
        /* Embed field, but not value. Field must be >= SDS_TYPE_8 to encode to
         * indicate this type of entry.
         *
         *     +-------+---------------+
         *     | value | field         |
         *     | ptr   | hdr8 "foo" \0 |
         *     +-------+---------------+
         */
        char field_sds_type = sdsReqType(field_len);
        if (field_sds_type == SDS_TYPE_5) field_sds_type = SDS_TYPE_8;
        field_size = sdsReqSize(field_len, field_sds_type);
        size_t alloc_size = sizeof(sds *) + field_size;
        char *buf = zmalloc(alloc_size);
        *(sds *)buf = value;
        embedded_field_sds = sdswrite(buf + sizeof(sds *), field_size, field_sds_type, field, field_len);
        /* Store the entry encoding type in sds aux bits. */
        sdsSetAuxBit(embedded_field_sds, FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR, 1);
        serverAssert(entryHasValuePtr(embedded_field_sds));
    }
    return (void *)embedded_field_sds;
}

/* The entry pointer is the field sds, but that's an implementation detail. */
sds hashTypeEntryGetField(const hashTypeEntry *entry) {
    return (sds)entry;
}

sds hashTypeEntryGetValue(const hashTypeEntry *entry) {
    if (entryHasValuePtr(entry)) {
        return *hashTypeEntryGetValueRef(entry);
    } else {
        /* Skip field content, field null terminator and value sds8 hdr. */
        size_t offset = sdslen(entry) + 1 + sdsHdrSize(SDS_TYPE_8);
        return (char *)entry + offset;
    }
}

/* Returns the address of the entry allocation. */
static void *hashTypeEntryAllocPtr(hashTypeEntry *entry) {
    char *buf = sdsAllocPtr(entry);
    if (entryHasValuePtr(entry)) {
        buf -= sizeof(sds *);
    }
    return buf;
}

/* Frees previous value, takes ownership of new value, returns entry (may be
 * reallocated). */
static hashTypeEntry *hashTypeEntryReplaceValue(hashTypeEntry *entry, sds value) {
    sds field = (sds)entry;
    size_t field_size = sdsHdrSize(sdsType(field)) + sdsalloc(field) + 1;
    size_t value_len = sdslen(value);
    size_t value_size = sdsReqSize(value_len, SDS_TYPE_8);
    if (!entryHasValuePtr(entry)) {
        /* Reuse the allocation if the new value fits and leaves no more than
         * 25% unused space after replacing the value. */
        char *alloc_ptr = sdsAllocPtr(entry);
        size_t required_size = field_size + value_size;
        size_t alloc_size;
        if (required_size <= EMBED_VALUE_MAX_ALLOC_SIZE &&
            required_size <= (alloc_size = hashTypeEntryMemUsage(entry)) &&
            required_size >= alloc_size * 3 / 4) {
            /* It fits in the allocation and leaves max 25% unused space. */
            sdswrite(alloc_ptr + field_size, alloc_size - field_size, SDS_TYPE_8, value, value_len);
            sdsfree(value);
            return entry;
        }
        hashTypeEntry *new_entry = hashTypeCreateEntry(hashTypeEntryGetField(entry), value);
        freeHashTypeEntry(entry);
        return new_entry;
    } else {
        /* The value pointer is located before the embedded field. */
        if (field_size + value_size <= EMBED_VALUE_MAX_ALLOC_SIZE) {
            /* Convert to entry with embedded value. */
            hashTypeEntry *new_entry = hashTypeCreateEntry(field, value);
            freeHashTypeEntry(entry);
            return new_entry;
        } else {
            /* Not embedded value. */
            sds *value_ref = hashTypeEntryGetValueRef(entry);
            sdsfree(*value_ref);
            *value_ref = value;
            return entry;
        }
    }
}

/* Returns memory usage of a hashTypeEntry, including all allocations owned by
 * the hashTypeEntry. */
size_t hashTypeEntryMemUsage(hashTypeEntry *entry) {
    size_t mem = 0;
    if (entryHasValuePtr(entry)) {
        /* Alloc size is not stored in the embedded field. */
        mem = zmalloc_usable_size(hashTypeEntryAllocPtr(entry));
        mem += sdsAllocSize(*hashTypeEntryGetValueRef(entry));
    } else {
        /* Remaining alloc size is encoded in the embedded value SDS header. */
        sds field = entry;
        sds value = (char *)entry + sdslen(field) + 1 + sdsHdrSize(SDS_TYPE_8);
        size_t field_size = sdsHdrSize(sdsType(field)) + sdslen(field) + 1;
        size_t value_size = sdsHdrSize(SDS_TYPE_8) + sdsalloc(value) + 1;
        mem = field_size + value_size;
    }
    return mem;
}

/* Defragments a hashtable entry (field-value pair) if needed, using the
 * provided defrag functions. The defrag functions return NULL if the allocation
 * was not moved, otherwise they return a pointer to the new memory location.
 * A separate sds defrag function is needed because of the unique memory layout
 * of sds strings.
 * If the location of the hashTypeEntry changed we return the new location,
 * otherwise we return NULL. */
hashTypeEntry *hashTypeEntryDefrag(hashTypeEntry *entry, void *(*defragfn)(void *), sds (*sdsdefragfn)(sds)) {
    if (entryHasValuePtr(entry)) {
        sds *value_ref = hashTypeEntryGetValueRef(entry);
        sds new_value = sdsdefragfn(*value_ref);
        if (new_value) *value_ref = new_value;
    }
    char *allocation = hashTypeEntryAllocPtr(entry);
    char *new_allocation = defragfn(allocation);
    if (new_allocation != NULL) {
        /* Return the same offset into the new allocation as the entry's offset
         * in the old allocation. */
        return new_allocation + ((char *)entry - allocation);
    }
    return NULL;
}

/* Used for releasing memory to OS to avoid unnecessary CoW. Called when we've
 * forked and memory won't be used again. See zmadvise_dontneed() */
void dismissHashTypeEntry(hashTypeEntry *entry) {
    /* Only dismiss values memory since the field size usually is small. */
    if (entryHasValuePtr(entry)) {
        dismissSds(*hashTypeEntryGetValueRef(entry));
    }
}

void freeHashTypeEntry(hashTypeEntry *entry) {
    if (entryHasValuePtr(entry)) {
        sdsfree(*hashTypeEntryGetValueRef(entry));
    }
    zfree(hashTypeEntryAllocPtr(entry));
}

/*-----------------------------------------------------------------------------
 * Hash type API
 *----------------------------------------------------------------------------*/

/* Check the length of a number of objects to see if we need to convert a
 * listpack to a real hash. Note that we only check string encoded objects
 * as their string length can be queried in constant time. */
void hashTypeTryConversion(robj *o, robj **argv, int start, int end) {
    int i;
    size_t sum = 0;

    if (o->encoding != OBJ_ENCODING_LISTPACK) return;

    /* We guess that most of the values in the input are unique, so
     * if there are enough arguments we create a pre-sized hash, which
     * might over allocate memory if there are duplicates. */
    size_t new_fields = (end - start + 1) / 2;
    if (new_fields > server.hash_max_listpack_entries) {
        hashTypeConvert(o, OBJ_ENCODING_HASHTABLE);
        hashtableExpand(o->ptr, new_fields);
        return;
    }

    for (i = start; i <= end; i++) {
        if (!sdsEncodedObject(argv[i])) continue;
        size_t len = sdslen(argv[i]->ptr);
        if (len > server.hash_max_listpack_value) {
            hashTypeConvert(o, OBJ_ENCODING_HASHTABLE);
            return;
        }
        sum += len;
    }
    if (!lpSafeToAdd(o->ptr, sum)) hashTypeConvert(o, OBJ_ENCODING_HASHTABLE);
}

/* Get the value from a listpack encoded hash, identified by field.
 * Returns -1 when the field cannot be found. */
int hashTypeGetFromListpack(robj *o, sds field, unsigned char **vstr, unsigned int *vlen, long long *vll) {
    unsigned char *zl, *fptr = NULL, *vptr = NULL;

    serverAssert(o->encoding == OBJ_ENCODING_LISTPACK);

    zl = o->ptr;
    fptr = lpFirst(zl);
    if (fptr != NULL) {
        fptr = lpFind(zl, fptr, (unsigned char *)field, sdslen(field), 1);
        if (fptr != NULL) {
            /* Grab pointer to the value (fptr points to the field) */
            vptr = lpNext(zl, fptr);
            serverAssert(vptr != NULL);
        }
    }

    if (vptr != NULL) {
        *vstr = lpGetValue(vptr, vlen, vll);
        return 0;
    }

    return -1;
}

/* Get the value from a hash table encoded hash, identified by field.
 * Returns NULL when the field cannot be found, otherwise the SDS value
 * is returned. */
sds hashTypeGetFromHashTable(robj *o, sds field) {
    serverAssert(o->encoding == OBJ_ENCODING_HASHTABLE);
    void *found_element;
    if (!hashtableFind(o->ptr, field, &found_element)) return NULL;
    return hashTypeEntryGetValue(found_element);
}

/* Higher level function of hashTypeGet*() that returns the hash value
 * associated with the specified field. If the field is found C_OK
 * is returned, otherwise C_ERR. The returned object is returned by
 * reference in either *vstr and *vlen if it's returned in string form,
 * or stored in *vll if it's returned as a number.
 *
 * If *vll is populated *vstr is set to NULL, so the caller
 * can always check the function return by checking the return value
 * for C_OK and checking if vll (or vstr) is NULL. */
int hashTypeGetValue(robj *o, sds field, unsigned char **vstr, unsigned int *vlen, long long *vll) {
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        *vstr = NULL;
        if (hashTypeGetFromListpack(o, field, vstr, vlen, vll) == 0) return C_OK;
    } else if (o->encoding == OBJ_ENCODING_HASHTABLE) {
        sds value = hashTypeGetFromHashTable(o, field);
        if (value != NULL) {
            *vstr = (unsigned char *)value;
            *vlen = sdslen(value);
            return C_OK;
        }
    } else {
        serverPanic("Unknown hash encoding");
    }
    return C_ERR;
}

/* Like hashTypeGetValue() but returns an Object, which is useful for
 * interaction with the hash type outside t_hash.c.
 * The function returns NULL if the field is not found in the hash. Otherwise
 * a newly allocated string object with the value is returned. */
robj *hashTypeGetValueObject(robj *o, sds field) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vll;

    if (hashTypeGetValue(o, field, &vstr, &vlen, &vll) == C_ERR) return NULL;
    if (vstr)
        return createStringObject((char *)vstr, vlen);
    else
        return createStringObjectFromLongLong(vll);
}

/* Higher level function using hashTypeGet*() to return the length of the
 * object associated with the requested field, or 0 if the field does not
 * exist. */
size_t hashTypeGetValueLength(robj *o, sds field) {
    size_t len = 0;
    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    if (hashTypeGetValue(o, field, &vstr, &vlen, &vll) == C_OK) len = vstr ? vlen : sdigits10(vll);

    return len;
}

/* Test if the specified field exists in the given hash. Returns 1 if the field
 * exists, and 0 when it doesn't. */
int hashTypeExists(robj *o, sds field) {
    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    return hashTypeGetValue(o, field, &vstr, &vlen, &vll) == C_OK;
}

/* Add a new field, overwrite the old with the new value if it already exists.
 * Return 0 on insert and 1 on update.
 *
 * By default, the field and value SDS strings are copied if needed, so the
 * caller retains ownership of the strings passed. However this behavior
 * can be effected by passing appropriate flags (possibly bitwise OR-ed):
 *
 * HASH_SET_TAKE_FIELD -- The SDS field ownership passes to the function.
 * HASH_SET_TAKE_VALUE -- The SDS value ownership passes to the function.
 *
 * When the flags are used the caller does not need to release the passed
 * SDS string(s). It's up to the function to use the string to create a new
 * entry or to free the SDS string before returning to the caller.
 *
 * HASH_SET_COPY corresponds to no flags passed, and means the default
 * semantics of copying the values if needed.
 *
 */
#define HASH_SET_TAKE_FIELD (1 << 0)
#define HASH_SET_TAKE_VALUE (1 << 1)
#define HASH_SET_COPY 0
int hashTypeSet(robj *o, sds field, sds value, int flags) {
    int update = 0;

    /* Check if the field is too long for listpack, and convert before adding the item.
     * This is needed for HINCRBY* case since in other commands this is handled early by
     * hashTypeTryConversion, so this check will be a NOP. */
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        if (sdslen(field) > server.hash_max_listpack_value || sdslen(value) > server.hash_max_listpack_value)
            hashTypeConvert(o, OBJ_ENCODING_HASHTABLE);
    }

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl, *fptr, *vptr;

        zl = o->ptr;
        fptr = lpFirst(zl);
        if (fptr != NULL) {
            fptr = lpFind(zl, fptr, (unsigned char *)field, sdslen(field), 1);
            if (fptr != NULL) {
                /* Grab pointer to the value (fptr points to the field) */
                vptr = lpNext(zl, fptr);
                serverAssert(vptr != NULL);
                update = 1;

                /* Replace value */
                zl = lpReplace(zl, &vptr, (unsigned char *)value, sdslen(value));
            }
        }

        if (!update) {
            /* Push new field/value pair onto the tail of the listpack */
            zl = lpAppend(zl, (unsigned char *)field, sdslen(field));
            zl = lpAppend(zl, (unsigned char *)value, sdslen(value));
        }
        o->ptr = zl;

        /* Check if the listpack needs to be converted to a hash table */
        if (hashTypeLength(o) > server.hash_max_listpack_entries) hashTypeConvert(o, OBJ_ENCODING_HASHTABLE);
    } else if (o->encoding == OBJ_ENCODING_HASHTABLE) {
        hashtable *ht = o->ptr;

        sds v;
        if (flags & HASH_SET_TAKE_VALUE) {
            v = value;
            value = NULL;
        } else {
            v = sdsdup(value);
        }

        hashtablePosition position;
        void *existing;
        if (hashtableFindPositionForInsert(ht, field, &position, &existing)) {
            /* does not exist yet */
            hashTypeEntry *entry = hashTypeCreateEntry(field, v);
            hashtableInsertAtPosition(ht, entry, &position);
        } else {
            /* exists: replace value */
            void *new_entry = hashTypeEntryReplaceValue(existing, v);
            if (new_entry != existing) {
                /* It has been reallocated. */
                int replaced = hashtableReplaceReallocatedEntry(ht, existing, new_entry);
                serverAssert(replaced);
            }
            update = 1;
        }
    } else {
        serverPanic("Unknown hash encoding");
    }

    /* Free SDS strings we did not referenced elsewhere if the flags
     * want this function to be responsible. */
    if (flags & HASH_SET_TAKE_FIELD && field) sdsfree(field);
    if (flags & HASH_SET_TAKE_VALUE && value) sdsfree(value);
    return update;
}

/* Delete an element from a hash.
 * Return 1 on deleted and 0 on not found. */
int hashTypeDelete(robj *o, sds field) {
    int deleted = 0;

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl, *fptr;

        zl = o->ptr;
        fptr = lpFirst(zl);
        if (fptr != NULL) {
            fptr = lpFind(zl, fptr, (unsigned char *)field, sdslen(field), 1);
            if (fptr != NULL) {
                /* Delete both field and value. */
                zl = lpDeleteRangeWithEntry(zl, &fptr, 2);
                o->ptr = zl;
                deleted = 1;
            }
        }
    } else if (o->encoding == OBJ_ENCODING_HASHTABLE) {
        hashtable *ht = o->ptr;
        deleted = hashtableDelete(ht, field);
    } else {
        serverPanic("Unknown hash encoding");
    }
    return deleted;
}

/* Return the number of elements in a hash. */
unsigned long hashTypeLength(const robj *o) {
    switch (o->encoding) {
    case OBJ_ENCODING_LISTPACK:
        return lpLength(o->ptr) / 2;
    case OBJ_ENCODING_HASHTABLE:
        return hashtableSize((const hashtable *)o->ptr);
    default:
        serverPanic("Unknown hash encoding");
        return ULONG_MAX;
    }
}

void hashTypeInitIterator(robj *subject, hashTypeIterator *hi) {
    hi->subject = subject;
    hi->encoding = subject->encoding;

    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        hi->fptr = NULL;
        hi->vptr = NULL;
    } else if (hi->encoding == OBJ_ENCODING_HASHTABLE) {
        hashtableInitIterator(&hi->iter, subject->ptr, 0);
    } else {
        serverPanic("Unknown hash encoding");
    }
}

void hashTypeResetIterator(hashTypeIterator *hi) {
    if (hi->encoding == OBJ_ENCODING_HASHTABLE) hashtableResetIterator(&hi->iter);
}

/* Move to the next entry in the hash. Return C_OK when the next entry
 * could be found and C_ERR when the iterator reaches the end. */
int hashTypeNext(hashTypeIterator *hi) {
    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl;
        unsigned char *fptr, *vptr;

        zl = hi->subject->ptr;
        fptr = hi->fptr;
        vptr = hi->vptr;

        if (fptr == NULL) {
            /* Initialize cursor */
            serverAssert(vptr == NULL);
            fptr = lpFirst(zl);
        } else {
            /* Advance cursor */
            serverAssert(vptr != NULL);
            fptr = lpNext(zl, vptr);
        }
        if (fptr == NULL) return C_ERR;

        /* Grab pointer to the value (fptr points to the field) */
        vptr = lpNext(zl, fptr);
        serverAssert(vptr != NULL);

        /* fptr, vptr now point to the first or next pair */
        hi->fptr = fptr;
        hi->vptr = vptr;
    } else if (hi->encoding == OBJ_ENCODING_HASHTABLE) {
        if (!hashtableNext(&hi->iter, &hi->next)) return C_ERR;
    } else {
        serverPanic("Unknown hash encoding");
    }
    return C_OK;
}

/* Get the field or value at iterator cursor, for an iterator on a hash value
 * encoded as a listpack. Prototype is similar to `hashTypeGetFromListpack`. */
void hashTypeCurrentFromListpack(hashTypeIterator *hi,
                                 int what,
                                 unsigned char **vstr,
                                 unsigned int *vlen,
                                 long long *vll) {
    serverAssert(hi->encoding == OBJ_ENCODING_LISTPACK);

    if (what & OBJ_HASH_FIELD) {
        *vstr = lpGetValue(hi->fptr, vlen, vll);
    } else {
        *vstr = lpGetValue(hi->vptr, vlen, vll);
    }
}

/* Get the field or value at iterator cursor, for an iterator on a hash value
 * encoded as a hash table. Prototype is similar to
 * `hashTypeGetFromHashTable`. */
sds hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what) {
    serverAssert(hi->encoding == OBJ_ENCODING_HASHTABLE);

    if (what & OBJ_HASH_FIELD) {
        return hashTypeEntryGetField(hi->next);
    } else {
        return hashTypeEntryGetValue(hi->next);
    }
}

/* Higher level function of hashTypeCurrent*() that returns the hash value
 * at current iterator position.
 *
 * The returned element is returned by reference in either *vstr and *vlen if
 * it's returned in string form, or stored in *vll if it's returned as
 * a number.
 *
 * If *vll is populated *vstr is set to NULL, so the caller
 * can always check the function return by checking the return value
 * type checking if vstr == NULL. */
static void hashTypeCurrentObject(hashTypeIterator *hi, int what, unsigned char **vstr, unsigned int *vlen, long long *vll) {
    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        *vstr = NULL;
        hashTypeCurrentFromListpack(hi, what, vstr, vlen, vll);
    } else if (hi->encoding == OBJ_ENCODING_HASHTABLE) {
        sds ele = hashTypeCurrentFromHashTable(hi, what);
        *vstr = (unsigned char *)ele;
        *vlen = sdslen(ele);
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/* Return the field or value at the current iterator position as a new
 * SDS string. */
sds hashTypeCurrentObjectNewSds(hashTypeIterator *hi, int what) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vll;

    hashTypeCurrentObject(hi, what, &vstr, &vlen, &vll);
    if (vstr) return sdsnewlen(vstr, vlen);
    return sdsfromlonglong(vll);
}

robj *hashTypeLookupWriteOrCreate(client *c, robj *key) {
    robj *o = lookupKeyWrite(c->db, key);
    if (checkType(c, o, OBJ_HASH)) return NULL;

    if (o == NULL) {
        o = createHashObject();
        dbAdd(c->db, key, &o);
    }
    return o;
}


void hashTypeConvertListpack(robj *o, int enc) {
    serverAssert(o->encoding == OBJ_ENCODING_LISTPACK);

    if (enc == OBJ_ENCODING_LISTPACK) {
        /* Nothing to do... */

    } else if (enc == OBJ_ENCODING_HASHTABLE) {
        hashTypeIterator hi;

        hashtable *ht = hashtableCreate(&hashHashtableType);

        /* Presize the hashtable to avoid rehashing */
        hashtableExpand(ht, hashTypeLength(o));

        hashTypeInitIterator(o, &hi);
        while (hashTypeNext(&hi) != C_ERR) {
            sds field = hashTypeCurrentObjectNewSds(&hi, OBJ_HASH_FIELD);
            sds value = hashTypeCurrentObjectNewSds(&hi, OBJ_HASH_VALUE);
            hashTypeEntry *entry = hashTypeCreateEntry(field, value);
            sdsfree(field);
            if (!hashtableAdd(ht, entry)) {
                freeHashTypeEntry(entry);
                hashTypeResetIterator(&hi); /* Needed for gcc ASAN */
                serverLogHexDump(LL_WARNING, "listpack with dup elements dump", o->ptr, lpBytes(o->ptr));
                serverPanic("Listpack corruption detected");
            }
        }
        hashTypeResetIterator(&hi);
        zfree(o->ptr);
        o->encoding = OBJ_ENCODING_HASHTABLE;
        o->ptr = ht;
    } else {
        serverPanic("Unknown hash encoding");
    }
}

void hashTypeConvert(robj *o, int enc) {
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        hashTypeConvertListpack(o, enc);
    } else if (o->encoding == OBJ_ENCODING_HASHTABLE) {
        serverPanic("Not implemented");
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/* This is a helper function for the COPY command.
 * Duplicate a hash object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *hashTypeDup(robj *o) {
    robj *hobj;
    hashTypeIterator hi;

    serverAssert(o->type == OBJ_HASH);

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = o->ptr;
        size_t sz = lpBytes(zl);
        unsigned char *new_zl = zmalloc(sz);
        memcpy(new_zl, zl, sz);
        hobj = createObject(OBJ_HASH, new_zl);
        hobj->encoding = OBJ_ENCODING_LISTPACK;
    } else if (o->encoding == OBJ_ENCODING_HASHTABLE) {
        hashtable *ht = hashtableCreate(&hashHashtableType);
        hashtableExpand(ht, hashtableSize((const hashtable *)o->ptr));

        hashTypeInitIterator(o, &hi);
        while (hashTypeNext(&hi) != C_ERR) {
            /* Extract a field-value pair from an original hash object.*/
            sds field = hashTypeCurrentFromHashTable(&hi, OBJ_HASH_FIELD);
            sds value = hashTypeCurrentFromHashTable(&hi, OBJ_HASH_VALUE);

            /* Add a field-value pair to a new hash object. */
            hashTypeEntry *entry = hashTypeCreateEntry(field, sdsdup(value));
            hashtableAdd(ht, entry);
        }
        hashTypeResetIterator(&hi);

        hobj = createObject(OBJ_HASH, ht);
        hobj->encoding = OBJ_ENCODING_HASHTABLE;
    } else {
        serverPanic("Unknown hash encoding");
    }
    return hobj;
}

/* Create a new sds string from the listpack entry. */
sds hashSdsFromListpackEntry(listpackEntry *e) {
    return e->sval ? sdsnewlen(e->sval, e->slen) : sdsfromlonglong(e->lval);
}

/* Reply with bulk string from the listpack entry. */
void hashReplyFromListpackEntry(client *c, listpackEntry *e) {
    if (e->sval)
        addReplyBulkCBuffer(c, e->sval, e->slen);
    else
        addReplyBulkLongLong(c, e->lval);
}

/* Return random element from a non empty hash.
 * 'field' and 'val' will be set to hold the element.
 * The memory in them is not to be freed or modified by the caller.
 * 'val' can be NULL in which case it's not extracted. */
static void hashTypeRandomElement(robj *hashobj, unsigned long hashsize, listpackEntry *field, listpackEntry *val) {
    if (hashobj->encoding == OBJ_ENCODING_HASHTABLE) {
        void *entry;
        hashtableFairRandomEntry(hashobj->ptr, &entry);
        sds sds_field = hashTypeEntryGetField(entry);
        field->sval = (unsigned char *)sds_field;
        field->slen = sdslen(sds_field);
        if (val) {
            sds sds_val = hashTypeEntryGetValue(entry);
            val->sval = (unsigned char *)sds_val;
            val->slen = sdslen(sds_val);
        }
    } else if (hashobj->encoding == OBJ_ENCODING_LISTPACK) {
        lpRandomPair(hashobj->ptr, hashsize, field, val);
    } else {
        serverPanic("Unknown hash encoding");
    }
}


/*-----------------------------------------------------------------------------
 * Hash type commands
 *----------------------------------------------------------------------------*/

void hsetnxCommand(client *c) {
    robj *o;
    if ((o = hashTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) return;

    if (hashTypeExists(o, c->argv[2]->ptr)) {
        addReply(c, shared.czero);
    } else {
        hashTypeTryConversion(o, c->argv, 2, 3);
        hashTypeSet(o, c->argv[2]->ptr, c->argv[3]->ptr, HASH_SET_COPY);
        addReply(c, shared.cone);
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_HASH, "hset", c->argv[1], c->db->id);
        server.dirty++;
    }
}

void hsetCommand(client *c) {
    int i, created = 0;
    robj *o;

    if ((c->argc % 2) == 1) {
        addReplyErrorArity(c);
        return;
    }

    if ((o = hashTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) return;
    hashTypeTryConversion(o, c->argv, 2, c->argc - 1);

    for (i = 2; i < c->argc; i += 2) created += !hashTypeSet(o, c->argv[i]->ptr, c->argv[i + 1]->ptr, HASH_SET_COPY);

    /* HMSET (deprecated) and HSET return value is different. */
    char *cmdname = c->argv[0]->ptr;
    if (cmdname[1] == 's' || cmdname[1] == 'S') {
        /* HSET */
        addReplyLongLong(c, created);
    } else {
        /* HMSET */
        addReply(c, shared.ok);
    }
    signalModifiedKey(c, c->db, c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_HASH, "hset", c->argv[1], c->db->id);
    server.dirty += (c->argc - 2) / 2;
}

void hincrbyCommand(client *c) {
    long long value, incr, oldvalue;
    robj *o;
    sds new;
    unsigned char *vstr;
    unsigned int vlen;

    if (getLongLongFromObjectOrReply(c, c->argv[3], &incr, NULL) != C_OK) return;
    if ((o = hashTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) return;
    if (hashTypeGetValue(o, c->argv[2]->ptr, &vstr, &vlen, &value) == C_OK) {
        if (vstr) {
            if (string2ll((char *)vstr, vlen, &value) == 0) {
                addReplyError(c, "hash value is not an integer");
                return;
            }
        } /* Else hashTypeGetValue() already stored it into &value */
    } else {
        value = 0;
    }

    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN - oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX - oldvalue))) {
        addReplyError(c, "increment or decrement would overflow");
        return;
    }
    value += incr;
    new = sdsfromlonglong(value);
    hashTypeSet(o, c->argv[2]->ptr, new, HASH_SET_TAKE_VALUE);
    addReplyLongLong(c, value);
    signalModifiedKey(c, c->db, c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_HASH, "hincrby", c->argv[1], c->db->id);
    server.dirty++;
}

void hincrbyfloatCommand(client *c) {
    long double value, incr;
    long long ll;
    robj *o;
    sds new;
    unsigned char *vstr;
    unsigned int vlen;

    if (getLongDoubleFromObjectOrReply(c, c->argv[3], &incr, NULL) != C_OK) return;
    if (isnan(incr) || isinf(incr)) {
        addReplyError(c, "value is NaN or Infinity");
        return;
    }
    if ((o = hashTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) return;
    if (hashTypeGetValue(o, c->argv[2]->ptr, &vstr, &vlen, &ll) == C_OK) {
        if (vstr) {
            if (string2ld((char *)vstr, vlen, &value) == 0) {
                addReplyError(c, "hash value is not a float");
                return;
            }
        } else {
            value = (long double)ll;
        }
    } else {
        value = 0;
    }

    value += incr;
    if (isnan(value) || isinf(value)) {
        addReplyError(c, "increment would produce NaN or Infinity");
        return;
    }

    char buf[MAX_LONG_DOUBLE_CHARS];
    int len = ld2string(buf, sizeof(buf), value, LD_STR_HUMAN);
    new = sdsnewlen(buf, len);
    hashTypeSet(o, c->argv[2]->ptr, new, HASH_SET_TAKE_VALUE);
    addReplyBulkCBuffer(c, buf, len);
    signalModifiedKey(c, c->db, c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_HASH, "hincrbyfloat", c->argv[1], c->db->id);
    server.dirty++;

    /* Always replicate HINCRBYFLOAT as an HSET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
    robj *newobj;
    newobj = createRawStringObject(buf, len);
    rewriteClientCommandArgument(c, 0, shared.hset);
    rewriteClientCommandArgument(c, 3, newobj);
    decrRefCount(newobj);
}

static void addHashFieldToReply(client *c, robj *o, sds field) {
    if (o == NULL) {
        addReplyNull(c);
        return;
    }

    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    if (hashTypeGetValue(o, field, &vstr, &vlen, &vll) == C_OK) {
        if (vstr) {
            addReplyBulkCBuffer(c, vstr, vlen);
        } else {
            addReplyBulkLongLong(c, vll);
        }
    } else {
        addReplyNull(c);
    }
}

void hgetCommand(client *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.null[c->resp])) == NULL || checkType(c, o, OBJ_HASH)) return;

    addHashFieldToReply(c, o, c->argv[2]->ptr);
}

void hmgetCommand(client *c) {
    robj *o;
    int i;

    /* Don't abort when the key cannot be found. Non-existing keys are empty
     * hashes, where HMGET should respond with a series of null bulks. */
    o = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_HASH)) return;

    addReplyArrayLen(c, c->argc - 2);
    for (i = 2; i < c->argc; i++) {
        addHashFieldToReply(c, o, c->argv[i]->ptr);
    }
}

void hdelCommand(client *c) {
    robj *o;
    int j, deleted = 0, keyremoved = 0;

    if ((o = lookupKeyWriteOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, o, OBJ_HASH)) return;

    for (j = 2; j < c->argc; j++) {
        if (hashTypeDelete(o, c->argv[j]->ptr)) {
            deleted++;
            if (hashTypeLength(o) == 0) {
                dbDelete(c->db, c->argv[1]);
                keyremoved = 1;
                break;
            }
        }
    }
    if (deleted) {
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_HASH, "hdel", c->argv[1], c->db->id);
        if (keyremoved) notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c, deleted);
}

void hlenCommand(client *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, o, OBJ_HASH)) return;

    addReplyLongLong(c, hashTypeLength(o));
}

void hstrlenCommand(client *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, o, OBJ_HASH)) return;
    addReplyLongLong(c, hashTypeGetValueLength(o, c->argv[2]->ptr));
}

static void addHashIteratorCursorToReply(writePreparedClient *wpc, hashTypeIterator *hi, int what) {
    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        hashTypeCurrentFromListpack(hi, what, &vstr, &vlen, &vll);
        if (vstr)
            addWritePreparedReplyBulkCBuffer(wpc, vstr, vlen);
        else
            addWritePreparedReplyBulkLongLong(wpc, vll);
    } else if (hi->encoding == OBJ_ENCODING_HASHTABLE) {
        sds value = hashTypeCurrentFromHashTable(hi, what);
        addWritePreparedReplyBulkCBuffer(wpc, value, sdslen(value));
    } else {
        serverPanic("Unknown hash encoding");
    }
}

void genericHgetallCommand(client *c, int flags) {
    robj *o;
    hashTypeIterator hi;
    int length, count = 0;

    robj *emptyResp = (flags & OBJ_HASH_FIELD && flags & OBJ_HASH_VALUE) ? shared.emptymap[c->resp] : shared.emptyarray;
    if ((o = lookupKeyReadOrReply(c, c->argv[1], emptyResp)) == NULL || checkType(c, o, OBJ_HASH)) return;

    writePreparedClient *wpc = prepareClientForFutureWrites(c);
    if (!wpc) return;
    /* We return a map if the user requested fields and values, like in the
     * HGETALL case. Otherwise to use a flat array makes more sense. */
    length = hashTypeLength(o);
    if (flags & OBJ_HASH_FIELD && flags & OBJ_HASH_VALUE) {
        addWritePreparedReplyMapLen(wpc, length);
    } else {
        addWritePreparedReplyArrayLen(wpc, length);
    }

    hashTypeInitIterator(o, &hi);
    while (hashTypeNext(&hi) != C_ERR) {
        if (flags & OBJ_HASH_FIELD) {
            addHashIteratorCursorToReply(wpc, &hi, OBJ_HASH_FIELD);
            count++;
        }
        if (flags & OBJ_HASH_VALUE) {
            addHashIteratorCursorToReply(wpc, &hi, OBJ_HASH_VALUE);
            count++;
        }
    }

    hashTypeResetIterator(&hi);

    /* Make sure we returned the right number of elements. */
    if (flags & OBJ_HASH_FIELD && flags & OBJ_HASH_VALUE) count /= 2;
    serverAssert(count == length);
}

void hkeysCommand(client *c) {
    genericHgetallCommand(c, OBJ_HASH_FIELD);
}

void hvalsCommand(client *c) {
    genericHgetallCommand(c, OBJ_HASH_VALUE);
}

void hgetallCommand(client *c) {
    genericHgetallCommand(c, OBJ_HASH_FIELD | OBJ_HASH_VALUE);
}

void hexistsCommand(client *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, o, OBJ_HASH)) return;

    addReply(c, hashTypeExists(o, c->argv[2]->ptr) ? shared.cone : shared.czero);
}

void hscanCommand(client *c) {
    robj *o;
    unsigned long long cursor;

    if (parseScanCursorOrReply(c, c->argv[2], &cursor) == C_ERR) return;
    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.emptyscan)) == NULL || checkType(c, o, OBJ_HASH)) return;
    scanGenericCommand(c, o, cursor);
}

static void hrandfieldReplyWithListpack(writePreparedClient *wpc, unsigned int count, listpackEntry *fields, listpackEntry *vals) {
    client *c = (client *)wpc;
    for (unsigned long i = 0; i < count; i++) {
        if (vals && c->resp > 2) addWritePreparedReplyArrayLen(wpc, 2);
        if (fields[i].sval)
            addWritePreparedReplyBulkCBuffer(wpc, fields[i].sval, fields[i].slen);
        else
            addWritePreparedReplyBulkLongLong(wpc, fields[i].lval);
        if (vals) {
            if (vals[i].sval)
                addWritePreparedReplyBulkCBuffer(wpc, vals[i].sval, vals[i].slen);
            else
                addWritePreparedReplyBulkLongLong(wpc, vals[i].lval);
        }
    }
}

/* How many times bigger should be the hash compared to the requested size
 * for us to not use the "remove elements" strategy? Read later in the
 * implementation for more info. */
#define HRANDFIELD_SUB_STRATEGY_MUL 3

/* If client is trying to ask for a very large number of random elements,
 * queuing may consume an unlimited amount of memory, so we want to limit
 * the number of randoms per time. */
#define HRANDFIELD_RANDOM_SAMPLE_LIMIT 1000

void hrandfieldWithCountCommand(client *c, long l, int withvalues) {
    unsigned long count, size;
    int uniq = 1;
    robj *hash;

    if ((hash = lookupKeyReadOrReply(c, c->argv[1], shared.emptyarray)) == NULL || checkType(c, hash, OBJ_HASH)) return;
    size = hashTypeLength(hash);

    if (l >= 0) {
        count = (unsigned long)l;
    } else {
        count = -l;
        uniq = 0;
    }

    /* If count is zero, serve it ASAP to avoid special cases later. */
    if (count == 0) {
        addReply(c, shared.emptyarray);
        return;
    }

    writePreparedClient *wpc = prepareClientForFutureWrites(c);
    if (!wpc) return;
    /* CASE 1: The count was negative, so the extraction method is just:
     * "return N random elements" sampling the whole set every time.
     * This case is trivial and can be served without auxiliary data
     * structures. This case is the only one that also needs to return the
     * elements in random order. */
    if (!uniq || count == 1) {
        if (withvalues && c->resp == 2)
            addWritePreparedReplyArrayLen(wpc, count * 2);
        else
            addWritePreparedReplyArrayLen(wpc, count);
        if (hash->encoding == OBJ_ENCODING_HASHTABLE) {
            while (count--) {
                void *entry;
                hashtableFairRandomEntry(hash->ptr, &entry);
                sds field = hashTypeEntryGetField(entry);
                sds value = hashTypeEntryGetValue(entry);
                if (withvalues && c->resp > 2) addWritePreparedReplyArrayLen(wpc, 2);
                addWritePreparedReplyBulkCBuffer(wpc, field, sdslen(field));
                if (withvalues) addWritePreparedReplyBulkCBuffer(wpc, value, sdslen(value));
                if (c->flag.close_asap) break;
            }
        } else if (hash->encoding == OBJ_ENCODING_LISTPACK) {
            listpackEntry *fields, *vals = NULL;
            unsigned long limit, sample_count;

            limit = count > HRANDFIELD_RANDOM_SAMPLE_LIMIT ? HRANDFIELD_RANDOM_SAMPLE_LIMIT : count;
            fields = zmalloc(sizeof(listpackEntry) * limit);
            if (withvalues) vals = zmalloc(sizeof(listpackEntry) * limit);
            while (count) {
                sample_count = count > limit ? limit : count;
                count -= sample_count;
                lpRandomPairs(hash->ptr, sample_count, fields, vals);
                hrandfieldReplyWithListpack(wpc, sample_count, fields, vals);
                if (c->flag.close_asap) break;
            }
            zfree(fields);
            zfree(vals);
        }
        return;
    }

    /* Initiate reply count, RESP3 responds with nested array, RESP2 with flat one. */
    long reply_size = count < size ? count : size;
    if (withvalues && c->resp == 2)
        addWritePreparedReplyArrayLen(wpc, reply_size * 2);
    else
        addWritePreparedReplyArrayLen(wpc, reply_size);

    /* CASE 2:
     * The number of requested elements is greater than the number of
     * elements inside the hash: simply return the whole hash. */
    if (count >= size) {
        hashTypeIterator hi;
        hashTypeInitIterator(hash, &hi);
        while (hashTypeNext(&hi) != C_ERR) {
            if (withvalues && c->resp > 2) addWritePreparedReplyArrayLen(wpc, 2);
            addHashIteratorCursorToReply(wpc, &hi, OBJ_HASH_FIELD);
            if (withvalues) addHashIteratorCursorToReply(wpc, &hi, OBJ_HASH_VALUE);
        }
        hashTypeResetIterator(&hi);
        return;
    }

    /* CASE 2.5 listpack only. Sampling unique elements, in non-random order.
     * Listpack encoded hashes are meant to be relatively small, so
     * HRANDFIELD_SUB_STRATEGY_MUL isn't necessary and we rather not make
     * copies of the entries. Instead, we emit them directly to the output
     * buffer.
     *
     * And it is inefficient to repeatedly pick one random element from a
     * listpack in CASE 4. So we use this instead. */
    if (hash->encoding == OBJ_ENCODING_LISTPACK) {
        listpackEntry *fields, *vals = NULL;
        fields = zmalloc(sizeof(listpackEntry) * count);
        if (withvalues) vals = zmalloc(sizeof(listpackEntry) * count);
        serverAssert(lpRandomPairsUnique(hash->ptr, count, fields, vals) == count);
        hrandfieldReplyWithListpack(wpc, count, fields, vals);
        zfree(fields);
        zfree(vals);
        return;
    }

    /* CASE 3:
     * The number of elements inside the hash is not greater than
     * HRANDFIELD_SUB_STRATEGY_MUL times the number of requested elements.
     * In this case we create a hash from scratch with all the elements, and
     * subtract random elements to reach the requested number of elements.
     *
     * This is done because if the number of requested elements is just
     * a bit less than the number of elements in the hash, the natural approach
     * used into CASE 4 is highly inefficient. */
    if (count * HRANDFIELD_SUB_STRATEGY_MUL > size) {
        /* Hashtable encoding (generic implementation) */
        hashtable *ht = hashtableCreate(&sdsReplyHashtableType);
        hashtableExpand(ht, size);
        hashtableIterator iter;
        hashtableInitIterator(&iter, hash->ptr, 0);
        void *entry;

        /* Add all the elements into the temporary hashtable. */
        while (hashtableNext(&iter, &entry)) {
            int res = hashtableAdd(ht, entry);
            serverAssert(res);
        }
        serverAssert(hashtableSize(ht) == size);
        hashtableResetIterator(&iter);

        /* Remove random elements to reach the right count. */
        while (size > count) {
            void *element;
            hashtableFairRandomEntry(ht, &element);
            hashtableDelete(ht, element);
            size--;
        }

        /* Reply with what's in the temporary hashtable and release memory */
        hashtableInitIterator(&iter, ht, 0);
        void *next;
        while (hashtableNext(&iter, &next)) {
            sds field = hashTypeEntryGetField(next);
            sds value = hashTypeEntryGetValue(next);
            if (withvalues && c->resp > 2) addWritePreparedReplyArrayLen(wpc, 2);
            addWritePreparedReplyBulkCBuffer(wpc, field, sdslen(field));
            if (withvalues) addWritePreparedReplyBulkCBuffer(wpc, value, sdslen(value));
        }

        hashtableResetIterator(&iter);
        hashtableRelease(ht);
    }

    /* CASE 4: We have a big hash compared to the requested number of elements.
     * In this case we can simply get random elements from the hash and add
     * to the temporary hash, trying to eventually get enough unique elements
     * to reach the specified count. */
    else {
        /* Hashtable encoding (generic implementation) */
        unsigned long added = 0;
        listpackEntry field, value;
        hashtable *ht = hashtableCreate(&setHashtableType);
        hashtableExpand(ht, count);
        while (added < count) {
            hashTypeRandomElement(hash, size, &field, withvalues ? &value : NULL);

            /* Try to add the object to the hashtable. If it already exists
             * free it, otherwise increment the number of objects we have
             * in the result hashtable. */
            sds sfield = hashSdsFromListpackEntry(&field);
            if (!hashtableAdd(ht, sfield)) {
                sdsfree(sfield);
                continue;
            }
            added++;

            /* We can reply right away, so that we don't need to store the value in the dict. */
            if (withvalues && c->resp > 2) addWritePreparedReplyArrayLen(wpc, 2);
            hashReplyFromListpackEntry(c, &field);
            if (withvalues) hashReplyFromListpackEntry(c, &value);
        }

        /* Release memory */
        hashtableRelease(ht);
    }
}

/* HRANDFIELD key [<count> [WITHVALUES]] */
void hrandfieldCommand(client *c) {
    long l;
    int withvalues = 0;
    robj *hash;
    listpackEntry ele;

    if (c->argc >= 3) {
        if (getRangeLongFromObjectOrReply(c, c->argv[2], -LONG_MAX, LONG_MAX, &l, NULL) != C_OK) return;
        if (c->argc > 4 || (c->argc == 4 && strcasecmp(c->argv[3]->ptr, "withvalues"))) {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        } else if (c->argc == 4) {
            withvalues = 1;
            if (l < -LONG_MAX / 2 || l > LONG_MAX / 2) {
                addReplyError(c, "value is out of range");
                return;
            }
        }
        hrandfieldWithCountCommand(c, l, withvalues);
        return;
    }

    /* Handle variant without <count> argument. Reply with simple bulk string */
    if ((hash = lookupKeyReadOrReply(c, c->argv[1], shared.null[c->resp])) == NULL || checkType(c, hash, OBJ_HASH)) {
        return;
    }

    hashTypeRandomElement(hash, hashTypeLength(hash), &ele, NULL);
    hashReplyFromListpackEntry(c, &ele);
}
