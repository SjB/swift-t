/*
  Data internal functions for cleaning up datums, including
  freeing memory and managing reference counts.
 */

#ifndef __XLB_DATA_CLEANUP_H
#define __XLB_DATA_CLEANUP_H

#include "adlb-defs.h"
#include "data.h"
#include "data_internal.h"


/* Cleanup datum by freeing memory and decrementing referand reference
   counts */
adlb_data_code xlb_datum_cleanup(adlb_datum_storage *d,
     adlb_data_type type, adlb_datum_id id, bool free_mem,
     bool release_read, bool release_write,
     refcount_scavenge to_acquire, xlb_rc_changes *rc_changes);

// Allow flexible freeing of memory/reference counts
adlb_data_code xlb_members_cleanup(adlb_container *container, bool free_mem,
  bool release_read, bool release_write, refcount_scavenge to_acquire,
  xlb_rc_changes *rc_changes);

#endif // __XLB_DATA_CLEANUP_H

