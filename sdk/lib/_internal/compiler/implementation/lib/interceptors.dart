// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library _interceptors;

import 'dart:collection';
import 'dart:collection-dev';
import 'dart:_js_helper' show allMatchesInStringUnchecked,
                              Null,
                              JSSyntaxRegExp,
                              Primitives,
                              checkGrowable,
                              checkMutable,
                              checkNull,
                              checkNum,
                              checkString,
                              getRuntimeTypeString,
                              listInsertRange,
                              regExpGetNative,
                              stringContainsUnchecked,
                              stringLastIndexOfUnchecked,
                              stringReplaceAllFuncUnchecked,
                              stringReplaceAllUnchecked,
                              stringReplaceFirstUnchecked,
                              TypeImpl;
import 'dart:_foreign_helper' show JS;

part 'js_array.dart';
part 'js_number.dart';
part 'js_string.dart';

/**
 * The interceptor class for all non-primitive objects. All its
 * members are synthethized by the compiler's emitter.
 */
class ObjectInterceptor {
  const ObjectInterceptor();
}

/**
 * Get the interceptor for [object]. Called by the compiler when it needs
 * to emit a call to an intercepted method, that is a method that is
 * defined in an interceptor class.
 */
getInterceptor() {
  // This is a magic method: the compiler does specialization of it
  // depending on the uses of intercepted methods and instantiated
  // primitive types.
}

/**
 * The interceptor class for tear-off static methods. Unlike
 * tear-off instance methods, tear-off static methods are just the JS
 * function, and methods inherited from Object must therefore be
 * intercepted.
 */
class JSFunction implements Function {
  const JSFunction();
  String toString() => 'Closure';
}

/**
 * The interceptor class for [bool].
 */
class JSBool implements bool {
  const JSBool();

  // Note: if you change this, also change the function [S].
  String toString() => JS('String', r'String(#)', this);

  // The values here are SMIs, co-prime and differ about half of the bit
  // positions, including the low bit, so they are different mod 2^k.
  int get hashCode => this ? (2 * 3 * 23 * 3761) : (269 * 811);

  Type get runtimeType => bool;
}

/**
 * The interceptor class for [Null].
 */
class JSNull implements Null {
  const JSNull();

  // Note: if you change this, also change the function [S].
  String toString() => 'null';

  int get hashCode => 0;
  Type get runtimeType => createRuntimeType('Null');
}
