// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

part of ssa;

abstract class OptimizationPhase {
  String get name;
  void visitGraph(HGraph graph);
}

class SsaOptimizerTask extends CompilerTask {
  final JavaScriptBackend backend;
  SsaOptimizerTask(JavaScriptBackend backend)
    : this.backend = backend,
      super(backend.compiler);
  String get name => 'SSA optimizer';
  Compiler get compiler => backend.compiler;

  void runPhases(HGraph graph, List<OptimizationPhase> phases) {
    for (OptimizationPhase phase in phases) {
      runPhase(graph, phase);
    }
  }

  void runPhase(HGraph graph, OptimizationPhase phase) {
    phase.visitGraph(graph);
    compiler.tracer.traceGraph(phase.name, graph);
    assert(graph.isValid());
  }

  void optimize(CodegenWorkItem work, HGraph graph, bool speculative) {
    ConstantSystem constantSystem = compiler.backend.constantSystem;
    JavaScriptItemCompilationContext context = work.compilationContext;
    HTypeMap types = context.types;
    measure(() {
      List<OptimizationPhase> phases = <OptimizationPhase>[
          // Run trivial constant folding first to optimize
          // some patterns useful for type conversion.
          new SsaConstantFolder(constantSystem, backend, work, types),
          new SsaTypeConversionInserter(compiler),
          new SsaTypePropagator(compiler, types),
          new SsaConstantFolder(constantSystem, backend, work, types),
          // The constant folder affects the types of instructions, so
          // we run the type propagator again. Note that this would
          // not be necessary if types were directly stored on
          // instructions.
          new SsaTypePropagator(compiler, types),
          new SsaCheckInserter(backend, work, types, context.boundsChecked),
          new SsaRedundantPhiEliminator(),
          new SsaDeadPhiEliminator(),
          new SsaConstantFolder(constantSystem, backend, work, types),
          new SsaTypePropagator(compiler, types),
          new SsaReceiverSpecialization(compiler),
          new SsaGlobalValueNumberer(compiler, types),
          new SsaCodeMotion(),
          new SsaValueRangeAnalyzer(constantSystem, types, work),
          // Previous optimizations may have generated new
          // opportunities for constant folding.
          new SsaConstantFolder(constantSystem, backend, work, types),
          new SsaSimplifyInterceptors(constantSystem),
          new SsaDeadCodeEliminator(types)];
      runPhases(graph, phases);
      if (!speculative) {
        runPhase(graph, new SsaConstructionFieldTypes(backend, work, types));
      }
    });
  }

  bool trySpeculativeOptimizations(CodegenWorkItem work, HGraph graph) {
    if (work.element.isField()) {
      // Lazy initializers may not have bailout methods.
      return false;
    }
    JavaScriptItemCompilationContext context = work.compilationContext;
    HTypeMap types = context.types;
    return measure(() {
      // Run the phases that will generate type guards.
      List<OptimizationPhase> phases = <OptimizationPhase>[
          new SsaSpeculativeTypePropagator(compiler, types),
          new SsaTypeGuardInserter(compiler, work, types),
          new SsaEnvironmentBuilder(compiler),
          // Change the propagated types back to what they were before we
          // speculatively propagated, so that we can generate the bailout
          // version.
          // Note that we do this even if there were no guards inserted. If a
          // guard is not beneficial enough we don't emit one, but there might
          // still be speculative types on the instructions.
          new SsaTypePropagator(compiler, types),
          // Then run the [SsaCheckInserter] because the type propagator also
          // propagated types non-speculatively. For example, it might have
          // propagated the type array for a call to the List constructor.
          new SsaCheckInserter(backend, work, types, context.boundsChecked)];
      runPhases(graph, phases);
      return !work.guards.isEmpty;
    });
  }

  void prepareForSpeculativeOptimizations(CodegenWorkItem work, HGraph graph) {
    JavaScriptItemCompilationContext context = work.compilationContext;
    HTypeMap types = context.types;
    measure(() {
      // In order to generate correct code for the bailout version, we did not
      // propagate types from the instruction to the type guard. We do it
      // now to be able to optimize further.
      work.guards.forEach((HTypeGuard guard) {
        guard.bailoutTarget.isEnabled = false;
        guard.isEnabled = true;
      });
      // We also need to insert range and integer checks for the type
      // guards. Now that they claim to have a certain type, some
      // depending instructions might become builtin (like native array
      // accesses) and need to be checked.
      // Also run the type propagator, to please the codegen in case
      // no other optimization is run.
      runPhases(graph, <OptimizationPhase>[
          new SsaCheckInserter(backend, work, types, context.boundsChecked),
          new SsaTypePropagator(compiler, types)]);
    });
  }
}

/**
 * If both inputs to known operations are available execute the operation at
 * compile-time.
 */
class SsaConstantFolder extends HBaseVisitor implements OptimizationPhase {
  final String name = "SsaConstantFolder";
  final JavaScriptBackend backend;
  final CodegenWorkItem work;
  final ConstantSystem constantSystem;
  final HTypeMap types;
  HGraph graph;
  Compiler get compiler => backend.compiler;

  SsaConstantFolder(this.constantSystem, this.backend, this.work, this.types);

  void visitGraph(HGraph visitee) {
    graph = visitee;
    visitDominatorTree(visitee);
  }

  visitBasicBlock(HBasicBlock block) {
    HInstruction instruction = block.first;
    while (instruction != null) {
      HInstruction next = instruction.next;
      HInstruction replacement = instruction.accept(this);
      if (!identical(replacement, instruction)) {
        if (!replacement.isInBasicBlock()) {
          // The constant folding can return an instruction that is already
          // part of the graph (like an input), so we only add the replacement
          // if necessary.
          block.addAfter(instruction, replacement);
        }
        block.rewrite(instruction, replacement);
        block.remove(instruction);

        // If we can replace [instruction] with [replacement], then
        // [replacement]'s type can be narrowed.
        types[replacement] =
            types[replacement].intersection(types[instruction], compiler);

        // If the replacement instruction does not know its
        // source element, use the source element of the
        // instruction.
        if (replacement.sourceElement == null) {
          replacement.sourceElement = instruction.sourceElement;
        }
        if (replacement.sourcePosition == null) {
          replacement.sourcePosition = instruction.sourcePosition;
        }
      }
      instruction = next;
    }
  }

  HInstruction visitInstruction(HInstruction node) {
    return node;
  }

  HInstruction visitBoolify(HBoolify node) {
    List<HInstruction> inputs = node.inputs;
    assert(inputs.length == 1);
    HInstruction input = inputs[0];
    if (input.isBoolean(types)) return input;
    // All values !== true are boolified to false.
    DartType type = types[input].computeType(compiler);
    if (type != null && !identical(type.element, compiler.boolClass)) {
      return graph.addConstantBool(false, constantSystem);
    }
    return node;
  }

  HInstruction visitNot(HNot node) {
    List<HInstruction> inputs = node.inputs;
    assert(inputs.length == 1);
    HInstruction input = inputs[0];
    if (input is HConstant) {
      HConstant constant = input;
      bool isTrue = constant.constant.isTrue();
      return graph.addConstantBool(!isTrue, constantSystem);
    } else if (input is HNot) {
      return input.inputs[0];
    }
    return node;
  }

  HInstruction visitInvokeUnary(HInvokeUnary node) {
    HInstruction folded =
        foldUnary(node.operation(constantSystem), node.operand);
    return folded != null ? folded : node;
  }

  HInstruction foldUnary(UnaryOperation operation, HInstruction operand) {
    if (operand is HConstant) {
      HConstant receiver = operand;
      Constant folded = operation.fold(receiver.constant);
      if (folded != null) return graph.addConstant(folded);
    }
    return null;
  }

  HInstruction handleInterceptorCall(HInvokeDynamic node) {
    // We only optimize for intercepted method calls in this method.
    if (node.selector.isGetter() || node.selector.isSetter()) return node;

    HInstruction input = node.inputs[1];
    if (input.isString(types)
        && node.selector.name == const SourceString('toString')) {
      return node.inputs[1];
    }

    // Try constant folding the instruction.
    Operation operation = node.specializer.operation(constantSystem);
    if (operation != null) {
      HInstruction instruction = node.inputs.length == 2
          ? foldUnary(operation, node.inputs[1])
          : foldBinary(operation, node.inputs[1], node.inputs[2]);
      if (instruction != null) return instruction;
    }

    // Try converting the instruction to a builtin instruction.
    HInstruction instruction =
        node.specializer.tryConvertToBuiltin(node, types);
    if (instruction != null) return instruction;

    // Check if this call does not need to be intercepted.
    HType type = types[input];
    var interceptor = node.inputs[0];
    if (interceptor is !HThis && !type.canBePrimitive()) {
      // If the type can be null, and the intercepted method can be in
      // the object class, keep the interceptor.
      if (type.canBeNull()) {
        Set<ClassElement> interceptedClasses;
        if (interceptor is HInterceptor) {
          interceptedClasses = interceptor.interceptedClasses;
        } else if (node is HOneShotInterceptor) {
          var oneShotInterceptor = node;
          interceptedClasses = oneShotInterceptor.interceptedClasses;
        }
        if (interceptedClasses.contains(compiler.objectClass)) return node;
      }
      // Change the call to a regular invoke dynamic call.
      return new HInvokeDynamicMethod(
          node.selector, node.inputs.getRange(1, node.inputs.length - 1));
    }

    Selector selector = node.selector;
    SourceString selectorName = selector.name;
    Element target;
    if (input.isExtendableArray(types)) {
      if (selectorName == backend.jsArrayRemoveLast.name
          && selector.argumentCount == 0) {
        target = backend.jsArrayRemoveLast;
      } else if (selectorName == backend.jsArrayAdd.name
                 && selector.argumentCount == 1
                 && selector.namedArgumentCount == 0
                 && !compiler.enableTypeAssertions) {
        target = backend.jsArrayAdd;
      }
    } else if (input.isString(types)) {
      if (selectorName == backend.jsStringSplit.name
          && selector.argumentCount == 1
          && selector.namedArgumentCount == 0
          && node.inputs[2].isString(types)) {
        target = backend.jsStringSplit;
      } else if (selectorName == backend.jsStringConcat.name
                 && selector.argumentCount == 1
                 && selector.namedArgumentCount == 0
                 && node.inputs[2].isString(types)) {
        target = backend.jsStringConcat;
      }
    }
    if (target != null) {
      HInvokeDynamicMethod result = new HInvokeDynamicMethod(
          node.selector, node.inputs.getRange(1, node.inputs.length - 1));
      result.element = target;
      return result;
    }
    return node;
  }

  bool isFixedSizeListConstructor(HInvokeStatic node) {
    Element element = node.target.element;
    if (backend.fixedLengthListConstructor == null) {
      backend.fixedLengthListConstructor =
        compiler.listClass.lookupConstructor(
            new Selector.callConstructor(const SourceString("fixedLength"),
                                         compiler.listClass.getLibrary()));
    }
    // TODO(ngeoffray): checking if the second input is an integer
    // should not be necessary but it currently makes it easier for
    // other optimizations to reason on a fixed length constructor
    // that we know takes an int.
    return element == backend.fixedLengthListConstructor
        && node.inputs[1].isInteger(types);
  }

  HInstruction visitInvokeStatic(HInvokeStatic node) {
    if (isFixedSizeListConstructor(node)) {
      node.guaranteedType = HType.FIXED_ARRAY;
    }
    return node;
  }

  HInstruction visitInvokeDynamicMethod(HInvokeDynamicMethod node) {
    if (node.isInterceptorCall) return handleInterceptorCall(node);
    HType receiverType = types[node.receiver];
    if (receiverType.isExact()) {
      HBoundedType type = receiverType;
      Element element = type.lookupMember(node.selector.name);
      // TODO(ngeoffray): Also fold if it's a getter or variable.
      if (element != null && element.isFunction()) {
        if (node.selector.applies(element, compiler)) {
          FunctionElement method = element;
          FunctionSignature parameters = method.computeSignature(compiler);
          if (parameters.optionalParameterCount == 0) {
            node.element = element;
          }
          // TODO(ngeoffray): If the method has optional parameters,
          // we should pass the default values here.
        }
      }
    }
    return node;
  }

  /**
   * Turns a primitive instruction (e.g. [HIndex], [HAdd], ...) into a
   * [HInvokeDynamic] because we know the receiver is not a JS
   * primitive object.
   */
  HInstruction fromPrimitiveInstructionToDynamicInvocation(HInstruction node,
                                                           Selector selector) {
    HBoundedType type = types[node.inputs[1]];
    HInvokeDynamicMethod result = new HInvokeDynamicMethod(
        selector,
        node.inputs.getRange(1, node.inputs.length - 1));
    if (type.isExact()) {
      HBoundedType concrete = type;
      // TODO(johnniwinther): Add lookup by selector to HBoundedType.
      Element element = concrete.lookupMember(selector.name);
      if (selector.applies(element, compiler)) {
        // The target is only valid if the selector applies.
        result.element = element;
      }
    }
    return result;
  }

  HInstruction visitIntegerCheck(HIntegerCheck node) {
    HInstruction value = node.value;
    if (value.isInteger(types)) return value;
    if (value.isConstant()) {
      HConstant constantInstruction = value;
      assert(!constantInstruction.constant.isInt());
      if (!constantSystem.isInt(constantInstruction.constant)) {
        // -0.0 is a double but will pass the runtime integer check.
        node.alwaysFalse = true;
      }
    }
    return node;
  }

  HInstruction foldBinary(BinaryOperation operation,
                          HInstruction left,
                          HInstruction right) {
    if (left is HConstant && right is HConstant) {
      HConstant op1 = left;
      HConstant op2 = right;
      Constant folded = operation.fold(op1.constant, op2.constant);
      if (folded != null) return graph.addConstant(folded);
    }
    return null;
  }

  HInstruction visitInvokeBinary(HInvokeBinary node) {
    HInstruction left = node.left;
    HInstruction right = node.right;
    BinaryOperation operation = node.operation(constantSystem);
    HConstant folded = foldBinary(operation, left, right);
    if (folded != null) return folded;
    return node;
  }

  bool allUsersAreBoolifies(HInstruction instruction) {
    List<HInstruction> users = instruction.usedBy;
    int length = users.length;
    for (int i = 0; i < length; i++) {
      if (users[i] is! HBoolify) return false;
    }
    return true;
  }

  HInstruction visitRelational(HRelational node) {
    if (allUsersAreBoolifies(node)) {
      // TODO(ngeoffray): Call a boolified selector.
      // This node stays the same, but the Boolify node will go away.
    }
    // Note that we still have to call [super] to make sure that we end up
    // in the remaining optimizations.
    return super.visitRelational(node);
  }

  HInstruction handleIdentityCheck(HRelational node) {
    HInstruction left = node.left;
    HInstruction right = node.right;
    HType leftType = types[left];
    HType rightType = types[right];

    // We don't optimize on numbers to preserve the runtime semantics.
    if (!(left.isNumberOrNull(types) && right.isNumberOrNull(types)) &&
        leftType.intersection(rightType, compiler).isConflicting()) {
      return graph.addConstantBool(false, constantSystem);
    }

    if (left.isConstantBoolean() && right.isBoolean(types)) {
      HConstant constant = left;
      if (constant.constant.isTrue()) {
        return right;
      } else {
        return new HNot(right);
      }
    }

    if (right.isConstantBoolean() && left.isBoolean(types)) {
      HConstant constant = right;
      if (constant.constant.isTrue()) {
        return left;
      } else {
        return new HNot(left);
      }
    }

    return null;
  }

  HInstruction visitIdentity(HIdentity node) {
    HInstruction newInstruction = handleIdentityCheck(node);
    return newInstruction == null ? super.visitIdentity(node) : newInstruction;
  }

  HInstruction visitTypeGuard(HTypeGuard node) {
    HInstruction value = node.guarded;
    // If the intersection of the types is still the incoming type then
    // the incoming type was a subtype of the guarded type, and no check
    // is required.
    HType combinedType = types[value].intersection(node.guardedType, compiler);
    return (combinedType == types[value]) ? value : node;
  }

  HInstruction visitIs(HIs node) {
    DartType type = node.typeExpression;
    Element element = type.element;
    if (element.isTypeVariable()) {
      compiler.unimplemented("visitIs for type variables");
    } if (element.isTypedef()) {
      return node;
    }

    HType expressionType = types[node.expression];
    if (identical(element, compiler.objectClass)
        || identical(element, compiler.dynamicClass)) {
      return graph.addConstantBool(true, constantSystem);
    } else if (expressionType.isInteger()) {
      if (identical(element, compiler.intClass)
          || identical(element, compiler.numClass)
          || Elements.isNumberOrStringSupertype(element, compiler)) {
        return graph.addConstantBool(true, constantSystem);
      } else if (identical(element, compiler.doubleClass)) {
        // We let the JS semantics decide for that check. Currently
        // the code we emit will always return true.
        return node;
      } else {
        return graph.addConstantBool(false, constantSystem);
      }
    } else if (expressionType.isDouble()) {
      if (identical(element, compiler.doubleClass)
          || identical(element, compiler.numClass)
          || Elements.isNumberOrStringSupertype(element, compiler)) {
        return graph.addConstantBool(true, constantSystem);
      } else if (identical(element, compiler.intClass)) {
        // We let the JS semantics decide for that check. Currently
        // the code we emit will return true for a double that can be
        // represented as a 31-bit integer and for -0.0.
        return node;
      } else {
        return graph.addConstantBool(false, constantSystem);
      }
    } else if (expressionType.isNumber()) {
      if (identical(element, compiler.numClass)) {
        return graph.addConstantBool(true, constantSystem);
      }
      // We cannot just return false, because the expression may be of
      // type int or double.
    } else if (expressionType.isString()) {
      if (identical(element, compiler.stringClass)
               || Elements.isStringOnlySupertype(element, compiler)
               || Elements.isNumberOrStringSupertype(element, compiler)) {
        return graph.addConstantBool(true, constantSystem);
      } else {
        return graph.addConstantBool(false, constantSystem);
      }
    } else if (expressionType.isArray()) {
      if (identical(element, compiler.listClass)
          || Elements.isListSupertype(element, compiler)) {
        return graph.addConstantBool(true, constantSystem);
      } else {
        return graph.addConstantBool(false, constantSystem);
      }
    // TODO(karlklose): remove the hasTypeArguments check.
    } else if (expressionType.isUseful()
               && !expressionType.canBeNull()
               && !RuntimeTypeInformation.hasTypeArguments(type)) {
      DartType receiverType = expressionType.computeType(compiler);
      if (receiverType != null) {
        if (!receiverType.isMalformed &&
            !type.isMalformed &&
            compiler.types.isSubtype(receiverType, type)) {
          return graph.addConstantBool(true, constantSystem);
        } else if (expressionType.isExact()) {
          return graph.addConstantBool(false, constantSystem);
        }
      }
    }
    return node;
  }

  HInstruction visitTypeConversion(HTypeConversion node) {
    HInstruction value = node.inputs[0];
    DartType type = types[node].computeType(compiler);
    if (identical(type.element, compiler.dynamicClass)
        || identical(type.element, compiler.objectClass)) {
      return value;
    }
    if (types[value].canBeNull() && node.isBooleanConversionCheck) {
      return node;
    }
    HType combinedType = types[value].intersection(types[node], compiler);
    return (combinedType == types[value]) ? value : node;
  }

  Element findConcreteFieldForDynamicAccess(HInstruction receiver,
                                            Selector selector) {
    HType receiverType = types[receiver];
    if (!receiverType.isUseful()) return null;
    if (receiverType.canBeNull()) return null;
    DartType type = receiverType.computeType(compiler);
    if (type == null) return null;
    return compiler.world.locateSingleField(type, selector);
  }

  HInstruction visitFieldGet(HFieldGet node) {
    if (node.element == backend.jsArrayLength) {
      if (node.receiver is HInvokeStatic) {
        // Try to recognize the length getter with input
        // [:new List.fixedLength(int):].
        HInvokeStatic call = node.receiver;
        if (isFixedSizeListConstructor(call)) {
          return call.inputs[1];
        }
      }
    }
    return node;
  }

  HInstruction optimizeLengthInterceptedCall(HInvokeDynamicGetter node) {
    HInstruction actualReceiver = node.inputs[1];
    if (actualReceiver.isIndexablePrimitive(types)) {
      if (actualReceiver.isConstantString()) {
        HConstant constantInput = actualReceiver;
        StringConstant constant = constantInput.constant;
        return graph.addConstantInt(constant.length, constantSystem);
      } else if (actualReceiver.isConstantList()) {
        HConstant constantInput = actualReceiver;
        ListConstant constant = constantInput.constant;
        return graph.addConstantInt(constant.length, constantSystem);
      }
      Element element;
      bool isAssignable;
      if (actualReceiver.isString(types)) {
        element = backend.jsStringLength;
        isAssignable = false;
      } else {
        element = backend.jsArrayLength;
        isAssignable = !actualReceiver.isFixedArray(types);
      }
      HFieldGet result = new HFieldGet(
          element, actualReceiver, isAssignable: isAssignable);
      result.guaranteedType = HType.INTEGER;
      types[result] = HType.INTEGER;
      return result;
    } else if (actualReceiver.isConstantMap()) {
      HConstant constantInput = actualReceiver;
      MapConstant constant = constantInput.constant;
      return graph.addConstantInt(constant.length, constantSystem);
    }
    return node;
  }

  HInstruction visitInvokeDynamicGetter(HInvokeDynamicGetter node) {
    if (node.selector.name == const SourceString('length')
        && node.isInterceptorCall) {
      return optimizeLengthInterceptedCall(node);
    }

    Element field =
        findConcreteFieldForDynamicAccess(node.receiver, node.selector);
    if (field == null) return node;

    Modifiers modifiers = field.modifiers;
    bool isFinalOrConst = modifiers.isFinal() || modifiers.isConst();
    if (!compiler.resolverWorld.hasInvokedSetter(field, compiler)) {
      // If no setter is ever used for this field it is only initialized in the
      // initializer list.
      isFinalOrConst = true;
    }
    HFieldGet result = new HFieldGet(
        field, node.inputs[0], isAssignable: !isFinalOrConst);
    HType type = backend.optimisticFieldType(field);
    if (type != null) {
      result.guaranteedType = type;
      backend.registerFieldTypesOptimization(
          work.element, field, result.guaranteedType);
    }
    return result;
  }

  HInstruction visitInvokeDynamicSetter(HInvokeDynamicSetter node) {
    Element field =
        findConcreteFieldForDynamicAccess(node.receiver, node.selector);
    if (field == null || !field.isAssignable()) return node;
    HInstruction value = node.inputs[1];
    if (compiler.enableTypeAssertions) {
      HInstruction other = value.convertType(
          compiler,
          field.computeType(compiler),
          HTypeConversion.CHECKED_MODE_CHECK);
      if (other != value) {
        node.block.addBefore(node, other);
        value = other;
      }
    }
    return new HFieldSet(field, node.inputs[0], value);
  }

  HInstruction visitStringConcat(HStringConcat node) {
    DartString folded = const LiteralDartString("");
    for (int i = 0; i < node.inputs.length; i++) {
      HInstruction part = node.inputs[i];
      if (!part.isConstant()) return node;
      HConstant constant = part;
      if (!constant.constant.isPrimitive()) return node;
      PrimitiveConstant primitive = constant.constant;
      folded = new DartString.concat(folded, primitive.toDartString());
    }
    return graph.addConstant(constantSystem.createString(folded, node.node));
  }

  HInstruction visitInterceptor(HInterceptor node) {
    if (node.isConstant()) return node;
    HInstruction constant = tryComputeConstantInterceptor(
        node.inputs[0], node.interceptedClasses);
    if (constant == null) return node;
    return constant;
  }

  HInstruction tryComputeConstantInterceptor(HInstruction input,
                                             Set<ClassElement> intercepted) {
    HType type = types[input];
    ClassElement constantInterceptor;
    if (type.isInteger()) {
      constantInterceptor = backend.jsIntClass;
    } else if (type.isDouble()) {
      constantInterceptor = backend.jsDoubleClass;
    } else if (type.isBoolean()) {
      constantInterceptor = backend.jsBoolClass;
    } else if (type.isString()) {
      constantInterceptor = backend.jsStringClass;
    } else if (type.isArray()) {
      constantInterceptor = backend.jsArrayClass;
    } else if (type.isNull()) {
      constantInterceptor = backend.jsIntClass;
    } else if (type.isNumber()) {
      // If the method being intercepted is not defined in [int] or
      // [double] we can safely use the number interceptor.
      if (!intercepted.contains(compiler.intClass)
          && !intercepted.contains(compiler.doubleClass)) {
        constantInterceptor = backend.jsNumberClass;
      }
    }

    if (constantInterceptor == null) return null;
    if (constantInterceptor == work.element.getEnclosingClass()) {
      return graph.thisInstruction;
    }

    Constant constant = new ConstructedConstant(
        constantInterceptor.computeType(compiler), <Constant>[]);
    return graph.addConstant(constant);
  }

  HInstruction visitOneShotInterceptor(HOneShotInterceptor node) {
    HInstruction newInstruction = handleInterceptorCall(node);
    if (newInstruction != node) return newInstruction;

    HInstruction constant = tryComputeConstantInterceptor(
        node.inputs[1], node.interceptedClasses);

    if (constant == null) return node;

    Selector selector = node.selector;
    // TODO(ngeoffray): make one shot interceptors know whether
    // they have side effects.
    if (selector.isGetter()) {
      HInstruction res = new HInvokeDynamicGetter(
          selector, node.element, constant, false);
      res.inputs.add(node.inputs[1]);
      return res;
    } else if (node.selector.isSetter()) {
      HInstruction res = new HInvokeDynamicSetter(
          selector, node.element, constant, node.inputs[1], false);
      res.inputs.add(node.inputs[2]);
      return res;
    } else {
      List<HInstruction> inputs = new List<HInstruction>.from(node.inputs);
      inputs[0] = constant;
      return new HInvokeDynamicMethod(selector, inputs, true);
    }
  }
}

class SsaCheckInserter extends HBaseVisitor implements OptimizationPhase {
  final HTypeMap types;
  final Set<HInstruction> boundsChecked;
  final CodegenWorkItem work;
  final JavaScriptBackend backend;
  final String name = "SsaCheckInserter";
  HGraph graph;

  SsaCheckInserter(this.backend,
                   this.work,
                   this.types,
                   this.boundsChecked);

  void visitGraph(HGraph graph) {
    this.graph = graph;
    visitDominatorTree(graph);
  }

  void visitBasicBlock(HBasicBlock block) {
    HInstruction instruction = block.first;
    while (instruction != null) {
      HInstruction next = instruction.next;
      instruction = instruction.accept(this);
      instruction = next;
    }
  }

  HBoundsCheck insertBoundsCheck(HInstruction node,
                                 HInstruction receiver,
                                 HInstruction index) {
    bool isAssignable = !receiver.isFixedArray(types);
    HFieldGet length = new HFieldGet(
        backend.jsArrayLength, receiver, isAssignable: isAssignable);
    length.guaranteedType = HType.INTEGER;
    types[length] = HType.INTEGER;
    node.block.addBefore(node, length);

    HBoundsCheck check = new HBoundsCheck(index, length);
    node.block.addBefore(node, check);
    boundsChecked.add(node);
    return check;
  }

  HIntegerCheck insertIntegerCheck(HInstruction node, HInstruction value) {
    HIntegerCheck check = new HIntegerCheck(value);
    node.block.addBefore(node, check);
    Set<HInstruction> dominatedUsers = value.dominatedUsers(node);
    for (HInstruction user in dominatedUsers) {
      user.changeUse(value, check);
    }
    return check;
  }

  void visitIndex(HIndex node) {
    if (boundsChecked.contains(node)) return;
    HInstruction index = node.index;
    if (!node.index.isInteger(types)) {
      index = insertIntegerCheck(node, index);
    }
    index = insertBoundsCheck(node, node.receiver, index);
    node.changeUse(node.index, index);
  }

  void visitIndexAssign(HIndexAssign node) {
    if (!node.receiver.isMutableArray(types)) return;
    if (boundsChecked.contains(node)) return;
    HInstruction index = node.index;
    if (!node.index.isInteger(types)) {
      index = insertIntegerCheck(node, index);
    }
    index = insertBoundsCheck(node, node.receiver, index);
    node.changeUse(node.index, index);
  }

  void visitInvokeDynamicMethod(HInvokeDynamicMethod node) {
    Element element = node.element;
    if (element != backend.jsArrayRemoveLast) return;
    if (boundsChecked.contains(node)) return;
    insertBoundsCheck(
        node, node.receiver, graph.addConstantInt(0, backend.constantSystem));
  }
}

class SsaDeadCodeEliminator extends HGraphVisitor implements OptimizationPhase {
  final HTypeMap types;
  final String name = "SsaDeadCodeEliminator";

  SsaDeadCodeEliminator(this.types);

  bool isDeadCode(HInstruction instruction) {
    return !instruction.hasSideEffects()
           && !instruction.canThrow()
           && instruction.usedBy.isEmpty
           && instruction is !HTypeGuard
           && instruction is !HParameterValue
           && instruction is !HLocalSet
           && !instruction.isControlFlow();
  }

  void visitGraph(HGraph graph) {
    visitPostDominatorTree(graph);
  }

  void visitBasicBlock(HBasicBlock block) {
    HInstruction instruction = block.last;
    while (instruction != null) {
      var previous = instruction.previous;
      if (isDeadCode(instruction)) block.remove(instruction);
      instruction = previous;
    }
  }
}

class SsaDeadPhiEliminator implements OptimizationPhase {
  final String name = "SsaDeadPhiEliminator";

  void visitGraph(HGraph graph) {
    final List<HPhi> worklist = <HPhi>[];
    // A set to keep track of the live phis that we found.
    final Set<HPhi> livePhis = new Set<HPhi>();

    // Add to the worklist all live phis: phis referenced by non-phi
    // instructions.
    for (final block in graph.blocks) {
      block.forEachPhi((HPhi phi) {
        for (final user in phi.usedBy) {
          if (user is !HPhi) {
            worklist.add(phi);
            livePhis.add(phi);
            break;
          }
        }
      });
    }

    // Process the worklist by propagating liveness to phi inputs.
    while (!worklist.isEmpty) {
      HPhi phi = worklist.removeLast();
      for (final input in phi.inputs) {
        if (input is HPhi && !livePhis.contains(input)) {
          worklist.add(input);
          livePhis.add(input);
        }
      }
    }

    // Remove phis that are not live.
    // Traverse in reverse order to remove phis with no uses before the
    // phis that they might use.
    // NOTICE: Doesn't handle circular references, but we don't currently
    // create any.
    List<HBasicBlock> blocks = graph.blocks;
    for (int i = blocks.length - 1; i >= 0; i--) {
      HBasicBlock block = blocks[i];
      HPhi current = block.phis.first;
      HPhi next = null;
      while (current != null) {
        next = current.next;
        if (!livePhis.contains(current)
            // TODO(ahe): Not sure the following is correct.
            && current.usedBy.isEmpty) {
          block.removePhi(current);
        }
        current = next;
      }
    }
  }
}

class SsaRedundantPhiEliminator implements OptimizationPhase {
  final String name = "SsaRedundantPhiEliminator";

  void visitGraph(HGraph graph) {
    final List<HPhi> worklist = <HPhi>[];

    // Add all phis in the worklist.
    for (final block in graph.blocks) {
      block.forEachPhi((HPhi phi) => worklist.add(phi));
    }

    while (!worklist.isEmpty) {
      HPhi phi = worklist.removeLast();

      // If the phi has already been processed, continue.
      if (!phi.isInBasicBlock()) continue;

      // Find if the inputs of the phi are the same instruction.
      // The builder ensures that phi.inputs[0] cannot be the phi
      // itself.
      assert(!identical(phi.inputs[0], phi));
      HInstruction candidate = phi.inputs[0];
      for (int i = 1; i < phi.inputs.length; i++) {
        HInstruction input = phi.inputs[i];
        // If the input is the phi, the phi is still candidate for
        // elimination.
        if (!identical(input, candidate) && !identical(input, phi)) {
          candidate = null;
          break;
        }
      }

      // If the inputs are not the same, continue.
      if (candidate == null) continue;

      // Because we're updating the users of this phi, we may have new
      // phis candidate for elimination. Add phis that used this phi
      // to the worklist.
      for (final user in phi.usedBy) {
        if (user is HPhi) worklist.add(user);
      }
      phi.block.rewrite(phi, candidate);
      phi.block.removePhi(phi);
    }
  }
}

class SsaGlobalValueNumberer implements OptimizationPhase {
  final String name = "SsaGlobalValueNumberer";
  final Compiler compiler;
  final HTypeMap types;
  final Set<int> visited;

  List<int> blockChangesFlags;
  List<int> loopChangesFlags;

  SsaGlobalValueNumberer(this.compiler, this.types) : visited = new Set<int>();

  void visitGraph(HGraph graph) {
    computeChangesFlags(graph);
    moveLoopInvariantCode(graph);
    visitBasicBlock(graph.entry, new ValueSet());
  }

  void moveLoopInvariantCode(HGraph graph) {
    for (int i = graph.blocks.length - 1; i >= 0; i--) {
      HBasicBlock block = graph.blocks[i];
      if (block.isLoopHeader()) {
        int changesFlags = loopChangesFlags[block.id];
        HLoopInformation info = block.loopInformation;
        // Iterate over all blocks of this loop. Note that blocks in
        // inner loops are not visited here, but we know they
        // were visited before because we are iterating in post-order.
        // So instructions that are GVN'ed in an inner loop are in their
        // loop entry, and [info.blocks] contains this loop entry.
        for (HBasicBlock other in info.blocks) {
          moveLoopInvariantCodeFromBlock(other, block, changesFlags);
        }
      }
    }
  }

  void moveLoopInvariantCodeFromBlock(HBasicBlock block,
                                      HBasicBlock loopHeader,
                                      int changesFlags) {
    assert(block.parentLoopHeader == loopHeader);
    HBasicBlock preheader = loopHeader.predecessors[0];
    int dependsFlags = HInstruction.computeDependsOnFlags(changesFlags);
    HInstruction instruction = block.first;
    while (instruction != null) {
      HInstruction next = instruction.next;
      if (instruction.useGvn()
          && (instruction is !HCheck)
          && (instruction.flags & dependsFlags) == 0) {
        bool loopInvariantInputs = true;
        List<HInstruction> inputs = instruction.inputs;
        for (int i = 0, length = inputs.length; i < length; i++) {
          if (isInputDefinedAfterDominator(inputs[i], preheader)) {
            loopInvariantInputs = false;
            break;
          }
        }

        // If the inputs are loop invariant, we can move the
        // instruction from the current block to the pre-header block.
        if (loopInvariantInputs) {
          block.detach(instruction);
          preheader.moveAtExit(instruction);
        }
      }
      int oldChangesFlags = changesFlags;
      changesFlags |= instruction.getChangesFlags();
      if (oldChangesFlags != changesFlags) {
        dependsFlags = HInstruction.computeDependsOnFlags(changesFlags);
      }
      instruction = next;
    }
  }

  bool isInputDefinedAfterDominator(HInstruction input,
                                    HBasicBlock dominator) {
    return input.block.id > dominator.id;
  }

  void visitBasicBlock(HBasicBlock block, ValueSet values) {
    HInstruction instruction = block.first;
    if (block.isLoopHeader()) {
      int flags = loopChangesFlags[block.id];
      values.kill(flags);
    }
    while (instruction != null) {
      HInstruction next = instruction.next;
      int flags = instruction.getChangesFlags();
      assert(flags == 0 || !instruction.useGvn());
      values.kill(flags);
      if (instruction.useGvn()) {
        HInstruction other = values.lookup(instruction);
        if (other != null) {
          assert(other.gvnEquals(instruction) && instruction.gvnEquals(other));
          block.rewriteWithBetterUser(instruction, other);
          block.remove(instruction);
        } else {
          values.add(instruction);
        }
      }
      instruction = next;
    }

    List<HBasicBlock> dominatedBlocks = block.dominatedBlocks;
    for (int i = 0, length = dominatedBlocks.length; i < length; i++) {
      HBasicBlock dominated = dominatedBlocks[i];
      // No need to copy the value set for the last child.
      ValueSet successorValues = (i == length - 1) ? values : values.copy();
      // If we have no values in our set, we do not have to kill
      // anything. Also, if the range of block ids from the current
      // block to the dominated block is empty, there is no blocks on
      // any path from the current block to the dominated block so we
      // don't have to do anything either.
      assert(block.id < dominated.id);
      if (!successorValues.isEmpty && block.id + 1 < dominated.id) {
        visited.clear();
        int changesFlags = getChangesFlagsForDominatedBlock(block, dominated);
        successorValues.kill(changesFlags);
      }
      visitBasicBlock(dominated, successorValues);
    }
  }

  void computeChangesFlags(HGraph graph) {
    // Create the changes flags lists. Make sure to initialize the
    // loop changes flags list to zero so we can use bitwise or when
    // propagating loop changes upwards.
    final int length = graph.blocks.length;
    blockChangesFlags = new List<int>.fixedLength(length);
    loopChangesFlags = new List<int>.fixedLength(length);
    for (int i = 0; i < length; i++) loopChangesFlags[i] = 0;

    // Run through all the basic blocks in the graph and fill in the
    // changes flags lists.
    for (int i = length - 1; i >= 0; i--) {
      final HBasicBlock block = graph.blocks[i];
      final int id = block.id;

      // Compute block changes flags for the block.
      int changesFlags = 0;
      HInstruction instruction = block.first;
      while (instruction != null) {
        changesFlags |= instruction.getChangesFlags();
        instruction = instruction.next;
      }
      assert(blockChangesFlags[id] == null);
      blockChangesFlags[id] = changesFlags;

      // Loop headers are part of their loop, so update the loop
      // changes flags accordingly.
      if (block.isLoopHeader()) {
        loopChangesFlags[id] |= changesFlags;
      }

      // Propagate loop changes flags upwards.
      HBasicBlock parentLoopHeader = block.parentLoopHeader;
      if (parentLoopHeader != null) {
        loopChangesFlags[parentLoopHeader.id] |= (block.isLoopHeader())
            ? loopChangesFlags[id]
            : changesFlags;
      }
    }
  }

  int getChangesFlagsForDominatedBlock(HBasicBlock dominator,
                                       HBasicBlock dominated) {
    int changesFlags = 0;
    List<HBasicBlock> predecessors = dominated.predecessors;
    for (int i = 0, length = predecessors.length; i < length; i++) {
      HBasicBlock block = predecessors[i];
      int id = block.id;
      // If the current predecessor block is on the path from the
      // dominator to the dominated, it must have an id that is in the
      // range from the dominator to the dominated.
      if (dominator.id < id && id < dominated.id && !visited.contains(id)) {
        visited.add(id);
        changesFlags |= blockChangesFlags[id];
        // Loop bodies might not be on the path from dominator to dominated,
        // but they can invalidate values.
        changesFlags |= loopChangesFlags[id];
        changesFlags |= getChangesFlagsForDominatedBlock(dominator, block);
      }
    }
    return changesFlags;
  }
}

// This phase merges equivalent instructions on different paths into
// one instruction in a dominator block. It runs through the graph
// post dominator order and computes a ValueSet for each block of
// instructions that can be moved to a dominator block. These
// instructions are the ones that:
// 1) can be used for GVN, and
// 2) do not use definitions of their own block.
//
// A basic block looks at its sucessors and finds the intersection of
// these computed ValueSet. It moves all instructions of the
// intersection into its own list of instructions.
class SsaCodeMotion extends HBaseVisitor implements OptimizationPhase {
  final String name = "SsaCodeMotion";

  List<ValueSet> values;

  void visitGraph(HGraph graph) {
    values = new List<ValueSet>.fixedLength(graph.blocks.length);
    for (int i = 0; i < graph.blocks.length; i++) {
      values[graph.blocks[i].id] = new ValueSet();
    }
    visitPostDominatorTree(graph);
  }

  void visitBasicBlock(HBasicBlock block) {
    List<HBasicBlock> successors = block.successors;

    // Phase 1: get the ValueSet of all successors (if there are more than one),
    // compute the intersection and move the instructions of the intersection
    // into this block.
    if (successors.length > 1) {
      ValueSet instructions = values[successors[0].id];
      for (int i = 1; i < successors.length; i++) {
        ValueSet other = values[successors[i].id];
        instructions = instructions.intersection(other);
      }

      if (!instructions.isEmpty) {
        List<HInstruction> list = instructions.toList();
        for (HInstruction instruction in list) {
          // Move the instruction to the current block.
          instruction.block.detach(instruction);
          block.moveAtExit(instruction);
          // Go through all successors and rewrite their instruction
          // to the shared one.
          for (final successor in successors) {
            HInstruction toRewrite = values[successor.id].lookup(instruction);
            if (toRewrite != instruction) {
              successor.rewriteWithBetterUser(toRewrite, instruction);
              successor.remove(toRewrite);
            }
          }
        }
      }
    }

    // Don't try to merge instructions to a dominator if we have
    // multiple predecessors.
    if (block.predecessors.length != 1) return;

    // Phase 2: Go through all instructions of this block and find
    // which instructions can be moved to a dominator block.
    ValueSet set_ = values[block.id];
    HInstruction instruction = block.first;
    int flags = 0;
    while (instruction != null) {
      int dependsFlags = HInstruction.computeDependsOnFlags(flags);
      flags |= instruction.getChangesFlags();

      HInstruction current = instruction;
      instruction = instruction.next;

      // TODO(ngeoffray): this check is needed because we currently do
      // not have flags to express 'Gvn'able', but not movable.
      if (current is HCheck) continue;
      if (!current.useGvn()) continue;
      if ((current.flags & dependsFlags) != 0) continue;

      bool canBeMoved = true;
      for (final HInstruction input in current.inputs) {
        if (input.block == block) {
          canBeMoved = false;
          break;
        }
      }
      if (!canBeMoved) continue;

      // This is safe because we are running after GVN.
      // TODO(ngeoffray): ensure GVN has been run.
      set_.add(current);
    }
  }
}

class SsaTypeConversionInserter extends HBaseVisitor
    implements OptimizationPhase {
  final String name = "SsaTypeconversionInserter";
  final Compiler compiler;

  SsaTypeConversionInserter(this.compiler);

  void visitGraph(HGraph graph) {
    visitDominatorTree(graph);
  }


  // Update users of [input] that are dominated by [:dominator.first:]
  // to use [newInput] instead.
  void changeUsesDominatedBy(HBasicBlock dominator,
                             HInstruction input,
                             HType convertedType) {
    Set<HInstruction> dominatedUsers = input.dominatedUsers(dominator.first);
    if (dominatedUsers.isEmpty) return;

    HTypeConversion newInput = new HTypeConversion(convertedType, input);
    dominator.addBefore(dominator.first, newInput);
    dominatedUsers.forEach((HInstruction user) {
      user.changeUse(input, newInput);
    });
  }

  void visitIs(HIs instruction) {
    HInstruction input = instruction.expression;
    HType convertedType =
        new HType.fromBoundedType(instruction.typeExpression, compiler);

    List<HInstruction> ifUsers = <HInstruction>[];
    List<HInstruction> notIfUsers = <HInstruction>[];

    for (HInstruction user in instruction.usedBy) {
      if (user is HIf) {
        ifUsers.add(user);
      } else if (user is HNot) {
        for (HInstruction notUser in user.usedBy) {
          if (notUser is HIf) notIfUsers.add(notUser);
        }
      }
    }

    if (ifUsers.isEmpty && notIfUsers.isEmpty) return;

    for (HIf ifUser in ifUsers) {
      changeUsesDominatedBy(ifUser.thenBlock, input, convertedType);
      // TODO(ngeoffray): Also change uses for the else block on a HType
      // that knows it is not of a specific Type.
    }

    for (HIf ifUser in notIfUsers) {
      changeUsesDominatedBy(ifUser.elseBlock, input, convertedType);
      // TODO(ngeoffray): Also change uses for the then block on a HType
      // that knows it is not of a specific Type.
    }
  }
}


// Analyze the constructors to see if some fields will always have a specific
// type after construction. If this is the case we can ignore the type given
// by the field initializer. This is especially useful when the field
// initializer is initializing the field to null.
class SsaConstructionFieldTypes
    extends HBaseVisitor implements OptimizationPhase {
  final JavaScriptBackend backend;
  final CodegenWorkItem work;
  final HTypeMap types;
  final String name = "SsaConstructionFieldTypes";
  final Set<HInstruction> thisUsers;
  final Set<Element> allSetters;
  final Map<HBasicBlock, Map<Element, HType>> blockFieldSetters;
  bool thisExposed = false;
  HGraph currentGraph;
  Map<Element, HType> currentFieldSetters;

  SsaConstructionFieldTypes(JavaScriptBackend this.backend,
         CodegenWorkItem this.work,
         HTypeMap this.types)
      : thisUsers = new Set<HInstruction>(),
        allSetters = new Set<Element>(),
        blockFieldSetters = new Map<HBasicBlock, Map<Element, HType>>();

  void visitGraph(HGraph graph) {
    currentGraph = graph;
    if (!work.element.isGenerativeConstructorBody() &&
        !work.element.isGenerativeConstructor()) return;
    visitDominatorTree(graph);
    if (work.element.isGenerativeConstructor()) {
      backend.registerConstructor(work.element);
    }
  }

  visitBasicBlock(HBasicBlock block) {
    if (block.predecessors.length == 0) {
      // Create a new empty map for the first block.
      currentFieldSetters = new Map<Element, HType>();
    } else {
      // Build a map which intersects the fields from all predecessors. For
      // each field in this intersection it unions the types.
      currentFieldSetters =
          new Map.from(blockFieldSetters[block.predecessors[0]]);
      // Loop headers are the only nodes with back edges.
      if (!block.isLoopHeader()) {
        for (int i = 1; i < block.predecessors.length; i++) {
          Map<Element, HType> predecessorsFieldSetters =
              blockFieldSetters[block.predecessors[i]];
          Map<Element, HType> newFieldSetters = new Map<Element, HType>();
          predecessorsFieldSetters.forEach((Element element, HType type) {
            HType currentType = currentFieldSetters[element];
            if (currentType != null) {
              newFieldSetters[element] =
                  currentType.union(type, backend.compiler);
            }
          });
          currentFieldSetters = newFieldSetters;
        }
      } else {
        assert(block.predecessors.length <= 2);
      }
    }
    block.forEachPhi((HPhi phi) => phi.accept(this));
    block.forEachInstruction(
        (HInstruction instruction) => instruction.accept(this));
    assert(currentFieldSetters != null);
    blockFieldSetters[block] = currentFieldSetters;
  }

  visitInstruction(HInstruction instruction) {
    // All instructions not explicitly handled below will flag the this
    // exposure if using this.
    thisExposed = thisExposed || thisUsers.contains(instruction);
  }

  visitPhi(HPhi phi) {
    if (thisUsers.contains(phi)) {
      thisUsers.addAll(phi.usedBy);
    }
  }

  visitThis(HThis instruction) {
    // Collect all users of this in a set to make the this exposed check simple
    // and cheap.
    thisUsers.addAll(instruction.usedBy);
  }

  visitFieldGet(HInstruction _) {
    // The field get instruction is allowed to use this.
  }

  visitForeignNew(HForeignNew node) {
    // The HForeignNew instruction is used in the generative constructor to
    // initialize all fields in newly created objects. The fields are
    // initialized to the value present in the initializer list or set to null
    // if not otherwise initialized.
    // Here we handle members in superclasses as well, as the handling of
    // the generative constructor bodies will ensure, that the initializer
    // type will not be used if the field is in any of these.
    int j = 0;
    node.element.forEachInstanceField(
        (ClassElement enclosingClass, Element element) {
          backend.registerFieldInitializer(element, types[node.inputs[j]]);
          j++;
        },
        includeBackendMembers: false,
        includeSuperMembers: true);
  }

  visitFieldSet(HFieldSet node) {
    Element field = node.element;
    HInstruction value = node.value;
    HType type = types[value];
    // [HFieldSet] is also used for variables in try/catch.
    if (field.isField()) allSetters.add(field);
    // Don't handle fields defined in superclasses. Given that the field is
    // always added to the [allSetters] set, setting a field defined in a
    // superclass will get an inferred type of UNKNOWN.
    if (identical(work.element.getEnclosingClass(), field.getEnclosingClass()) &&
        value.hasGuaranteedType()) {
      currentFieldSetters[field] = type;
    }
  }

  visitExit(HExit node) {
    // If this has been exposed then we cannot say anything about types after
    // construction.
    if (!thisExposed) {
      // Register the known field types.
      currentFieldSetters.forEach((Element element, HType type) {
        backend.registerFieldConstructor(element, type);
        allSetters.remove(element);
      });
    }

    // For other fields having setters in the generative constructor body, set
    // the type to UNKNOWN to avoid relying on the type set in the initializer
    // list.
    allSetters.forEach((Element element) {
      backend.registerFieldConstructor(element, HType.UNKNOWN);
    });
  }
}

/**
 * This phase specializes dominated uses of a call, where the call
 * can give us some type information of what the receiver might be.
 * For example, after a call to [:a.foo():], if [:foo:] is only
 * in class [:A:], a can be of type [:A:].
 */
class SsaReceiverSpecialization extends HBaseVisitor
    implements OptimizationPhase {
  final String name = "SsaReceiverSpecialization";
  final Compiler compiler;

  SsaReceiverSpecialization(this.compiler);

  void visitGraph(HGraph graph) {
    visitDominatorTree(graph);
  }

  void visitInterceptor(HInterceptor interceptor) {
    HInstruction receiver = interceptor.receiver;
    JavaScriptBackend backend = compiler.backend;
    for (var user in receiver.usedBy) {
      if (user is HInterceptor && interceptor.dominates(user)) {
        Set<ClassElement> otherIntercepted = user.interceptedClasses;
        // If the dominated interceptor intercepts the int class or
        // the double class, we make sure these classes are also being
        // intercepted by the dominating interceptor. Otherwise, the
        // dominating interceptor could just intercept the number
        // class and therefore not implement the methods in the int or
        // double class.
        if (otherIntercepted.contains(backend.jsIntClass)
            || otherIntercepted.contains(backend.jsDoubleClass)) {
          interceptor.interceptedClasses.addAll(user.interceptedClasses);
        }
        user.interceptedClasses = interceptor.interceptedClasses;
      }
    }
  }

  // TODO(ngeoffray): Also implement it for non-intercepted calls.
}

/**
 * This phase replaces all interceptors that are used only once with
 * one-shot interceptors. It saves code size and makes the receiver of
 * an intercepted call a candidate for being generated at use site.
 */
class SsaSimplifyInterceptors extends HBaseVisitor
    implements OptimizationPhase {
  final String name = "SsaSimplifyInterceptors";
  final ConstantSystem constantSystem;
  HGraph graph;

  SsaSimplifyInterceptors(this.constantSystem);

  void visitGraph(HGraph graph) {
    this.graph = graph;
    visitDominatorTree(graph);
  }

  void visitInterceptor(HInterceptor node) {
    if (node.usedBy.length != 1) return;
    // [HBailoutTarget] instructions might have the interceptor as
    // input. In such situation we let the dead code analyzer find out
    // the interceptor is not needed.
    if (node.usedBy[0] is !HInvokeDynamic) return;

    HInvokeDynamic user = node.usedBy[0];

    // If [node] was loop hoisted, we keep the interceptor.
    if (!user.hasSameLoopHeaderAs(node)) return;

    // Replace the user with a [HOneShotInterceptor].
    HConstant nullConstant = graph.addConstantNull(constantSystem);
    List<HInstruction> inputs = new List<HInstruction>.from(user.inputs);
    inputs[0] = nullConstant;
    HOneShotInterceptor interceptor = new HOneShotInterceptor(
        user.selector, inputs, node.interceptedClasses);
    interceptor.sourcePosition = user.sourcePosition;
    interceptor.sourceElement = user.sourceElement;

    HBasicBlock block = user.block;
    block.addAfter(user, interceptor);
    block.rewrite(user, interceptor);
    block.remove(user);

    // The interceptor will be removed in the dead code elimination
    // phase. Note that removing it here would not work because of how
    // the [visitBasicBlock] is implemented.
  }
}
