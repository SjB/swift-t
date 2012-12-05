// Test app array expansion with multi-d arrays

#include <builtins.swift>

main {
  void done = echo([["one", "two"], ["three"], [], ["four", "five"]]);
  wait (done) {
    trace("DONE1");
  }

  void done2 = echo2([
      [
        ["ein"], ["zwei", "drei"], ["vier"]
      ],
      [ ["fuenf", "sechs"] ],
      [ ["sieben"] ],
      [ [] ],
      [
        ["acht"], ["neun"], ["zehn"]
      ]]);
  wait (done2) {
    trace("DONE2");
  }
}
app (void signal) echo (string args[][]) {
    "/usr/bin/env" "echo" args; 
}

app (void signal) echo2 (string args[][][]) {
    "/usr/bin/env" "echo" args; 
}
