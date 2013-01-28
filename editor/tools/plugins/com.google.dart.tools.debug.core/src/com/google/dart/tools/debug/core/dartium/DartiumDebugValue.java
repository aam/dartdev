/*
 * Copyright (c) 2012, the Dart project authors.
 * 
 * Licensed under the Eclipse Public License v1.0 (the "License"); you may not use this file except
 * in compliance with the License. You may obtain a copy of the License at
 * 
 * http://www.eclipse.org/legal/epl-v10.html
 * 
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied. See the License for the specific language governing permissions and limitations under
 * the License.
 */
package com.google.dart.tools.debug.core.dartium;

import com.google.dart.tools.debug.core.DartDebugCorePlugin;
import com.google.dart.tools.debug.core.util.DebuggerUtils;
import com.google.dart.tools.debug.core.util.IDartDebugValue;
import com.google.dart.tools.debug.core.webkit.WebkitCallback;
import com.google.dart.tools.debug.core.webkit.WebkitRemoteObject;
import com.google.dart.tools.debug.core.webkit.WebkitResult;

import org.eclipse.core.runtime.IStatus;
import org.eclipse.core.runtime.Status;
import org.eclipse.debug.core.DebugException;
import org.eclipse.debug.core.model.IValue;
import org.eclipse.debug.core.model.IVariable;

import java.io.IOException;
import java.util.Collections;

/**
 * The IValue implementation of Dartium Debug element
 */
public class DartiumDebugValue extends DartiumDebugElement implements IValue, IDartDebugValue {
  public static interface ValueCallback {
    public void detailComputed(String stringValue);
  }

  private static final int MAX_DISPLAYABLE_LIST_LENGTH = 2000;

  private DartiumDebugVariable variable;
  private WebkitRemoteObject value;

  private VariableCollector variableCollector;

  public DartiumDebugValue(DartiumDebugTarget target, DartiumDebugVariable variable,
      WebkitRemoteObject value) {
    super(target);

    this.variable = variable;
    this.value = value;

    if (!value.isList()) {
      populate();
    }
  }

  public void computeDetail(final ValueCallback callback) {
    // If the value is a primitive type, just return the display string.
    if (value.isPrimitive() || (variable != null && variable.isLibraryObject())) {
      callback.detailComputed(getDisplayString());

      return;
    }

    if (value.isNull()) {
      callback.detailComputed(getDisplayString());
      return;
    }

    // Otherwise try and call the toString() method of the object.
    try {
      getConnection().getRuntime().callToString(value.getObjectId(), new WebkitCallback<String>() {
        @Override
        public void handleResult(WebkitResult<String> result) {
          if (result.isError()) {
            callback.detailComputed(result.getErrorMessage());
          } else {
            callback.detailComputed(result.getResult());
          }
        }
      });
    } catch (IOException e) {
      DartDebugCorePlugin.logError(e);

      callback.detailComputed(null);
    }
  }

  /**
   * @return a user-consumable string for the value object
   * @throws DebugException
   */
  public String getDisplayString() {
    if (variable != null && variable.isLibraryObject()) {
      return "";
    }

    if (value.isNull()) {
      return "null";
    }

    if (isPrimitive() && value.isString()) {
      return DebuggerUtils.printString(value.getValue());
    }

    if (isPrimitive()) {
      return value.getValue();
    }

    if (isListValue()) {
      if (value.getClassName() != null) {
        return value.getClassName() + "[" + getListLength() + "]";
      } else {
        return "List[" + getListLength() + "]";
      }
    }

    try {
      return getReferenceTypeName();
    } catch (DebugException e) {
      return value.getClassName();
    }
  }

  @Override
  public String getReferenceTypeName() throws DebugException {
    // The special property @classInfo contains this like the class name. We need to populate the
    // variable information before we attempt to retrieve the class name.
    getVariables();

    return DebuggerUtils.demangleVmName(getConnection().getDebugger().getClassNameSync(value));
  }

  @Override
  public String getValueString() {
    return getDisplayString();
  }

  @Override
  public IVariable[] getVariables() throws DebugException {
    if (variableCollector == null) {
      populate();
    }

    try {
      return variableCollector.getVariables();
    } catch (InterruptedException e) {
      throw new DebugException(new Status(
          IStatus.ERROR,
          DartDebugCorePlugin.PLUGIN_ID,
          e.toString(),
          e));
    }
  }

  @Override
  public boolean hasVariables() throws DebugException {
    if (isListValue()) {
      return getListLength() > 0;
    } else {
      return value.hasObjectId();
    }
  }

  @Override
  public boolean isAllocated() throws DebugException {
    return true;
  }

  @Override
  public boolean isListValue() {
    return value.isList();
  }

  @Override
  public boolean isNull() {
    return value.isNull();
  }

  public boolean isPrimitive() {
    return value.isPrimitive();
  }

  private int getListLength() {
    // value.getDescription() == "Array[x]"

    String str = value.getDescription();

    int startIndex = str.indexOf('[');
    int endIndex = str.indexOf(']', startIndex);

    if (startIndex != -1 && endIndex != -1) {
      String val = str.substring(startIndex + 1, endIndex);

      try {
        return Integer.parseInt(val);
      } catch (NumberFormatException nfe) {

      }
    }

    return 0;
  }

  private void populate() {
    if (value.hasObjectId()) {
      // TODO(devoncarew): this prevents crashes in the editor and Dartium, but a better
      // solution is needed. That solution will likely involve use of the Runtime.callFunctionOn
      // method. See https://code.google.com/p/dart/issues/detail?id=7794.
      if (value.isList() && getListLength() > MAX_DISPLAYABLE_LIST_LENGTH) {
        variableCollector = VariableCollector.empty();
      } else {
        variableCollector = VariableCollector.createCollector(
            getTarget(),
            variable,
            Collections.singletonList(value));
      }
    } else {
      variableCollector = VariableCollector.empty();
    }
  }

}
