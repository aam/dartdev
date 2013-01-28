// Copyright (c) 2011, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

part of html_common;

class Lists {

  /**
   * Returns the index in the array [a] of the given [element], starting
   * the search at index [startIndex] to [endIndex] (exclusive).
   * Returns -1 if [element] is not found.
   */
  static int indexOf(List a,
                     Object element,
                     int startIndex,
                     int endIndex) {
    if (startIndex >= a.length) {
      return -1;
    }
    if (startIndex < 0) {
      startIndex = 0;
    }
    for (int i = startIndex; i < endIndex; i++) {
      if (a[i] == element) {
        return i;
      }
    }
    return -1;
  }

  /**
   * Returns the last index in the array [a] of the given [element], starting
   * the search at index [startIndex] to 0.
   * Returns -1 if [element] is not found.
   */
  static int lastIndexOf(List a, Object element, int startIndex) {
    if (startIndex < 0) {
      return -1;
    }
    if (startIndex >= a.length) {
      startIndex = a.length - 1;
    }
    for (int i = startIndex; i >= 0; i--) {
      if (a[i] == element) {
        return i;
      }
    }
    return -1;
  }

  /**
   * Returns a sub list copy of this list, from [start] to
   * [:start + length:].
   * Returns an empty list if [length] is 0.
   * Throws an [ArgumentError] if [length] is negative.
   * Throws a [RangeError] if [start] or [:start + length:] are out of range.
   */
  static List getRange(List a, int start, int length, List accumulator) {
    if (length < 0) throw new ArgumentError('length');
    if (start < 0) throw new RangeError.value(start);
    int end = start + length;
    if (end > a.length) throw new RangeError.value(end);
    for (int i = start; i < end; i++) {
      accumulator.add(a[i]);
    }
    return accumulator;
  }

  static String join(List<Object> list, [String separator]) {
    if (list.isEmpty) return "";
    if (list.length == 1) return "${list[0]}";
    StringBuffer buffer = new StringBuffer();
    if (separator == null || separator == "") {
      for (int i = 0; i < list.length; i++) {
        buffer.add("${list[i]}");
      }
    } else {
      buffer.add("${list[0]}");
      for (int i = 1; i < list.length; i++) {
        buffer.add(separator);
        buffer.add("${list[i]}");
      }
    }
    return buffer.toString();
  }
}
