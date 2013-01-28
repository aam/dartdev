// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library TransferableTest;
import '../../pkg/unittest/lib/unittest.dart';
import '../../pkg/unittest/lib/html_config.dart';
import 'dart:html';

main() {
  useHtmlConfiguration();

  var isArrayBuffer =
      predicate((x) => x is ArrayBuffer, 'is an ArrayBuffer');

  test('TransferableTest', () {
    window.onMessage.listen(expectAsync1((messageEvent) {
      expect(messageEvent.data, isArrayBuffer);
    }));
    final buffer = (new Float32Array(3)).buffer;
    window.postMessage(buffer, '*', [buffer]);
  });
}
