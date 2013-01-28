// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import "dart:io";
import "process_test_util.dart";

test(args) {
  var future = Process.start(new Options().executable, args);
  future.then((process) {
    process.onExit = (exitCode) {
      Expect.equals(0, exitCode);
    };
    // Drain stdout and stderr.
    process.stdout.onData = process.stdout.read;
    process.stderr.onData = process.stderr.read;
  });
}

main() {
  // Get the Dart script file which checks arguments.
  var scriptFile =
    new File("tests/standalone/io/process_check_arguments_script.dart");
  if (!scriptFile.existsSync()) {
    scriptFile =
        new File("../tests/standalone/io/process_check_arguments_script.dart");
  }
  test([scriptFile.name, '3', '0', 'a']);
  test([scriptFile.name, '3', '0', 'a b']);
  test([scriptFile.name, '3', '0', 'a\tb']);
  test([scriptFile.name, '3', '1', 'a\tb"']);
  test([scriptFile.name, '3', '1', 'a"\tb']);
  test([scriptFile.name, '3', '1', 'a"\t\\\\"b"']);
  test([scriptFile.name, '4', '0', 'a\tb', 'a']);
  test([scriptFile.name, '4', '0', 'a\tb', 'a\t\t\t\tb']);
  test([scriptFile.name, '4', '0', 'a\tb', 'a    b']);
}

