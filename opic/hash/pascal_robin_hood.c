/* pascal_robin_hood.c ---
 *
 * Filename: pascal_robin_hood.c
 * Description:
 * Author: Felix Chern
 * Maintainer:
 * Created: Sat May 13 15:57:42 2017 (-0700)
 * Version:
 * Package-Requires: ()
 * Last-Updated:
 *           By:
 *     Update #: 0
 * URL:
 * Doc URL:
 * Keywords:
 * Compatibility:
 *
 */

/* Commentary:
 *
 *
 *
 */

/* Change Log:
 *
 *
 */

/* This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

/* Code: */

#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "opic/common/op_assert.h"
#include "opic/common/op_utils.h"
#include "opic/common/op_log.h"
#include "opic/op_malloc.h"
#include "pascal_robin_hood.h"

#define PROBE_STATS_SIZE 64
#define DEFAULT_LARGE_DATA_THRESHOLD (1UL << 30)
#define PRHH_EMPTY_KEY 0
#define PRHH_TOMBSTONE_KEY ~0ULL
#define VISIT_IDX_CACHE 8

OP_LOGGER_FACTORY(logger, "opic.hash.pascal_robin_hood");

enum upsert_result_t
  {
    UPSERT_EMPTY,
    UPSERT_DUP,
    UPSERT_PUSHDOWN,
  };

static inline enum upsert_result_t
PRHHUpsertNewKey(PascalRobinHoodHash* rhh, OPHash hasher,
                 void* key, size_t keylen, uint64_t hashed_key,
                 uint8_t** matched_bucket, int* probe_state);

static inline void
PRHHUpsertPushDown(PascalRobinHoodHash* rhh, OPHash hasher,
                   uint8_t* bucket_cpy, int probe,
                   uint8_t* avoid_bucket, bool* resize);

static bool
PRHHSizeUp(PascalRobinHoodHash* rhh, OPHash hasher);

struct PascalRobinHoodHash
{
  uint64_t objcnt;
  uint64_t objcnt_high;
  uint64_t objcnt_low;
  uint64_t large_data_threshold;
  uint8_t capacity_clz;    // leading zeros of capacity
  uint8_t capacity_ms4b;   // most significant 4 bits
  uint16_t longest_probes;
  size_t valsize;
  uint32_t stats[PROBE_STATS_SIZE];
  opref_t bucket_ref;
};

bool PRHHNew(OPHeap* heap, PascalRobinHoodHash** rhh,
             uint64_t num_objects, double load, size_t valsize)
{
  uint64_t capacity;
  uint32_t capacity_clz, capacity_ms4b, capacity_msb;
  size_t bucket_size;
  void* bucket_ptr;

  op_assert(load > 0.0 && load < 1.0,
            "load %lf must within close interval (0.0, 1.0)\n", load);
  capacity = (uint64_t)(num_objects / load);
  if (capacity < 8)
    capacity = 8;
  capacity_clz = __builtin_clzl(capacity);
  capacity_msb = 64 - capacity_clz;
  capacity_ms4b = round_up_div(capacity, 1UL << (capacity_msb - 4));
  capacity = (uint64_t)capacity_ms4b << (capacity_msb - 4);

  bucket_size = sizeof(oplenref_t) + valsize;

  *rhh = OPCalloc(heap, 1, sizeof(PascalRobinHoodHash));
  if (!*rhh)
    return false;
  bucket_ptr = OPCalloc(heap, 1, bucket_size * capacity);
  if (!bucket_ptr)
    {
      OPDealloc(rhh);
      return false;
    }
  (*rhh)->bucket_ref = OPPtr2Ref(bucket_ptr);
  (*rhh)->large_data_threshold = DEFAULT_LARGE_DATA_THRESHOLD;
  (*rhh)->capacity_clz = capacity_clz;
  (*rhh)->capacity_ms4b = capacity_ms4b;
  (*rhh)->objcnt_high = (uint64_t)(capacity * load);
  (*rhh)->objcnt_low = capacity * 2 / 10;
  (*rhh)->valsize = valsize;
  return true;
}

void PRHHDestroy(PascalRobinHoodHash* rhh)
{
  const size_t bucket_size = sizeof(oplenref_t) + rhh->valsize;
  uint8_t* const buckets = OPRef2Ptr(rhh, rhh->bucket_ref);
  oplenref_t* recref;
  void* recptr;
  uint64_t capacity = PRHHCapacity(rhh);

  for (uint64_t idx = 0; idx < capacity; idx++)
    {
      recref = (oplenref_t*)&buckets[idx * bucket_size];
      if (*recref != PRHH_EMPTY_KEY &&
          *recref != PRHH_TOMBSTONE_KEY)
        {
          recptr = OPLenRef2Ptr(rhh, *recref);
          OPDealloc(recptr);
        }
    }
  OPDealloc(OPRef2Ptr(rhh, rhh->bucket_ref));
  OPDealloc(rhh);
}

uint64_t PRHHObjcnt(PascalRobinHoodHash* rhh)
{
  return rhh->objcnt;
}

static inline
uint64_t PRHHCapacityInternal(uint8_t capacity_clz, uint8_t capacity_ms4b)
{
  return (1UL << (64 - capacity_clz - 4)) * capacity_ms4b;
}

uint64_t PRHHCapacity(PascalRobinHoodHash* rhh)
{
  return PRHHCapacityInternal(rhh->capacity_clz, rhh->capacity_ms4b);
}

size_t PRHHValsize(PascalRobinHoodHash* rhh)
{
  return rhh->valsize;
}


static inline void
IncreaseProbeStat(PascalRobinHoodHash* rhh, int probe)
{
  rhh->objcnt++;
  rhh->longest_probes = probe > rhh->longest_probes ?
    probe : rhh->longest_probes;
  if (probe < PROBE_STATS_SIZE)
    rhh->stats[probe]++;
  else
    OP_LOG_WARN(logger, "Large probe: %d\n", probe);
}

static inline uintptr_t
hash_with_probe(PascalRobinHoodHash* rhh, uint64_t key, int probe)
{
  uintptr_t mask = (1ULL << (64 - rhh->capacity_clz)) - 1;

  // linear probing
  // uint64_t probed_hash = key + probe * 2;

  // quadratic probing
  uint64_t probed_hash = key + probe * probe * 2;

  // Fast mod and scale
  return (probed_hash & mask) * rhh->capacity_ms4b >> 4;
}

static inline int
findprobe(PascalRobinHoodHash* rhh, OPHash hasher, uintptr_t idx)
{
  const size_t valsize = rhh->valsize;
  const size_t bucket_size = sizeof(oplenref_t) + valsize;
  uint8_t* const buckets = OPRef2Ptr(rhh, rhh->bucket_ref);
  uint64_t hashed_key;
  oplenref_t *keyref;
  void* keyptr;
  size_t keysize;

  keyref = (oplenref_t*)&buckets[idx * bucket_size];
  keyptr = OPLenRef2Ptr(rhh, *keyref);
  keysize = OPLenRef2Size(*keyref);
  hashed_key = hasher(keyptr, keysize);
  for (int i = 0; i <= rhh->longest_probes; i++)
    {
      if (hash_with_probe(rhh, hashed_key, i) == idx)
        return i;
    }
  op_assert(false, "Didn't find any match probe!\n");
  return -1;
}

static inline enum upsert_result_t
PRHHUpsertNewKey(PascalRobinHoodHash* rhh, OPHash hasher,
                 void* key, size_t keysize, uint64_t hashed_key,
                 uint8_t** matched_bucket, int* probe_state)
{
  const size_t refsize = sizeof(oplenref_t);
  const size_t valsize = rhh->valsize;
  const size_t bucket_size = refsize + valsize;
  uint8_t* buckets;
  int probe, old_probe;
  uintptr_t idx, _idx;
  oplenref_t *recref, *_recref;
  void *recptr, *_recptr;
  size_t recsize, _recsize;

  buckets = OPRef2Ptr(rhh, rhh->bucket_ref);
  probe = 0;

  while (true)
    {
      idx = hash_with_probe(rhh, hashed_key, probe);
      if (probe > PROBE_STATS_SIZE)
        {
          PRHHSizeUp(rhh, hasher);
          probe = 0;
          buckets = OPRef2Ptr(rhh, rhh->bucket_ref);
          continue;
        }
      recref = (oplenref_t*)&buckets[idx * bucket_size];
      if (*recref == PRHH_EMPTY_KEY)
        {
          IncreaseProbeStat(rhh, probe);
          *matched_bucket = &buckets[idx * bucket_size];
          return UPSERT_EMPTY;
        }
      else if (*recref == PRHH_TOMBSTONE_KEY)
        {
          for (int p = probe+1; p <= rhh->longest_probes; p++)
            {
              _idx = hash_with_probe(rhh, hashed_key, p);
              _recref = (oplenref_t*)&buckets[_idx * bucket_size];
              if (*_recref == PRHH_EMPTY_KEY ||
                  *_recref == PRHH_TOMBSTONE_KEY)
                continue;
              _recptr = OPLenRef2Ptr(rhh, *_recref);
              _recsize = OPLenRef2Size(*_recref);
              if (_recsize == keysize &&
                  !memcmp(key, _recptr, keysize))
                {
                  *matched_bucket = &buckets[_idx * bucket_size];
                  return UPSERT_DUP;
                }
            }
          IncreaseProbeStat(rhh, probe);
          *matched_bucket = &buckets[idx * bucket_size];
          return UPSERT_EMPTY;
        }
      recptr = OPLenRef2Ptr(rhh, *recref);
      recsize = OPLenRef2Size(*recref);
      if (keysize == recsize &&
          !memcmp(key, recptr, keysize))
        {
          *matched_bucket = &buckets[idx * bucket_size];
          return UPSERT_DUP;
        }
      old_probe = findprobe(rhh, hasher, idx);
      if (probe > old_probe)
        {
          rhh->longest_probes = probe > rhh->longest_probes ?
            probe : rhh->longest_probes;
          rhh->stats[old_probe]--;
          rhh->stats[probe]++;
          *matched_bucket = &buckets[idx * bucket_size];
          *probe_state = old_probe+1;
          return UPSERT_PUSHDOWN;
        }
      probe++;
    }
}

static inline void
PRHHUpsertPushDown(PascalRobinHoodHash* rhh, OPHash hasher,
                   uint8_t* bucket_cpy, int probe, uint8_t* avoid_bucket,
                   bool* resized)
{
  const size_t refsize = sizeof(oplenref_t);
  const size_t valsize = rhh->valsize;
  const size_t bucket_size = refsize + valsize;
  oplenref_t *recref, *_recref;
  void *recptr;
  size_t recsize;
  uint8_t* buckets;
  int old_probe;
  uint8_t bucket_tmp[bucket_size];
  uintptr_t idx;
  uint64_t hashed_key;
  int visit;
  uintptr_t visited_idx[VISIT_IDX_CACHE];

  visit = 0;
  *resized = false;
  buckets = OPRef2Ptr(rhh, rhh->bucket_ref);
  recref = (oplenref_t*)&bucket_cpy[0];
  recptr = OPLenRef2Ptr(rhh, *recref);
  recsize = OPLenRef2Size(*recref);
  hashed_key = hasher(recptr, recsize);
  while (true)
    {
    next_iter:
      idx = hash_with_probe(rhh, hashed_key, probe);

      if (probe > PROBE_STATS_SIZE)
        {
          PRHHSizeUp(rhh, hasher);
          probe = 0;
          buckets = OPRef2Ptr(rhh, rhh->bucket_ref);
          *resized = true;
          continue;
        }
      if (&buckets[idx * bucket_size] == avoid_bucket)
        {
          probe++;
          continue;
        }
      if (visit > 2)  // cycle can happen only after two visit
        {
          if (visit < VISIT_IDX_CACHE)
            {
              for (int i = 0; i < visit; i++)
                {
                  if (idx == visited_idx[i])
                    {
                      probe++;
                      goto next_iter;
                    }
                }
            }
          else
            {
              for (int i = visit + 1; i < visit + VISIT_IDX_CACHE; i++)
                {
                  if (idx == visited_idx[i % VISIT_IDX_CACHE])
                    {
                      probe++;
                      goto next_iter;
                    }
                }
            }
        }
      visited_idx[visit % VISIT_IDX_CACHE] = idx;
      visit++;

      _recref = (oplenref_t*)&buckets[idx * bucket_size];
      // empty bucket and tombstone bucket
      if (*_recref == PRHH_EMPTY_KEY ||
          *_recref == PRHH_TOMBSTONE_KEY)
        {
          IncreaseProbeStat(rhh, probe);
          memcpy(&buckets[idx * bucket_size], bucket_cpy, bucket_size);
          return;
        }

      old_probe = findprobe(rhh, hasher, idx);
      if (probe > old_probe)
        {
          rhh->longest_probes = probe > rhh->longest_probes ?
            probe : rhh->longest_probes;
          rhh->stats[old_probe]--;
          rhh->stats[probe]++;
          memcpy(bucket_tmp, &buckets[idx * bucket_size], bucket_size);
          memcpy(&buckets[idx * bucket_size], bucket_cpy, bucket_size);
          memcpy(bucket_cpy, bucket_tmp, bucket_size);
          probe = old_probe + 1;
          recref = (oplenref_t*)&bucket_cpy[0];
          recptr = OPLenRef2Ptr(rhh, *recref);
          recsize = OPLenRef2Size(*recref);
          hashed_key = hasher(recptr, recsize);
          continue;
        }
      probe++;
    }
}

static bool
PRHHSizeUp(PascalRobinHoodHash* rhh, OPHash hasher)
{
  const size_t keysize = sizeof(oplenref_t);
  const size_t valsize = rhh->valsize;
  const size_t bucket_size = keysize + valsize;
  const size_t large_data_threshold = rhh->large_data_threshold;
  uint8_t* old_buckets;
  uint8_t* new_buckets;
  oplenref_t *recref;
  bool resized;
  uint8_t new_capacity_ms4b, new_capacity_clz;
  uint64_t old_capacity, new_capacity;

  old_capacity = PRHHCapacity(rhh);
  old_buckets = OPRef2Ptr(rhh, rhh->bucket_ref);

  if (old_capacity * bucket_size >= large_data_threshold)
    {
      // increase size by 20% ~ 33%
      switch(rhh->capacity_ms4b)
        {
        case 8:
          new_capacity_ms4b = 10;
          new_capacity_clz = rhh->capacity_clz;
          break;
        case 9:
        case 10:
          new_capacity_ms4b = 12;
          new_capacity_clz = rhh->capacity_clz;
          break;
        case 11:
        case 12:
          new_capacity_ms4b = 14;
          new_capacity_clz = rhh->capacity_clz;
          break;
        case 13:
        case 14:
          new_capacity_ms4b = 8;
          new_capacity_clz = rhh->capacity_clz - 1;
          break;
        case 15:
          new_capacity_ms4b = 10;
          new_capacity_clz = rhh->capacity_clz - 1;
          break;
        default: op_assert(false, "Unknown capacity_ms4b %d\n",
                           rhh->capacity_ms4b);
        }
    }
  else
    {
      new_capacity_ms4b = 8;
      new_capacity_clz = rhh->capacity_ms4b == 8 ?
        rhh->capacity_clz - 1 : rhh->capacity_clz - 2;
    }
  new_capacity = PRHHCapacityInternal(new_capacity_clz, new_capacity_ms4b);
  OP_LOG_INFO(logger, "Resize from %" PRIu64 " to %" PRIu64,
              old_capacity, new_capacity);

  new_buckets = OPCalloc(ObtainOPHeap(rhh), 1, bucket_size * new_capacity);
  if (!new_buckets)
    {
      OP_LOG_ERROR(logger, "Cannot obtain new bucket for size %" PRIu64,
                   new_capacity);
      return false;
    }

  rhh->objcnt = 0;
  rhh->objcnt_high = new_capacity * 8 / 10;
  rhh->objcnt_low = new_capacity * 2 / 10;
  rhh->capacity_clz = new_capacity_clz;
  rhh->capacity_ms4b = new_capacity_ms4b;
  rhh->longest_probes = 0;
  memset(rhh->stats, 0x00, sizeof(uint32_t) * PROBE_STATS_SIZE);
  rhh->bucket_ref = OPPtr2Ref(new_buckets);

  for (uint64_t idx = 0; idx < old_capacity; idx++)
    {
      recref = (oplenref_t*)&old_buckets[idx * bucket_size];
      if (*recref != PRHH_EMPTY_KEY &&
          *recref != PRHH_TOMBSTONE_KEY)
        {
          PRHHUpsertPushDown(rhh, hasher, &old_buckets[idx * bucket_size],
                             0, NULL, &resized);
        }
    }
  OPDealloc(old_buckets);
  return true;
}

static bool
PRHHSizeDown(PascalRobinHoodHash* rhh, OPHash hasher)
{
  const size_t valsize = rhh->valsize;
  const size_t bucket_size = sizeof(oplenref_t) + valsize;
  uint8_t* old_buckets;
  uint8_t* new_buckets;
  uint8_t new_capacity_ms4b, new_capacity_clz;
  uint64_t old_capacity, new_capacity;
  oplenref_t *recref;
  bool resized;

  old_capacity = PRHHCapacity(rhh);
  old_buckets = OPRef2Ptr(rhh, rhh->bucket_ref);
  op_assert(old_capacity > 16,
            "Can not resize smaller than 16, but got old_capacity %"
            PRIu64 "\n", old_capacity);

  switch(rhh->capacity_ms4b)
    {
    case 8:  // new load 0.45
    case 9:  // new load 0.50
    case 10: // new load 0.55
    case 11: // new load 0.60
      new_capacity_ms4b = 8;
      new_capacity_clz = rhh->capacity_clz + 1;
      break;
    case 12: // new load 0.40
    case 13: // new load 0.43
    case 14: // new load 0.46
    case 15: // new load 0.50
      new_capacity_ms4b = 12;
      new_capacity_clz = rhh->capacity_clz + 1;
      break;
    default: op_assert(false, "Unknown capacity_ms4b %d\n",
                       rhh->capacity_ms4b);
    }

  new_capacity = PRHHCapacityInternal(new_capacity_clz, new_capacity_ms4b);
  OP_LOG_INFO(logger, "Resize from %" PRIu64 " to %" PRIu64,
              old_capacity, new_capacity);
  new_buckets = OPCalloc(ObtainOPHeap(rhh), 1, bucket_size * new_capacity);
  if (!new_buckets)
    {
      OP_LOG_ERROR(logger, "Cannot obtain new bucket for size %" PRIu64,
                   new_capacity);
      return false;
    }

  rhh->objcnt = 0;
  rhh->objcnt_high = new_capacity * 8 / 10;
  rhh->objcnt_low = new_capacity * 2 / 10;
  rhh->capacity_clz = new_capacity_clz;
  rhh->capacity_ms4b = new_capacity_ms4b;
  rhh->longest_probes = 0;
  memset(rhh->stats, 0x00, sizeof(uint32_t) * PROBE_STATS_SIZE);
  rhh->bucket_ref = OPPtr2Ref(new_buckets);

  for (uint64_t idx = 0; idx < old_capacity; idx++)
    {
      recref = (oplenref_t*)&old_buckets[idx * bucket_size];
      if (*recref != PRHH_EMPTY_KEY &&
          *recref != PRHH_TOMBSTONE_KEY)
        {
          PRHHUpsertPushDown(rhh, hasher, &old_buckets[idx * bucket_size],
                             0, NULL, &resized);
        }
    }
  OPDealloc(old_buckets);
  return true;
}

bool PRHHInsertCustom(PascalRobinHoodHash* rhh, OPHash hasher,
                      void* key, size_t keysize, void* val)
{
  const size_t refsize = sizeof(oplenref_t);
  const size_t valsize = rhh->valsize;
  const size_t bucket_size = refsize + valsize;
  enum upsert_result_t upsert_result;
  uint64_t hashed_key;
  oplenref_t keylref;
  uint8_t* matched_bucket;
  uint8_t bucket_cpy[bucket_size];
  int probe;
  bool resized;

  if (rhh->objcnt > rhh->objcnt_high)
    {
      if(!PRHHSizeUp(rhh, hasher))
        return false;
    }

  hashed_key = hasher(key, keysize);
  upsert_result = PRHHUpsertNewKey(rhh, hasher,
                                   key, keysize, hashed_key,
                                   &matched_bucket, &probe);

  switch (upsert_result)
    {
    case UPSERT_EMPTY:
      keylref = OPLenRefCreate(ObtainOPHeap(rhh), key, keysize);
      op_assert(keylref, "allocated pointer should not be NULL");
      memcpy(matched_bucket, &keylref, sizeof(oplenref_t));
      memcpy(&matched_bucket[refsize], val, valsize);
      break;
    case UPSERT_DUP:
      memcpy(&matched_bucket[refsize], val, valsize);
      break;
    case UPSERT_PUSHDOWN:
      memcpy(bucket_cpy, matched_bucket, bucket_size);
      keylref = OPLenRefCreate(ObtainOPHeap(rhh), key, keysize);
      op_assert(keylref, "allocated pointer should not be NULL");
      memcpy(matched_bucket, &keylref, sizeof(oplenref_t));
      memcpy(&matched_bucket[refsize], val, valsize);
      PRHHUpsertPushDown(rhh, hasher, bucket_cpy, probe,
                         matched_bucket, &resized);
    }
  return true;
}

bool PRHHUpsertCustom(PascalRobinHoodHash* rhh, OPHash hasher,
                      void* key, size_t keysize, void** val_ref,
                      bool* is_duplicate)
{
  const size_t refsize = sizeof(oplenref_t);
  const size_t valsize = rhh->valsize;
  const size_t bucket_size = refsize + valsize;
  enum upsert_result_t upsert_result;
  uint8_t* matched_bucket;
  uint64_t hashed_key;
  oplenref_t keylref;
  int probe;
  uint8_t bucket_cpy[bucket_size];
  bool resized;

  if (rhh->objcnt > rhh->objcnt_high)
    {
      if(!PRHHSizeUp(rhh, hasher))
        return false;
    }

  hashed_key = hasher(key, keysize);
  upsert_result = PRHHUpsertNewKey(rhh, hasher,
                                   key, keysize, hashed_key,
                                   &matched_bucket, &probe);
  *val_ref = &matched_bucket[sizeof(oplenref_t)];

  switch (upsert_result)
    {
    case UPSERT_DUP:
      *is_duplicate = true;
      break;
    case UPSERT_EMPTY:
      *is_duplicate = false;
      keylref = OPLenRefCreate(ObtainOPHeap(rhh), key, keysize);
      op_assert(keylref, "allocated pointer should not be NULL");
      memcpy(matched_bucket, &keylref, sizeof(oplenref_t));
      break;
    case UPSERT_PUSHDOWN:
      *is_duplicate = false;
      memcpy(bucket_cpy, matched_bucket, bucket_size);
      keylref = OPLenRefCreate(ObtainOPHeap(rhh), key, keysize);
      op_assert(keylref, "allocated pointer should not be NULL");
      memcpy(matched_bucket, &keylref, sizeof(oplenref_t));
      PRHHUpsertPushDown(rhh, hasher, bucket_cpy, probe,
                         matched_bucket, &resized);
      if (resized)
        {
          *val_ref = PRHHGetCustom(rhh, hasher, key, keysize);
        }
    }
  return true;
}

static inline bool
PRHHSearchIdx(PascalRobinHoodHash* rhh, OPHash hasher,
              void* key, size_t keysize, uintptr_t* idx)
{
  const size_t valsize = rhh->valsize;
  const size_t bucket_size = sizeof(oplenref_t) + valsize;
  uint8_t* const buckets = OPRef2Ptr(rhh, rhh->bucket_ref);
  uint64_t hashed_key;
  oplenref_t *recref;
  void* recptr;
  size_t recsize;

  hashed_key = hasher(key, keysize);

  for (int probe = 0; probe <= rhh->longest_probes; probe++)
    {
      *idx = hash_with_probe(rhh, hashed_key, probe);
      recref = (oplenref_t*)&buckets[*idx * bucket_size];
      switch (*recref)
        {
        case 0: return false;
        case ~0ULL: continue;
        default: break;
        }
      recptr = OPLenRef2Ptr(rhh, *recref);
      recsize = OPLenRef2Size(*recref);
      if (keysize == recsize &&
          !memcmp(key, recptr, keysize))
        return true;
    }
  return false;
}

void* PRHHGetCustom(PascalRobinHoodHash* rhh, OPHash hasher,
                    void* key, size_t keysize)
{
  const size_t bucket_size = sizeof(oplenref_t) + rhh->valsize;
  uint8_t* const buckets = OPRef2Ptr(rhh, rhh->bucket_ref);
  uintptr_t idx;
  if (PRHHSearchIdx(rhh, hasher, key, keysize, &idx))
    {
      return &buckets[idx * bucket_size + sizeof(oplenref_t)];
    }
  return NULL;
}

void* PRHHDeleteCustom(PascalRobinHoodHash* rhh, OPHash hasher,
                       void* key, size_t keysize)
{
  /*
   * This works for load that is not super high, i.e. < 0.9.
   * It slows down the growth for both E[psl] and Var[psl], but only
   * slows down, not bounding it.
   */
  const size_t valsize = rhh->valsize;
  const size_t bucket_size = sizeof(oplenref_t) + valsize;
  uint8_t* buckets;
  uintptr_t idx, premod_idx, candidate_idx;
  uintptr_t mask;
  int candidates;
  int record_probe;
  oplenref_t* recref;
  void* recptr;
  size_t recsize;
  uint64_t hashed_rec;
  uint8_t bucket_tmp[bucket_size];

  if (rhh->objcnt < rhh->objcnt_low &&
      rhh->objcnt > 16)
    {
      if (!PRHHSizeDown(rhh, hasher))
        return NULL;
    }

  if (!PRHHSearchIdx(rhh, hasher, key, keysize, &idx))
    return NULL;

  buckets = OPRef2Ptr(rhh, rhh->bucket_ref);
  mask = (1ULL << (64 - rhh->capacity_clz)) - 1;

  rhh->objcnt--;
  record_probe = findprobe(rhh, hasher, idx);
  if (record_probe < PROBE_STATS_SIZE)
    rhh->stats[record_probe]--;
  else
    OP_LOG_WARN(logger, "Large probe: %d\n", record_probe);

  if (record_probe == rhh->longest_probes &&
      rhh->stats[record_probe] == 0)
    {
      rhh->longest_probes--;
    }

  while (true)
    {
    next_iter:
      if (record_probe == 0)
        goto end_iter;
      switch (rhh->capacity_ms4b)
        {
        case 8: premod_idx =  round_up_div(16*idx, 8); break;
        case 9: premod_idx =  round_up_div(16*idx, 9); break;
        case 10: premod_idx = round_up_div(16*idx, 10); break;
        case 11: premod_idx = round_up_div(16*idx, 11); break;
        case 12: premod_idx = round_up_div(16*idx, 12); break;
        case 13: premod_idx = round_up_div(16*idx, 13); break;
        case 14: premod_idx = round_up_div(16*idx, 14); break;
        case 15: premod_idx = round_up_div(16*idx, 15); break;
        default: op_assert(false, "Unknown capacity_ms4b %d\n",
                           rhh->capacity_ms4b);
        }
      candidates = (((premod_idx + 1) & mask) * rhh->capacity_ms4b >> 4)
        == idx ? 2 : 1;
      for (int probe = rhh->longest_probes - 1;
           probe > 0; probe--)
        {
          for (int candidate = 0; candidate < candidates; candidate++)
            {
              candidate_idx =
                ((premod_idx + candidate + (probe + 1)*(probe + 1)*2
                  - probe*probe*2) & mask) * rhh->capacity_ms4b >> 4;
              recref = (oplenref_t*)&buckets[candidate_idx * bucket_size];
              if (*recref == 0 || *recref == ~0ULL)
                continue;
              recptr = OPLenRef2Ptr(rhh, *recref);
              recsize = OPLenRef2Size(*recref);
              hashed_rec = hasher(recptr, recsize);
              if (hash_with_probe(rhh, hashed_rec, probe + 1)
                  == candidate_idx &&
                  hash_with_probe(rhh, hashed_rec, probe)
                  == idx)
                {
                  if (probe + 1 < PROBE_STATS_SIZE)
                    rhh->stats[probe + 1]--;
                  if (probe < PROBE_STATS_SIZE)
                    rhh->stats[probe]++;
                  else
                    OP_LOG_WARN(logger, "Large probe: %d\n", probe);
                  if (probe + 1 == rhh->longest_probes &&
                      rhh->stats[probe+1] == 0)
                    {
                      rhh->longest_probes--;
                    }
                  memcpy(bucket_tmp, &buckets[idx * bucket_size], bucket_size);
                  memcpy(&buckets[idx * bucket_size],
                         &buckets[candidate_idx * bucket_size],
                         bucket_size);
                  memcpy(&buckets[candidate_idx * bucket_size],
                         bucket_tmp, bucket_size);
                  idx = candidate_idx;
                  record_probe--;
                  goto next_iter;
                }
            }
        }
      goto end_iter;
    }

 end_iter:
  recref = (oplenref_t*)&buckets[idx * bucket_size];
  recsize = OPLenRef2Size(*recref);
  recptr = OPLenRef2Ptr(rhh, *recref);
  OPDealloc(recptr);
  *recref = PRHH_TOMBSTONE_KEY;
  return &buckets[idx * bucket_size + sizeof(oplenref_t)];
}

void PRHHIterate(PascalRobinHoodHash* rhh,
                 OPHashIterator iterator, void* context)
{
  const size_t valsize = rhh->valsize;
  const size_t bucket_size = sizeof(oplenref_t) + valsize;
  uint8_t* const buckets = OPRef2Ptr(rhh, rhh->bucket_ref);
  uint64_t capacity = PRHHCapacity(rhh);
  oplenref_t* recref;
  void* recptr;
  size_t recsize;

  for (uint64_t idx = 0; idx < capacity; idx++)
    {
      recref = (oplenref_t*)&buckets[idx * bucket_size];
      if (*recref != PRHH_EMPTY_KEY &&
          *recref != PRHH_TOMBSTONE_KEY)
        {
          recptr = OPLenRef2Ptr(rhh, *recref);
          recsize = OPLenRef2Size(*recref);
          iterator(recptr, &buckets[idx * bucket_size + sizeof(oplenref_t)],
                   recsize, valsize, context);
        }
    }
}

void PRHHPrintStat(PascalRobinHoodHash* rhh)
{
  for (int i = 0; i < PROBE_STATS_SIZE; i++)
    if (rhh->stats[i])
      printf("probe %02d: %d\n", i, rhh->stats[i]);
}

/* pascal_robin_hood.c ends here */
