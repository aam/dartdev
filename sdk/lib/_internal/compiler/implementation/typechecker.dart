// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

part of dart2js;

class TypeCheckerTask extends CompilerTask {
  TypeCheckerTask(Compiler compiler) : super(compiler);
  String get name => "Type checker";

  static const bool LOG_FAILURES = false;

  void check(Node tree, TreeElements elements) {
    measure(() {
      Visitor visitor =
          new TypeCheckerVisitor(compiler, elements, compiler.types);
      try {
        tree.accept(visitor);
      } on CancelTypeCheckException catch (e) {
        if (LOG_FAILURES) {
          // Do not warn about unimplemented features; log message instead.
          compiler.log("'${e.node}': ${e.reason}");
        }
      }
    });
  }
}

class TypeKind {
  final String id;

  const TypeKind(String this.id);

  static const TypeKind FUNCTION = const TypeKind('function');
  static const TypeKind INTERFACE = const TypeKind('interface');
  static const TypeKind STATEMENT = const TypeKind('statement');
  static const TypeKind TYPEDEF = const TypeKind('typedef');
  static const TypeKind TYPE_VARIABLE = const TypeKind('type variable');
  static const TypeKind MALFORMED_TYPE = const TypeKind('malformed');
  static const TypeKind VOID = const TypeKind('void');

  String toString() => id;
}

abstract class DartType {
  SourceString get name;

  TypeKind get kind;

  const DartType();

  /**
   * Returns the [Element] which declared this type.
   *
   * This can be [ClassElement] for classes, [TypedefElement] for typedefs,
   * [TypeVariableElement] for type variables and [FunctionElement] for
   * function types.
   *
   * Invariant: [element] must be a declaration element.
   */
  Element get element;

  /**
   * Performs the substitution [: [arguments[i]/parameters[i]]this :].
   *
   * The notation is known from this lambda calculus rule:
   *
   *     (lambda x.e0)e1 -> [e1/x]e0.
   *
   * See [TypeVariableType] for a motivation for this method.
   *
   * Invariant: There must be the same number of [arguments] and [parameters].
   */
  DartType subst(Link<DartType> arguments, Link<DartType> parameters);

  /**
   * Returns the unaliased type of this type.
   *
   * The unaliased type of a typedef'd type is the unaliased type to which its
   * name is bound. The unaliased version of any other type is the type itself.
   *
   * For example, the unaliased type of [: typedef A Func<A,B>(B b) :] is the
   * function type [: (B) -> A :] and the unaliased type of
   * [: Func<int,String> :] is the function type [: (String) -> int :].
   */
  DartType unalias(Compiler compiler);

  /**
   * A type is malformed if it is itself a malformed type or contains a
   * malformed type.
   */
  bool get isMalformed => false;

  /**
   * Calls [f] with each [MalformedType] within this type.
   *
   * If [f] returns [: false :], the traversal stops prematurely.
   *
   * [forEachMalformedType] returns [: false :] if the traversal was stopped
   * prematurely.
   */
  bool forEachMalformedType(bool f(MalformedType type)) => true;

  bool operator ==(other);

  /**
   * Is [: true :] if this type has no explict type arguments.
   */
  bool get isRaw => true;

  DartType asRaw() => this;
}

/**
 * Represents a type variable, that is the type parameters of a class type.
 *
 * For example, in [: class Array<E> { ... } :], E is a type variable.
 *
 * Each class should have its own unique type variables, one for each type
 * parameter. A class with type parameters is said to be parameterized or
 * generic.
 *
 * Non-static members, constructors, and factories of generic
 * class/interface can refer to type variables of the current class
 * (not of supertypes).
 *
 * When using a generic type, also known as an application or
 * instantiation of the type, the actual type arguments should be
 * substituted for the type variables in the class declaration.
 *
 * For example, given a box, [: class Box<T> { T value; } :], the
 * type of the expression [: new Box<String>().value :] is
 * [: String :] because we must substitute [: String :] for the
 * the type variable [: T :].
 */
class TypeVariableType extends DartType {
  final TypeVariableElement element;

  TypeVariableType(this.element);

  TypeKind get kind => TypeKind.TYPE_VARIABLE;

  SourceString get name => element.name;

  DartType subst(Link<DartType> arguments, Link<DartType> parameters) {
    if (parameters.isEmpty) {
      assert(arguments.isEmpty);
      // Return fast on empty substitutions.
      return this;
    }
    Link<DartType> parameterLink = parameters;
    Link<DartType> argumentLink = arguments;
    while (!argumentLink.isEmpty && !parameterLink.isEmpty) {
      TypeVariableType parameter = parameterLink.head;
      DartType argument = argumentLink.head;
      if (parameter == this) {
        assert(argumentLink.tail.isEmpty == parameterLink.tail.isEmpty);
        return argument;
      }
      parameterLink = parameterLink.tail;
      argumentLink = argumentLink.tail;
    }
    assert(argumentLink.isEmpty && parameterLink.isEmpty);
    // The type variable was not substituted.
    return this;
  }

  DartType unalias(Compiler compiler) => this;

  int get hashCode => 17 * element.hashCode;

  bool operator ==(other) {
    if (other is !TypeVariableType) return false;
    return identical(other.element, element);
  }

  String toString() => name.slowToString();
}

/**
 * A statement type tracks whether a statement returns or may return.
 */
class StatementType extends DartType {
  final String stringName;

  Element get element => null;

  TypeKind get kind => TypeKind.STATEMENT;

  SourceString get name => new SourceString(stringName);

  const StatementType(this.stringName);

  static const RETURNING = const StatementType('<returning>');
  static const NOT_RETURNING = const StatementType('<not returning>');
  static const MAYBE_RETURNING = const StatementType('<maybe returning>');

  /** Combine the information about two control-flow edges that are joined. */
  StatementType join(StatementType other) {
    return (identical(this, other)) ? this : MAYBE_RETURNING;
  }

  DartType subst(Link<DartType> arguments, Link<DartType> parameters) {
    // Statement types are not substitutable.
    return this;
  }

  DartType unalias(Compiler compiler) => this;

  int get hashCode => 17 * stringName.hashCode;

  bool operator ==(other) {
    if (other is !StatementType) return false;
    return other.stringName == stringName;
  }

  String toString() => stringName;
}

class VoidType extends DartType {
  const VoidType(this.element);

  TypeKind get kind => TypeKind.VOID;

  SourceString get name => element.name;

  final Element element;

  DartType subst(Link<DartType> arguments, Link<DartType> parameters) {
    // Void cannot be substituted.
    return this;
  }

  DartType unalias(Compiler compiler) => this;

  int get hashCode => 1729;

  bool operator ==(other) => other is VoidType;

  String toString() => name.slowToString();
}

class MalformedType extends DartType {
  final ErroneousElement element;

  /**
   * [declaredType] holds the type which the user wrote in code.
   *
   * For instance, for a resolved but malformed type like [: Map<String> :] the
   * [declaredType] is [: Map<String> :] whereas for an unresolved type
   */
  final DartType userProvidedBadType;

  /**
   * Type arguments for the malformed typed, if these cannot fit in the
   * [declaredType].
   *
   * This field is for instance used for [: dynamic<int> :] and [: T<int> :]
   * where [: T :] is a type variable, in which case [declaredType] holds
   * [: dynamic :] and [: T :], respectively, or for [: X<int> :] where [: X :]
   * is not resolved or does not imply a type.
   */
  final Link<DartType> typeArguments;

  MalformedType(this.element, this.userProvidedBadType,
                [this.typeArguments = null]);

  TypeKind get kind => TypeKind.MALFORMED_TYPE;

  SourceString get name => element.name;

  DartType subst(Link<DartType> arguments, Link<DartType> parameters) {
    // Malformed types are not substitutable.
    return this;
  }

  bool get isMalformed => true;

  bool forEachMalformedType(bool f(MalformedType type)) => f(this);

  DartType unalias(Compiler compiler) => this;

  String toString() {
    var sb = new StringBuffer();
    if (typeArguments != null) {
      if (userProvidedBadType != null) {
        sb.add(userProvidedBadType.name.slowToString());
      } else {
        sb.add(element.name.slowToString());
      }
      if (!typeArguments.isEmpty) {
        sb.add('<');
        typeArguments.printOn(sb, ', ');
        sb.add('>');
      }
    } else {
      sb.add(userProvidedBadType.toString());
    }
    return sb.toString();
  }
}

bool hasMalformed(Link<DartType> types) {
  for (DartType typeArgument in types) {
    if (typeArgument.isMalformed) {
      return true;
    }
  }
  return false;
}

// TODO(johnniwinther): Add common supertype for InterfaceType and TypedefType.
class InterfaceType extends DartType {
  final ClassElement element;
  final Link<DartType> typeArguments;
  final bool isMalformed;

  InterfaceType(this.element,
                [Link<DartType> typeArguments = const Link<DartType>()])
      : this.typeArguments = typeArguments,
        this.isMalformed = hasMalformed(typeArguments) {
    assert(invariant(element, element.isDeclaration));
  }

  TypeKind get kind => TypeKind.INTERFACE;

  SourceString get name => element.name;

  DartType subst(Link<DartType> arguments, Link<DartType> parameters) {
    if (typeArguments.isEmpty) {
      // Return fast on non-generic types.
      return this;
    }
    if (parameters.isEmpty) {
      assert(arguments.isEmpty);
      // Return fast on empty substitutions.
      return this;
    }
    Link<DartType> newTypeArguments =
        Types.substTypes(typeArguments, arguments, parameters);
    if (!identical(typeArguments, newTypeArguments)) {
      // Create a new type only if necessary.
      return new InterfaceType(element, newTypeArguments);
    }
    return this;
  }

  bool forEachMalformedType(bool f(MalformedType type)) {
    for (DartType typeArgument in typeArguments) {
      if (!typeArgument.forEachMalformedType(f)) {
        return false;
      }
    }
    return true;
  }

  /**
   * Returns the type as an instance of class [other], if possible, null
   * otherwise.
   */
  DartType asInstanceOf(ClassElement other) {
    if (element == other) return this;
    for (InterfaceType supertype in element.allSupertypes) {
      ClassElement superclass = supertype.element;
      if (superclass == other) {
        Link<DartType> arguments = Types.substTypes(supertype.typeArguments,
                                                    typeArguments,
                                                    element.typeVariables);
        return new InterfaceType(superclass, arguments);
      }
    }
    return null;
  }

  DartType unalias(Compiler compiler) => this;

  String toString() {
    StringBuffer sb = new StringBuffer();
    sb.add(name.slowToString());
    if (!isRaw) {
      sb.add('<');
      typeArguments.printOn(sb, ', ');
      sb.add('>');
    }
    return sb.toString();
  }

  int get hashCode {
    int hash = element.hashCode;
    for (Link<DartType> arguments = this.typeArguments;
         !arguments.isEmpty;
         arguments = arguments.tail) {
      int argumentHash = arguments.head != null ? arguments.head.hashCode : 0;
      hash = 17 * hash + 3 * argumentHash;
    }
    return hash;
  }

  bool operator ==(other) {
    if (other is !InterfaceType) return false;
    if (!identical(element, other.element)) return false;
    return typeArguments == other.typeArguments;
  }

  bool get isRaw => typeArguments.isEmpty || identical(this, element.rawType);

  InterfaceType asRaw() => element.rawType;
}

class FunctionType extends DartType {
  final Element element;
  DartType returnType;
  Link<DartType> parameterTypes;
  final bool isMalformed;

  FunctionType(DartType returnType, Link<DartType> parameterTypes,
               Element this.element)
      : this.returnType = returnType,
        this.parameterTypes = parameterTypes,
        this.isMalformed = returnType != null && returnType.isMalformed ||
            hasMalformed(parameterTypes) {
    assert(element == null || invariant(element, element.isDeclaration));
  }

  TypeKind get kind => TypeKind.FUNCTION;

  DartType subst(Link<DartType> arguments, Link<DartType> parameters) {
    if (parameters.isEmpty) {
      assert(arguments.isEmpty);
      // Return fast on empty substitutions.
      return this;
    }
    var newReturnType = returnType.subst(arguments, parameters);
    bool changed = !identical(newReturnType, returnType);
    var newParameterTypes = Types.substTypes(parameterTypes, arguments,
                                             parameters);
    if (!changed && !identical(parameterTypes, newParameterTypes)) {
      changed = true;
    }
    if (changed) {
      // Create a new type only if necessary.
      return new FunctionType(newReturnType, newParameterTypes, element);
    }
    return this;
  }

  bool forEachMalformedType(bool f(MalformedType type)) {
    if (!returnType.forEachMalformedType(f)) {
      return false;
    }
    for (DartType parameterType in parameterTypes) {
      if (!parameterType.forEachMalformedType(f)) {
        return false;
      }
    }
    return true;
  }

  DartType unalias(Compiler compiler) => this;

  String toString() {
    StringBuffer sb = new StringBuffer();
    bool first = true;
    sb.add('(');
    parameterTypes.printOn(sb, ', ');
    sb.add(') -> ${returnType}');
    return sb.toString();
  }

  SourceString get name => const SourceString('Function');

  int computeArity() {
    int arity = 0;
    parameterTypes.forEach((_) { arity++; });
    return arity;
  }

  void initializeFrom(FunctionType other) {
    assert(returnType == null);
    assert(parameterTypes == null);
    returnType = other.returnType;
    parameterTypes = other.parameterTypes;
  }

  int get hashCode {
    int hash = 17 * element.hashCode + 3 * returnType.hashCode;
    for (Link<DartType> parameters = parameterTypes;
         !parameters.isEmpty;
        parameters = parameters.tail) {
      hash = 17 * hash + 3 * parameters.head.hashCode;
    }
    return hash;
  }

  bool operator ==(other) {
    if (other is !FunctionType) return false;
    return returnType == other.returnType
           && parameterTypes == other.parameterTypes;
  }
}

class TypedefType extends DartType {
  final TypedefElement element;
  final Link<DartType> typeArguments;
  final bool isMalformed;

  TypedefType(this.element,
              [Link<DartType> typeArguments = const Link<DartType>()])
      : this.typeArguments = typeArguments,
        this.isMalformed = hasMalformed(typeArguments);

  TypeKind get kind => TypeKind.TYPEDEF;

  SourceString get name => element.name;

  DartType subst(Link<DartType> arguments, Link<DartType> parameters) {
    if (typeArguments.isEmpty) {
      // Return fast on non-generic typedefs.
      return this;
    }
    if (parameters.isEmpty) {
      assert(arguments.isEmpty);
      // Return fast on empty substitutions.
      return this;
    }
    Link<DartType> newTypeArguments = Types.substTypes(typeArguments, arguments,
                                                       parameters);
    if (!identical(typeArguments, newTypeArguments)) {
      // Create a new type only if necessary.
      return new TypedefType(element, newTypeArguments);
    }
    return this;
  }

  bool forEachMalformedType(bool f(MalformedType type)) {
    for (DartType typeArgument in typeArguments) {
      if (!typeArgument.forEachMalformedType(f)) {
        return false;
      }
    }
    return true;
  }

  DartType unalias(Compiler compiler) {
    // TODO(ahe): This should be [ensureResolved].
    compiler.resolveTypedef(element);
    // TODO(johnniwinther): Perform substitution on the unaliased type.
    return element.alias.unalias(compiler);
  }

  String toString() {
    StringBuffer sb = new StringBuffer();
    sb.add(name.slowToString());
    if (!isRaw) {
      sb.add('<');
      typeArguments.printOn(sb, ', ');
      sb.add('>');
    }
    return sb.toString();
  }

  int get hashCode => 17 * element.hashCode;

  bool operator ==(other) {
    if (other is !TypedefType) return false;
    if (!identical(element, other.element)) return false;
    return typeArguments == other.typeArguments;
  }

  bool get isRaw => typeArguments.isEmpty || identical(this, element.rawType);

  TypedefType asRaw() => element.rawType;
}

/**
 * Special type to hold the [dynamic] type. Used for correctly returning
 * 'dynamic' on [toString].
 */
class DynamicType extends InterfaceType {
  DynamicType(ClassElement element) : super(element);

  SourceString get name => const SourceString('dynamic');
}

class Types {
  final Compiler compiler;
  // TODO(karlklose): should we have a class Void?
  final VoidType voidType;
  final DynamicType dynamicType;

  factory Types(Compiler compiler, ClassElement dynamicElement) {
    LibraryElement library = new LibraryElementX(new Script(null, null));
    VoidType voidType = new VoidType(new VoidElementX(library));
    DynamicType dynamicType = new DynamicType(dynamicElement);
    dynamicElement.rawType = dynamicElement.thisType = dynamicType;
    return new Types.internal(compiler, voidType, dynamicType);
  }

  Types.internal(this.compiler, this.voidType, this.dynamicType);

  /** Returns true if t is a subtype of s */
  bool isSubtype(DartType t, DartType s) {
    if (identical(t, s) ||
        identical(t, dynamicType) ||
        identical(s, dynamicType) ||
        t.isMalformed ||
        s.isMalformed ||
        identical(s.element, compiler.objectClass) ||
        identical(t.element, compiler.nullClass)) {
      return true;
    }
    t = t.unalias(compiler);
    s = s.unalias(compiler);

    if (t is VoidType) {
      return false;
    } else if (t is InterfaceType) {
      if (s is !InterfaceType) return false;
      ClassElement tc = t.element;
      if (identical(tc, s.element)) return true;
      for (Link<DartType> supertypes = tc.allSupertypes;
           supertypes != null && !supertypes.isEmpty;
           supertypes = supertypes.tail) {
        DartType supertype = supertypes.head;
        if (identical(supertype.element, s.element)) return true;
      }
      return false;
    } else if (t is FunctionType) {
      if (identical(s.element, compiler.functionClass)) return true;
      if (s is !FunctionType) return false;
      FunctionType tf = t;
      FunctionType sf = s;
      Link<DartType> tps = tf.parameterTypes;
      Link<DartType> sps = sf.parameterTypes;
      while (!tps.isEmpty && !sps.isEmpty) {
        if (!isAssignable(tps.head, sps.head)) return false;
        tps = tps.tail;
        sps = sps.tail;
      }
      if (!tps.isEmpty || !sps.isEmpty) return false;
      if (!isAssignable(sf.returnType, tf.returnType)) return false;
      return true;
    } else if (t is TypeVariableType) {
      if (s is !TypeVariableType) return false;
      return (identical(t.element, s.element));
    } else {
      throw 'internal error: unknown type kind';
    }
  }

  bool isAssignable(DartType r, DartType s) {
    return isSubtype(r, s) || isSubtype(s, r);
  }


  /**
   * Helper method for performing substitution of a linked list of types.
   *
   * If no types are changed by the substitution, the [types] is returned
   * instead of a newly created linked list.
   */
  static Link<DartType> substTypes(Link<DartType> types,
                                   Link<DartType> arguments,
                                   Link<DartType> parameters) {
    bool changed = false;
    var builder = new LinkBuilder<DartType>();
    Link<DartType> typeLink = types;
    while (!typeLink.isEmpty) {
      var argument = typeLink.head.subst(arguments, parameters);
      if (!changed && !identical(argument, typeLink.head)) {
        changed = true;
      }
      builder.addLast(argument);
      typeLink = typeLink.tail;
    }
    if (changed) {
      // Create a new link only if necessary.
      return builder.toLink();
    }
    return types;
  }

  /**
   * Combine error messages in a malformed type to a single message string.
   */
  static String fetchReasonsFromMalformedType(DartType type) {
    // TODO(johnniwinther): Figure out how to produce good error message in face
    // of multiple errors, and how to ensure non-localized error messages.
    var reasons = new List<String>();
    type.forEachMalformedType((MalformedType malformedType) {
      ErroneousElement error = malformedType.element;
      Message message = error.messageKind.message(error.messageArguments);
      reasons.add(message.toString());
      return true;
    });
    return Strings.join(reasons, ', ');
  }
}

class CancelTypeCheckException {
  final Node node;
  final String reason;

  CancelTypeCheckException(this.node, this.reason);
}

class TypeCheckerVisitor implements Visitor<DartType> {
  final Compiler compiler;
  final TreeElements elements;
  final Types types;

  Node lastSeenNode;
  DartType expectedReturnType;
  ClassElement currentClass;

  Link<DartType> cascadeTypes = const Link<DartType>();

  DartType intType;
  DartType doubleType;
  DartType boolType;
  DartType stringType;
  DartType objectType;
  DartType listType;

  TypeCheckerVisitor(this.compiler, this.elements, this.types) {
    intType = compiler.intClass.computeType(compiler);
    doubleType = compiler.doubleClass.computeType(compiler);
    boolType = compiler.boolClass.computeType(compiler);
    stringType = compiler.stringClass.computeType(compiler);
    objectType = compiler.objectClass.computeType(compiler);
    listType = compiler.listClass.computeType(compiler);
  }

  DartType fail(node, [reason]) {
    String message = 'cannot type-check';
    if (reason != null) {
      message = '$message: $reason';
    }
    throw new CancelTypeCheckException(node, message);
  }

  reportTypeWarning(Node node, MessageKind kind, [List arguments = const []]) {
    compiler.reportWarning(node, new TypeWarning(kind, arguments));
  }

  // TODO(karlklose): remove these functions.
  DartType unhandledStatement() => StatementType.NOT_RETURNING;
  DartType unhandledExpression() => types.dynamicType;

  DartType analyzeNonVoid(Node node) {
    DartType type = analyze(node);
    if (type == types.voidType) {
      reportTypeWarning(node, MessageKind.VOID_EXPRESSION);
    }
    return type;
  }

  DartType analyzeWithDefault(Node node, DartType defaultValue) {
    return node != null ? analyze(node) : defaultValue;
  }

  DartType analyze(Node node) {
    if (node == null) {
      final String error = 'internal error: unexpected node: null';
      if (lastSeenNode != null) {
        fail(null, error);
      } else {
        compiler.cancel(error);
      }
    } else {
      lastSeenNode = node;
    }
    DartType result = node.accept(this);
    // TODO(karlklose): record type?
    if (result == null) {
      fail(node, 'internal error: type is null');
    }
    return result;
  }

  /**
   * Check if a value of type t can be assigned to a variable,
   * parameter or return value of type s.
   */
  checkAssignable(Node node, DartType s, DartType t) {
    if (!types.isAssignable(s, t)) {
      reportTypeWarning(node, MessageKind.NOT_ASSIGNABLE, [s, t]);
    }
  }

  checkCondition(Expression condition) {
    checkAssignable(condition, boolType, analyze(condition));
  }

  void pushCascadeType(DartType type) {
    cascadeTypes = cascadeTypes.prepend(type);
  }

  DartType popCascadeType() {
    DartType type = cascadeTypes.head;
    cascadeTypes = cascadeTypes.tail;
    return type;
  }

  DartType visitBlock(Block node) {
    return analyze(node.statements);
  }

  DartType visitCascade(Cascade node) {
    analyze(node.expression);
    return popCascadeType();
  }

  DartType visitCascadeReceiver(CascadeReceiver node) {
    DartType type = analyze(node.expression);
    pushCascadeType(type);
    return type;
  }

  DartType visitClassNode(ClassNode node) {
    fail(node);
  }

  DartType visitMixinApplication(MixinApplication node) {
    fail(node);
  }

  DartType visitNamedMixinApplication(NamedMixinApplication node) {
    fail(node);
  }

  DartType visitDoWhile(DoWhile node) {
    StatementType bodyType = analyze(node.body);
    checkCondition(node.condition);
    return bodyType.join(StatementType.NOT_RETURNING);
  }

  DartType visitExpressionStatement(ExpressionStatement node) {
    analyze(node.expression);
    return StatementType.NOT_RETURNING;
  }

  /** Dart Programming Language Specification: 11.5.1 For Loop */
  DartType visitFor(For node) {
    analyzeWithDefault(node.initializer, StatementType.NOT_RETURNING);
    checkCondition(node.condition);
    analyzeWithDefault(node.update, StatementType.NOT_RETURNING);
    StatementType bodyType = analyze(node.body);
    return bodyType.join(StatementType.NOT_RETURNING);
  }

  DartType visitFunctionDeclaration(FunctionDeclaration node) {
    analyze(node.function);
    return StatementType.NOT_RETURNING;
  }

  DartType visitFunctionExpression(FunctionExpression node) {
    DartType type;
    DartType returnType;
    DartType previousType;
    final FunctionElement element = elements[node];
    if (Elements.isUnresolved(element)) return types.dynamicType;
    if (identical(element.kind, ElementKind.GENERATIVE_CONSTRUCTOR) ||
        identical(element.kind, ElementKind.GENERATIVE_CONSTRUCTOR_BODY)) {
      type = types.dynamicType;
      returnType = types.voidType;
    } else {
      FunctionType functionType = computeType(element);
      returnType = functionType.returnType;
      type = functionType;
    }
    DartType previous = expectedReturnType;
    expectedReturnType = returnType;
    if (element.isMember()) currentClass = element.getEnclosingClass();
    StatementType bodyType = analyze(node.body);
    if (returnType != types.voidType && returnType != types.dynamicType
        && bodyType != StatementType.RETURNING) {
      MessageKind kind;
      if (bodyType == StatementType.MAYBE_RETURNING) {
        kind = MessageKind.MAYBE_MISSING_RETURN;
      } else {
        kind = MessageKind.MISSING_RETURN;
      }
      reportTypeWarning(node.name, kind);
    }
    expectedReturnType = previous;
    return type;
  }

  DartType visitIdentifier(Identifier node) {
    if (node.isThis()) {
      return currentClass.computeType(compiler);
    } else {
      // This is an identifier of a formal parameter.
      return types.dynamicType;
    }
  }

  DartType visitIf(If node) {
    checkCondition(node.condition);
    StatementType thenType = analyze(node.thenPart);
    StatementType elseType = node.hasElsePart ? analyze(node.elsePart)
                                              : StatementType.NOT_RETURNING;
    return thenType.join(elseType);
  }

  DartType visitLoop(Loop node) {
    return unhandledStatement();
  }

  DartType lookupMethodType(Node node, ClassElement classElement,
                            SourceString name) {
    Element member = classElement.lookupLocalMember(name);
    if (member == null) {
      classElement.ensureResolved(compiler);
      for (Link<DartType> supertypes = classElement.allSupertypes;
           !supertypes.isEmpty && member == null;
           supertypes = supertypes.tail) {
        ClassElement lookupTarget = supertypes.head.element;
        member = lookupTarget.lookupLocalMember(name);
      }
    }
    if (member != null && member.kind == ElementKind.FUNCTION) {
      return computeType(member);
    }
    reportTypeWarning(node, MessageKind.METHOD_NOT_FOUND,
                      [classElement.name, name]);
    return types.dynamicType;
  }

  void analyzeArguments(Send send, DartType type) {
    Link<Node> arguments = send.arguments;
    if (type == null || identical(type, types.dynamicType)) {
      while(!arguments.isEmpty) {
        analyze(arguments.head);
        arguments = arguments.tail;
      }
    } else {
      FunctionType funType = type;
      Link<DartType> parameterTypes = funType.parameterTypes;
      while (!arguments.isEmpty && !parameterTypes.isEmpty) {
        checkAssignable(arguments.head, parameterTypes.head,
                        analyze(arguments.head));
        arguments = arguments.tail;
        parameterTypes = parameterTypes.tail;
      }
      if (!arguments.isEmpty) {
        reportTypeWarning(arguments.head, MessageKind.ADDITIONAL_ARGUMENT);
      } else if (!parameterTypes.isEmpty) {
        reportTypeWarning(send, MessageKind.MISSING_ARGUMENT,
                          [parameterTypes.head]);
      }
    }
  }

  DartType visitSend(Send node) {
    Element element = elements[node];

    if (Elements.isClosureSend(node, element)) {
      // TODO(karlklose): Finish implementation.
      return types.dynamicType;
    }

    Identifier selector = node.selector.asIdentifier();
    String name = selector.source.stringValue;

    if (node.isOperator && identical(name, 'is')) {
      analyze(node.receiver);
      return boolType;
    } else if (node.isOperator) {
      final Node firstArgument = node.receiver;
      final DartType firstArgumentType = analyze(node.receiver);
      final arguments = node.arguments;
      final Node secondArgument = arguments.isEmpty ? null : arguments.head;
      final DartType secondArgumentType =
          analyzeWithDefault(secondArgument, null);

      if (identical(name, '+') || identical(name, '=') || identical(name, '-')
          || identical(name, '*') || identical(name, '/') || identical(name, '%')
          || identical(name, '~/') || identical(name, '|') || identical(name, '&')
          || identical(name, '^') || identical(name, '~')|| identical(name, '<<')
          || identical(name, '>>') || identical(name, '[]')) {
        return types.dynamicType;
      } else if (identical(name, '<') || identical(name, '>') || identical(name, '<=')
                 || identical(name, '>=') || identical(name, '==') || identical(name, '!=')
                 || identical(name, '===') || identical(name, '!==')) {
        return boolType;
      } else if (identical(name, '||') || identical(name, '&&') || identical(name, '!')) {
        checkAssignable(firstArgument, boolType, firstArgumentType);
        if (!arguments.isEmpty) {
          // TODO(karlklose): check number of arguments in validator.
          checkAssignable(secondArgument, boolType, secondArgumentType);
        }
        return boolType;
      }
      fail(selector, 'unexpected operator ${name}');

    } else if (node.isPropertyAccess) {
      if (node.receiver != null) {
        // TODO(karlklose): we cannot handle fields.
        return unhandledExpression();
      }
      if (element == null) return types.dynamicType;
      return computeType(element);

    } else if (node.isFunctionObjectInvocation) {
      fail(node.receiver, 'function object invocation unimplemented');

    } else {
      FunctionType computeFunType() {
        if (node.receiver != null) {
          DartType receiverType = analyze(node.receiver);
          if (receiverType.element == compiler.dynamicClass) return null;
          if (receiverType == null) {
            fail(node.receiver, 'receivertype is null');
          }
          if (identical(receiverType.element.kind, ElementKind.GETTER)) {
            FunctionType getterType  = receiverType;
            receiverType = getterType.returnType;
          }
          ElementKind receiverKind = receiverType.element.kind;
          if (identical(receiverKind, ElementKind.TYPEDEF)) {
            // TODO(karlklose): handle typedefs.
            return null;
          }
          if (identical(receiverKind, ElementKind.TYPE_VARIABLE)) {
            // TODO(karlklose): handle type variables.
            return null;
          }
          if (!identical(receiverKind, ElementKind.CLASS)) {
            fail(node.receiver, 'unexpected receiver kind: ${receiverKind}');
          }
          ClassElement classElement = receiverType.element;
          // TODO(karlklose): substitute type arguments.
          DartType memberType =
            lookupMethodType(selector, classElement, selector.source);
          if (identical(memberType.element, compiler.dynamicClass)) return null;
          return memberType;
        } else {
          if (Elements.isUnresolved(element)) {
            fail(node, 'unresolved ${node.selector}');
          } else if (identical(element.kind, ElementKind.FUNCTION)) {
            return computeType(element);
          } else if (element.isForeign(compiler)) {
            return null;
          } else if (identical(element.kind, ElementKind.VARIABLE)
                     || identical(element.kind, ElementKind.FIELD)) {
            // TODO(karlklose): handle object invocations.
            return null;
          } else {
            fail(node, 'unexpected element kind ${element.kind}');
          }
        }
      }
      FunctionType funType = computeFunType();
      analyzeArguments(node, funType);
      return (funType != null) ? funType.returnType : types.dynamicType;
    }
  }

  visitSendSet(SendSet node) {
    Identifier selector = node.selector;
    final name = node.assignmentOperator.source.stringValue;
    if (identical(name, '++') || identical(name, '--')) {
      final Element element = elements[node.selector];
      final DartType receiverType = computeType(element);
      // TODO(karlklose): this should be the return type instead of int.
      return node.isPrefix ? intType : receiverType;
    } else {
      DartType targetType = computeType(elements[node]);
      Node value = node.arguments.head;
      checkAssignable(value, targetType, analyze(value));
      return targetType;
    }
  }

  DartType visitLiteralInt(LiteralInt node) {
    return intType;
  }

  DartType visitLiteralDouble(LiteralDouble node) {
    return doubleType;
  }

  DartType visitLiteralBool(LiteralBool node) {
    return boolType;
  }

  DartType visitLiteralString(LiteralString node) {
    return stringType;
  }

  DartType visitStringJuxtaposition(StringJuxtaposition node) {
    analyze(node.first);
    analyze(node.second);
    return stringType;
  }

  DartType visitLiteralNull(LiteralNull node) {
    return types.dynamicType;
  }

  DartType visitNewExpression(NewExpression node) {
    Element element = elements[node.send];
    analyzeArguments(node.send, computeType(element));
    return analyze(node.send.selector);
  }

  DartType visitLiteralList(LiteralList node) {
    return listType;
  }

  DartType visitNodeList(NodeList node) {
    DartType type = StatementType.NOT_RETURNING;
    bool reportedDeadCode = false;
    for (Link<Node> link = node.nodes; !link.isEmpty; link = link.tail) {
      DartType nextType = analyze(link.head);
      if (type == StatementType.RETURNING) {
        if (!reportedDeadCode) {
          reportTypeWarning(link.head, MessageKind.UNREACHABLE_CODE);
          reportedDeadCode = true;
        }
      } else if (type == StatementType.MAYBE_RETURNING){
        if (nextType == StatementType.RETURNING) {
          type = nextType;
        }
      } else {
        type = nextType;
      }
    }
    return type;
  }

  DartType visitOperator(Operator node) {
    fail(node, 'internal error');
  }

  /** Dart Programming Language Specification: 11.10 Return */
  DartType visitReturn(Return node) {
    if (identical(node.getBeginToken().stringValue, 'native')) {
      return StatementType.RETURNING;
    }
    if (node.isRedirectingFactoryBody) {
      // TODO(lrn): Typecheck the body. It must refer to the constructor
      // of a subtype.
      return StatementType.RETURNING;
    }

    final expression = node.expression;
    final isVoidFunction = (identical(expectedReturnType, types.voidType));

    // Executing a return statement return e; [...] It is a static type warning
    // if the type of e may not be assigned to the declared return type of the
    // immediately enclosing function.
    if (expression != null) {
      final expressionType = analyze(expression);
      if (isVoidFunction
          && !types.isAssignable(expressionType, types.voidType)) {
        reportTypeWarning(expression, MessageKind.RETURN_VALUE_IN_VOID,
                          [expressionType]);
      } else {
        checkAssignable(expression, expectedReturnType, expressionType);
      }

    // Let f be the function immediately enclosing a return statement of the
    // form 'return;' It is a static warning if both of the following conditions
    // hold:
    // - f is not a generative constructor.
    // - The return type of f may not be assigned to void.
    } else if (!types.isAssignable(expectedReturnType, types.voidType)) {
      reportTypeWarning(node, MessageKind.RETURN_NOTHING, [expectedReturnType]);
    }
    return StatementType.RETURNING;
  }

  DartType visitThrow(Throw node) {
    if (node.expression != null) analyze(node.expression);
    return StatementType.RETURNING;
  }

  DartType computeType(Element element) {
    if (Elements.isUnresolved(element)) return types.dynamicType;
    DartType result = element.computeType(compiler);
    return (result != null) ? result : types.dynamicType;
  }

  DartType visitTypeAnnotation(TypeAnnotation node) {
    return elements.getType(node);
  }

  visitTypeVariable(TypeVariable node) {
    return types.dynamicType;
  }

  DartType visitVariableDefinitions(VariableDefinitions node) {
    DartType type = analyzeWithDefault(node.type, types.dynamicType);
    if (type == types.voidType) {
      reportTypeWarning(node.type, MessageKind.VOID_VARIABLE);
      type = types.dynamicType;
    }
    for (Link<Node> link = node.definitions.nodes; !link.isEmpty;
         link = link.tail) {
      Node initialization = link.head;
      compiler.ensure(initialization is Identifier
                      || initialization is Send);
      if (initialization is Send) {
        DartType initializer = analyzeNonVoid(link.head);
        checkAssignable(node, type, initializer);
      }
    }
    return StatementType.NOT_RETURNING;
  }

  DartType visitWhile(While node) {
    checkCondition(node.condition);
    StatementType bodyType = analyze(node.body);
    Expression cond = node.condition.asParenthesizedExpression().expression;
    if (cond.asLiteralBool() != null && cond.asLiteralBool().value == true) {
      // If the condition is a constant boolean expression denoting true,
      // control-flow always enters the loop body.
      // TODO(karlklose): this should be StatementType.RETURNING unless there
      // is a break in the loop body that has the loop or a label outside the
      // loop as a target.
      return bodyType;
    } else {
      return bodyType.join(StatementType.NOT_RETURNING);
    }
  }

  DartType visitParenthesizedExpression(ParenthesizedExpression node) {
    return analyze(node.expression);
  }

  DartType visitConditional(Conditional node) {
    checkCondition(node.condition);
    DartType thenType = analyzeNonVoid(node.thenExpression);
    DartType elseType = analyzeNonVoid(node.elseExpression);
    if (types.isSubtype(thenType, elseType)) {
      return thenType;
    } else if (types.isSubtype(elseType, thenType)) {
      return elseType;
    } else {
      return objectType;
    }
  }

  DartType visitModifiers(Modifiers node) {}

  visitStringInterpolation(StringInterpolation node) {
    node.visitChildren(this);
    return stringType;
  }

  visitStringInterpolationPart(StringInterpolationPart node) {
    node.visitChildren(this);
    return stringType;
  }

  visitEmptyStatement(EmptyStatement node) {
    return StatementType.NOT_RETURNING;
  }

  visitBreakStatement(BreakStatement node) {
    return StatementType.NOT_RETURNING;
  }

  visitContinueStatement(ContinueStatement node) {
    return StatementType.NOT_RETURNING;
  }

  visitForIn(ForIn node) {
    analyze(node.expression);
    StatementType bodyType = analyze(node.body);
    return bodyType.join(StatementType.NOT_RETURNING);
  }

  visitLabel(Label node) { }

  visitLabeledStatement(LabeledStatement node) {
    return node.statement.accept(this);
  }

  visitLiteralMap(LiteralMap node) {
    return unhandledExpression();
  }

  visitLiteralMapEntry(LiteralMapEntry node) {
    return unhandledExpression();
  }

  visitNamedArgument(NamedArgument node) {
    return unhandledExpression();
  }

  visitSwitchStatement(SwitchStatement node) {
    return unhandledStatement();
  }

  visitSwitchCase(SwitchCase node) {
    return unhandledStatement();
  }

  visitCaseMatch(CaseMatch node) {
    return unhandledStatement();
  }

  visitTryStatement(TryStatement node) {
    return unhandledStatement();
  }

  visitScriptTag(ScriptTag node) {
    return unhandledExpression();
  }

  visitCatchBlock(CatchBlock node) {
    return unhandledStatement();
  }

  visitTypedef(Typedef node) {
    return unhandledStatement();
  }

  DartType visitNode(Node node) {
    compiler.unimplemented('visitNode', node: node);
  }

  DartType visitCombinator(Combinator node) {
    compiler.unimplemented('visitNode', node: node);
  }

  DartType visitExport(Export node) {
    compiler.unimplemented('visitNode', node: node);
  }

  DartType visitExpression(Expression node) {
    compiler.unimplemented('visitNode', node: node);
  }

  DartType visitGotoStatement(GotoStatement node) {
    compiler.unimplemented('visitNode', node: node);
  }

  DartType visitImport(Import node) {
    compiler.unimplemented('visitNode', node: node);
  }

  DartType visitLibraryName(LibraryName node) {
    compiler.unimplemented('visitNode', node: node);
  }

  DartType visitLibraryTag(LibraryTag node) {
    compiler.unimplemented('visitNode', node: node);
  }

  DartType visitLiteral(Literal node) {
    compiler.unimplemented('visitNode', node: node);
  }

  DartType visitPart(Part node) {
    compiler.unimplemented('visitNode', node: node);
  }

  DartType visitPartOf(PartOf node) {
    compiler.unimplemented('visitNode', node: node);
  }

  DartType visitPostfix(Postfix node) {
    compiler.unimplemented('visitNode', node: node);
  }

  DartType visitPrefix(Prefix node) {
    compiler.unimplemented('visitNode', node: node);
  }

  DartType visitStatement(Statement node) {
    compiler.unimplemented('visitNode', node: node);
  }

  DartType visitStringNode(StringNode node) {
    compiler.unimplemented('visitNode', node: node);
  }

  DartType visitLibraryDependency(LibraryDependency node) {
    compiler.unimplemented('visitNode', node: node);
  }
}
