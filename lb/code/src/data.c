/*
 * Copyright 2013 University of Chicago and Argonne National Laboratory
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License
 */


#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <list.h>
#include <list_i.h>
#include <list_l.h>
#include <table_bp.h>
#include <table_lp.h>
#include <vint.h>

#include "adlb.h"
#include "adlb_types.h"
#include "data.h"
#include "data_cleanup.h"
#include "data_internal.h"
#include "data_structs.h"
#include "debug.h"
#include "multiset.h"
#include "notifications.h"
#include "refcount.h"
#include "sync.h"

/**
   Map from adlb_datum_id to adlb_datum
*/
static struct table_lp tds;

typedef struct {
  adlb_datum_id id;
  adlb_refc acquire;
  int write_decr;
  size_t subscript_len;
  char subscript_data[];
} container_reference;

/**
   Map from "container,subscript" specifier to list of listening references.
 */
static struct table_bp container_references;

/**
   Map from "container,subscript" specifier to list of xlb_listeners
 */
static struct table_bp container_ix_listeners;

/**
   Map from adlb_datum_id to int rank if locked
*/
static struct table_lp locked;

/**
   Number of ADLB servers
*/
static int servers = 1;

/**
   Unique datum id.  Note that 0 is ADLB_DATA_ID_NULL.
*/
static adlb_datum_id unique = -1;

/**
   When data_unique hits this, return an error- we have exhausted the
   longs. Defined in data_init()
 */
static adlb_datum_id last_id;

/**
   Lowest negative system data ID allocated so far.
 */
static adlb_datum_id xlb_min_alloced_system_id;

static adlb_data_code
datum_init_props(adlb_datum_id id, adlb_datum *d,
                 const adlb_create_props *props);

static adlb_data_code
add_close_notifs(adlb_datum_id id, adlb_datum *d,
                    adlb_notif_t *notifs);

static adlb_data_code
lookup_subscript(adlb_datum_id id, adlb_dsym dsym,
    const adlb_datum_storage *d,
    adlb_subscript subscript, adlb_data_type type,
    bool *sub_exists, const adlb_datum_storage **result,
    adlb_data_type *result_type);

static adlb_data_code datum_gc(adlb_datum_id id, adlb_datum* d,
           xlb_refc_acquire acquire, xlb_refc_changes *refcs);

static container_reference *
alloc_container_reference(size_t subscript_len);

static adlb_data_code
data_store_root(adlb_datum_id id, adlb_datum *d,
    void *buffer, size_t length, bool copy, bool *took_ownership,
    adlb_data_type type,
    adlb_refc store_refcounts,
    adlb_notif_t *notifs, bool *freed_datum);

static adlb_data_code
data_store_subscript(adlb_datum_id id, adlb_datum *d,
    adlb_subscript subscript, void *value, size_t length, bool copy,
    bool *took_ownership, adlb_data_type type, adlb_refc store_refcounts,
    adlb_notif_t *notifs, bool *freed_datum);

static adlb_data_code
data_reserve_subscript(adlb_datum_id id, adlb_datum *d,
    adlb_subscript subscript);

static adlb_data_code
insert_notifications(adlb_datum *d, adlb_datum_id id,
    adlb_subscript subscript, const adlb_datum_storage *value,
    const void *value_buffer, size_t value_len, adlb_data_type value_type,
    adlb_notif_t *notifs, bool *garbage_collected);

static adlb_data_code
add_recursive_notifs(adlb_datum *d, adlb_datum_id id,
      adlb_subscript assigned_sub,
      const adlb_datum_storage *value, adlb_data_type value_type,
      adlb_notif_t *notifs, bool *garbage_collected);

static adlb_data_code
subscript_notifs_rec(adlb_datum *d, adlb_datum_id id,
          const adlb_datum_storage *data, adlb_data_type type,
          adlb_buffer *sub_buf, bool *sub_caller_buf,
          adlb_subscript subscript, adlb_notif_t *notifs,
          bool *garbage_collected);

static adlb_data_code
container_notifs_rec(adlb_datum *d, adlb_datum_id id,
          const adlb_container *s, adlb_buffer *sub_buf, bool *sub_caller_buf,
          adlb_subscript subscript, adlb_notif_t *notifs,
          bool *garbage_collected);

static adlb_data_code
struct_notifs_rec(adlb_datum *d, adlb_datum_id id,
          const adlb_struct *c, adlb_buffer *sub_buf, bool *sub_caller_buf,
          adlb_subscript subscript, adlb_notif_t *notifs,
          bool *garbage_collected);

static adlb_data_code
check_subscript_notifications(adlb_datum_id container_id,
    adlb_subscript subscript, struct list **ref_list,
    struct list_b **listener_list);

static adlb_data_code
insert_notifications2(adlb_datum *d,
      adlb_datum_id id, adlb_subscript subscript,
      bool copy_sub, adlb_data_type value_type,
      const void *value_buffer, size_t value_len,
      struct list *ref_list, struct list_b *listener_list,
      adlb_notif_t *notifs, bool *garbage_collected);

static
adlb_data_code process_ref_list(struct list *subscribers,
          adlb_notif_t *notifs, adlb_data_type type,
          const void *value, size_t value_len,
          adlb_refc *to_acquire);

static
adlb_data_code append_notifs(struct list_b *listeners, bool free_list_root,
  adlb_datum_id id, adlb_dsym dsym, adlb_subscript sub, adlb_notif_ranks *notify);


static bool container_value_exists(const adlb_container *c,
                                   adlb_subscript sub);
static bool container_lookup(const adlb_container *c, adlb_subscript sub,
                             adlb_container_val *val);
static bool container_set(adlb_container *c, adlb_subscript sub,
                              adlb_container_val val,
                              adlb_container_val *prev);
static adlb_data_code container_add(adlb_container *c, adlb_subscript sub,
                              adlb_container_val val);

static void report_leaks(void);


/**
   @param s Number of servers
   @param server_num Number amongst servers
 */
adlb_data_code
xlb_data_init(int s, int server_num)
{
  assert(server_num >= 0 && server_num < s);
  servers = s;
  unique = server_num;
  if (unique == 0) unique += s;

  bool result;
  result = table_lp_init(&tds, 1024*1024);
  if (!result)
    return ADLB_DATA_ERROR_OOM;
  result = table_bp_init(&container_references, 1024*1024);
  if (!result)
    return ADLB_DATA_ERROR_OOM;
  result = table_bp_init(&container_ix_listeners, 1024*1024);
  if (!result)
    return ADLB_DATA_ERROR_OOM;

  result = table_lp_init(&locked, 16);
  if (!result)
    return ADLB_DATA_ERROR_OOM;

  last_id = LONG_MAX - servers - 1;

  xlb_min_alloced_system_id = 0;

  return ADLB_DATA_SUCCESS;
}

adlb_data_code
xlb_data_create(adlb_datum_id id, adlb_data_type type,
            const adlb_type_extra *type_extra,
            const adlb_create_props *props)
{
  TRACE("data_create(%"PRId64")", id);
  check_verbose(id != ADLB_DATA_ID_NULL, ADLB_DATA_ERROR_INVALID,
                "ERROR: attempt to create data: id=%"PRId64"\n", id);

  DEBUG("Create "ADLB_PRID" t:%s r:%i w:%i",
        ADLB_PRID_ARGS(id, props->symbol),
        ADLB_Data_type_tostring(type),
        props->read_refcount, props->write_refcount);
  if (type == ADLB_DATA_TYPE_CONTAINER)
    DEBUG("Create container "ADLB_PRID" k:%s v:%s",
          ADLB_PRID_ARGS(id, props->symbol),
          ADLB_Data_type_tostring(type_extra->CONTAINER.key_type),
          ADLB_Data_type_tostring(type_extra->CONTAINER.val_type));

#ifndef NDEBUG
  check_verbose(!table_lp_contains(&tds, id), ADLB_DATA_ERROR_DOUBLE_DECLARE,
                ADLB_PRID" already exists",
                ADLB_PRID_ARGS(id, props->symbol));
#endif

  if (props->read_refcount <= 0 && props->write_refcount <= 0)
  {
    DEBUG("Skipped creation of "ADLB_PRID,
          ADLB_PRID_ARGS(id, props->symbol));
    return ADLB_DATA_SUCCESS;
  }

  adlb_datum* d = malloc(sizeof(adlb_datum));
  check_verbose(d != NULL, ADLB_DATA_ERROR_OOM,
                "Out of memory while allocating datum");
  d->type = type;
  d->symbol = props->symbol;
  list_b_init(&d->listeners);

  table_lp_add(&tds, id, d);

  adlb_data_code dc = datum_init_props(id, d, props);
  DATA_CHECK(dc);

  if (ADLB_Data_is_compound(type))
  {
    dc = ADLB_Init_compound(&d->data, type, *type_extra, false);
    DATA_CHECK(dc);
    d->status.set = true;
  }
  return ADLB_DATA_SUCCESS;
}

adlb_data_code xlb_data_multicreate(const xlb_create_spec *specs,
          int count, adlb_datum_id *new_ids)
{
  adlb_data_code dc;
  for (int i = 0; i < count; i++) {
    const ADLB_create_spec *spec = &specs[i].spec;
    if (specs[i].spec.id != ADLB_DATA_ID_NULL) {
      DEBUG("non-null data id: %"PRId64"", spec->id);
      return ADLB_DATA_ERROR_INVALID;
    }
    adlb_datum_id new_id;
    dc = xlb_data_unique(&new_id);
    DATA_CHECK(dc);
    new_ids[i] = new_id;

    dc = xlb_data_create(new_id, spec->type, &spec->type_extra,
                     &spec->props);
    DATA_CHECK(dc);
  }

  return ADLB_DATA_SUCCESS;
}

/*
  Initialize datum with props.  This will garbage collect datum
  if initialized with 0 refcounts so should be called after
  the datum is otherwise set up
 */
static adlb_data_code
datum_init_props(adlb_datum_id id, adlb_datum *d,
                 const adlb_create_props *props) {
  check_verbose(props->read_refcount >= 0, ADLB_DATA_ERROR_INVALID,
                "read_refcount negative: %i", props->read_refcount);
  check_verbose(props->write_refcount >= 0, ADLB_DATA_ERROR_INVALID,
                "write_refcount negative: %i", props->write_refcount);
  d->read_refcount = props->read_refcount;
  d->write_refcount = props->write_refcount;
  xlb_data_init_status(&d->status); // default status
  d->status.permanent = props->permanent;
  d->status.release_write_refs = props->release_write_refs;
  d->status.subscript_notifs = false;

  return ADLB_DATA_SUCCESS;
}

adlb_data_code
xlb_data_exists(adlb_datum_id id, adlb_subscript subscript, bool* result)
{
  adlb_data_code dc;
  adlb_datum* d;
  table_lp_search(&tds, id, (void**)&d);

  // if subscript provided, check that subscript exists
  if (!adlb_has_sub(subscript))
  {
      if (d == NULL || !d->status.set)
        *result = false;
      else
        *result = true;
      DEBUG("Exists: "ADLB_PRID" => %s",
        ADLB_PRID_ARGS(id, d ? d->symbol : ADLB_DSYM_NULL),
        bool2string(*result));
  }
  else
  {
    check_verbose(d != NULL, ADLB_DATA_ERROR_INVALID,
        ADLB_PRID" does not exist, can't check existence of subscript",
        ADLB_PRID_ARGS(id, ADLB_DSYM_NULL));
    bool sub_exists;
    const adlb_datum_storage *lookup_result;
    adlb_data_type result_type;
    dc = lookup_subscript(id, d->symbol, &d->data, subscript, d->type,
                     &sub_exists, &lookup_result, &result_type);
    DATA_CHECK(dc);

    *result = (lookup_result != NULL);
    DEBUG("Exists: "ADLB_PRIDSUB" => %s", ADLB_PRIDSUB_ARGS(
            id, d->symbol, subscript), bool2string(*result));
  }
  return ADLB_DATA_SUCCESS;
}

adlb_data_code
xlb_data_typeof(adlb_datum_id id, adlb_data_type* type)
{
  check_verbose(id != ADLB_DATA_ID_NULL, ADLB_DATA_ERROR_NULL,
                "given ADLB_DATA_ID_NULL");

  adlb_datum* d;
  adlb_data_code dc = xlb_datum_lookup(id, &d);
  DATA_CHECK(dc);

  *type = d->type;
  DEBUG("typeof: "ADLB_PRID" => %i", ADLB_PRID_ARGS(id,
        d->symbol), *type);
  return ADLB_DATA_SUCCESS;
}

/**
   @param type output: the type of the subscript
               for the given container id
 */
adlb_data_code
xlb_data_container_typeof(adlb_datum_id id, adlb_data_type* key_type,
                                        adlb_data_type* val_type)
{
  adlb_datum* d;
  adlb_data_code dc = xlb_datum_lookup(id, &d);
  DATA_CHECK(dc);

  adlb_data_type t = d->type;
  check_verbose(t == ADLB_DATA_TYPE_CONTAINER, ADLB_DATA_ERROR_TYPE,
                "not a container: "ADLB_PRID,
                ADLB_PRID_ARGS(id, d->symbol));
  *key_type = (adlb_data_type)d->data.CONTAINER.key_type;
  *val_type = (adlb_data_type)d->data.CONTAINER.val_type;
  DEBUG("container_type: "ADLB_PRID" => (%i, %i)",
        ADLB_PRID_ARGS(id, d->symbol), *key_type, *val_type);
  return ADLB_DATA_SUCCESS;
}

adlb_data_code xlb_data_permanent(adlb_datum_id id)
{
  adlb_datum* d;
  adlb_data_code dc = xlb_datum_lookup(id, &d);
  DATA_CHECK(dc);

  d->status.permanent = true;
  return ADLB_DATA_SUCCESS;
}

adlb_data_code
xlb_datum_lookup(adlb_datum_id id, adlb_datum **d)
{
  bool found = table_lp_search(&tds, id, (void**)d);
  check_verbose(found, ADLB_DATA_ERROR_NOT_FOUND,
                "not found: "ADLB_PRID,
                ADLB_PRID_ARGS(id, ADLB_DSYM_NULL));
  assert(*d != NULL);
  return ADLB_DATA_SUCCESS;
}

adlb_data_code xlb_data_get_reference_count(adlb_datum_id id,
          adlb_refc *result)
{
  adlb_datum *d;
  adlb_data_code dc = xlb_datum_lookup(id, &d);
  DATA_CHECK(dc);

  result->read_refcount = d->read_refcount;
  result->write_refcount = d->write_refcount;
  return ADLB_DATA_SUCCESS;
}

/**
   @param garbaged_collected: whether the data was freed
                              (if null, not modified);
   Allocates fresh memory in notify_ranks unless notify_count==0
   Caller must free result
 */
adlb_data_code
xlb_data_reference_count(adlb_datum_id id, adlb_refc change,
          xlb_refc_acquire acquire, bool *garbage_collected,
          adlb_notif_t *notifs)
{
  adlb_datum* d;
  adlb_data_code dc = xlb_datum_lookup(id, &d);
  DATA_CHECK(dc);
  return xlb_refc_incr(d, id, change, acquire, garbage_collected, notifs);
}

adlb_data_code
xlb_refc_incr(adlb_datum *d, adlb_datum_id id,
          adlb_refc change, xlb_refc_acquire acquire,
          bool *garbage_collected, adlb_notif_t *notifs)
{
  adlb_data_code dc;

  // default: didn't garbage collect
  if (garbage_collected != NULL)
    *garbage_collected = false;

  assert(acquire.refcounts.read_refcount >= 0);
  assert(acquire.refcounts.write_refcount >= 0);

  int read_incr = change.read_refcount;
  int write_incr = change.write_refcount;

  if (xlb_s.read_refc_enabled && read_incr != 0 &&
                                   !d->status.permanent) {
    // Shouldn't get here if disabled
    check_verbose(xlb_s.read_refc_enabled, ADLB_DATA_ERROR_INVALID,
                  "Internal error: should not get here with read reference "
                  "counting disabled");

    /*
      Should not go negative.  Can go to zero and back up if write
      refcount still present.
     */
    check_verbose(d->read_refcount >= 0 &&
                   d->read_refcount + read_incr >= 0,
                ADLB_DATA_ERROR_REFCOUNT_NEGATIVE,
                ADLB_PRID" read_refcount: %i incr: %i",
                ADLB_PRID_ARGS(id, d->symbol),
                d->read_refcount, read_incr);
    d->read_refcount += read_incr;
    DEBUG("read_refcount: "ADLB_PRID" => %i",
          ADLB_PRID_ARGS(id, d->symbol), d->read_refcount);
  }

  // True if we just closed the ID here
  bool closed = false;

  if (write_incr != 0) {
    // Should not go negative
    check_verbose(d->write_refcount > 0 &&
                   d->write_refcount + write_incr >= 0,
                ADLB_DATA_ERROR_REFCOUNT_NEGATIVE,
                ADLB_PRID" write_refcount: %i incr: %i",
                ADLB_PRID_ARGS(id, d->symbol),
                d->write_refcount, write_incr);
    d->write_refcount += write_incr;
    if (d->write_refcount == 0) {
      // If we're keeping around read-only version, release
      // only write refs here
      dc = add_close_notifs(id, d, notifs);
      DATA_CHECK(dc);

      closed = true;
    }
    DEBUG("write_refcount: "ADLB_PRID" => %i",
      ADLB_PRID_ARGS(id, d->symbol), d->write_refcount);
  }

  if (d->read_refcount == 0 && d->write_refcount == 0)
  {
    if (garbage_collected != NULL)
      *garbage_collected = true;
    dc = datum_gc(id, d, acquire, &notifs->refcs);
    DATA_CHECK(dc);
  }
  else
  {
    bool release_write_refs = closed && d->status.release_write_refs;
    if (release_write_refs || ADLB_REFC_NOT_NULL(acquire.refcounts))
    {
      DEBUG("Updating referand refcounts. release write refs: %i, "
            "Acquire sub: [%.*s] r: %i w: %i ", (int)release_write_refs,
            (int)acquire.subscript.length, (const char*)acquire.subscript.key,
            acquire.refcounts.read_refcount, acquire.refcounts.write_refcount);
      // Have to release or acquire references
      dc = xlb_incr_referand(&d->data, d->type, false, release_write_refs,
                   acquire, &notifs->refcs);
      DATA_CHECK(dc);
    }
  }
  return ADLB_DATA_SUCCESS;
}

static adlb_data_code
extract_members(adlb_container *c, int count, int offset,
                bool include_keys, bool include_vals,
                const adlb_buffer *caller_buffer,
                adlb_buffer *output);

static adlb_data_code
datum_gc(adlb_datum_id id, adlb_datum* d,
           xlb_refc_acquire to_acquire, xlb_refc_changes *refcs)
{
  DEBUG("datum_gc: "ADLB_PRID,
      ADLB_PRID_ARGS(id, d->symbol));
  check_verbose(!d->status.permanent, ADLB_DATA_ERROR_UNKNOWN,
          "Garbage collecting permanent data");

  if (d->status.set)
  {
    // Cleanup the storage if initialized
    adlb_data_code dc = xlb_datum_cleanup(&d->data, d->type, true,
                                true, true, to_acquire, refcs);
    DATA_CHECK(dc);
  }

  // This list should be empty since data being destroyed
  check_verbose(d->listeners.size == 0, ADLB_DATA_ERROR_TYPE,
        "%i listeners for garbage collected datum "ADLB_PRID,
        d->listeners.size, ADLB_PRID_ARGS(id, d->symbol));

  void *tmp;
  table_lp_remove(&tds, id, &tmp);
  assert(tmp == d);

  free(d);
  return ADLB_DATA_SUCCESS;
}

adlb_data_code
xlb_data_lock(adlb_datum_id id, int rank, bool* result)
{
  adlb_datum* d;
  adlb_data_code dc = xlb_datum_lookup(id, &d);
  DATA_CHECK(dc);

  if (table_lp_contains(&locked, id))
  {
    *result = false;
    return ADLB_DATA_SUCCESS;
  }
  else
  {
    int* r = malloc(sizeof(int));
    *r = rank;
    *result = true;
    table_lp_add(&locked, id, (void*)r);
  }

  return ADLB_DATA_SUCCESS;
}

adlb_data_code
xlb_data_unlock(adlb_datum_id id)
{
  int* r;
  bool found = table_lp_remove(&locked, id, (void**)&r);
  check_verbose(found, ADLB_DATA_ERROR_NOT_FOUND,
                "not found: "ADLB_PRID,
                ADLB_PRID_ARGS(id, ADLB_DSYM_NULL));
  free(r);
  return ADLB_DATA_SUCCESS;
}

/**
   @param subscript if not null and data type is container, subscribe
          to this subscript
   @param work_type send notification to worker with this work type.
                If server, this has no effect
   @param result set to true iff subscribed, else false (td closed)
   @return ADLB_SUCCESS or ADLB_ERROR
 */
adlb_data_code
xlb_data_subscribe(adlb_datum_id id, adlb_subscript subscript,
              int rank, int work_type, bool* subscribed)
{
  adlb_datum* d;
  adlb_data_code dc = xlb_datum_lookup(id, &d);
  DATA_CHECK(dc);

  if (!adlb_has_sub(subscript))
  {
    DEBUG("data_subscribe(): "ADLB_PRID,
          ADLB_PRID_ARGS(id, d->symbol));
  }
  else
  {
    DEBUG("data_subscribe(): "ADLB_PRIDSUB,
          ADLB_PRIDSUB_ARGS(id, d->symbol, subscript));
  }


  if (adlb_has_sub(subscript))
  {
    bool is_container = d->type == ADLB_DATA_TYPE_CONTAINER;
    bool is_struct = d->type == ADLB_DATA_TYPE_STRUCT;
    check_verbose(is_container || is_struct,
            ADLB_DATA_ERROR_INVALID, "subscribing to subscript on "
            "invalid type: %s for "ADLB_PRIDSUB,
            ADLB_Data_type_tostring(d->type), ADLB_PRIDSUB_ARGS(
            id, d->symbol, subscript));

    bool found = false;
    if (is_container &&
        container_value_exists(&d->data.CONTAINER, subscript))
    {
      found = true;
    }
    else if (is_struct)
    {
      check_verbose(d->status.set, ADLB_DATA_ERROR_INVALID, "Can't set "
          "subscript of struct initialized without type "ADLB_PRID,
          ADLB_PRID_ARGS(id, d->symbol));
      // This will check validity of subscript as side-effect
      dc = xlb_struct_subscript_init(d->data.STRUCT, subscript,
                                    true, &found);
      DATA_CHECK(dc);
      TRACE("Struct subscript initialized: %i", (int)found);
    }

    if (!found)
    {
      // encode container, index and ref type into string
      char key[xlb_id_sub_buflen(subscript)];
      size_t key_len = xlb_write_id_sub(key, id, subscript);

      DEBUG("Subscribe to "ADLB_PRIDSUB,
        ADLB_PRIDSUB_ARGS(id, d->symbol, subscript));

      struct list_b* listeners = NULL;
      found = table_bp_search(&container_ix_listeners, key, key_len,
                                (void*)&listeners);
      if (!found)
      {
        // Nobody else has subscribed to this pair yet
        listeners = list_b_create();
        table_bp_add(&container_ix_listeners, key, key_len, listeners);
      }
      TRACE("Added %i to listeners for "ADLB_PRIDSUB, rank,
          ADLB_PRIDSUB_ARGS(id, d->symbol, subscript));

      xlb_listener listener = { .rank = rank, .work_type = work_type };
      list_b_add(listeners, &listener, sizeof(listener));
      *subscribed = true;
    }
    else
    {
      *subscribed = false;
    }

    if (*subscribed)
    {
      d->status.subscript_notifs = true;
    }
  }
  else
  {
    // No subscript, so subscribing to top-level datum
    if (d->write_refcount == 0)
    {
      *subscribed = false;
    }
    else
    {
      xlb_listener listener = { .rank = rank, .work_type = work_type };
      list_b_add(&d->listeners, &listener, sizeof(listener));
      *subscribed = true;
    }
  }

  return ADLB_DATA_SUCCESS;
}

/*
    data_container_reference consumes a read reference count unless
    it immediately returns a result.  If it returns a result,
    the caller is responsible for setting references and then
    decrementing the read reference count of the container.
 */
adlb_data_code xlb_data_container_reference(adlb_datum_id id,
                                        adlb_subscript subscript,
                                        adlb_datum_id ref_id,
                                        adlb_subscript ref_sub,
                                        bool copy_subscript,
                                        adlb_data_type ref_type,
                                        adlb_refc to_acquire,
                                        int ref_write_decr,
                                        const adlb_buffer *caller_buffer,
                                        adlb_binary_data *result,
                                        adlb_notif_t *notifs)
{
  // Check that container_id is an initialized container
  adlb_code ac;
  adlb_datum* d;
  adlb_data_code dc = xlb_datum_lookup(id, &d);
  DATA_CHECK(dc);

  bool sub_exists;
  // Is the subscript already pointing to a data identifier?
  const adlb_datum_storage *val_data;
  adlb_data_type val_type;

  dc = lookup_subscript(id, d->symbol, &d->data, subscript, d->type,
                        &sub_exists, &val_data, &val_type);
  DATA_CHECK(dc);

  TRACE("lookup datum for ref: "ADLB_PRIDSUB": %p",
          ADLB_PRIDSUB_ARGS(id, d->symbol, subscript), val_data);

  check_verbose(ref_type == val_type, ADLB_DATA_ERROR_TYPE,
    ADLB_PRIDSUB" type mismatch when setting up reference expected"
    " %i actual %i\n", ADLB_PRIDSUB_ARGS(id, d->symbol, subscript),
    ref_type, val_type);

  if (val_data != NULL)
  {
    dc = ADLB_Pack(val_data, val_type, caller_buffer, result);
    DATA_CHECK(dc);

    // Get ownership in case internal pointer freed later
    dc = ADLB_Own_data(caller_buffer, result);
    DATA_CHECK(dc);

    if (caller_buffer == NULL ||
        result->caller_data != caller_buffer->data)
    {
      // Allocated memory, must free
      ac = xlb_to_free_add(notifs, result->caller_data);
      DATA_CHECK_ADLB(ac, ADLB_DATA_ERROR_OOM);
    }

    if (adlb_has_sub(ref_sub) && copy_subscript)
    {
      // Need to make a copy of the subscript data
      void *sub_storage = malloc(ref_sub.length);
      DATA_CHECK_MALLOC(sub_storage);

      memcpy(sub_storage, ref_sub.key, ref_sub.length);
      ref_sub.key = sub_storage;

      ac = xlb_to_free_add(notifs, sub_storage);
      DATA_CHECK_ADLB(ac, ADLB_DATA_ERROR_OOM);
    }

    // add reference setting work to notifications
    xlb_refs_add(&notifs->references, ref_id, ref_sub,
                 ref_type, result->data, result->length,
                 to_acquire, ref_write_decr);

    // Need to acquire references
    adlb_refc decr = { .read_refcount = -1,
                            .write_refcount = 0 };
    xlb_refc_acquire to_acquire2 = { .subscript = subscript,
                                   .refcounts = to_acquire };
    dc = xlb_refc_incr(d, id, decr, to_acquire2, NULL, notifs);
    DATA_CHECK(dc);

    return ADLB_DATA_SUCCESS;
  }

  result->data = result->caller_data = NULL; // Signal data not found

  // Is the container closed?
  check_verbose(d->write_refcount > 0, ADLB_DATA_ERROR_INVALID,
        "Attempting to subscribe to non-existent subscript\n"
        "on a closed container:  "ADLB_PRIDSUB,
        ADLB_PRIDSUB_ARGS(id, d->symbol, subscript));
  check_verbose(d->read_refcount > 0, ADLB_DATA_ERROR_INVALID,
        "Container_reference consumes a read reference count, but "
        "reference count was %d for "ADLB_PRID, d->read_refcount,
        ADLB_PRID_ARGS(id, d->symbol));


  // encode container, index and ref type into string
  char key[xlb_id_sub_buflen(subscript)];
  size_t key_len = xlb_write_id_sub(key, id, subscript);

  struct list* listeners = NULL;
  bool found = table_bp_search(&container_references, key, key_len,
                            (void*)&listeners);
  TRACE("search container_ref "ADLB_PRIDSUB": %i",
          ADLB_PRIDSUB_ARGS(id, d->symbol, subscript), (int)found);
  if (!found)
  {
    // Nobody else has subscribed to this pair yet
    listeners = list_create();
    TRACE("add container_ref "ADLB_PRIDSUB,
        ADLB_PRIDSUB_ARGS(id, d->symbol, subscript));
    table_bp_add(&container_references, key, key_len, listeners);
    d->status.subscript_notifs = true;
  }
  else
  {
    // Only have one read refcount per subscribed index
    // There should be at least 2 read refcounts: one for
    //  this call to container_reference, and one for the
    //  subscriber list
    if (xlb_s.read_refc_enabled) {
      assert(d->read_refcount >= 2);
      d->read_refcount--;

      DEBUG("read_refcount in container_reference: "ADLB_PRID
            " => %i", ADLB_PRID_ARGS(id, d->symbol), d->read_refcount);
    }
  }

  check_verbose(listeners != NULL, ADLB_DATA_ERROR_NULL,
                "Found null value in listeners table\n"
                "for:  "ADLB_PRIDSUB, ADLB_PRIDSUB_ARGS(id,
                d->symbol, subscript));

  container_reference *entry = alloc_container_reference(ref_sub.length);
  check_verbose(entry != NULL, ADLB_DATA_ERROR_OOM,
                "Could not allocate memory");
  entry->id = ref_id;
  entry->acquire = to_acquire;
  entry->write_decr = ref_write_decr;
  entry->subscript_len = ref_sub.length;
  memcpy(entry->subscript_data, ref_sub.key, ref_sub.length);

  list_add(listeners, entry);
  result->data = NULL;
  return ADLB_DATA_SUCCESS;
}

static container_reference *
alloc_container_reference(size_t subscript_len)
{
  return malloc(sizeof(container_reference) + subscript_len);
}


/**
   Can allocate fresh memory in notifications
   Caller must free result
   type: type of data to be assigned
   notifs: list of notifications, must be initialized
 */
adlb_data_code
xlb_data_store(adlb_datum_id id, adlb_subscript subscript,
          void *buffer, size_t length, bool copy, bool *took_ownership,
          adlb_data_type type,
          adlb_refc refcount_decr, adlb_refc store_refcounts,
          adlb_notif_t *notifs)
{
  adlb_datum* d;
  adlb_data_code dc = xlb_datum_lookup(id, &d);
  DATA_CHECK(dc);

  // Make sure we are allowed to write this data
  if (d->write_refcount <= 0)
  {
    // Don't print error by default: caller may want to handle
    DEBUG("attempt to write closed var: "ADLB_PRID,
          ADLB_PRID_ARGS(id, d->symbol));
    return ADLB_DATA_ERROR_DOUBLE_WRITE;
  }

  // Track if we freed datum for error detection
  bool freed_datum = false;

  if (adlb_has_sub(subscript))
  {
    dc = data_store_subscript(id, d, subscript, buffer, length, copy,
          took_ownership, type, store_refcounts, notifs, &freed_datum);
    DATA_CHECK(dc);
  }
  else
  {
    dc = data_store_root(id, d, buffer, length, copy, took_ownership,
          type, store_refcounts, notifs, &freed_datum);
    DATA_CHECK(dc);
  }

  // Handle reference count decrease
  assert(refcount_decr.write_refcount >= 0);
  assert(refcount_decr.read_refcount >= 0);
  if (refcount_decr.write_refcount > 0 || refcount_decr.read_refcount > 0)
  {
    // Avoid accessing freed memory
    check_verbose(!freed_datum, ADLB_DATA_ERROR_REFCOUNT_NEGATIVE,
        "Taking write reference count below zero on datum "
        ADLB_PRID, ADLB_PRID_ARGS(id, d->symbol));

    adlb_refc incr = { .read_refcount = xlb_s.read_refc_enabled ?
                                            -refcount_decr.read_refcount : 0,
                            .write_refcount = -refcount_decr.write_refcount };
    dc = xlb_refc_incr(d, id, incr, XLB_NO_ACQUIRE, NULL, notifs);
    DATA_CHECK(dc);
  }

  return ADLB_DATA_SUCCESS;
}


/**
 * Internal function to store data at root of datum
 */
static adlb_data_code
data_store_root(adlb_datum_id id, adlb_datum *d,
    void *buffer, size_t length, bool copy, bool *took_ownership,
    adlb_data_type type, adlb_refc store_refcounts,
    adlb_notif_t *notifs, bool *freed_datum)
{
  adlb_data_code dc;

  check_verbose(type == d->type, ADLB_DATA_ERROR_TYPE,
          "Type mismatch: expected %s actual %s\n",
          ADLB_Data_type_tostring(type), ADLB_Data_type_tostring(d->type));

  // Handle store to top-level datum
  bool initialize = !d->status.set;
  dc = ADLB_Unpack2(&d->data, d->type, buffer, length, copy,
                    store_refcounts, initialize, took_ownership);
  DATA_CHECK(dc);
  d->status.set = true;

  if (ENABLE_LOG_DEBUG && xlb_debug_enabled)
  {
    char *val_s = ADLB_Data_repr(&d->data, d->type);
    DEBUG("data_store "ADLB_PRID"=%s | refs: r: %i w: %i\n",
          ADLB_PRID_ARGS(id, d->symbol), val_s,
          store_refcounts.read_refcount, store_refcounts.write_refcount);
    free(val_s);
  }

  if (d->status.subscript_notifs)
  {
    // Need to handle subscript notifications
    dc = add_recursive_notifs(d, id, ADLB_NO_SUB, &d->data, d->type,
                              notifs, freed_datum);
    DATA_CHECK(dc);
  }

  return ADLB_DATA_SUCCESS;
}

/**
 * Internal function to store data in a subscript of a datum.
 * If value is set to NULL, reserve the subscript intead of assigning
 */
static adlb_data_code
data_store_subscript(adlb_datum_id id, adlb_datum *d,
    adlb_subscript subscript, void *value, size_t length, bool copy,
    bool *took_ownership, adlb_data_type type, adlb_refc store_refcounts,
    adlb_notif_t *notifs, bool *freed_datum)
{
  adlb_data_code dc;

  adlb_datum_storage *data = &d->data;
  adlb_data_type data_type = d->type;

  check_verbose(d->status.set, ADLB_DATA_ERROR_INVALID, "Can't set "
      "subscript of datum initialized without type "ADLB_PRID,
      ADLB_PRID_ARGS(id, d->symbol));

  // Modify subscript as we progress
  adlb_subscript curr_sub = subscript;

  // Loop to go through multiple components of subscript
  while (true)
  {
    if (data_type == ADLB_DATA_TYPE_MULTISET)
    {
      // Any subscript appends to multiset
      assert(adlb_has_sub(curr_sub));
      adlb_data_type elem_type;
      elem_type = (adlb_data_type)data->MULTISET->elem_type;
      check_verbose(value != NULL, ADLB_DATA_ERROR_INVALID,
              "Don't support reserving subscripts for multiset type");
      check_verbose(type == elem_type, ADLB_DATA_ERROR_TYPE,
              "Type mismatch for multiset val: expected %s actual %s\n",
              ADLB_Data_type_tostring(elem_type),
              ADLB_Data_type_tostring(type));
      // Handle addition to multiset
      const adlb_datum_storage *elem;
      dc = xlb_multiset_add(data->MULTISET, value, length,
                            store_refcounts, &elem);
      DATA_CHECK(dc);

      if (ENABLE_LOG_DEBUG && xlb_debug_enabled)
      {
        char *val_s = ADLB_Data_repr(elem, elem_type);
        DEBUG("data_store "ADLB_PRID"+=%s | refs: r: %i w: %i\n",
              ADLB_PRID_ARGS(id, d->symbol),
              val_s, store_refcounts.read_refcount,
              store_refcounts.write_refcount);
        free(val_s);
      }
      return ADLB_DATA_SUCCESS;
    }
    else if (data_type == ADLB_DATA_TYPE_CONTAINER)
    {
      // Handle insert

      adlb_container *c = &data->CONTAINER;

      // Does the link already exist?
      adlb_container_val t = NULL;
      bool found = container_lookup(c, curr_sub, &t);

      if (found && (value == NULL || t != NULL))
      {
        // Can overwrite reserved (unlinked) entries with actual data, but
        // cannot double reserve entries.

        // Don't print error by default: caller may want to handle
        DEBUG("already exists: "ADLB_PRIDSUB,
              ADLB_PRIDSUB_ARGS(id, d->symbol, subscript));
        return ADLB_DATA_ERROR_DOUBLE_WRITE;
      }

      if (value != NULL)
      {
        check_verbose(type == (adlb_data_type)c->val_type,
                    ADLB_DATA_ERROR_TYPE,
                    "Type mismatch for container value: "
                    "given: %s required: %s\n",
                    ADLB_Data_type_tostring(type),
                    ADLB_Data_type_tostring(c->val_type));

        // Now we are guaranteed to succeed
        adlb_datum_storage *entry = malloc(sizeof(adlb_datum_storage));
        dc = ADLB_Unpack2(entry, (adlb_data_type)c->val_type, value,
                  length, copy, store_refcounts, true, took_ownership);
        DATA_CHECK(dc);

        if (found)
        {
          DEBUG("Assigning unlinked precreated entry");
          // Ok- somebody did an Insert_atomic
          adlb_container_val v;
          // Reset entry
          bool b = container_set(c, curr_sub, entry, &v);
          ASSERT(b);
          ASSERT(v == NULL); // Should have been NULL for unlinked
        }
        else
        {
          DEBUG("Creating new container entry");
          dc = container_add(c, curr_sub, entry);
          DATA_CHECK(dc);
        }

        if (ENABLE_LOG_DEBUG && xlb_debug_enabled)
        {
          char *val_s = ADLB_Data_repr(entry, type);
          DEBUG("data_store "ADLB_PRIDSUB"=%s | refs: r: %i w: %i\n",
             ADLB_PRIDSUB_ARGS(id, d->symbol, subscript), val_s,
             store_refcounts.read_refcount, store_refcounts.write_refcount);
          free(val_s);
        }

        if (d->status.subscript_notifs)
        {
          dc = insert_notifications(d, id, subscript,
                    entry, value, length, type,
                    notifs, freed_datum);
          DATA_CHECK(dc);
        }
      }
      else
      {
        // Use NULL pointer value to represent reserved but not set
        dc = container_add(c, curr_sub, NULL);
        DATA_CHECK(dc);
      }
      return ADLB_DATA_SUCCESS;
    }
    else if (data_type == ADLB_DATA_TYPE_STRUCT)
    {
      // Handle assigning struct field
      adlb_struct_field *field;
      adlb_struct_field_type field_type;
      size_t curr_sub_pos;
      dc = xlb_struct_lookup(data->STRUCT, curr_sub, true, &field,
                             &field_type, &curr_sub_pos);
      DATA_CHECK(dc);

      assert(curr_sub_pos <= curr_sub.length);

      if (curr_sub_pos == curr_sub.length) {
        // Located field to assign.  This function will check
        // that it hasn't already been assigned

        if (value != NULL)
        {
          dc = xlb_struct_assign_field(field, field_type, value, length,
                                       type, store_refcounts);
          DATA_CHECK(dc);

          if (ENABLE_LOG_DEBUG && xlb_debug_enabled)
          {
            char *val_s = ADLB_Data_repr(&field->data, type);
            DEBUG("data_store "ADLB_PRIDSUB"=%s | refs: r: %i w: %i\n",
              ADLB_PRIDSUB_ARGS(id, d->symbol, subscript), val_s,
              store_refcounts.read_refcount, store_refcounts.write_refcount);
            free(val_s);
          }

          dc = insert_notifications(d, id, subscript,
                    &field->data, value, length, type,
                    notifs, freed_datum);
          DATA_CHECK(dc);
        }
        else
        {
          // Need to reserve
          if (field->reserved || field->initialized) {
            // Don't print error by default: caller may want to handle
            DEBUG("already exists: "ADLB_PRIDSUB,
                  ADLB_PRIDSUB_ARGS(id, d->symbol, subscript));
            return ADLB_DATA_ERROR_DOUBLE_WRITE;
          } else {
            field->reserved = true;
          }
        }
        return ADLB_DATA_SUCCESS;
      }
      else
      {
        // Some of subscript left, must continue
        if (!field->initialized)
        {
          // Can't initialize non-compound fields like this
          check_verbose(ADLB_Data_is_compound(field_type.type),
            ADLB_DATA_ERROR_SUBSCRIPT_NOT_FOUND,
            "Uninitialized subscript:  "ADLB_PRIDSUB " "
            "Remaining bytes %zu", ADLB_PRIDSUB_ARGS(id, d->symbol,
              subscript), curr_sub.length - curr_sub_pos);

          dc = ADLB_Init_compound(&d->data, field_type.type,
                                  field_type.extra, true);
          DATA_CHECK(dc);
        }
        // Some of subscript left:
        // update data, subscript, etc. for next iteration
        data = &field->data;
        data_type = field_type.type;
        curr_sub.key = ((const char*)curr_sub.key) + curr_sub_pos;
        curr_sub.length -= curr_sub_pos;
      }
    }
    else
    {
      verbose_error(ADLB_DATA_ERROR_TYPE, "type %s not subscriptable: "
              ADLB_PRID, ADLB_Data_type_tostring(data_type),
              ADLB_PRID_ARGS(id, d->symbol));
    }
  }

  return ADLB_DATA_SUCCESS;
}

/*
  Reserve a subscript of a datum
 */
static adlb_data_code
data_reserve_subscript(adlb_datum_id id, adlb_datum *d,
    adlb_subscript subscript)
{
  return data_store_subscript(id, d, subscript,
          NULL, 0, true, NULL, ADLB_DATA_TYPE_NULL,
          ADLB_NO_REFC, NULL, NULL);
}


/**
   Notify all waiters on variable that it was closed

   Allocates fresh memory in result unless count==0
   Caller must free result

   will_be_gced:
 */
static adlb_data_code
add_close_notifs(adlb_datum_id id, adlb_datum *d, adlb_notif_t *notifs)
{
  assert(d != NULL);
  DEBUG("data_close: "ADLB_PRID" listeners: %i",
        ADLB_PRID_ARGS(id, d->symbol), d->listeners.size);
  adlb_data_code dc = append_notifs(&d->listeners, false, id, d->symbol,
                                    ADLB_NO_SUB, &notifs->notify);
  DATA_CHECK(dc);

  return ADLB_DATA_SUCCESS;
}

/**
   Used by data_retrieve()
*/
#define CHECK_SET(id, d)                              \
  if (!d->status.set) {                               \
    printf("not set: "ADLB_PRID,                 \
           ADLB_PRID_ARGS(id, d->symbol));       \
    return ADLB_DATA_ERROR_UNSET;                     \
  }

adlb_data_code
xlb_data_retrieve(adlb_datum_id id, adlb_subscript subscript,
                 adlb_refc decr, adlb_refc to_acquire,
                 adlb_data_type* type, const adlb_buffer *caller_buffer,
                 adlb_binary_data *result, adlb_notif_t *notifs)
{
  // TODO: How to get subscript key as string safely?
  TRACE("data_retrieve(%"PRId64")", id); // subscript.key

  adlb_data_code dc;

  result->data = result->caller_data = NULL;

  adlb_datum* d;
  dc = xlb_datum_lookup(id, &d);
  DATA_CHECK(dc);

  // Result code for retrieve
  adlb_data_code result_code = ADLB_DATA_SUCCESS;

  const adlb_datum_storage *val_data;
  adlb_data_type val_type;

  if (!adlb_has_sub(subscript))
  {
    CHECK_SET(id, d);

    val_type = d->type;
    val_data = &d->data;
  }
  else
  {
    bool sub_exists;
    dc = lookup_subscript(id, d->symbol, &d->data, subscript, d->type,
                          &sub_exists, &val_data, &val_type);
    DATA_CHECK(dc);

    if (val_data == NULL)
    {
      return ADLB_DATA_ERROR_SUBSCRIPT_NOT_FOUND;
    }
  }

  dc = ADLB_Pack(val_data, val_type, caller_buffer, result);
  DATA_CHECK(dc);

  *type = val_type;

  if (ADLB_REFC_NOT_NULL(decr) || ADLB_REFC_NOT_NULL(to_acquire)) {
    // own data in case we free it
    if (ADLB_REFC_NOT_NULL(decr))
    {
      dc = ADLB_Own_data(caller_buffer, result);
      DATA_CHECK(dc);
    }

    xlb_refc_acquire to_acquire2 = { .refcounts = to_acquire,
                                   .subscript = subscript };
    dc = xlb_refc_incr(d, id, adlb_refc_negate(decr),
                to_acquire2, NULL, notifs);
    DATA_CHECK(dc);
  }

  return result_code;
}

/**
  Lookup subscript in any datum that supports it.
  result is NULL if not present.
  sub_exists is set to true if subscript reserved or value present
  result_type is always set
 */
static adlb_data_code
lookup_subscript(adlb_datum_id id, adlb_dsym dsym,
    const adlb_datum_storage *d,
    adlb_subscript subscript, adlb_data_type type,
    bool *sub_exists, const adlb_datum_storage **result,
    adlb_data_type *result_type)
{
  adlb_data_code dc;
  while (true)
  {
    switch(type)
    {
      case ADLB_DATA_TYPE_CONTAINER:
        // Assume remainder of subscript is container key
        *result_type = (adlb_data_type)d->CONTAINER.val_type;
        adlb_container_val tmp_val;

        // We don't distinguish unlinked and non-existent subscript here
        *sub_exists = container_lookup(&d->CONTAINER, subscript, &tmp_val);
        if (*sub_exists)
        {
          *result = tmp_val;
        }
        else
        {
          *result = NULL;
        }
        return ADLB_DATA_SUCCESS;
      case ADLB_DATA_TYPE_STRUCT:
      {
        check_verbose(d->STRUCT != NULL, ADLB_DATA_ERROR_INVALID, "Can't set "
            "subscript of struct initialized without type "ADLB_PRID,
            ADLB_PRID_ARGS(id, dsym));

        adlb_struct_field *field;
        adlb_struct_field_type field_type;
        size_t sub_pos;
        dc = xlb_struct_lookup(d->STRUCT, subscript, true, &field,
                               &field_type, &sub_pos);
        DATA_CHECK(dc);

        assert(sub_pos <= subscript.length);

        if (!field->initialized)
        {
          *sub_exists = field->reserved;
          *result = NULL;
          *result_type = field_type.type;
          return ADLB_DATA_SUCCESS;
        }
        else if (sub_pos == subscript.length)
        {
          *sub_exists = true;
          *result = &field->data;
          *result_type = field_type.type;
          return ADLB_DATA_SUCCESS;
        }
        else
        {
          // update subscript, type and data for next iteration
          subscript.length -= sub_pos;
          subscript.key = ((const char*)subscript.key) + sub_pos;
          type = field_type.type;
          d = &field->data;
        }
        break;
      }
      default:
        verbose_error(ADLB_DATA_ERROR_TYPE, "Expected "ADLB_PRID
              "to support subscripting, but type %s doesn't",
              ADLB_PRID_ARGS(id, dsym),
              ADLB_Data_type_tostring(type));
        return ADLB_DATA_ERROR_UNKNOWN;
    }
  }
}

/**
   Helper function to add to container
 */
static adlb_data_code container_add(adlb_container *c, adlb_subscript sub,
                              adlb_container_val val)
{
  TRACE("Adding %p to %p", val, c);
  bool ok = table_bp_add(c->members, sub.key, sub.length, val);

  return ok ? ADLB_DATA_SUCCESS : ADLB_DATA_ERROR_OOM;
}

/**
   Helper function to set existing container val
 */
static bool container_set(adlb_container *c, adlb_subscript sub,
                              adlb_container_val val,
                              adlb_container_val *prev)
{
  return table_bp_set(c->members, sub.key, sub.length, val, (void**)prev);
}

/**
   Helper function for looking up container
  */
static bool container_lookup(const adlb_container *c, adlb_subscript sub,
                             adlb_container_val *val)
{
  return table_bp_search(c->members, sub.key, sub.length, (void**)val);
}

/**
  Return true if the key exists and has a non-null value
 */
static bool container_value_exists(const adlb_container *c,
                                   adlb_subscript sub)
{
  adlb_container_val tmp;
  if (!container_lookup(c, sub, &tmp))
  {
    // Key doesn't exist
    return false;
  }

  return (tmp != NULL);
}

static adlb_data_code
pack_member(adlb_container *cont, table_bp_entry *item,
            bool include_keys, bool include_vals,
            const adlb_buffer *tmp_buf, adlb_buffer *result,
            bool *result_caller_buffer, size_t* result_pos);

/**
   Extract the table members into a buffer.
   count: -1 for all past offset, or the exact expected count based
        on the array size.
 */
static adlb_data_code
extract_members(adlb_container *cont, int count, int offset,
                bool include_keys, bool include_vals,
                const adlb_buffer *caller_buffer,
                adlb_buffer *output)
{
  int c = 0; // Count of members seen
  adlb_data_code dc;
  struct table_bp* members = cont->members;
  bool use_caller_buf;

  dc = ADLB_Init_buf(caller_buffer, output, &use_caller_buf, 65536);
  DATA_CHECK(dc);

  // Allocate some temporary storage on stack
  adlb_buffer tmp_buf;
  tmp_buf.length = XLB_STACK_BUFFER_LEN;
  char tmp_storage[XLB_STACK_BUFFER_LEN];
  tmp_buf.data = tmp_storage;

  size_t output_pos = 0; // Amount of output used

  TABLE_BP_FOREACH(members, item)
  {
    if (c >= offset)
    {
      if (c >= count+offset && count != -1)
      {
        TRACE("Got %i/%i items, done\n", c+1, count);
        goto extract_members_done;
      }
      dc = pack_member(cont, item, include_keys, include_vals, &tmp_buf,
                       output, &use_caller_buf, &output_pos);
      DATA_CHECK(dc);
    }
    c++;
  }

  TRACE("Got %i/%i entries at offset %i table size %i\n", c-offset, count,
                offset, members->size);
  // Should have found requested number
  if (count != -1 && c - offset != count)
  {
    DEBUG("Warning: did not get expected count when enumerating array. "
          "Got %i/%i entries at offset %i table size %i\n",
          c-offset, count, offset, members->size);
  }

extract_members_done:
  // Mark actual length of output
  output->length = output_pos;
  TRACE("extract_members: output_length: %zu\n", output->length);
  return ADLB_DATA_SUCCESS;
}

static adlb_data_code
pack_member(adlb_container *cont, table_bp_entry *item,
            bool include_keys, bool include_vals,
            const adlb_buffer *tmp_buf, adlb_buffer *result,
            bool *result_caller_buffer, size_t* result_pos)
{
  assert(table_bp_entry_valid(item));

  adlb_data_code dc;
  if (include_keys)
  {
    assert(table_bp_key_len(item) <= INT_MAX);
    dc = ADLB_Append_buffer(ADLB_DATA_TYPE_NULL,
            table_bp_get_key(item), table_bp_key_len(item),
            true, result, result_caller_buffer, result_pos);
    DATA_CHECK(dc);
  }
  if (include_vals)
  {
    dc = ADLB_Pack_buffer(item->data, (adlb_data_type)cont->val_type,
          true, tmp_buf, result, result_caller_buffer, result_pos);
    DATA_CHECK(dc);
  }

  return ADLB_DATA_SUCCESS;
}

static int
enumerate_slice_size(int offset, int count, int actual_size)
{
  // Number of elements after offset
  int post_offset = actual_size - offset;
  if (post_offset < 0)
  {
    // might be negative
    post_offset = 0;
  }
  if (count < 0) {
    // Unlimited count
    return post_offset;
  } else if (count <= post_offset) {
    // Slice size limited by specified count
    return count;
  } else {
    return post_offset;
  }
}

/**
   @param container_id
   @param count maximum number of elements to return, negative for unlimited
   @param offset offset of member to start at
   @param data Filled in with output location for encoded binary keys and
               values.  Members are stored with key first, then value.  The
               length in bytes of the key and value is encoded with vint_encode
               and prefixed to the actual data
   @param length Length of data in data
   @param include_keys whether to include keys in result
   @param include_vals whether to include values in result
   @param actual Returns the number of entries in the container
 */
adlb_data_code
xlb_data_enumerate(adlb_datum_id id, int count, int offset,
               bool include_keys, bool include_vals,
               const adlb_buffer *caller_buffer,
               adlb_buffer *data, int* actual,
               adlb_data_type *key_type, adlb_data_type *val_type)
{
  TRACE("data_enumerate(%"PRId64")", id);
  adlb_datum* d;
  adlb_data_code dc = xlb_datum_lookup(id, &d);
  DATA_CHECK(dc);

  if (d->type == ADLB_DATA_TYPE_CONTAINER)
  {
    int slice_size = enumerate_slice_size(offset, count,
                        d->data.CONTAINER.members->size);

    if (include_keys || include_vals)
    {
      dc = extract_members(&d->data.CONTAINER, count, offset,
                                  include_keys, include_vals,
                                  caller_buffer, data);
      DATA_CHECK(dc);
    }

    *actual = slice_size;
    *key_type = (adlb_data_type)d->data.CONTAINER.key_type;
    *val_type = (adlb_data_type)d->data.CONTAINER.val_type;
    TRACE("Enumerate container: %i elems %zu bytes\n", slice_size,
                                                      data->length);
    return ADLB_DATA_SUCCESS;
  }
  else if (d->type == ADLB_DATA_TYPE_MULTISET)
  {
    check_verbose(!include_keys, ADLB_DATA_ERROR_TYPE, ADLB_PRID
        " with type multiset does not have keys to enumerate",
        ADLB_PRID_ARGS(id, d->symbol));
    int slice_size = enumerate_slice_size(offset, count,
                              (int)xlb_multiset_size(d->data.MULTISET));

    if (include_vals) {
      // Extract members to buffer
      dc = xlb_multiset_extract_slice(d->data.MULTISET, offset, slice_size,
                                      caller_buffer, data);
      DATA_CHECK(dc);
    }

    *actual = slice_size;
    *key_type = ADLB_DATA_TYPE_NULL;
    *val_type = (adlb_data_type)d->data.MULTISET->elem_type;
    TRACE("Enumerate multiset: %i elems %zu bytes\n", slice_size,
                                                     data->length);
    return ADLB_DATA_SUCCESS;
  }
  else
  {
    verbose_error(ADLB_DATA_ERROR_TYPE, "enumeration of "ADLB_PRID
      " with type %s not supported", ADLB_PRID_ARGS(id, d->symbol),
      ADLB_Data_type_tostring(d->type));
  }
  // Unreachable
  return ADLB_DATA_ERROR_UNKNOWN;
}

adlb_data_code
xlb_data_container_size(adlb_datum_id container_id, int* size)
{
  adlb_datum* d;
  adlb_data_code dc = xlb_datum_lookup(container_id, &d);
  DATA_CHECK(dc);

  switch (d->type)
  {
    case ADLB_DATA_TYPE_CONTAINER:
      *size = d->data.CONTAINER.members->size;
      return ADLB_DATA_SUCCESS;
    case ADLB_DATA_TYPE_MULTISET:
      *size = (int)xlb_multiset_size(d->data.MULTISET);
      return ADLB_DATA_SUCCESS;
    default:
      printf("not a container or multiset: "ADLB_PRID,
          ADLB_PRID_ARGS(container_id, d->symbol));
      return ADLB_DATA_ERROR_TYPE;
  }
}

/**
 * Add notifications resulting from assignment to subscript.
 *
 * subscript: we assume that a point to the subscript can be added to
 *          notifications, i.e. that lifetime of subscript is longer than
 *          notifications structure
 *
 * TODO: we don't handle subscription to nodes that are neither the root
 * nor the leaf here.  In principle a subtree could be fully assigned
 * before the full datum is assigned, but we don't detect that.
 */
static adlb_data_code
insert_notifications(adlb_datum *d, adlb_datum_id id,
    adlb_subscript subscript, const adlb_datum_storage *value,
    const void *value_buffer, size_t value_len, adlb_data_type value_type,
    adlb_notif_t *notifs, bool *garbage_collected)
{
  adlb_data_code dc;

  struct list *ref_list = NULL;
  struct list_b *listener_list = NULL;

  // Find, remove, and return any listeners/references
  dc = check_subscript_notifications(id, subscript, &ref_list,
                                     &listener_list);
  DATA_CHECK(dc);

  DEBUG("Notifications for "ADLB_PRIDSUB" refs %i subscribers %i",
    ADLB_PRIDSUB_ARGS(id, d->symbol, subscript),
    ref_list != NULL ? ref_list->size : 0,
    listener_list != NULL ? listener_list->size : 0);

  // Track whether we garbage collected the data
  assert(garbage_collected != NULL);
  *garbage_collected = false;

  dc = insert_notifications2(d, id, subscript, false,
      value_type, value_buffer, value_len,
      ref_list, listener_list, notifs, garbage_collected);
  DATA_CHECK(dc);

  TRACE("remove container_ref "ADLB_PRIDSUB,
      ADLB_PRIDSUB_ARGS(id, d->symbol, subscript));

  dc = add_recursive_notifs(d, id, subscript, value,
              value_type, notifs, garbage_collected);
  DATA_CHECK(dc);

  return ADLB_DATA_SUCCESS;
}

/*
  Check for subscribers for an id/subscript pair and set output arguments
  if found.
 */
static adlb_data_code
check_subscript_notifications(adlb_datum_id id,
    adlb_subscript subscript, struct list **ref_list,
    struct list_b **listener_list) {
  char s[xlb_id_sub_buflen(subscript)];
  size_t s_len = xlb_write_id_sub(s, id, subscript);
  void *data;
  bool result = table_bp_remove(&container_references, s, s_len, &data);

  if (result)
  {
    *ref_list = (struct list*) data;
  }

  result = table_bp_remove(&container_ix_listeners, s, s_len, &data);

  if (result)
  {
    *listener_list = (struct list_b*) data;
  }

  return ADLB_DATA_SUCCESS;
}

/*
  Process the notifications once we've extracted lists
  value_buffer/value_len: data must be provided if ref_list is set.
        This memory should have a lifetime matching that of the
        whole notification data structure (i.e. we don't make a
        copy of it and store a pointer in there)
  copy_sub: if true, subscript data has shorter lifetime than
            notifications, so much copy data to add to notifs
 */
static adlb_data_code
insert_notifications2(adlb_datum *d,
      adlb_datum_id id, adlb_subscript subscript,
      bool copy_sub, adlb_data_type value_type,
      const void *value_buffer, size_t value_len,
      struct list *ref_list, struct list_b *listener_list,
      adlb_notif_t *notifs, bool *garbage_collected)
{
  adlb_data_code dc;
  if (ref_list != NULL)
  {
    DEBUG("Processing references for subscript assign");
    xlb_refc_acquire referand_acquire = XLB_NO_ACQUIRE;
    referand_acquire.subscript = subscript;

    dc = process_ref_list(ref_list, notifs, value_type,
                     value_buffer, value_len,
                     &referand_acquire.refcounts);
    DATA_CHECK(dc);

    // Need to free refcount we were holding for reference notifs
    adlb_refc read_decr = { .read_refcount = -1,
                                 .write_refcount = 0 };
    if (!xlb_s.read_refc_enabled)
    {
      read_decr.read_refcount = 0;
      referand_acquire.refcounts.read_refcount = 0;
    }

    // Update refcounts if necessary
    dc = xlb_refc_incr(d, id, read_decr, referand_acquire,
                     garbage_collected, notifs);
    DATA_CHECK(dc);
  }


  if (listener_list != NULL && listener_list->size > 0)
  {
    if (copy_sub && adlb_has_sub(subscript))
    {
      void *subscript_ptr = malloc(subscript.length);
      DATA_CHECK_MALLOC(subscript_ptr);

      memcpy(subscript_ptr, subscript.key, subscript.length);
      subscript.key = subscript_ptr;

      adlb_code ac = xlb_to_free_add(notifs, subscript_ptr);
      DATA_CHECK_ADLB(ac, ADLB_DATA_ERROR_OOM);
    }
    dc = append_notifs(listener_list, true, id, d->symbol, subscript,
                       &notifs->notify);
    DATA_CHECK(dc);
  }
  return ADLB_DATA_SUCCESS;
}


/**
  Handle processing for a single subscript when processing
  entire container.
  copy_sub: if true, must copy sub if we want to add to notifications
 */
static adlb_data_code
all_notifs_step(adlb_datum *d, adlb_datum_id id, adlb_subscript sub,
                bool copy_sub, const adlb_datum_storage *val,
                adlb_data_type val_type, adlb_notif_t *notifs,
                bool *garbage_collected)
{
  adlb_data_code dc;

  // Find, remove, and return any listeners/references
  struct list *ref_list = NULL;
  struct list_b *listener_list = NULL;
  dc = check_subscript_notifications(id, sub, &ref_list, &listener_list);
  DATA_CHECK(dc);

  DEBUG("Notifications for "ADLB_PRIDSUB" refs %i subscribers %i",
        ADLB_PRIDSUB_ARGS(id, d->symbol, sub),
        ref_list != NULL ? ref_list->size : 0,
        listener_list != NULL ? listener_list->size : 0);

  if (ref_list != NULL || listener_list != NULL)
  {
    adlb_binary_data val_data;
    if (ref_list != NULL)
    {
      // Pack container value to binary value if needed
      dc = ADLB_Pack(val, val_type, NULL, &val_data);
      DATA_CHECK(dc);

      // Take ownership of data in case it is freed
      dc = ADLB_Own_data(NULL, &val_data);
      DATA_CHECK(dc);

      adlb_code ac = xlb_to_free_add(notifs, val_data.caller_data);
      DATA_CHECK_ADLB(ac, ADLB_DATA_ERROR_OOM);
    }
    dc = insert_notifications2(d, id, sub, copy_sub, val_type,
                val_data.data, val_data.length, ref_list, listener_list,
                notifs, garbage_collected);
    DATA_CHECK(dc);
  }
  return ADLB_DATA_SUCCESS;
}

/**
 * Check for references or subscript to all members.
 */
static adlb_data_code
add_recursive_notifs(adlb_datum *d, adlb_datum_id id,
      adlb_subscript assigned_sub,
      const adlb_datum_storage *value, adlb_data_type value_type,
      adlb_notif_t *notifs, bool *garbage_collected)
{
  adlb_data_code dc;
  *garbage_collected = false;

  if (value_type == ADLB_DATA_TYPE_CONTAINER ||
      value_type == ADLB_DATA_TYPE_STRUCT)
  {
    char sub_storage[XLB_STACK_BUFFER_LEN];
    adlb_buffer sub_buffer = { .data = sub_storage,
                               .length = XLB_STACK_BUFFER_LEN };
    bool sub_caller_buf = true; // don't free buffer

    dc = subscript_notifs_rec(d, id, value, value_type,
          &sub_buffer, &sub_caller_buf, assigned_sub, notifs,
          garbage_collected);
    DATA_CHECK(dc);

    ADLB_Free_buf(&sub_buffer, sub_caller_buf);
  }
  return ADLB_DATA_SUCCESS;
}

static adlb_data_code
subscript_notifs_rec(adlb_datum *d, adlb_datum_id id,
          const adlb_datum_storage *data, adlb_data_type type,
          adlb_buffer *sub_buf, bool *sub_caller_buf,
          adlb_subscript subscript, adlb_notif_t *notifs,
          bool *garbage_collected)
{
  assert(d != NULL);
  switch (type)
  {
    case ADLB_DATA_TYPE_CONTAINER:
      return container_notifs_rec(d, id, &data->CONTAINER,
          sub_buf, sub_caller_buf, subscript, notifs,
          garbage_collected);
    case ADLB_DATA_TYPE_STRUCT:
      return struct_notifs_rec(d, id, data->STRUCT,
          sub_buf, sub_caller_buf, subscript, notifs,
          garbage_collected);
    default:
      return ADLB_DATA_SUCCESS;
  }
}

/**
 * Concatenate two subscripts, optionally using buffer
 * sub1: base, can be empty
 * sub2: appended to end, cannot be empty
 * sub_buf: buffer.  sub1 may use buffer
 */
static adlb_data_code
concat_subscripts(adlb_subscript sub1, adlb_subscript sub2,
                  adlb_subscript *result, adlb_buffer *sub_buf,
                  bool *sub_caller_buf)
{
  adlb_data_code dc;

  if (!adlb_has_sub(sub1))
  {
    *result = sub2;
  }
  else
  {
    // Combine subscripts
    result->length = sub1.length + sub2.length;
    dc = ADLB_Resize_buf(sub_buf, sub_caller_buf, result->length);
    DATA_CHECK(dc);

    if (sub1.key != sub_buf->data)
    {
      memcpy(sub_buf->data, sub1.key, sub1.length);
    }
    sub_buf->data[sub1.length - 1] = '.';
    memcpy(&sub_buf->data[sub1.length], sub2.key, sub2.length);
    result->key = sub_buf->data;
  }
  return ADLB_DATA_SUCCESS;
}

/*
  Check for references to all members.
 */
static adlb_data_code
container_notifs_rec(adlb_datum *d, adlb_datum_id id,
    const adlb_container *c, adlb_buffer *sub_buf, bool *sub_caller_buf,
    adlb_subscript subscript, adlb_notif_t *notifs,
    bool *garbage_collected)
{
  adlb_data_code dc;

  /*
   * It's possible that sub_buf is reallocated, in which case
   * we need to keep the subscript pointer pointed to it
   */
  bool subscript_uses_buf = (subscript.key == sub_buf->data);
  TABLE_BP_FOREACH(c->members, item)
  {
    adlb_subscript component = { .key = table_bp_get_key(item),
                                 .length = table_bp_key_len(item) };

    // Ensure subscript valid in event of reallocation
    if (subscript_uses_buf)
    {
      subscript.key = sub_buf->data;
      assert(subscript.length <= sub_buf->length);
    }

    /*
     * Note: concatenating subscripts like this will modify the data
     * pointed to by subscript.  But redoing the concatenation will
     * still be valid.
     */
    adlb_subscript child_sub;
    dc = concat_subscripts(subscript, component, &child_sub,
                           sub_buf, sub_caller_buf);
    DATA_CHECK(dc);

    // Check for subscriptions on this subscript
    dc = all_notifs_step(d, id, child_sub, true, item->data,
            (adlb_data_type)c->val_type, notifs, garbage_collected);
    DATA_CHECK(dc);

    if (*garbage_collected)
    {
      // We just processed the last pending notification for the
      // container: we're done!
      return ADLB_DATA_SUCCESS;
    }

    dc = subscript_notifs_rec(d, id, item->data,
        (adlb_data_type)c->val_type, sub_buf, sub_caller_buf, child_sub,
        notifs, garbage_collected);
    DATA_CHECK(dc);

    if (*garbage_collected)
    {
      return ADLB_DATA_SUCCESS;
    }
  }
  return ADLB_DATA_SUCCESS;
}


static adlb_data_code
struct_notifs_rec(adlb_datum *d, adlb_datum_id id,
      const adlb_struct *s, adlb_buffer *sub_buf, bool *sub_caller_buf,
      adlb_subscript subscript, adlb_notif_t *notifs,
      bool *garbage_collected)
{
  adlb_data_code dc;
  const xlb_struct_type_info *st = xlb_get_struct_type_info(s->type);
  assert(st != NULL);

  /*
   * It's possible that sub_buf is reallocated, in which case
   * we need to keep the subscript pointer pointed to it
   */
  bool subscript_uses_buf = (subscript.key == sub_buf->data);

  for (int i = 0; i < st->field_count; i++)
  {
    const adlb_struct_field *field = &s->fields[i];
    if (field->initialized)
    {
      // key from field index
      size_t max_key_len = 21;
      char key[max_key_len];
      adlb_subscript child_sub;

      if (adlb_has_sub(subscript))
      {
        // Append child subscript to buffer
        dc = ADLB_Resize_buf(sub_buf, sub_caller_buf,
                             subscript.length + max_key_len);
        DATA_CHECK(dc);

        if (subscript_uses_buf)
        {
          // Ensure subscript valid in event of reallocation
          subscript.key = sub_buf->data;
          assert(subscript.length <= sub_buf->length);
        }
        else
        {
          memcpy(sub_buf->data, subscript.key, subscript.length);
        }

        child_sub.key = sub_buf->data;
        char *buf_ptr = &((char*)sub_buf->data)[subscript.length - 1];
        // Separator and null terminator cancel out for length
        child_sub.length = (size_t)sprintf(buf_ptr, ".%i", i) +
                           subscript.length;
      }
      else
      {
        child_sub.key = key;
        child_sub.length = 1 + (size_t)sprintf(key, "%i", i);
      }

      adlb_data_type field_type = st->field_types[i].type;
      // Check for subscriptions on this subscript
      dc = all_notifs_step(d, id, child_sub, true, &field->data,
              field_type, notifs, garbage_collected);
      DATA_CHECK(dc);

      if (*garbage_collected)
      {
        // We just processed the last pending notification: we're done!
        return ADLB_DATA_SUCCESS;
      }

      dc = subscript_notifs_rec(d, id, &field->data, field_type,
                     sub_buf, sub_caller_buf, child_sub, notifs,
                     garbage_collected);
      DATA_CHECK(dc);

      if (*garbage_collected)
      {
        return ADLB_DATA_SUCCESS;
      }
    }
  }
  return ADLB_DATA_SUCCESS;
}


/**
 * Process reference list:
 * subscribers: list memory is freed
 */
static
adlb_data_code process_ref_list(struct list *subscribers,
          adlb_notif_t *notifs, adlb_data_type type,
          const void *value, size_t value_len,
          adlb_refc *to_acquire)
{
  assert(subscribers != NULL);
  adlb_ref_data *references = &notifs->references;

  int nsubs = subscribers->size;
  assert(nsubs >= 0);

  // append reference data
  adlb_code ac = xlb_refs_expand(references, nsubs);
  DATA_CHECK_ADLB(ac, ADLB_DATA_ERROR_OOM);

  struct list_item *node = subscribers->head;
  for (int i = 0; i < nsubs; i++)
  {
    assert(node != NULL); // Shouldn't fail if size was ok
    container_reference *entry = node->data;
    adlb_ref_datum *ref = &references->data[i + references->count];
    ref->id = entry->id;
    ref->type = type;
    ref->refcounts = entry->acquire;
    ref->write_decr = entry->write_decr;
    ref->value = value;
    ref->value_len = value_len;

    to_acquire->read_refcount += entry->acquire.read_refcount;
    to_acquire->write_refcount += entry->acquire.write_refcount;

    ref->subscript.length = entry->subscript_len;
    ref->subscript.key = entry->subscript_len == 0 ?
                         NULL : entry->subscript_data;

    // Retain memory entry with subscript
    ac = xlb_to_free_add(notifs, entry);
    DATA_CHECK_ADLB(ac, ADLB_DATA_ERROR_OOM);

    struct list_item *next = node->next;
    free(node);
    node = next;
  }
  references->count += nsubs;

  // We freed list nodes, now free head
  free(subscribers);

  return ADLB_DATA_SUCCESS;
}

/*
 * listeners: list items are freed, list root is freed
              if free_list_root is true
 */
static
adlb_data_code append_notifs(struct list_b *listeners,
  bool free_list_root, adlb_datum_id id, adlb_dsym dsym,
  adlb_subscript sub, adlb_notif_ranks *notify)
{
  assert(listeners != NULL);
  assert(notify->count >= 0);
  int nlisteners = listeners->size;
  assert(nlisteners >= 0);
  if (nlisteners == 0)
    return ADLB_DATA_SUCCESS;

  adlb_code ac = xlb_notifs_expand(notify, nlisteners);
  DATA_CHECK_ADLB(ac, ADLB_DATA_ERROR_OOM);

  struct list_b_item *node = listeners->head;
  for (int i = 0; i < nlisteners; i++)
  {
    assert(node != NULL); // If null, list size was wrong

    xlb_listener *listener = (xlb_listener*)node->data;
    assert(node->data_len == sizeof(*listener));

    adlb_notif_rank *nrank = &notify->notifs[i + notify->count];
    xlb_notif_init(nrank, listener->rank, id, sub, listener->work_type);

    // #define ADLB_PRIDSUB "<%"PRId64">:%s[%.*s] (%s)"
    TRACE("Add notif "ADLB_PRIDSUB" to rank %i",
          ADLB_PRIDSUB_ARGS(id, dsym, sub), nrank->rank);

    struct list_b_item *next = node->next;
    free(node);
    node = next;
  }
  notify->count += nlisteners;

  // We freed list items, now free or clear root
  if (free_list_root)
  {
    free(listeners);
  }
  else
  {
    listeners->head = listeners->tail = NULL;
    listeners->size = 0;
  }

  return ADLB_DATA_SUCCESS;
}

adlb_data_code
xlb_data_insert_atomic(adlb_datum_id id, adlb_subscript subscript,
                   bool *created, bool *value_present)
{
  adlb_datum *d;
  adlb_data_code dc = xlb_datum_lookup(id, &d);
  DATA_CHECK(dc);

  // Attempt to reserve
  dc = data_reserve_subscript(id, d, subscript);
  if (dc == ADLB_DATA_ERROR_DOUBLE_WRITE)
  {
    // Return data if present
    bool sub_exists;
    const adlb_datum_storage *val;
    adlb_data_type type;
    dc = lookup_subscript(id, d->symbol, &d->data, subscript, d->type,
                    &sub_exists, &val, &type);
    DATA_CHECK(dc);

    assert(sub_exists);

    *created = false;
    *value_present = (val != NULL);
  }
  else
  {
    DATA_CHECK(dc); // Check for other errors

    *created = true;
    *value_present = false;
  }
  return ADLB_DATA_SUCCESS;
}

adlb_data_code
xlb_data_system_reserve(int count, adlb_datum_id *start)
{
  xlb_min_alloced_system_id = xlb_min_alloced_system_id - count;

  *start = xlb_min_alloced_system_id;

  return ADLB_DATA_SUCCESS;
}

/**
   Obtain an unused TD
   @return Successful unless we have exhausted the
           set of signed long integers,
           in which case return ADLB_DATA_ID_NULL
 */
adlb_data_code
xlb_data_unique(adlb_datum_id* result)
{
  if (unique >= last_id)
  {
    *result = ADLB_DATA_ID_NULL;
    return ADLB_DATA_ERROR_LIMIT;
  }
  *result = unique;
  unique += servers;
  return ADLB_DATA_SUCCESS;
}

const char*
xlb_data_refc_type_tostring(adlb_refcount_type refc_type)
{
  switch (refc_type)
  {
    case ADLB_READ_REFCOUNT:
      return "r";
    case ADLB_WRITE_REFCOUNT:
      return "w";
    case ADLB_READWRITE_REFCOUNT:
      return "rw";
    default:
      return "<UNKNOWN_REFCOUNT_TYPE>";
  }
}

/*
  Attempt to get debug symbol for id locally, return
  ADLB_DSYM_NULL if not possible.
 */
adlb_dsym xlb_get_dsym(adlb_datum_id id)
{
  adlb_datum* d;
  bool found = table_lp_search(&tds, id, (void**)&d);
  if (found)
  {
    assert(d != NULL);
    return d->symbol;
  }
  return ADLB_DSYM_NULL;
}

static void free_td_entry(adlb_datum_id id, void *val)
{
  adlb_data_code dc;
  adlb_datum *d = (adlb_datum *)val;
  if (d != NULL)
  {
    if (d->status.set)
    {
      dc = ADLB_Free_storage(&d->data, d->type);
      if (dc != ADLB_DATA_SUCCESS)
        printf("Error while freeing "ADLB_PRID" %d\n",
               ADLB_PRID_ARGS(id, d->symbol), dc);
    }

    list_b_clear(&d->listeners);

    free(d);
  }
}

/** True iff we failed during finalize */
static bool failed_during_finalize = false;

/**
   This function is only used inside table_bp_free_callback()
 */
static void free_cref_entry(const void *key, size_t key_len, void *val)
{
  assert(key != NULL && val != NULL);
  struct list* listeners = val;
  struct list_item *curr;

  for (curr = listeners->head; curr != NULL; curr = curr->next)
  {
    adlb_datum_id src_id;
    adlb_subscript src_sub, dst_sub;
    adlb_dsym src_symbol, dst_symbol;
    xlb_read_id_sub(key, key_len, &src_id, &src_sub);
    src_symbol = xlb_get_dsym(src_id);

    container_reference *dst = curr->data;
    dst_sub.length = dst->subscript_len;
    dst_sub.key = dst->subscript_data;
    dst_symbol = xlb_get_dsym(dst->id);

    // TODO: pass waiting tasks to higher-level handling code
    printf("UNFILLED CONTAINER REFERENCE "
          ADLB_PRIDSUB" => "ADLB_PRIDSUB"\n",
          ADLB_PRIDSUB_ARGS(src_id, src_symbol, src_sub),
          ADLB_PRIDSUB_ARGS(dst->id, dst_symbol, dst_sub));
    failed_during_finalize = true;
  }
  list_free(listeners);
}

static void free_ix_l_entry(const void *key, size_t key_len, void *val)
{
  assert(key != NULL && val != NULL);
  struct list_b* listeners = val;

  struct list_b_item *curr;
  for (curr = listeners->head; curr != NULL; curr = curr->next)
  {
    adlb_datum_id src_id;
    adlb_subscript src_sub;
    adlb_dsym src_symbol;
    xlb_read_id_sub(key, key_len, &src_id, &src_sub);
    src_symbol = xlb_get_dsym(src_id);

    xlb_listener *listener = (xlb_listener*)curr->data;

    // TODO: pass waiting tasks to higher-level handling code
    printf("UNFILLED CONTAINER SUBSCRIBE "
          ADLB_PRIDSUB" rank: %i work_type: %i\n",
          ADLB_PRIDSUB_ARGS(src_id, src_symbol, src_sub),
          listener->rank, listener->work_type);
    failed_during_finalize = true;
  }

  list_b_free(listeners);
}

static void free_locked_entry(int64_t key, void *val)
{
  assert(val != NULL);
  free(val);
}

adlb_data_code
xlb_data_finalize()
{
  // First report any leaks or other problems
  report_leaks();

  // Second free and report problems with subscriptions.
  // This step may lookup tds table, so free that later.
  table_bp_free_callback(&container_references, false, free_cref_entry);
  table_bp_free_callback(&container_ix_listeners, false, free_ix_l_entry);

  table_lp_free_callback(&locked, false, free_locked_entry);

  // Finally free up memory allocated in this module
  table_lp_free_callback(&tds, false, free_td_entry);

  adlb_data_code dc = xlb_struct_finalize();
  DATA_CHECK(dc);

  xlb_data_types_finalize();
  if (failed_during_finalize)
    return ADLB_DATA_ERROR_UNRESOLVED;
  return ADLB_DATA_SUCCESS;
}

static void
report_leaks()
{
  bool report_leaks_setting;
  getenv_boolean("ADLB_REPORT_LEAKS", false, &report_leaks_setting);

  TABLE_LP_FOREACH(&tds, item)
  {
    adlb_datum *d = item->data;
    if (d == NULL || !d->status.permanent)
    {
      // Distinguish between leaked data, and unassigned data
      if (d->write_refcount == 0)
      {
        DEBUG("LEAK: "ADLB_PRID, ADLB_PRID_ARGS(item->key,
                                                        d->symbol));
        if (report_leaks_setting)
        {
          char *repr = ADLB_Data_repr(&d->data, d->type);
          printf("LEAK DETECTED: "ADLB_PRID" t:%s r:%i w:%i v:%s\n",
                ADLB_PRID_ARGS(item->key, d->symbol),
                ADLB_Data_type_tostring(d->type),
                d->read_refcount, d->write_refcount,
                repr);
          free(repr);
        }
      }
      else
      {
        DEBUG("UNSET VARIABLE: "ADLB_PRID, ADLB_PRID_ARGS(
                                              item->key, d->symbol));
        if (report_leaks_setting)
        {
          printf("UNSET VARIABLE DETECTED: "ADLB_PRID"\n",
              ADLB_PRID_ARGS(item->key, d->symbol));
        }
      }
    }
  }
}
