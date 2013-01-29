// Copyright (c) 2011, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

part of org_dartlang_compiler_util;

class Link<T> extends Iterable<T> {
  T get head => null;
  Link<T> get tail => null;

  factory Link.fromList(List<T> list) {
    switch (list.length) {
      case 0:
        return new Link<T>();
      case 1:
        return new LinkEntry<T>(list[0]);
      case 2:
        return new LinkEntry<T>(list[0], new LinkEntry<T>(list[1]));
      case 3:
        return new LinkEntry<T>(
            list[0], new LinkEntry<T>(list[1], new LinkEntry<T>(list[2])));
    }
    Link link = new Link<T>();
    for (int i = list.length ; i > 0; i--) {
      link = link.prepend(list[i - 1]);
    }
    return link;
  }

  const Link();

  Link<T> prepend(T element) {
    return new LinkEntry<T>(element, this);
  }

  Iterator<T> get iterator => new LinkIterator<T>(this);

  void printOn(StringBuffer buffer, [separatedBy]) {
  }

  List toList() => new List<T>.fixedLength(0);

  bool get isEmpty => true;

  Link<T> reverse() => this;

  Link<T> reversePrependAll(Link<T> from) {
    if (from.isEmpty) return this;
    return this.prepend(from.head).reversePrependAll(from.tail);
  }

  Link<T> skip(int n) {
    if (n == 0) return this;
    throw new RangeError('Index $n out of range');
  }

  void forEach(void f(T element)) {}

  bool operator ==(other) {
    if (other is !Link<T>) return false;
    return other.isEmpty;
  }

  String toString() => "[]";

  get length {
    throw new UnsupportedError('get:length');
  }
}

abstract class LinkBuilder<T> {
  factory LinkBuilder() = LinkBuilderImplementation;

  Link<T> toLink();
  Link<T> peekLink();
  void addLast(T t);

  LinkEntry<T> get lastLink;
  void truncateTo(Link<T> newLast);

  final int length;
  final bool isEmpty;

  toString() => "ord[${peekLink()}]=$length";
  
/*
  static test1() {
    LinkBuilder<String> builder = new LinkBuilder<String>();
    builder.addLast("Mary");
    builder.addLast("had");
    Link<String> had = builder.lastLink;
    builder.addLast("a little lamb");
    print("$builder");
    builder.truncateTo(had);
    print("$builder");
    builder.addLast("a red corvette");
    print("$builder");
  }

  static test2() {
    LinkBuilder<String> builder = new LinkBuilder<String>();
    Link<String> head = builder.lastLink;
    builder.addLast("Winne the Pooh");
    builder.addLast("Eeyore");
    print("$builder");
    builder.truncateTo(head);
    print("$builder");
    builder.addLast("Piglet");
    print("$builder");
  }

  static test3() {
    LinkBuilder<String> builder = new LinkBuilder<String>();
    Link<String> head = builder.lastLink;
    print("$builder");
    builder.truncateTo(head);
    print("$builder");
    builder.addLast("Pigs on the Wings");
    print("$builder");
  }

  static test4() {
    LinkBuilder<String> builder = new LinkBuilder<String>();
    builder.addLast("1");
    Link<String> head = builder.lastLink;
    builder.addLast("2");
    print("$builder");
    builder.truncateTo(head);
    print("$builder");
    builder.addLast("3");
    print("$builder");
  }

  static test5() {
    LinkBuilder<String> builder = new LinkBuilder<String>();
    builder.addLast("10");
    builder.addLast("20");
    Link<String> lastLink = builder.lastLink;
    print("$builder");
    builder.truncateTo(lastLink);
    print("$builder");
    builder.addLast("30");
    print("$builder");
  }

  static test() {
    test1();
    test2();
    test3();
    test4();
    test5();
  }
*/  
}
