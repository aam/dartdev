// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

patch class String {
  /* patch */ factory String.fromCharCodes(List<int> charCodes) {
    return _StringBase.createFromCharCodes(charCodes);
  }
}

patch class Strings {
  /* patch */ static String join(Iterable<String> strings, String separator) {
    return _StringBase.join(strings, separator);
  }

  /* patch */ static String concatAll(List<String> strings) {
    return _StringBase.concatAll(strings);
  }
}
