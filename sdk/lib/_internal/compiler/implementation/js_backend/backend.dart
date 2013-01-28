// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

part of js_backend;

typedef void Recompile(Element element);

class ReturnInfo {
  HType returnType;
  List<Element> compiledFunctions;

  ReturnInfo(HType this.returnType)
      : compiledFunctions = new List<Element>();

  ReturnInfo.unknownType() : this(null);

  void update(HType type, Recompile recompile, Compiler compiler) {
    HType newType =
        returnType != null ? returnType.union(type, compiler) : type;
    if (newType != returnType) {
      if (returnType == null && identical(newType, HType.UNKNOWN)) {
        // If the first actual piece of information is not providing any type
        // information there is no need to recompile callers.
        compiledFunctions.clear();
      }
      returnType = newType;
      if (recompile != null) {
        compiledFunctions.forEach(recompile);
      }
      compiledFunctions.clear();
    }
  }

  // Note that lazy initializers are treated like functions (but are not
  // of type [FunctionElement].
  addCompiledFunction(Element function) => compiledFunctions.add(function);
}

class OptionalParameterTypes {
  final List<SourceString> names;
  final List<HType> types;

  OptionalParameterTypes(int optionalArgumentsCount)
      : names = new List<SourceString>.fixedLength(optionalArgumentsCount),
        types = new List<HType>.fixedLength(optionalArgumentsCount);

  int get length => names.length;
  SourceString name(int index) => names[index];
  HType type(int index) => types[index];
  int indexOf(SourceString name) => names.indexOf(name);

  HType typeFor(SourceString name) {
    int index = indexOf(name);
    if (index == -1) return null;
    return type(index);
  }

  void update(int index, SourceString name, HType type) {
    names[index] = name;
    types[index] = type;
  }

  String toString() => "OptionalParameterTypes($names, $types)";
}

class HTypeList {
  final List<HType> types;
  final List<SourceString> namedArguments;

  HTypeList(int length)
      : types = new List<HType>.fixedLength(length),
        namedArguments = null;
  HTypeList.withNamedArguments(int length, this.namedArguments)
      : types = new List<HType>.fixedLength(length);
  const HTypeList.withAllUnknown()
      : types = null,
        namedArguments = null;

  factory HTypeList.fromStaticInvocation(HInvokeStatic node, HTypeMap types) {
    bool allUnknown = true;
    for (int i = 1; i < node.inputs.length; i++) {
      if (types[node.inputs[i]] != HType.UNKNOWN) {
        allUnknown = false;
        break;
      }
    }
    if (allUnknown) return HTypeList.ALL_UNKNOWN;

    HTypeList result = new HTypeList(node.inputs.length - 1);
    for (int i = 0; i < result.types.length; i++) {
      result.types[i] = types[node.inputs[i + 1]];
    }
    return result;
  }

  factory HTypeList.fromDynamicInvocation(HInvokeDynamic node,
                                          Selector selector,
                                          HTypeMap types) {
    HTypeList result;
    int argumentsCount = node.inputs.length - 1;
    int startInvokeIndex = HInvoke.ARGUMENTS_OFFSET;

    if (node.isInterceptorCall) {
      argumentsCount--;
      startInvokeIndex++;
    }

    if (selector.namedArgumentCount > 0) {
      result =
          new HTypeList.withNamedArguments(
              argumentsCount, selector.namedArguments);
    } else {
      result = new HTypeList(argumentsCount);
    }

    for (int i = 0; i < result.types.length; i++) {
      result.types[i] = types[node.inputs[i + startInvokeIndex]];
    }
    return result;
  }

  static const HTypeList ALL_UNKNOWN = const HTypeList.withAllUnknown();

  bool get allUnknown => types == null;
  bool get hasNamedArguments => namedArguments != null;
  int get length => types.length;
  HType operator[](int index) => types[index];
  void operator[]=(int index, HType type) { types[index] = type; }

  HTypeList union(HTypeList other, Compiler compiler) {
    if (allUnknown) return this;
    if (other.allUnknown) return other;
    if (length != other.length) return HTypeList.ALL_UNKNOWN;
    bool onlyUnknown = true;
    HTypeList result = this;
    for (int i = 0; i < length; i++) {
      HType newType = this[i].union(other[i], compiler);
      if (result == this && newType != this[i]) {
        // Create a new argument types object with the matching types copied.
        result = new HTypeList(length);
        result.types.setRange(0, i, this.types);
      }
      if (result != this) {
        result.types[i] = newType;
      }
      if (result[i] != HType.UNKNOWN) onlyUnknown = false;
    }
    return onlyUnknown ? HTypeList.ALL_UNKNOWN : result;
  }

  HTypeList unionWithOptionalParameters(
      Selector selector,
      FunctionSignature signature,
      OptionalParameterTypes defaultValueTypes) {
    assert(allUnknown || selector.argumentCount == this.length);
    // Create a new HTypeList for holding types for all parameters.
    HTypeList result = new HTypeList(signature.parameterCount);

    // First fill in the type of the positional arguments.
    int nextTypeIndex = -1;
    if (allUnknown) {
      for (int i = 0; i < selector.positionalArgumentCount; i++) {
        result.types[i] = HType.UNKNOWN;
      }
    } else {
      result.types.setRange(0, selector.positionalArgumentCount, this.types);
      nextTypeIndex = selector.positionalArgumentCount;
    }

    // Next fill the type of the optional arguments.
    // As the selector can pass optional arguments positionally some of the
    // optional arguments might already have a type set. We only need to look
    // at the optional arguments not passed positionally.
    // The variable 'index' is counting the signatures optional arguments, the
    // variable 'next' is set to the next optional arguments to look at and
    // is used to skip some optional arguments.
    int next = selector.positionalArgumentCount;
    int index = signature.requiredParameterCount;
    signature.forEachOptionalParameter((Element element) {
      // If some optional parameters were passed positionally these have
      // already been filled.
      if (index == next) {
        assert(result.types[index] == null);
        HType type = null;
        if (hasNamedArguments &&
            selector.namedArguments.indexOf(element.name) >= 0) {
          type = types[nextTypeIndex++];
        } else {
          type = defaultValueTypes.typeFor(element.name);
        }
        result.types[index] = type;
        next++;
      }
      index++;
    });
    return result;
  }

  String toString() =>
      allUnknown ? "HTypeList.ALL_UNKNOWN" : "HTypeList $types";
}

class FieldTypesRegistry {
  final JavaScriptBackend backend;

  /**
   * For each class, [constructors] holds the set of constructors. If there is
   * more than one constructor for a class it is currently not possible to
   * infer the field types from construction, as the information collected does
   * not correlate the generative constructors and generative constructor
   * body/bodies.
   */
  final Map<ClassElement, Set<Element>> constructors;

  /**
   * The collected type information is stored in three maps. One for types
   * assigned in the initializer list(s) [fieldInitializerTypeMap], one for
   * types assigned in the constructor(s) [fieldConstructorTypeMap], and one
   * for types assigned in the rest of the code, where the field can be
   * resolved [fieldTypeMap].
   *
   * If a field has a type both from constructors and from the initializer
   * list(s), then the type from the constructor(s) will owerride the one from
   * the initializer list(s).
   *
   * Because the order in which generative constructors, generative constructor
   * bodies and normal method/function bodies are compiled is undefined, and
   * because they can all be recompiled, it is not possible to combine this
   * information into one map at the moment.
   */
  final Map<Element, HType> fieldInitializerTypeMap;
  final Map<Element, HType> fieldConstructorTypeMap;
  final Map<Element, HType> fieldTypeMap;

  /**
   * The set of current names setter selectors used. If a named selector is
   * used it is currently not possible to infer the type of the field.
   */
  final Set<SourceString> setterSelectorsUsed;

  final Map<Element, Set<Element>> optimizedStaticFunctions;
  final Map<Element, FunctionSet> optimizedFunctions;

  FieldTypesRegistry(JavaScriptBackend backend)
      : constructors =  new Map<ClassElement, Set<Element>>(),
        fieldInitializerTypeMap = new Map<Element, HType>(),
        fieldConstructorTypeMap = new Map<Element, HType>(),
        fieldTypeMap = new Map<Element, HType>(),
        setterSelectorsUsed = new Set<SourceString>(),
        optimizedStaticFunctions = new Map<Element, Set<Element>>(),
        optimizedFunctions = new Map<Element, FunctionSet>(),
        this.backend = backend;

  Compiler get compiler => backend.compiler;

  void scheduleRecompilation(Element field) {
    Set optimizedStatics = optimizedStaticFunctions[field];
    if (optimizedStatics != null) {
      optimizedStatics.forEach(backend.scheduleForRecompilation);
      optimizedStaticFunctions.remove(field);
    }
    FunctionSet optimized = optimizedFunctions[field];
    if (optimized != null) {
      optimized.forEach(backend.scheduleForRecompilation);
      optimizedFunctions.remove(field);
    }
  }

  int constructorCount(Element element) {
    assert(element.isClass());
    Set<Element> ctors = constructors[element];
    return ctors == null ? 0 : ctors.length;
  }

  void registerFieldType(Map<Element, HType> typeMap,
                         Element field,
                         HType type) {
    assert(field.isField());
    HType before = optimisticFieldType(field);

    HType oldType = typeMap[field];
    HType newType;

    if (oldType != null) {
      newType = oldType.union(type, compiler);
    } else {
      newType = type;
    }
    typeMap[field] = newType;
    if (oldType != newType) {
      scheduleRecompilation(field);
    }
  }

  void registerConstructor(Element element) {
    assert(element.isGenerativeConstructor());
    Element cls = element.getEnclosingClass();
    constructors.putIfAbsent(cls, () => new Set<Element>());
    Set<Element> ctors = constructors[cls];
    if (ctors.contains(element)) return;
    ctors.add(element);
    // We cannot infer field types for classes with more than one constructor.
    // When the second constructor is seen, recompile all functions relying on
    // optimistic field types for that class.
    // TODO(sgjesse): Handle field types for classes with more than one
    // constructor.
    if (ctors.length == 2) {
      optimizedFunctions.forEach((Element field, _) {
        if (identical(field.enclosingElement, cls)) {
          scheduleRecompilation(field);
        }
      });
    }
  }

  void registerFieldInitializer(Element field, HType type) {
    registerFieldType(fieldInitializerTypeMap, field, type);
  }

  void registerFieldConstructor(Element field, HType type) {
    registerFieldType(fieldConstructorTypeMap, field, type);
  }

  void registerFieldSetter(FunctionElement element, Element field, HType type) {
    HType initializerType = fieldInitializerTypeMap[field];
    HType constructorType = fieldConstructorTypeMap[field];
    HType setterType = fieldTypeMap[field];
    if (type == HType.UNKNOWN
        && initializerType == null
        && constructorType == null
        && setterType == null) {
      // Don't register UNKONWN if there is currently no type information
      // present for the field. Instead register the function holding the
      // setter for recompilation if better type information for the field
      // becomes available.
      registerOptimizedFunction(element, field, type);
      return;
    }
    registerFieldType(fieldTypeMap, field, type);
  }

  void addedDynamicSetter(Selector setter, HType type) {
    // Field type optimizations are disabled for all fields matching a
    // setter selector.
    assert(setter.isSetter());
    // TODO(sgjesse): Take the type of the setter into account.
    if (setterSelectorsUsed.contains(setter.name)) return;
    setterSelectorsUsed.add(setter.name);
    optimizedStaticFunctions.forEach((Element field, _) {
      if (field.name == setter.name) {
        scheduleRecompilation(field);
      }
    });
    optimizedFunctions.forEach((Element field, _) {
      if (field.name == setter.name) {
        scheduleRecompilation(field);
      }
    });
  }

  HType optimisticFieldType(Element field) {
    assert(field.isField());
    if (constructorCount(field.getEnclosingClass()) > 1) {
      return HType.UNKNOWN;
    }
    if (setterSelectorsUsed.contains(field.name)) {
      return HType.UNKNOWN;
    }
    HType initializerType = fieldInitializerTypeMap[field];
    HType constructorType = fieldConstructorTypeMap[field];
    if (initializerType == null && constructorType == null) {
      // If there are no constructor type information return UNKNOWN. This
      // ensures that the function will be recompiled if useful constructor
      // type information becomes available.
      return HType.UNKNOWN;
    }
    // A type set through the constructor overrides the type from the
    // initializer list.
    HType result = constructorType != null ? constructorType : initializerType;
    HType type = fieldTypeMap[field];
    if (type != null) result = result.union(type, compiler);
    return result;
  }

  void registerOptimizedFunction(FunctionElement element,
                                 Element field,
                                 HType type) {
    assert(field.isField());
    if (Elements.isStaticOrTopLevel(element)) {
      optimizedStaticFunctions.putIfAbsent(
          field, () => new Set<Element>());
      optimizedStaticFunctions[field].add(element);
    } else {
      optimizedFunctions.putIfAbsent(
          field, () => new FunctionSet(backend.compiler));
      optimizedFunctions[field].add(element);
    }
  }

  void dump() {
    Set<Element> allFields = new Set<Element>();
    fieldInitializerTypeMap.keys.forEach(allFields.add);
    fieldConstructorTypeMap.keys.forEach(allFields.add);
    fieldTypeMap.keys.forEach(allFields.add);
    allFields.forEach((Element field) {
      print("Inferred $field has type ${optimisticFieldType(field)}");
    });
  }
}

class ArgumentTypesRegistry {
  final JavaScriptBackend backend;

  /**
   * Documentation wanted -- johnniwinther
   *
   * Invariant: Keys must be declaration elements.
   */
  final Map<Element, HTypeList> staticTypeMap;

  /**
   * Documentation wanted -- johnniwinther
   *
   * Invariant: Elements must be declaration elements.
   */
  final Set<Element> optimizedStaticFunctions;
  final SelectorMap<HTypeList> selectorTypeMap;
  final FunctionSet optimizedFunctions;

  /**
   * Documentation wanted -- johnniwinther
   *
   * Invariant: Keys must be declaration elements.
   */
  final Map<Element, HTypeList> optimizedTypes;
  final Map<Element, OptionalParameterTypes> optimizedDefaultValueTypes;

  ArgumentTypesRegistry(JavaScriptBackend backend)
      : staticTypeMap = new Map<Element, HTypeList>(),
        optimizedStaticFunctions = new Set<Element>(),
        selectorTypeMap = new SelectorMap<HTypeList>(backend.compiler),
        optimizedFunctions = new FunctionSet(backend.compiler),
        optimizedTypes = new Map<Element, HTypeList>(),
        optimizedDefaultValueTypes =
            new Map<Element, OptionalParameterTypes>(),
        this.backend = backend;

  Compiler get compiler => backend.compiler;

  bool updateTypes(HTypeList oldTypes, HTypeList newTypes, var key, var map) {
    if (oldTypes.allUnknown) return false;
    newTypes = oldTypes.union(newTypes, backend.compiler);
    if (identical(newTypes, oldTypes)) return false;
    map[key] = newTypes;
    return true;
  }

  void registerStaticInvocation(HInvokeStatic node, HTypeMap types) {
    Element element = node.element;
    assert(invariant(node, element.isDeclaration));
    HTypeList oldTypes = staticTypeMap[element];
    HTypeList newTypes = new HTypeList.fromStaticInvocation(node, types);
    if (oldTypes == null) {
      staticTypeMap[element] = newTypes;
    } else if (updateTypes(oldTypes, newTypes, element, staticTypeMap)) {
      if (optimizedStaticFunctions.contains(element)) {
        backend.scheduleForRecompilation(element);
      }
    }
  }

  void registerNonCallStaticUse(HStatic node) {
    // When a static is used for anything else than a call target we cannot
    // infer anything about its parameter types.
    Element element = node.element;
    assert(invariant(node, element.isDeclaration));
    if (optimizedStaticFunctions.contains(element)) {
      backend.scheduleForRecompilation(element);
    }
    staticTypeMap[element] = HTypeList.ALL_UNKNOWN;
  }

  void registerDynamicInvocation(HTypeList providedTypes, Selector selector) {
    if (selector.isClosureCall()) {
      // We cannot use the current framework to do optimizations based
      // on the 'call' selector because we are also generating closure
      // calls during the emitter phase, which at this point, does not
      // track parameter types, nor invalidates optimized methods.
      return;
    }
    if (!selectorTypeMap.containsKey(selector)) {
      selectorTypeMap[selector] = providedTypes;
    } else {
      HTypeList oldTypes = selectorTypeMap[selector];
      updateTypes(oldTypes, providedTypes, selector, selectorTypeMap);
    }

    // If we're not compiling, we don't have to do anything.
    if (compiler.phase != Compiler.PHASE_COMPILING) return;

    // Run through all optimized functions and figure out if they need
    // to be recompiled because of this new invocation.
    optimizedFunctions.filterBySelector(selector).forEach((Element element) {
      // TODO(kasperl): Maybe check if the element is already marked for
      // recompilation? Could be pretty cheap compared to computing
      // union types.
      HTypeList newTypes =
          parameterTypes(element, optimizedDefaultValueTypes[element]);
      bool recompile = false;
      if (newTypes.allUnknown) {
        recompile = true;
      } else {
        HTypeList oldTypes = optimizedTypes[element];
        assert(newTypes.length == oldTypes.length);
        for (int i = 0; i < oldTypes.length; i++) {
          if (newTypes[i] != oldTypes[i]) {
            recompile = true;
            break;
          }
        }
      }
      if (recompile) backend.scheduleForRecompilation(element);
    });
  }

  HTypeList parameterTypes(FunctionElement element,
                           OptionalParameterTypes defaultValueTypes) {
    assert(invariant(element, element.isDeclaration));
    // Handle static functions separately.
    if (Elements.isStaticOrTopLevelFunction(element) ||
        element.kind == ElementKind.GENERATIVE_CONSTRUCTOR) {
      HTypeList types = staticTypeMap[element];
      if (types != null) {
        if (!optimizedStaticFunctions.contains(element)) {
          optimizedStaticFunctions.add(element);
        }
        return types;
      } else {
        return HTypeList.ALL_UNKNOWN;
      }
    }

    // Getters have no parameters.
    if (element.isGetter()) return HTypeList.ALL_UNKNOWN;

    // TODO(kasperl): What kind of non-members do we get here?
    if (!element.isMember()) return HTypeList.ALL_UNKNOWN;

    // If there are any getters for this method we cannot know anything about
    // the types of the provided parameters. Use resolverWorld for now as that
    // information does not change during compilation.
    // TODO(ngeoffray): These checks should use the codegenWorld and keep track
    // of changes to this information.
    if (compiler.resolverWorld.hasInvokedGetter(element, compiler)) {
      return HTypeList.ALL_UNKNOWN;
    }

    FunctionSignature signature = element.computeSignature(compiler);
    HTypeList found = null;
    selectorTypeMap.visitMatching(element,
        (Selector selector, HTypeList types) {
      if (selector.argumentCount != signature.parameterCount ||
          selector.namedArgumentCount > 0) {
        types = types.unionWithOptionalParameters(selector,
                                                  signature,
                                                  defaultValueTypes);
      }
      assert(types.allUnknown || types.length == signature.parameterCount);
      found = (found == null) ? types : found.union(types, compiler);
      return !found.allUnknown;
    });
    return found != null ? found : HTypeList.ALL_UNKNOWN;
  }

  void registerOptimizedFunction(Element element,
                                 HTypeList parameterTypes,
                                 OptionalParameterTypes defaultValueTypes) {
    if (Elements.isStaticOrTopLevelFunction(element)) {
      if (parameterTypes.allUnknown) {
        optimizedStaticFunctions.remove(element);
      } else {
        optimizedStaticFunctions.add(element);
      }
    }

    // TODO(kasperl): What kind of non-members do we get here?
    if (!element.isMember()) return;

    if (parameterTypes.allUnknown) {
      optimizedFunctions.remove(element);
      optimizedTypes.remove(element);
      optimizedDefaultValueTypes.remove(element);
    } else {
      optimizedFunctions.add(element);
      optimizedTypes[element] = parameterTypes;
      optimizedDefaultValueTypes[element] = defaultValueTypes;
    }
  }

  void dump() {
    optimizedFunctions.forEach((Element element) {
      HTypeList types = optimizedTypes[element];
      print("Inferred $element has argument types ${types.types}");
    });
  }
}

class JavaScriptItemCompilationContext extends ItemCompilationContext {
  final HTypeMap types;
  final Set<HInstruction> boundsChecked;

  JavaScriptItemCompilationContext()
      : types = new HTypeMap(),
        boundsChecked = new Set<HInstruction>();
}

class JavaScriptBackend extends Backend {
  SsaBuilderTask builder;
  SsaOptimizerTask optimizer;
  SsaCodeGeneratorTask generator;
  CodeEmitterTask emitter;

  ClassElement jsStringClass;
  ClassElement jsArrayClass;
  ClassElement jsNumberClass;
  ClassElement jsIntClass;
  ClassElement jsDoubleClass;
  ClassElement jsFunctionClass;
  ClassElement jsNullClass;
  ClassElement jsBoolClass;
  ClassElement objectInterceptorClass;
  Element jsArrayLength;
  Element jsStringLength;
  Element jsArrayRemoveLast;
  Element jsArrayAdd;
  Element jsStringSplit;
  Element jsStringConcat;
  Element getInterceptorMethod;
  Element fixedLengthListConstructor;
  bool seenAnyClass = false;

  final Namer namer;

  /**
   * Interface used to determine if an object has the JavaScript
   * indexing behavior. The interface is only visible to specific
   * libraries.
   */
  ClassElement jsIndexingBehaviorInterface;

  final Map<Element, ReturnInfo> returnInfo;

  /**
   * Documentation wanted -- johnniwinther
   *
   * Invariant: Elements must be declaration elements.
   */
  final List<Element> invalidateAfterCodegen;
  ArgumentTypesRegistry argumentTypes;
  FieldTypesRegistry fieldTypes;

  /**
   * A collection of selectors of intercepted method calls. The
   * emitter uses this set to generate the [:ObjectInterceptor:] class
   * whose members just forward the call to the intercepted receiver.
   */
  final Set<Selector> usedInterceptors;

  /**
   * A collection of selectors that must have a one shot interceptor
   * generated.
   */
  final Set<Selector> oneShotInterceptors;

  /**
   * The members of instantiated interceptor classes: maps a member
   * name to the list of members that have that name. This map is used
   * by the codegen to know whether a send must be intercepted or not.
   */
  final Map<SourceString, Set<Element>> interceptedElements;

  /**
   * A map of specialized versions of the [getInterceptorMethod].
   * Since [getInterceptorMethod] is a hot method at runtime, we're
   * always specializing it based on the incoming type. The keys in
   * the map are the names of these specialized versions. Note that
   * the generic version that contains all possible type checks is
   * also stored in this map.
   */
  final Map<String, Collection<ClassElement>> specializedGetInterceptors;

  /**
   * Set of classes whose instances are intercepted. Implemented as a
   * [LinkedHashMap] to preserve the insertion order.
   * TODO(ngeoffray): No need to preserve order anymore.
   */
  final Map<ClassElement, ClassElement> interceptedClasses;

  List<CompilerTask> get tasks {
    return <CompilerTask>[builder, optimizer, generator, emitter];
  }

  final RuntimeTypeInformation rti;

  JavaScriptBackend(Compiler compiler, bool generateSourceMap, bool disableEval)
      : namer = determineNamer(compiler),
        returnInfo = new Map<Element, ReturnInfo>(),
        invalidateAfterCodegen = new List<Element>(),
        usedInterceptors = new Set<Selector>(),
        oneShotInterceptors = new Set<Selector>(),
        interceptedElements = new Map<SourceString, Set<Element>>(),
        rti = new RuntimeTypeInformation(compiler),
        specializedGetInterceptors =
            new Map<String, Collection<ClassElement>>(),
        interceptedClasses = new LinkedHashMap<ClassElement, ClassElement>(),
        super(compiler, JAVA_SCRIPT_CONSTANT_SYSTEM) {
    emitter = disableEval
        ? new CodeEmitterNoEvalTask(compiler, namer, generateSourceMap)
        : new CodeEmitterTask(compiler, namer, generateSourceMap);
    builder = new SsaBuilderTask(this);
    optimizer = new SsaOptimizerTask(this);
    generator = new SsaCodeGeneratorTask(this);
    argumentTypes = new ArgumentTypesRegistry(this);
    fieldTypes = new FieldTypesRegistry(this);
  }

  static Namer determineNamer(Compiler compiler) {
    return compiler.enableMinification ?
        new MinifyNamer(compiler) :
        new Namer(compiler);
  }

  bool isInterceptorClass(Element element) {
    if (element == null) return false;
    return interceptedClasses.containsKey(element);
  }

  void addInterceptedSelector(Selector selector) {
    usedInterceptors.add(selector);
  }

  void addOneShotInterceptor(Selector selector) {
    oneShotInterceptors.add(selector);
  }

  /**
   * Returns a set of interceptor classes that contain a member whose
   * signature matches the given [selector]. Returns [:null:] if there
   * is no class.
   */
  Set<ClassElement> getInterceptedClassesOn(Selector selector) {
    Set<Element> intercepted = interceptedElements[selector.name];
    if (intercepted == null) return null;
    Set<ClassElement> result = new Set<ClassElement>();
    for (Element element in intercepted) {
      if (selector.applies(element, compiler)) {
        result.add(element.getEnclosingClass());
      }
    }
    if (result.isEmpty) return null;
    return result;
  }

  List<ClassElement> getListOfInterceptedClasses() {
      return <ClassElement>[jsStringClass, jsArrayClass, jsIntClass,
                            jsDoubleClass, jsNumberClass, jsNullClass,
                            jsFunctionClass, jsBoolClass];
  }

  void initializeInterceptorElements() {
    objectInterceptorClass =
        compiler.findInterceptor(const SourceString('ObjectInterceptor'));
    getInterceptorMethod =
        compiler.findInterceptor(const SourceString('getInterceptor'));
    List<ClassElement> classes = [
      jsStringClass = compiler.findInterceptor(const SourceString('JSString')),
      jsArrayClass = compiler.findInterceptor(const SourceString('JSArray')),
      // The int class must be before the double class, because the
      // emitter relies on this list for the order of type checks.
      jsIntClass = compiler.findInterceptor(const SourceString('JSInt')),
      jsDoubleClass = compiler.findInterceptor(const SourceString('JSDouble')),
      jsNumberClass = compiler.findInterceptor(const SourceString('JSNumber')),
      jsNullClass = compiler.findInterceptor(const SourceString('JSNull')),
      jsFunctionClass =
          compiler.findInterceptor(const SourceString('JSFunction')),
      jsBoolClass = compiler.findInterceptor(const SourceString('JSBool'))];

    jsArrayClass.ensureResolved(compiler);
    jsArrayLength = compiler.lookupElementIn(
        jsArrayClass, const SourceString('length'));
    jsArrayRemoveLast = compiler.lookupElementIn(
        jsArrayClass, const SourceString('removeLast'));
    jsArrayAdd = compiler.lookupElementIn(
        jsArrayClass, const SourceString('add'));

    jsStringClass.ensureResolved(compiler);
    jsStringLength = compiler.lookupElementIn(
        jsStringClass, const SourceString('length'));
    jsStringSplit = compiler.lookupElementIn(
        jsStringClass, const SourceString('split'));
    jsStringConcat = compiler.lookupElementIn(
        jsStringClass, const SourceString('concat'));

    for (ClassElement cls in classes) {
      if (cls != null) interceptedClasses[cls] = null;
    }
  }

  void addInterceptors(ClassElement cls, Enqueuer enqueuer) {
    if (enqueuer.isResolutionQueue) {
      cls.ensureResolved(compiler);
      cls.forEachMember((ClassElement classElement, Element member) {
          Set<Element> set = interceptedElements.putIfAbsent(
              member.name, () => new Set<Element>());
          set.add(member);
        },
        includeSuperMembers: true);
    }
    enqueuer.registerInstantiatedClass(cls);
  }

  void registerSpecializedGetInterceptor(Set<ClassElement> classes) {
    compiler.enqueuer.codegen.registerInstantiatedClass(objectInterceptorClass);
    String name = namer.getInterceptorName(getInterceptorMethod, classes);
    if (classes.contains(compiler.objectClass)) {
      // We can't use a specialized [getInterceptorMethod], so we make
      // sure we emit the one with all checks.
      specializedGetInterceptors.putIfAbsent(name, () {
        // It is important to take the order provided by the map,
        // because we want the int type check to happen before the
        // double type check: the double type check covers the int
        // type check. Also we don't need to do a number type check
        // because that is covered by the double type check.
        List<ClassElement> keys = <ClassElement>[];
        interceptedClasses.forEach((ClassElement cls, _) {
          if (cls != jsNumberClass) keys.add(cls);
        });
        return keys;
      });
    } else {
      specializedGetInterceptors[name] = classes;
    }
  }

  void initializeNoSuchMethod() {
    // In case the emitter generates noSuchMethod calls, we need to
    // make sure all [noSuchMethod] methods know they might take a
    // [JsInvocationMirror] as parameter.
    HTypeList types = new HTypeList(1);
    types[0] = new HType.fromBoundedType(
        compiler.jsInvocationMirrorClass.computeType(compiler),
        compiler,
        false);
    argumentTypes.registerDynamicInvocation(types, new Selector.noSuchMethod());
  }

  void registerInstantiatedClass(ClassElement cls, Enqueuer enqueuer) {
    if (!seenAnyClass) {
      initializeInterceptorElements();
      initializeNoSuchMethod();
      seenAnyClass = true;
    }
    ClassElement result = null;
    if (cls == compiler.stringClass) {
      addInterceptors(jsStringClass, enqueuer);
    } else if (cls == compiler.listClass) {
      addInterceptors(jsArrayClass, enqueuer);
      // The backend will try to optimize array access and use the
      // `ioore` and `iae` helpers directly.
      enqueuer.registerStaticUse(
          compiler.findHelper(const SourceString('ioore')));
      enqueuer.registerStaticUse(
          compiler.findHelper(const SourceString('iae')));
    } else if (cls == compiler.intClass) {
      addInterceptors(jsIntClass, enqueuer);
      addInterceptors(jsNumberClass, enqueuer);
    } else if (cls == compiler.doubleClass) {
      addInterceptors(jsDoubleClass, enqueuer);
      addInterceptors(jsNumberClass, enqueuer);
    } else if (cls == compiler.functionClass) {
      addInterceptors(jsFunctionClass, enqueuer);
    } else if (cls == compiler.boolClass) {
      addInterceptors(jsBoolClass, enqueuer);
    } else if (cls == compiler.nullClass) {
      addInterceptors(jsNullClass, enqueuer);
    } else if (cls == compiler.numClass) {
      addInterceptors(jsIntClass, enqueuer);
      addInterceptors(jsDoubleClass, enqueuer);
      addInterceptors(jsNumberClass, enqueuer);
    }
  }

  Element get cyclicThrowHelper {
    return compiler.findHelper(const SourceString("throwCyclicInit"));
  }

  JavaScriptItemCompilationContext createItemCompilationContext() {
    return new JavaScriptItemCompilationContext();
  }

  void enqueueHelpers(ResolutionEnqueuer world) {
    enqueueAllTopLevelFunctions(compiler.jsHelperLibrary, world);

    jsIndexingBehaviorInterface =
        compiler.findHelper(const SourceString('JavaScriptIndexingBehavior'));
    if (jsIndexingBehaviorInterface != null) {
      world.registerIsCheck(jsIndexingBehaviorInterface.computeType(compiler));
    }

    for (var helper in [const SourceString('Closure'),
                        const SourceString('ConstantMap'),
                        const SourceString('ConstantProtoMap')]) {
      var e = compiler.findHelper(helper);
      if (e != null) world.registerInstantiatedClass(e);
    }
  }

  void codegen(CodegenWorkItem work) {
    if (work.element.kind.category == ElementCategory.VARIABLE) {
      Constant initialValue = compiler.constantHandler.compileWorkItem(work);
      if (initialValue != null) {
        return;
      } else {
        // If the constant-handler was not able to produce a result we have to
        // go through the builder (below) to generate the lazy initializer for
        // the static variable.
        // We also need to register the use of the cyclic-error helper.
        compiler.enqueuer.codegen.registerStaticUse(cyclicThrowHelper);
      }
    }

    HGraph graph = builder.build(work);
    optimizer.optimize(work, graph, false);
    if (work.allowSpeculativeOptimization
        && optimizer.trySpeculativeOptimizations(work, graph)) {
      js.Expression code = generator.generateBailoutMethod(work, graph);
      compiler.codegenWorld.addBailoutCode(work, code);
      optimizer.prepareForSpeculativeOptimizations(work, graph);
      optimizer.optimize(work, graph, true);
    }
    js.Expression code = generator.generateCode(work, graph);
    compiler.codegenWorld.addGeneratedCode(work, code);
    invalidateAfterCodegen.forEach(compiler.enqueuer.codegen.eagerRecompile);
    invalidateAfterCodegen.clear();
  }

  native.NativeEnqueuer nativeResolutionEnqueuer(Enqueuer world) {
    return new native.NativeResolutionEnqueuer(world, compiler);
  }

  native.NativeEnqueuer nativeCodegenEnqueuer(Enqueuer world) {
    return new native.NativeCodegenEnqueuer(world, compiler, emitter);
  }

  void assembleProgram() {
    emitter.assembleProgram();
  }

  /**
   * Documentation wanted -- johnniwinther
   *
   * Invariant: [element] must be a declaration element.
   */
  void scheduleForRecompilation(Element element) {
    assert(invariant(element, element.isDeclaration));
    if (compiler.phase == Compiler.PHASE_COMPILING) {
      invalidateAfterCodegen.add(element);
    }
  }

  /**
   *  Register a dynamic invocation and collect the provided types for the
   *  named selector.
   */
  void registerDynamicInvocation(HInvokeDynamic node,
                                 Selector selector,
                                 HTypeMap types) {
    HTypeList providedTypes =
        new HTypeList.fromDynamicInvocation(node, selector, types);
    argumentTypes.registerDynamicInvocation(providedTypes, selector);
  }

  /**
   *  Register a static invocation and collect the provided types for the
   *  named selector.
   */
  void registerStaticInvocation(HInvokeStatic node, HTypeMap types) {
    argumentTypes.registerStaticInvocation(node, types);
  }

  /**
   *  Register that a static is used for something else than a direct call
   *  target.
   */
  void registerNonCallStaticUse(HStatic node) {
    argumentTypes.registerNonCallStaticUse(node);
  }

  /**
   * Retrieve the types of the parameters used for calling the [element]
   * function. The types are optimistic in the sense as they are based on the
   * possible invocations of the function seen so far.
   *
   * Invariant: [element] must be a declaration element.
   */
  HTypeList optimisticParameterTypes(
      FunctionElement element,
      OptionalParameterTypes defaultValueTypes) {
    assert(invariant(element, element.isDeclaration));
    if (element.parameterCount(compiler) == 0) return HTypeList.ALL_UNKNOWN;
    return argumentTypes.parameterTypes(element, defaultValueTypes);
  }

  /**
   * Register that the function [element] has been optimized under the
   * assumptions that the types [parameterType] will be used for calling it.
   * The passed [defaultValueTypes] holds the types of default values for
   * the optional parameters. If this assumption fail the function will be
   * scheduled for recompilation.
   *
   * Invariant: [element] must be a declaration element.
   */
  registerParameterTypesOptimization(
      FunctionElement element,
      HTypeList parameterTypes,
      OptionalParameterTypes defaultValueTypes) {
    assert(invariant(element, element.isDeclaration));
    if (element.parameterCount(compiler) == 0) return;
    argumentTypes.registerOptimizedFunction(
        element, parameterTypes, defaultValueTypes);
  }

  registerFieldTypesOptimization(FunctionElement element,
                                 Element field,
                                 HType type) {
    fieldTypes.registerOptimizedFunction(element, field, type);
  }

  /**
   * Documentation wanted -- johnniwinther
   *
   * Invariant: [element] must be a declaration element.
   */
  void registerReturnType(FunctionElement element, HType returnType) {
    assert(invariant(element, element.isDeclaration));
    ReturnInfo info = returnInfo[element];
    if (info != null) {
      info.update(returnType, scheduleForRecompilation, compiler);
    } else {
      returnInfo[element] = new ReturnInfo(returnType);
    }
  }

  /**
   * Retrieve the return type of the function [callee]. The type is optimistic
   * in the sense that is is based on the compilation of [callee]. If [callee]
   * is recompiled the return type might change to someting broader. For that
   * reason [caller] is registered for recompilation if this happens. If the
   * function [callee] has not yet been compiled the returned type is [null].
   *
   * Invariant: Both [caller] and [callee] must be declaration elements.
   */
  HType optimisticReturnTypesWithRecompilationOnTypeChange(
      Element caller, FunctionElement callee) {
    assert(invariant(callee, callee.isDeclaration));
    returnInfo.putIfAbsent(callee, () => new ReturnInfo.unknownType());
    ReturnInfo info = returnInfo[callee];
    HType returnType = info.returnType;
    if (returnType != HType.UNKNOWN && returnType != null && caller != null) {
      assert(invariant(caller, caller.isDeclaration));
      info.addCompiledFunction(caller);
    }
    return info.returnType;
  }

  void dumpReturnTypes() {
    returnInfo.forEach((Element element, ReturnInfo info) {
      if (info.returnType != HType.UNKNOWN) {
        print("Inferred $element has return type ${info.returnType}");
      }
    });
  }

  void registerConstructor(Element element) {
    fieldTypes.registerConstructor(element);
  }

  void registerFieldInitializer(Element field, HType type) {
    fieldTypes.registerFieldInitializer(field, type);
  }

  void registerFieldConstructor(Element field, HType type) {
    fieldTypes.registerFieldConstructor(field, type);
  }

  void registerFieldSetter(FunctionElement element, Element field, HType type) {
    fieldTypes.registerFieldSetter(element, field, type);
  }

  void addedDynamicSetter(Selector setter, HType type) {
    fieldTypes.addedDynamicSetter(setter, type);
  }

  HType optimisticFieldType(Element element) {
    return fieldTypes.optimisticFieldType(element);
  }

  SourceString getCheckedModeHelper(DartType type) {
    Element element = type.element;
    bool nativeCheck =
          emitter.nativeEmitter.requiresNativeIsCheck(element);
    if (type.isMalformed) {
      // Check for malformed types first, because the type may be a list type
      // with a malformed argument type.
      return const SourceString('malformedTypeCheck');
    } else if (type == compiler.types.voidType) {
      return const SourceString('voidTypeCheck');
    } else if (element == compiler.stringClass) {
      return const SourceString('stringTypeCheck');
    } else if (element == compiler.doubleClass) {
      return const SourceString('doubleTypeCheck');
    } else if (element == compiler.numClass) {
      return const SourceString('numTypeCheck');
    } else if (element == compiler.boolClass) {
      return const SourceString('boolTypeCheck');
    } else if (element == compiler.functionClass) {
      return const SourceString('functionTypeCheck');
    } else if (element == compiler.intClass) {
      return const SourceString('intTypeCheck');
    } else if (Elements.isNumberOrStringSupertype(element, compiler)) {
      return nativeCheck
          ? const SourceString('numberOrStringSuperNativeTypeCheck')
          : const SourceString('numberOrStringSuperTypeCheck');
    } else if (Elements.isStringOnlySupertype(element, compiler)) {
      return nativeCheck
          ? const SourceString('stringSuperNativeTypeCheck')
          : const SourceString('stringSuperTypeCheck');
    } else if (identical(element, compiler.listClass)) {
      return const SourceString('listTypeCheck');
    } else {
      if (Elements.isListSupertype(element, compiler)) {
        return nativeCheck
            ? const SourceString('listSuperNativeTypeCheck')
            : const SourceString('listSuperTypeCheck');
      } else {
        return nativeCheck
            ? const SourceString('callTypeCheck')
            : const SourceString('propertyTypeCheck');
      }
    }
  }

  void dumpInferredTypes() {
    print("Inferred argument types:");
    print("------------------------");
    argumentTypes.dump();
    print("");
    print("Inferred return types:");
    print("----------------------");
    dumpReturnTypes();
    print("");
    print("Inferred field types:");
    print("------------------------");
    fieldTypes.dump();
    print("");
  }

  Element getExceptionUnwrapper() {
    return compiler.findHelper(const SourceString('unwrapException'));
  }

  Element getThrowRuntimeError() {
    return compiler.findHelper(const SourceString('throwRuntimeError'));
  }

  Element getThrowMalformedSubtypeError() {
    return compiler.findHelper(
        const SourceString('throwMalformedSubtypeError'));
  }

  Element getThrowAbstractClassInstantiationError() {
    return compiler.findHelper(
        const SourceString('throwAbstractClassInstantiationError'));
  }

  Element getClosureConverter() {
    return compiler.findHelper(const SourceString('convertDartClosureToJS'));
  }

  Element getTraceFromException() {
    return compiler.findHelper(const SourceString('getTraceFromException'));
  }

  Element getMapMaker() {
    return compiler.findHelper(const SourceString('makeLiteralMap'));
  }

  Element getSetRuntimeTypeInfo() {
    return compiler.findHelper(const SourceString('setRuntimeTypeInfo'));
  }

  Element getGetRuntimeTypeInfo() {
    return compiler.findHelper(const SourceString('getRuntimeTypeInfo'));
  }
}
