/*
 * Copyright 2013-2014 University of Chicago and Argonne National Laboratory
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
package exm.stc.common.lang;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import exm.stc.common.util.Pair;

/**
 * Class that represents where a bit of code should be executed.
 */
public class ExecTarget {
  
  private ExecTarget(boolean async, boolean dispatched, ExecContext targetContext) {
    validate(async, dispatched, targetContext);
    this.async = async;
    this.dispatched = dispatched;
    this.targetContext = targetContext;
  }

  /**
   * Check that the combination of parameters makes sense.
   * @param async
   * @param dispatched
   * @param targetContext
   */
  private static void validate(boolean async, boolean dispatched,
                               ExecContext targetContext) {
    if (!async) {
      // Must be local if synchronous
      assert(!dispatched);
    } else if (dispatched) {
      // Must specify context if we're doing non-locally
      assert (targetContext != null);
    }
  }
  
  private static final ExecTarget SYNC_ANY = sync(null);
  private static final ExecTarget SYNC_CONTROL = sync(ExecContext.control());
  private static final ExecTarget NON_DISPATCHED_ANY = nonDispatched(null);
  private static final ExecTarget NON_DISPATCHED_CONTROL = nonDispatched(ExecContext.control());
  private static final ExecTarget DISPATCHED_CONTROL = dispatched(ExecContext.control());
  private static final ExecTarget DISPATCHED_DEFAULT_WORK = 
                                                dispatched(ExecContext.defaultWorker());
  
  public static ExecTarget sync(ExecContext targetContext) {
    return new ExecTarget(false, false, targetContext);
  }

  public static ExecTarget syncAny() {
    return SYNC_ANY;
  }
  
  public static ExecTarget syncControl() {
    return SYNC_CONTROL;
  }

  public static ExecTarget nonDispatched(ExecContext targetContext) {
    return new ExecTarget(true, false, targetContext);
  }
  
  public static ExecTarget nonDispatchedAny() {
    return NON_DISPATCHED_ANY;
  }
  
  public static ExecTarget nonDispatchedControl() {
    return NON_DISPATCHED_CONTROL;
  }
  
  public static ExecTarget dispatched(ExecContext targetContext) {
    return new ExecTarget(true, true, targetContext);
  }
  
  /**
   * Control task eligible to be sent elsewhere
   * @return
   */
  public static ExecTarget dispatchedControl() {
    return DISPATCHED_CONTROL;
  }


  /** If true, executes asynchronously */
  private final boolean async;
  
  /** If true, must be dispatched to be balanced among resources */
  private final boolean dispatched;
  
  /** If non-null, must execute in the specified context
   *  and cannot be relocated. */
  private final ExecContext targetContext;

  public static final ExecTarget DEFAULT_BUILTIN_MODE = NON_DISPATCHED_ANY;
  
  public ExecContext targetContext() {
    return targetContext;
  }

  public void checkCanRunIn(ExecContext cx) {
    assert(canRunIn(cx)) : cx + " " + this;
  }

  /**
   * Check if a task with this mode can be spawned in the given
   * execution context.
   * @param context
   * @return
   */
  public boolean canRunIn(ExecContext context) {
    if (this.dispatched) {
      return true;
    } else {
      return targetContextMatches(context);
    }
  }

  /**
   * @param context
   * @return true if target context is compatible with provided context
   */
  public boolean targetContextMatches(ExecContext context) {
    return targetContext == null || targetContext.equals(context);
  }
  
  /**
   * @param curr the current context
   * @return non-null context that will result if run from curr
   */
  public ExecContext actualContext(ExecContext curr) {
    if (dispatched) {
      return targetContext;
    } else {
      return curr;
    }
  }
  
  public boolean isDispatched() {
    return dispatched;
  }
  
  public boolean isAsync() {
    return async;
  }
  
  /**
   * Return builtin context names.
   */
  public static List<Pair<String, ExecTarget>> builtinExecTargets() {
    List<Pair<String, ExecTarget>> targets = new 
        ArrayList<Pair<String, ExecTarget>>(Arrays.asList(
          Pair.create("CONTROL", DISPATCHED_CONTROL),
          Pair.create("WORKER", DISPATCHED_DEFAULT_WORK),
          /* Deprecated synonym */
          Pair.create("LEAF", DISPATCHED_DEFAULT_WORK)));
    
    for (Pair<String, ExecContext> cx: AsyncExecutor.execContexts()) {
      targets.add(Pair.create(cx.val1, dispatched(cx.val2)));
    }
    
    return targets;
  }

  @Override
  public String toString() {
    return targetContext + "[async=" + async + ", dispatched=" + dispatched + "]";
  }

}