#include "data_cleanup.h"

#include "data_structs.h"
#include "debug.h"
#include "multiset.h"
#include "refcount.h"
#include "table_bp.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

adlb_data_code
xlb_datum_cleanup(adlb_datum_storage *d,
     adlb_data_type type, adlb_datum_id id, bool free_mem,
     bool release_read, bool release_write,
     refcount_scavenge to_acquire, xlb_rc_changes *rc_changes)
{
  // Sanity-check scavenges
  assert(to_acquire.refcounts.read_refcount >= 0);
  assert(to_acquire.refcounts.write_refcount >= 0);
  adlb_data_code dc;
  
  if (type == ADLB_DATA_TYPE_CONTAINER)
  {
    // Optimization: do single pass over container for freeing memory and
    // reference counting
    return xlb_members_cleanup(&d->CONTAINER, free_mem, release_read,
            release_write, to_acquire, rc_changes);
  }
  else if (type == ADLB_DATA_TYPE_MULTISET)
  {
    return xlb_multiset_cleanup(d->MULTISET, free_mem, free_mem, release_read,
            release_write, to_acquire, rc_changes);
  }
  else if (type == ADLB_DATA_TYPE_STRUCT)
  {
    int scav_ix = -1; // negative == don't scavenge
    if (adlb_has_sub(scav.subscript)) 
    {
      dc = xlb_struct_str_to_ix(scav.subscript, &scav_ix);
      DATA_CHECK(dc);
    }
    return xlb_struct_cleanup(d->STRUCT, free_mem, release_read,
              release_write, scav.refcounts, scav_ix, rc_changes);
  }
  else
  {
    if (!ADLB_RC_IS_NULL(rc_change))
    {
      // Decrement any reference counts required
      assert(!adlb_has_sub(scav.subscript));
      dc = xlb_incr_referand(d, type, release_read, release_write,
                             scav.refcounts, rc_changes);
      DATA_CHECK(dc);
    }

    if (free_mem)
    {
      // Then free memory
      dc = ADLB_Free_storage(d, type);
      DATA_CHECK(dc);
    }
    return ADLB_DATA_SUCCESS;
  }
}

/*
  Free memory held by container data structure
  Decrement reference counts for other data if required
 */
adlb_data_code
xlb_members_cleanup(adlb_container *container, bool free_mem,
  bool release_read, bool release_write, refcount_scavenge to_acquire,
  xlb_rc_changes *rc_changes)
{
  adlb_data_code dc;
  struct table_bp* members = container->members;

  TRACE("Freeing container %p", container);
  for (int i = 0; i < members->capacity; i++)
  {
    table_bp_entry* head = &members->array[i];
    if (!table_bp_entry_valid(head))
    {
      // Empty bucket
      continue;
    }

    // Keep next pointer to allow freeing of item
    table_bp_entry* item, *next;
    for (item = head, next = head->next; item != NULL;
         item = next)
    {
      next = item->next; // Store next pointer immediately

      adlb_datum_storage *d = (adlb_datum_storage*)item->data;
      
      TRACE("Freeing %p in %p", d, container);
      // Value may be null when insert_atomic occurred, but nothing inserted
      if (!ADLB_RC_IS_NULL(rc_change) && d != NULL)
      {
        adlb_subscript *sub = &scav.subscript;
        bool do_scavenge = !ADLB_RC_IS_NULL(scav.refcounts) && 
           (!adlb_has_sub(*sub) ||
             table_bp_key_match(sub->key, sub->length, item));

        if (do_scavenge)
        {
          // Note: if we're scavenging entire container, refcounts_scavenged
          // will be overwritten many times, but will have correct result
          dc = xlb_incr_scav_referand(d, container->val_type,
                                     scav.refcounts);
          DATA_CHECK(dc);
        }
        else
        {
          dc = xlb_incr_referand(d, container->val_type, rc_change);
          DATA_CHECK(dc);
        }
      }

      // Free the memory for value and key
      if (free_mem)
      {
        if (d != NULL)
        {
          dc = ADLB_Free_storage(d, container->val_type);
          DATA_CHECK(dc);
          free(d);
        }
        if (!table_bp_inline_key(item->key_len))
        {
          assert(item->__key != NULL);
          free(item->__key);
        }
      }
     
      // Free list node and move to next
      if (free_mem && item != head) // Head is part of array
        free(item);
    }

    // Mark bucket empty
    table_bp_clear_entry(head);
  }
  if (free_mem)
    table_bp_free(members);
  return ADLB_DATA_SUCCESS;
}


