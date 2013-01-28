// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
//
// Test a dart sub-process handling stdio with different types of
// redirection.
//
// VMOptions=
// VMOptions=--short_socket_read
// VMOptions=--short_socket_write
// VMOptions=--short_socket_read --short_socket_write

import "dart:io";
import "process_test_util.dart";

void checkFileEmpty(String fileName) {
  RandomAccessFile pipeOut  = new File(fileName).openSync();
  Expect.equals(0, pipeOut.lengthSync());
  pipeOut.closeSync();
}


void checkFileContent(String fileName, String content) {
  RandomAccessFile pipeOut  = new File(fileName).openSync();
  int length = pipeOut.lengthSync();
  List data = new List<int>.fixedLength(length);
  pipeOut.readListSync(data, 0, length);
  Expect.equals(content, new String.fromCharCodes(data));
  pipeOut.closeSync();
}


void test(String shellScript, String dartScript, String type) {
  Directory dir = new Directory("").createTempSync();

  // The shell script will run the dart executable passed with a
  // number of different redirections of stdio.
  String pipeOutFile = "${dir.path}/pipe";
  String redirectOutFile = "${dir.path}/redirect";
  String executable = new Options().executable;
  List args =
      [executable, dartScript, type, pipeOutFile, redirectOutFile];
  var future = Process.start(shellScript, args);
  future.then((process) {
    process.onExit = (exitCode) {
      Expect.equals(0, exitCode);

      // Check the expected file contents.
      if (type == "0") {
        checkFileContent("${pipeOutFile}", "Hello\n");
        checkFileEmpty("${redirectOutFile}.stderr");
        checkFileContent("${redirectOutFile}.stdout", "Hello\nHello\n");
      }
      if (type == "1") {
        checkFileContent("${pipeOutFile}", "Hello\n");
        checkFileEmpty("${redirectOutFile}.stdout");
        checkFileContent("${redirectOutFile}.stderr", "Hello\nHello\n");
      }
      if (type == "2") {
        checkFileContent("${pipeOutFile}", "Hello\nHello\n");
        checkFileContent("${redirectOutFile}.stdout",
                         "Hello\nHello\nHello\nHello\n");
        checkFileContent("${redirectOutFile}.stderr",
                         "Hello\nHello\nHello\nHello\n");
      }

      // Cleanup test directory.
      dir.deleteSync(recursive: true);
    };
    // Drain out and err streams so they close.
    process.stdout.onData = process.stdout.read;
    process.stderr.onData = process.stderr.read;
  });
  future.catchError((error) {
    dir.deleteSync(recursive: true);
    Expect.fail(error.error.toString());
  });
}

// This tests that the Dart standalone VM can handle piping to stdin
// and can pipe to stdout.
main() {
  // Don't try to run shell scripts on Windows.
  var os = Platform.operatingSystem;
  if (os == 'windows') return;

  // Get the shell script for testing the Standalone Dart VM with
  // piping and redirections of stdio.
  var shellScript = new File("tests/standalone/io/dart_std_io_pipe_test.sh");
  if (!shellScript.existsSync()) {
    shellScript = new File("../tests/standalone/io/dart_std_io_pipe_test.sh");
  }
  // Get the Dart script file which echoes stdin to stdout or stderr or both.
  var scriptFile = new File("tests/standalone/io/process_std_io_script.dart");
  if (!scriptFile.existsSync()) {
    scriptFile = new File("../tests/standalone/io/process_std_io_script.dart");
  }

  // Run the shell script.
  test(shellScript.name, scriptFile.name, "0");
  test(shellScript.name, scriptFile.name, "1");
  test(shellScript.name, scriptFile.name, "2");
}
