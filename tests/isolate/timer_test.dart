// Copyright (c) 2011, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library timer_test;

import 'dart:async';
import '../../pkg/unittest/lib/unittest.dart';

const int STARTTIMEOUT = 1050;
const int DECREASE = 200;
const int ITERATIONS = 5;

int startTime;
int timeout;
int iteration;

void timeoutHandler(Timer timer) {
  int endTime = (new DateTime.now()).millisecondsSinceEpoch;
  expect(endTime - startTime, greaterThanOrEqualTo(timeout));
  if (iteration < ITERATIONS) {
    iteration++;
    timeout = timeout - DECREASE;
    startTime = (new DateTime.now()).millisecondsSinceEpoch;
    new Timer(timeout, expectAsync1(timeoutHandler));
  }
}

main() {
  test("timeout test", () {
    iteration = 0;
    timeout = STARTTIMEOUT;
    startTime = (new DateTime.now()).millisecondsSinceEpoch;
    new Timer(timeout, expectAsync1(timeoutHandler));
  });
}
