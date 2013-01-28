/*
 * Copyright (c) 2013, the Dart project authors.
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
package com.google.dart.engine.internal.element.handle;

import com.google.dart.engine.context.AnalysisContext;
import com.google.dart.engine.element.Annotation;
import com.google.dart.engine.element.ClassElement;
import com.google.dart.engine.element.CompilationUnitElement;
import com.google.dart.engine.element.ConstructorElement;
import com.google.dart.engine.element.Element;
import com.google.dart.engine.element.ElementLocation;
import com.google.dart.engine.element.FieldElement;
import com.google.dart.engine.element.FunctionElement;
import com.google.dart.engine.element.LabelElement;
import com.google.dart.engine.element.LibraryElement;
import com.google.dart.engine.element.MethodElement;
import com.google.dart.engine.element.ParameterElement;
import com.google.dart.engine.element.PrefixElement;
import com.google.dart.engine.element.PropertyAccessorElement;
import com.google.dart.engine.element.TypeAliasElement;
import com.google.dart.engine.element.TypeVariableElement;
import com.google.dart.engine.element.VariableElement;

import java.lang.ref.WeakReference;

/**
 * The abstract class {@code ElementHandle} implements the behavior common to objects that implement
 * a handle to an {@link Element}.
 */
public abstract class ElementHandle implements Element {
  /**
   * Return a handle on the given element. If the element is already a handle, then it will be
   * returned directly, otherwise a handle of the appropriate class will be constructed.
   * 
   * @param element the element for which a handle is to be constructed
   * @return a handle on the given element
   */
  @SuppressWarnings("unchecked")
  public static <E extends Element> E forElement(E element) {
    if (element instanceof ElementHandle) {
      return element;
    }
    switch (element.getKind()) {
      case CLASS:
        return (E) new ClassElementHandle((ClassElement) element);
      case COMPILATION_UNIT:
        return (E) new CompilationUnitElementHandle((CompilationUnitElement) element);
      case CONSTRUCTOR:
        return (E) new ConstructorElementHandle((ConstructorElement) element);
      case FIELD:
        return (E) new FieldElementHandle((FieldElement) element);
      case FUNCTION:
        return (E) new FunctionElementHandle((FunctionElement) element);
      case GETTER:
        return (E) new PropertyAccessorElementHandle((PropertyAccessorElement) element);
      case LABEL:
        return (E) new LabelElementHandle((LabelElement) element);
      case LIBRARY:
        return (E) new LibraryElementHandle((LibraryElement) element);
      case METHOD:
        return (E) new MethodElementHandle((MethodElement) element);
      case PARAMETER:
        return (E) new ParameterElementHandle((ParameterElement) element);
      case PREFIX:
        return (E) new PrefixElementHandle((PrefixElement) element);
      case SETTER:
        return (E) new PropertyAccessorElementHandle((PropertyAccessorElement) element);
      case TYPE_ALIAS:
        return (E) new TypeAliasElementHandle((TypeAliasElement) element);
      case TYPE_VARIABLE:
        return (E) new TypeVariableElementHandle((TypeVariableElement) element);
      case VARIABLE:
        return (E) new VariableElementHandle((VariableElement) element);
      case ERROR:
      default:
        throw new UnsupportedOperationException();
    }
  }

  /**
   * Return an array of the same size as the given array where each element of the returned array is
   * a handle for the corresponding element of the given array.
   * 
   * @param elements the elements for which handles are to be created
   * @return an array of handles to the given elements
   */
  public static <E extends Element> E[] forElements(E[] elements) {
    int length = elements.length;
    E[] handles = elements.clone();
    for (int i = 0; i < length; i++) {
      handles[i] = forElement(elements[i]);
    }
    return handles;
  }

  /**
   * The context in which the element is defined.
   */
  private AnalysisContext context;

  /**
   * The location of this element, used to reconstitute the element if it has been garbage
   * collected.
   */
  private ElementLocation location;

  /**
   * A reference to the element being referenced by this handle, or {@code null} if the element has
   * been garbage collected.
   */
  private WeakReference<Element> elementReference;

  /**
   * Initialize a newly created element handle to represent the given element.
   * 
   * @param element the element being represented
   */
  public ElementHandle(Element element) {
    context = element.getContext();
    location = element.getLocation();
    elementReference = new WeakReference<Element>(element);
  }

  @Override
  public boolean equals(Object object) {
    return object instanceof Element && ((Element) object).getLocation().equals(location);
  }

  @Override
  public <E extends Element> E getAncestor(Class<E> elementClass) {
    return getActualElement().getAncestor(elementClass);
  }

  @Override
  public AnalysisContext getContext() {
    return context;
  }

  @Override
  public Element getEnclosingElement() {
    return getActualElement().getEnclosingElement();
  }

  @Override
  public LibraryElement getLibrary() {
    return getAncestor(LibraryElement.class);
  }

  @Override
  public ElementLocation getLocation() {
    return location;
  }

  @Override
  public Annotation[] getMetadata() {
    return getActualElement().getMetadata();
  }

  @Override
  public String getName() {
    return getActualElement().getName();
  }

  @Override
  public int getNameOffset() {
    return getActualElement().getNameOffset();
  }

  @Override
  public int hashCode() {
    return location.hashCode();
  }

  @Override
  public boolean isSynthetic() {
    return getActualElement().isSynthetic();
  }

  /**
   * Return the element being represented by this handle, reconstituting the element if the
   * reference has been set to {@code null}.
   * 
   * @return the element being represented by this handle
   */
  protected Element getActualElement() {
    Element element = elementReference.get();
    if (element == null) {
      element = context.getElement(location);
      elementReference = new WeakReference<Element>(element);
    }
    return element;
  }
}
