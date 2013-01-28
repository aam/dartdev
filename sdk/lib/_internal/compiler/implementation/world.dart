// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

part of dart2js;

class World {
  final Compiler compiler;
  final Map<ClassElement, Set<ClassElement>> subtypes;
  final Map<ClassElement, Set<MixinApplicationElement>> mixinUses;
  final Map<ClassElement, Set<ClassElement>> typesImplementedBySubclasses;
  final Set<ClassElement> classesNeedingRti;
  final Map<ClassElement, Set<ClassElement>> rtiDependencies;
  final FunctionSet userDefinedGetters;
  final FunctionSet userDefinedSetters;

  World(Compiler compiler)
      : subtypes = new Map<ClassElement, Set<ClassElement>>(),
        mixinUses = new Map<ClassElement, Set<MixinApplicationElement>>(),
        typesImplementedBySubclasses =
            new Map<ClassElement, Set<ClassElement>>(),
        userDefinedGetters = new FunctionSet(compiler),
        userDefinedSetters = new FunctionSet(compiler),
        classesNeedingRti = new Set<ClassElement>(),
        rtiDependencies = new Map<ClassElement, Set<ClassElement>>(),
        this.compiler = compiler;

  void populate() {
    void addSubtypes(ClassElement cls) {
      if (cls.resolutionState != STATE_DONE) {
        compiler.internalErrorOnElement(
            cls, 'Class "${cls.name.slowToString()}" is not resolved.');
      }

      for (DartType type in cls.allSupertypes) {
        Set<Element> subtypesOfCls =
          subtypes.putIfAbsent(type.element, () => new Set<ClassElement>());
        subtypesOfCls.add(cls);
      }

      // Walk through the superclasses, and record the types
      // implemented by that type on the superclasses.
      DartType type = cls.supertype;
      while (type != null) {
        Set<Element> typesImplementedBySubclassesOfCls =
          typesImplementedBySubclasses.putIfAbsent(
              type.element, () => new Set<ClassElement>());
        for (DartType current in cls.allSupertypes) {
          typesImplementedBySubclassesOfCls.add(current.element);
        }
        ClassElement classElement = type.element;
        type = classElement.supertype;
      }
    }

    compiler.resolverWorld.instantiatedClasses.forEach(addSubtypes);

    // Find the classes that need runtime type information. Such
    // classes are:
    // (1) used in a is check with type variables,
    // (2) dependencies of classes in (1),
    // (3) subclasses of (2) and (3).

    void potentiallyAddForRti(ClassElement cls) {
      if (cls.typeVariables.isEmpty) return;
      if (classesNeedingRti.contains(cls)) return;
      classesNeedingRti.add(cls);

      Set<ClassElement> classes = subtypes[cls];
      if (classes != null) {
        classes.forEach((ClassElement sub) {
          potentiallyAddForRti(sub);
        });
      }

      Set<ClassElement> dependencies = rtiDependencies[cls];
      if (dependencies != null) {
        dependencies.forEach((ClassElement other) {
          potentiallyAddForRti(other);
        });
      }
    }

    compiler.resolverWorld.isChecks.forEach((DartType type) {
      if (type is InterfaceType) {
        InterfaceType itf = type;
        if (!itf.isRaw) {
          potentiallyAddForRti(itf.element);
        }
      }
    });
  }

  bool needsRti(ClassElement cls) {
    return classesNeedingRti.contains(cls) || compiler.enabledRuntimeType;
  }

  void registerMixinUse(MixinApplicationElement mixinApplication,
                        ClassElement mixin) {
    Set<MixinApplicationElement> users =
        mixinUses.putIfAbsent(mixin, () =>
                              new Set<MixinApplicationElement>());
    users.add(mixinApplication);
  }

  void registerRtiDependency(Element element, Element dependency) {
    // We're not dealing with typedef for now.
    if (!element.isClass() || !dependency.isClass()) return;
    Set<ClassElement> classes =
        rtiDependencies.putIfAbsent(element, () => new Set<ClassElement>());
    classes.add(dependency);
  }

  void recordUserDefinedGetter(Element element) {
    assert(element.isGetter());
    userDefinedGetters.add(element);
  }

  void recordUserDefinedSetter(Element element) {
    assert(element.isSetter());
    userDefinedSetters.add(element);
  }

  bool hasAnyUserDefinedGetter(Selector selector) {
    return userDefinedGetters.hasAnyElementMatchingSelector(selector);
  }

  bool hasAnyUserDefinedSetter(Selector selector) {
    return userDefinedSetters.hasAnyElementMatchingSelector(selector);
  }

  // Returns whether a subclass of [superclass] implements [type].
  bool hasAnySubclassThatImplements(ClassElement superclass, DartType type) {
    Set<ClassElement> subclasses= typesImplementedBySubclasses[superclass];
    if (subclasses == null) return false;
    return subclasses.contains(type.element);
  }

  bool hasNoOverridingMember(Element element) {
    ClassElement cls = element.getEnclosingClass();
    Set<ClassElement> subclasses = compiler.world.subtypes[cls];
    // TODO(ngeoffray): Implement the full thing.
    return subclasses == null || subclasses.isEmpty;
  }

  void registerUsedElement(Element element) {
    if (element.isMember()) {
      if (element.isGetter()) {
        // We're collecting user-defined getters to let the codegen know which
        // field accesses might have side effects.
        recordUserDefinedGetter(element);
      } else if (element.isSetter()) {
        recordUserDefinedSetter(element);
      }
    }
  }

  /**
   * Returns a [MemberSet] that contains the possible targets of the given
   * [selector] on a receiver with the given [type]. This includes all sub
   * types.
   */
  MemberSet _memberSetFor(DartType type, Selector selector) {
    assert(compiler != null);
    ClassElement cls = type.element;
    SourceString name = selector.name;
    LibraryElement library = selector.library;
    MemberSet result = new MemberSet(name);
    Element element = cls.implementation.lookupSelector(selector);
    if (element != null) result.add(element);

    bool isPrivate = name.isPrivate();
    Set<ClassElement> subtypesOfCls = subtypes[cls];
    if (subtypesOfCls != null) {
      for (ClassElement sub in subtypesOfCls) {
        // Private members from a different library are not visible.
        if (isPrivate && sub.getLibrary() != library) continue;
        element = sub.implementation.lookupLocalMember(name);
        if (element != null) result.add(element);
      }
    }
    return result;
  }

  /**
   * Returns the field in [type] described by the given [selector].
   * If no such field exists, or a subclass overrides the field
   * returns [:null:].
   */
  VariableElement locateSingleField(DartType type, Selector selector) {
    MemberSet memberSet = _memberSetFor(type, selector);
    ClassElement cls = type.element;
    Element result = cls.implementation.lookupSelector(selector);
    if (result == null) return null;
    if (!result.isField()) return null;

    // Verify that no subclass overrides the field.
    if (memberSet.elements.length != 1) return null;
    assert(memberSet.elements.contains(result));
    return result;
  }

  Set<ClassElement> findNoSuchMethodHolders(DartType type) {
    Set<ClassElement> result = new Set<ClassElement>();
    Selector noSuchMethodSelector = new Selector.noSuchMethod();
    MemberSet memberSet = _memberSetFor(type, noSuchMethodSelector);
    for (Element element in memberSet.elements) {
      ClassElement holder = element.getEnclosingClass();
      if (!identical(holder, compiler.objectClass) &&
          noSuchMethodSelector.applies(element, compiler)) {
        result.add(holder);
      }
    }
    return result;
  }
}

/**
 * A [MemberSet] contains all the possible targets for a selector.
 */
class MemberSet {
  final Set<Element> elements;
  final SourceString name;

  MemberSet(SourceString this.name) : elements = new Set<Element>();

  void add(Element element) {
    elements.add(element);
  }

  bool get isEmpty => elements.isEmpty;
}
