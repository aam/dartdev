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
package com.google.dart.engine.internal.context;

import com.google.dart.engine.EngineTestCase;
import com.google.dart.engine.ast.ClassDeclaration;
import com.google.dart.engine.ast.CompilationUnit;
import com.google.dart.engine.context.AnalysisContextFactory;
import com.google.dart.engine.element.Element;
import com.google.dart.engine.element.ElementLocation;
import com.google.dart.engine.error.AnalysisError;
import com.google.dart.engine.error.GatheringErrorListener;
import com.google.dart.engine.internal.element.ElementLocationImpl;
import com.google.dart.engine.scanner.Token;
import com.google.dart.engine.source.FileBasedSource;
import com.google.dart.engine.source.Source;
import com.google.dart.engine.source.SourceFactory;
import com.google.dart.engine.source.TestSource;

import static com.google.dart.engine.element.ElementFactory.library;
import static com.google.dart.engine.utilities.io.FileUtilities2.createFile;

public class AnalysisContextImplTest extends EngineTestCase {
  public void fail_getElement_location() {
    AnalysisContextImpl context = new AnalysisContextImpl();
    ElementLocation location = new ElementLocationImpl("dart:core;Object");
    Element element = context.getElement(location);
    assertNotNull(element);
    assertEquals(location, element.getLocation());
  }

  public void fail_getErrors_none() throws Exception {
    AnalysisContextImpl context = new AnalysisContextImpl();
    SourceFactory sourceFactory = new SourceFactory();
    context.setSourceFactory(sourceFactory);
    Source source = new FileBasedSource(sourceFactory, createFile("/lib.dart"));
    sourceFactory.setContents(source, "library lib;");
    AnalysisError[] errors = context.getErrors(source);
    assertNotNull(errors);
    assertLength(0, errors);
  }

  public void fail_getErrors_some() throws Exception {
    AnalysisContextImpl context = new AnalysisContextImpl();
    SourceFactory sourceFactory = new SourceFactory();
    context.setSourceFactory(sourceFactory);
    Source source = new FileBasedSource(sourceFactory, createFile("/lib.dart"));
    sourceFactory.setContents(source, "library lib;");
    AnalysisError[] errors = context.getErrors(source);
    assertNotNull(errors);
    assertTrue(errors.length > 0);
  }

  public void fail_resolve() throws Exception {
    AnalysisContextImpl context = AnalysisContextFactory.contextWithCore();
    SourceFactory sourceFactory = context.getSourceFactory();
    Source source = sourceFactory.forFile(createFile("/lib.dart"));
    sourceFactory.setContents(source, "library lib;");
    GatheringErrorListener listener = new GatheringErrorListener();
    CompilationUnit compilationUnit = context.resolve(source, library(context, "lib"), listener);
    assertNotNull(compilationUnit);
  }

  public void test_creation() {
    assertNotNull(new AnalysisContextImpl());
  }

  public void test_parse_no_errors() throws Exception {
    AnalysisContextImpl context = new AnalysisContextImpl();
    SourceFactory sourceFactory = new SourceFactory();
    context.setSourceFactory(sourceFactory);
    Source source = new TestSource(sourceFactory, createFile("/lib.dart"), "library lib;");
    CompilationUnit compilationUnit = context.parse(source);
    assertEquals(0, compilationUnit.getSyntacticErrors().length);
    // TODO (danrubel): assert no semantic errors
//    assertEquals(null, compilationUnit.getSemanticErrors());
//    assertEquals(null, compilationUnit.getErrors());
  }

  public void test_parse_with_errors() throws Exception {
    AnalysisContextImpl context = new AnalysisContextImpl();
    SourceFactory sourceFactory = new SourceFactory();
    context.setSourceFactory(sourceFactory);
    Source source = new TestSource(sourceFactory, createFile("/lib.dart"), "library {");
    CompilationUnit compilationUnit = context.parse(source);
    assertTrue("Expected syntax errors", compilationUnit.getSyntacticErrors().length > 0);
    // TODO (danrubel): assert no semantic errors
//  assertEquals(null, compilationUnit.getSemanticErrors());
//  assertEquals(null, compilationUnit.getErrors());
  }

  public void test_parse_with_listener() throws Exception {
    AnalysisContextImpl context = new AnalysisContextImpl();
    SourceFactory sourceFactory = new SourceFactory();
    context.setSourceFactory(sourceFactory);
    Source source = new FileBasedSource(sourceFactory, createFile("/lib.dart"));
    sourceFactory.setContents(source, "library lib;");
    GatheringErrorListener listener = new GatheringErrorListener();
    CompilationUnit compilationUnit = context.parse(source, listener);
    assertNotNull(compilationUnit);
  }

  public void test_scan() throws Exception {
    AnalysisContextImpl context = new AnalysisContextImpl();
    SourceFactory sourceFactory = new SourceFactory();
    context.setSourceFactory(sourceFactory);
    Source source = new FileBasedSource(sourceFactory, createFile("/lib.dart"));;
    sourceFactory.setContents(source, "library lib;");
    GatheringErrorListener listener = new GatheringErrorListener();
    Token token = context.scan(source, listener);
    assertNotNull(token);
  }

  public void test_setSourceFactory() {
    AnalysisContextImpl context = new AnalysisContextImpl();
    SourceFactory sourceFactory = new SourceFactory();
    context.setSourceFactory(sourceFactory);
    assertEquals(sourceFactory, context.getSourceFactory());
  }

  public void test_sourceChanged() throws Exception {
    AnalysisContextImpl context = new AnalysisContextImpl();
    SourceFactory sourceFactory = new SourceFactory();
    context.setSourceFactory(sourceFactory);
    Source source = sourceFactory.forFile(createFile("/lib.dart"));

    sourceFactory.setContents(source, "class A {}");
    CompilationUnit unit = context.parse(source, new GatheringErrorListener());
    assertEquals("A", ((ClassDeclaration) unit.getDeclarations().get(0)).getName().getName());

    sourceFactory.setContents(source, "class B {}");
    context.sourceChanged(source);
    unit = context.parse(source, new GatheringErrorListener());
    assertEquals("B", ((ClassDeclaration) unit.getDeclarations().get(0)).getName().getName());
  }
}
