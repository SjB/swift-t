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


/*
 * steal.h
 *
 *  Created on: Aug 20, 2012
 *      Author: wozniak
 */

#ifndef STEAL_H
#define STEAL_H

#include <stdbool.h>

// The number of work units to send at a time
#define XLB_STEAL_CHUNK_SIZE 64

/**
   Are there any other servers?
   Are we allowed to steal yet?
 */
bool steal_allowed(void);

/**
   Issue sync() and steal.
   @return result true if stole something, else false
 */
adlb_code steal(bool* result);

/**
   Handle an accepted steal request
  */
adlb_code handle_steal(int caller, const struct packed_steal *req);

#endif
