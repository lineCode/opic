/* robin_hood_test.c ---
 *
 * Filename: robin_hood_test.c
 * Description:
 * Author: Felix Chern
 * Maintainer:
 * Copyright: (c) 2017 Felix Chern
 * Created: Sat Apr 22 16:53:56 2017 (-0700)
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
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Code: */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <sys/mman.h>
#include <string.h>
#include <cmocka.h>

#include "opic/common/op_log.h"
#include "robin_hood.h"

OP_LOGGER_FACTORY(logger, "opic.hash.robin_hood_test");

#define TEST_OBJECTS (1<<15)
#define SMALL_TEST_OBJECTS 20

static int objcnt = 0;
static uint8_t objmap[TEST_OBJECTS];

void ResetObjcnt(void)
{
  objcnt = 0;
}

void ResetObjmap(void)
{
  memset(objmap, 0x00, sizeof(objmap));
}

void CountObjects(void* key, void* val,
                  size_t keysize, size_t valsize, void* ctx)
{
  objcnt++;
}

void CheckObjects(void* key, void* val,
                  size_t keysize, size_t valsize, void* ctx)
{
  int* intkey = key;
  objmap[*intkey] = 1;
}

static void
test_RHHNew(void** context)
{
  OPHeap* heap;
  RobinHoodHash* rhh;
  assert_true(OPHeapNew(&heap));
  assert_true(RHHNew(heap, &rhh, TEST_OBJECTS,
                     0.95, sizeof(int), 0));
  RHHDestroy(rhh);
  OPHeapDestroy(heap);
}

static void
test_BasicInsert(void** context)
{
  OPHeap* heap;
  RobinHoodHash* rhh;

  OP_LOG_INFO(logger, "Starting basic insert");
  assert_true(OPHeapNew(&heap));
  assert_true(RHHNew(heap, &rhh, 20,
                     0.80, sizeof(int), 0));
  OP_LOG_DEBUG(logger, "RHH addr %p", rhh);
  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      RHHInsert(rhh, &i, NULL);
    }
  RHHPrintStat(rhh);
  assert_int_equal(TEST_OBJECTS, RHHObjcnt(rhh));
  ResetObjcnt();
  RHHIterate(rhh, CountObjects, NULL);
  assert_int_equal(TEST_OBJECTS, objcnt);
  ResetObjmap();
  RHHIterate(rhh, CheckObjects, NULL);
  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      assert_int_equal(1, objmap[i]);
    }

  // test mismatch objects
  for (int i = TEST_OBJECTS; i < TEST_OBJECTS*2; i++)
    {
      assert_null(RHHGet(rhh, &i));
    }

  RHHDestroy(rhh);
  OPHeapDestroy(heap);
}

static void
test_BasicDelete(void** context)
{
  OPHeap* heap;
  RobinHoodHash* rhh;
  int i;

  assert_true(OPHeapNew(&heap));
  assert_true(RHHNew(heap, &rhh, TEST_OBJECTS,
                     0.95, sizeof(int), 0));
  for (i = 0; i < TEST_OBJECTS; i++)
    {
      RHHInsert(rhh, &i, NULL);
    }
  assert_int_equal(TEST_OBJECTS, RHHObjcnt(rhh));

  for (i = 0; i < TEST_OBJECTS; i++)
    {
      assert_non_null(RHHGet(rhh, &i));
    }

  for (i = 0; i < TEST_OBJECTS; i++)
    {
      assert_non_null(RHHDelete(rhh, &i));
    }
  assert_int_equal(0, RHHObjcnt(rhh));
  ResetObjcnt();
  RHHIterate(rhh, CountObjects, NULL);
  assert_int_equal(0, objcnt);
  RHHDestroy(rhh);
  OPHeapDestroy(heap);
}

static void
test_DistributionForUpdate(void** context)
{
  OPHeap* heap;
  RobinHoodHash* rhh;
  int key;

  assert_true(OPHeapNew(&heap));
  assert_true(RHHNew(heap, &rhh, TEST_OBJECTS,
                     0.70, sizeof(int), 0));

  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      RHHInsert(rhh, &i, NULL);
    }
  assert_int_equal(TEST_OBJECTS, RHHObjcnt(rhh));
  // TODO Change API to test the highest probe
  RHHPrintStat(rhh);

  for (int i = TEST_OBJECTS; i < TEST_OBJECTS*10; i++)
    {
      key = i - TEST_OBJECTS;
      RHHDelete(rhh, &key);
      RHHInsert(rhh, &i, NULL);
    }
  RHHPrintStat(rhh);
  RHHDestroy(rhh);
  OPHeapDestroy(heap);
}

static void
test_Upsert(void** context)
{
  OPHeap* heap;
  RobinHoodHash* rhh;
  int* val;
  bool is_duplicate;

  assert_true(OPHeapNew(&heap));
  assert_true(RHHNew(heap, &rhh, 20,
                     0.7, sizeof(int), sizeof(int)));

  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      assert_true(RHHUpsert(rhh, &i, (void**)&val, &is_duplicate));
      assert_false(is_duplicate);
      *val = i;
    }

  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      assert_true(RHHUpsert(rhh, &i, (void**)&val, &is_duplicate));
      assert_true(is_duplicate);
      assert_int_equal(i, *val);
    }
  RHHDestroy(rhh);
  OPHeapDestroy(heap);
}

static void
test_BasicInsertSmall(void** context)
{
  OPHeap* heap;
  RobinHoodHash* rhh;

  OP_LOG_INFO(logger, "Starting basic insert");
  assert_true(OPHeapNew(&heap));
  assert_true(RHHNew(heap, &rhh, 20,
                     0.80, sizeof(int), 0));
  OP_LOG_DEBUG(logger, "RHH addr %p", rhh);
  for (int i = 0; i < SMALL_TEST_OBJECTS; i++)
    {
      RHHInsert(rhh, &i, NULL);
    }
  RHHPrintStat(rhh);
  assert_int_equal(SMALL_TEST_OBJECTS, RHHObjcnt(rhh));
  ResetObjcnt();
  RHHIterate(rhh, CountObjects, NULL);
  assert_int_equal(SMALL_TEST_OBJECTS, objcnt);

  // test mismatch objects
  for (int i = SMALL_TEST_OBJECTS; i < SMALL_TEST_OBJECTS*2; i++)
    {
      assert_null(RHHGet(rhh, &i));
    }
  RHHDestroy(rhh);
  OPHeapDestroy(heap);
}

static void
test_BasicDeleteSmall(void** context)
{
  OPHeap* heap;
  RobinHoodHash* rhh;
  int i;

  assert_true(OPHeapNew(&heap));
  assert_true(RHHNew(heap, &rhh, SMALL_TEST_OBJECTS,
                     0.95, sizeof(int), 0));
  for (i = 0; i < SMALL_TEST_OBJECTS; i++)
    {
      RHHInsert(rhh, &i, NULL);
    }
  assert_int_equal(SMALL_TEST_OBJECTS, RHHObjcnt(rhh));

  for (i = 0; i < SMALL_TEST_OBJECTS; i++)
    {
      assert_non_null(RHHGet(rhh, &i));
    }

  for (i = 0; i < SMALL_TEST_OBJECTS; i++)
    {
      assert_non_null(RHHDelete(rhh, &i));
    }
  assert_int_equal(0, RHHObjcnt(rhh));
  ResetObjcnt();
  RHHIterate(rhh, CountObjects, NULL);
  assert_int_equal(0, objcnt);
  RHHDestroy(rhh);
  OPHeapDestroy(heap);
}

static void
test_DistributionForUpdateSmall(void** context)
{
  OPHeap* heap;
  RobinHoodHash* rhh;
  int key;

  assert_true(OPHeapNew(&heap));
  assert_true(RHHNew(heap, &rhh, SMALL_TEST_OBJECTS,
                     0.70, sizeof(int), 0));

  for (int i = 0; i < SMALL_TEST_OBJECTS; i++)
    {
      RHHInsert(rhh, &i, NULL);
    }
  assert_int_equal(SMALL_TEST_OBJECTS, RHHObjcnt(rhh));
  // TODO Change API to test the highest probe
  RHHPrintStat(rhh);

  for (int i = SMALL_TEST_OBJECTS; i < SMALL_TEST_OBJECTS*10; i++)
    {
      key = i - SMALL_TEST_OBJECTS;
      RHHDelete(rhh, &key);
      RHHInsert(rhh, &i, NULL);
    }
  RHHPrintStat(rhh);
  RHHDestroy(rhh);
  OPHeapDestroy(heap);
}

static void
test_UpsertSmall(void** context)
{
  OPHeap* heap;
  RobinHoodHash* rhh;
  int* val;
  bool is_duplicate;

  assert_true(OPHeapNew(&heap));
  assert_true(RHHNew(heap, &rhh, 20,
                     0.7, sizeof(int), sizeof(int)));

  for (int i = 0; i < SMALL_TEST_OBJECTS; i++)
    {
      assert_true(RHHUpsert(rhh, &i, (void**)&val, &is_duplicate));
      assert_false(is_duplicate);
      *val = i;
    }

  for (int i = 0; i < SMALL_TEST_OBJECTS; i++)
    {
      assert_true(RHHUpsert(rhh, &i, (void**)&val, &is_duplicate));
      assert_true(is_duplicate);
      assert_int_equal(i, *val);
    }
}

static void
test_FunnelInsert(void** context)
{
  OPHeap* heap;
  RobinHoodHash* rhh;
  RHHFunnel* funnel;

  OP_LOG_INFO(logger, "Starting funnel insert");
  assert_true(OPHeapNew(&heap));
  assert_true(RHHNew(heap, &rhh, TEST_OBJECTS,
                     0.80, sizeof(int), 0));
  funnel = RHHFunnelNew(rhh, NULL, 2048, 2048);
  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      RHHFunnelInsert(funnel, &i, NULL);
    }
  RHHFunnelInsertFlush(funnel);
  RHHFunnelDestroy(funnel);
  RHHPrintStat(rhh);
  assert_int_equal(TEST_OBJECTS, RHHObjcnt(rhh));
  ResetObjcnt();
  RHHIterate(rhh, CountObjects, NULL);
  assert_int_equal(TEST_OBJECTS, objcnt);
  ResetObjmap();
  RHHIterate(rhh, CheckObjects, NULL);
  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      assert_int_equal(1, objmap[i]);
    }
  RHHDestroy(rhh);
  OPHeapDestroy(heap);
}

void upsert_empty_bucket(void* key,
                         void* table_value,
                         void* funnel_value,
                         void* ctx,
                         size_t keysize, size_t valsize,
                         size_t ctxsize, bool is_duplicate)
{
  int* f_val, *ctx_val;
  f_val = (int*)funnel_value;
  ctx_val = (int*)ctx;
  memcpy(table_value, funnel_value, valsize);
  assert_false(is_duplicate);
  assert_int_equal(*f_val, *ctx_val);
}

void upsert_dup_bucket(void* key,
                       void* table_value,
                       void* funnel_value,
                       void* ctx,
                       size_t keysize, size_t valsize,
                       size_t ctxsize, bool is_duplicate)
{
  int* t_val, *f_val;
  assert_true(is_duplicate);
  t_val = (int*)table_value;
  f_val = (int*)funnel_value;
  assert_int_equal(*t_val, *f_val);
  assert_int_equal(0, ctxsize);
}

static void
test_FunnelUpsert(void** context)
{
  OPHeap* heap;
  RobinHoodHash* rhh;
  RHHFunnel* funnel;

  assert_true(OPHeapNew(&heap));
  assert_true(RHHNew(heap, &rhh, TEST_OBJECTS,
                     0.8, sizeof(int), sizeof(int)));
  funnel = RHHFunnelNew(rhh, upsert_empty_bucket, 2048, 2048);

  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      RHHFunnelUpsert(funnel, &i, &i, &i, sizeof(int));
    }
  RHHFunnelUpsertFlush(funnel);
  RHHFunnelDestroy(funnel);

  RHHPrintStat(rhh);
  assert_int_equal(TEST_OBJECTS, RHHObjcnt(rhh));
  ResetObjcnt();
  RHHIterate(rhh, CountObjects, NULL);
  assert_int_equal(TEST_OBJECTS, objcnt);
  ResetObjmap();
  RHHIterate(rhh, CheckObjects, NULL);
  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      assert_int_equal(1, objmap[i]);
    }

  funnel = RHHFunnelNew(rhh, upsert_dup_bucket, 2048, 2048);
  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      RHHFunnelUpsert(funnel, &i, &i, NULL, 0);
    }
  RHHFunnelUpsertFlush(funnel);
  RHHFunnelDestroy(funnel);
  RHHDestroy(rhh);
  OPHeapDestroy(heap);
}

void funnel_count_objects(void* key, void* value, void* ctx,
                          size_t keysize, size_t valsize,
                          size_t ctxsize)
{
  if (!value)
    return;

  objcnt++;
  int* intval = (int*)value;
  int* intctx = (int*)ctx;
  assert_int_equal(*intval, *intctx);
}

void funnel_check_objects(void* key, void* value, void* ctx,
                          size_t keysize, size_t valsize,
                          size_t ctxsize)
{
  if (!value)
    return;

  int* intkey = key;
  objmap[*intkey] = 1;
}

static void
test_FunnelGet(void** context)
{
  OPHeap* heap;
  RobinHoodHash* rhh;
  RHHFunnel* funnel;

  assert_true(OPHeapNew(&heap));
  assert_true(RHHNew(heap, &rhh, TEST_OBJECTS,
                     0.8, sizeof(int), sizeof(int)));

  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      RHHInsert(rhh, &i, &i);
    }

  ResetObjcnt();
  funnel = RHHFunnelNew(rhh, funnel_count_objects, 2048, 2048);
  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      RHHFunnelGet(funnel, &i, &i, sizeof(int));
    }
  RHHFunnelGetFlush(funnel);
  RHHFunnelDestroy(funnel);
  assert_int_equal(TEST_OBJECTS, objcnt);

  ResetObjmap();
  funnel = RHHFunnelNew(rhh, funnel_check_objects, 2048, 2048);
  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      RHHFunnelGet(funnel, &i, NULL, 0);
    }
  RHHFunnelGetFlush(funnel);
  RHHFunnelDestroy(funnel);
  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      assert_int_equal(1, objmap[i]);
    }

  // test mismatch object case
  ResetObjcnt();
  funnel = RHHFunnelNew(rhh, funnel_count_objects, 2048, 2048);
  for (int i = TEST_OBJECTS; i < TEST_OBJECTS*2; i++)
    {
      RHHFunnelGet(funnel, &i, &i, sizeof(int));
    }
  RHHFunnelGetFlush(funnel);
  RHHFunnelDestroy(funnel);
  assert_int_equal(0, objcnt);

  ResetObjmap();
  funnel = RHHFunnelNew(rhh, funnel_check_objects, 2048, 2048);
  for (int i = TEST_OBJECTS; i < TEST_OBJECTS*2; i++)
    {
      RHHFunnelGet(funnel, &i, NULL, 0);
    }
  RHHFunnelGetFlush(funnel);
  RHHFunnelDestroy(funnel);
  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      assert_int_equal(0, objmap[i]);
    }

  RHHDestroy(rhh);
  OPHeapDestroy(heap);
}

static void
test_FunnelDelete(void** context)
{
  OPHeap* heap;
  RobinHoodHash* rhh;
  RHHFunnel* funnel;

  assert_true(OPHeapNew(&heap));
  assert_true(RHHNew(heap, &rhh, TEST_OBJECTS,
                     0.8, sizeof(int), sizeof(int)));

  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      RHHInsert(rhh, &i, &i);
    }

  ResetObjmap();
  funnel = RHHFunnelNew(rhh, funnel_check_objects, 2048, 2048);
  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      RHHFunnelDelete(funnel, &i, NULL, 0);
    }
  RHHFunnelDeleteFlush(funnel);
  RHHFunnelDestroy(funnel);
  for (int i = 0; i < TEST_OBJECTS; i++)
    {
      assert_int_equal(1, objmap[i]);
    }
  assert_int_equal(0, RHHObjcnt(rhh));
  ResetObjcnt();
  RHHIterate(rhh, CountObjects, NULL);
  assert_int_equal(0, objcnt);

  RHHDestroy(rhh);
  OPHeapDestroy(heap);
}

int
main (void)
{
  const struct CMUnitTest rhh_tests[] =
    {
      cmocka_unit_test(test_RHHNew),
      cmocka_unit_test(test_BasicInsert),
      cmocka_unit_test(test_BasicDelete),
      cmocka_unit_test(test_DistributionForUpdate),
      cmocka_unit_test(test_Upsert),
      cmocka_unit_test(test_BasicInsertSmall),
      cmocka_unit_test(test_BasicDeleteSmall),
      cmocka_unit_test(test_DistributionForUpdateSmall),
      cmocka_unit_test(test_UpsertSmall),
      cmocka_unit_test(test_FunnelInsert),
      cmocka_unit_test(test_FunnelUpsert),
      cmocka_unit_test(test_FunnelGet),
      cmocka_unit_test(test_FunnelDelete),
    };

  return cmocka_run_group_tests(rhh_tests, NULL, NULL);
}

/* robin_hood_test.c ends here */
