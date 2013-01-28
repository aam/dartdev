#!/usr/bin/python
# Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""This module provides shared functionality for systems to generate
Dart APIs from the IDL database."""

import copy
import json
import os
import re
from htmlrenamer import html_interface_renames

# Set up json file for retrieving comments.
_current_dir = os.path.dirname(__file__)
_json_path = os.path.join(_current_dir, '..', 'docs', 'docs.json')
_dom_json = json.load(open(_json_path))

_pure_interfaces = set([
    # TODO(sra): DOMStringMap should be a class implementing Map<String,String>.
    'DOMStringMap',
    'ElementTimeControl',
    'ElementTraversal',
    'EventListener',
    'MediaQueryListListener',
    'MutationCallback',
    'NodeSelector',
    'SVGExternalResourcesRequired',
    'SVGFilterPrimitiveStandardAttributes',
    'SVGFitToViewBox',
    'SVGLangSpace',
    'SVGLocatable',
    'SVGStylable',
    'SVGTests',
    'SVGTransformable',
    'SVGURIReference',
    'SVGZoomAndPan',
    'TimeoutHandler'])

def IsPureInterface(interface_name):
  return interface_name in _pure_interfaces


_methods_with_named_formals = set([
  'DataView.getFloat32',
  'DataView.getFloat64',
  'DataView.getInt16',
  'DataView.getInt32',
  'DataView.getInt8',
  'DataView.getUint16',
  'DataView.getUint32',
  'DataView.getUint8',
  'DataView.setFloat32',
  'DataView.setFloat64',
  'DataView.setInt16',
  'DataView.setInt32',
  'DataView.setInt8',
  'DataView.setUint16',
  'DataView.setUint32',
  'DataView.setUint8',
  'DirectoryEntry.getDirectory',
  'DirectoryEntry.getFile',
  ])

#
# Renames for attributes that have names that are not legal Dart names.
#
_dart_attribute_renames = {
    'default': 'defaultValue',
    'final': 'finalValue',
}

#
# Interface version of the DOM needs to delegate typed array constructors to a
# factory provider.
#
interface_factories = {
    'Float32Array': '_TypedArrayFactoryProvider',
    'Float64Array': '_TypedArrayFactoryProvider',
    'Int8Array': '_TypedArrayFactoryProvider',
    'Int16Array': '_TypedArrayFactoryProvider',
    'Int32Array': '_TypedArrayFactoryProvider',
    'Uint8Array': '_TypedArrayFactoryProvider',
    'Uint8ClampedArray': '_TypedArrayFactoryProvider',
    'Uint16Array': '_TypedArrayFactoryProvider',
    'Uint32Array': '_TypedArrayFactoryProvider',
}

#
# Custom native specs for the dart2js dom.
#
_dart2js_dom_custom_native_specs = {
    # Decorate the singleton Console object, if present (workers do not have a
    # console).
    'Console': "=(typeof console == 'undefined' ? {} : console)",

    # DOMWindow aliased with global scope.
    'DOMWindow': '@*DOMWindow',
}

def IsRegisteredType(type_name):
  return type_name in _idl_type_registry

def MakeNativeSpec(javascript_binding_name):
  if javascript_binding_name in _dart2js_dom_custom_native_specs:
    return _dart2js_dom_custom_native_specs[javascript_binding_name]
  else:
    # Make the class 'hidden' so it is dynamically patched at runtime.  This
    # is useful for browser compat.
    return '*' + javascript_binding_name


def MatchSourceFilter(thing):
  return 'WebKit' in thing.annotations or 'Dart' in thing.annotations


class ParamInfo(object):
  """Holder for various information about a parameter of a Dart operation.

  Attributes:
    name: Name of parameter.
    type_id: Original type id.  None for merged types.
    is_optional: Parameter optionality.
  """
  def __init__(self, name, type_id, is_optional):
    self.name = name
    self.type_id = type_id
    self.is_optional = is_optional

  def Copy(self):
    return ParamInfo(self.name, self.type_id, self.is_optional)

  def __repr__(self):
    content = 'name = %s, type_id = %s, is_optional = %s' % (
        self.name, self.type_id, self.is_optional)
    return '<ParamInfo(%s)>' % content


# Given a list of overloaded arguments, render dart arguments.
def _BuildArguments(args, interface, constructor=False):
  def IsOptional(argument):
    if 'Callback' in argument.ext_attrs:
      # Callbacks with 'Optional=XXX' are treated as optional arguments.
      return 'Optional' in argument.ext_attrs
    if constructor:
      # FIXME: Constructors with 'Optional=XXX' shouldn't be treated as
      # optional arguments.
      return 'Optional' in argument.ext_attrs
    return False

  # Given a list of overloaded arguments, choose a suitable name.
  def OverloadedName(args):
    return '_OR_'.join(sorted(set(arg.id for arg in args)))

  def DartType(idl_type_name):
   if idl_type_name in _idl_type_registry:
     return _idl_type_registry[idl_type_name].dart_type or idl_type_name
   return idl_type_name

  # Given a list of overloaded arguments, choose a suitable type.
  def OverloadedType(args):
    type_ids = sorted(set(arg.type.id for arg in args))
    if len(set(DartType(arg.type.id) for arg in args)) == 1:
      return type_ids[0]
    else:
      return None

  result = []

  is_optional = False
  for arg_tuple in map(lambda *x: x, *args):
    is_optional = is_optional or any(arg is None or IsOptional(arg) for arg in arg_tuple)

    filtered = filter(None, arg_tuple)
    type_id = OverloadedType(filtered)
    name = OverloadedName(filtered)
    result.append(ParamInfo(name, type_id, is_optional))

  return result

def IsOptional(argument):
  return ('Optional' in argument.ext_attrs and
          argument.ext_attrs['Optional'] == None)

def AnalyzeOperation(interface, operations):
  """Makes operation calling convention decision for a set of overloads.

  Returns: An OperationInfo object.
  """

  # split operations with optional args into multiple operations
  split_operations = []
  for operation in operations:
    for i in range(0, len(operation.arguments)):
      if IsOptional(operation.arguments[i]):
        new_operation = copy.deepcopy(operation)
        new_operation.arguments = new_operation.arguments[:i]
        split_operations.append(new_operation)
    split_operations.append(operation)

  # Zip together arguments from each overload by position, then convert
  # to a dart argument.
  info = OperationInfo()
  info.operations = operations
  info.overloads = split_operations
  info.declared_name = operations[0].id
  info.name = operations[0].ext_attrs.get('DartName', info.declared_name)
  info.constructor_name = None
  info.js_name = info.declared_name
  info.type_name = operations[0].type.id   # TODO: widen.
  info.param_infos = _BuildArguments([op.arguments for op in split_operations], interface)
  full_name = '%s.%s' % (interface.id, info.declared_name)
  info.requires_named_arguments = full_name in _methods_with_named_formals
  return info


def AnalyzeConstructor(interface):
  """Returns an OperationInfo object for the constructor.

  Returns None if the interface has no Constructor.
  """
  if 'Constructor' in interface.ext_attrs:
    name = None
    func_value = interface.ext_attrs.get('Constructor')
    if not func_value:
      args = []
      idl_args = []
  elif 'NamedConstructor' in interface.ext_attrs:
    func_value = interface.ext_attrs.get('NamedConstructor')
    name = func_value.id
  else:
    return None

  if func_value:
    idl_args = func_value.arguments
    args =_BuildArguments([idl_args], interface, True)

  info = OperationInfo()
  info.overloads = None
  info.idl_args = idl_args
  info.declared_name = name
  info.name = name
  info.constructor_name = None
  info.js_name = name
  info.type_name = interface.id
  info.param_infos = args
  info.requires_named_arguments = False
  info.pure_dart_constructor = False
  return info

def IsDartListType(type):
  return type == 'List' or type.startswith('sequence<')

def IsDartCollectionType(type):
  return IsDartListType(type)

def FindMatchingAttribute(interface, attr1):
  matches = [attr2 for attr2 in interface.attributes
             if attr1.id == attr2.id]
  if matches:
    assert len(matches) == 1
    return matches[0]
  return None


def DartDomNameOfAttribute(attr):
  """Returns the Dart name for an IDLAttribute.

  attr.id is the 'native' or JavaScript name.

  To ensure uniformity, work with the true IDL name until as late a possible,
  e.g. translate to the Dart name when generating Dart code.
  """
  name = attr.id
  name = _dart_attribute_renames.get(name, name)
  name = attr.ext_attrs.get('DartName', None) or name
  return name


def TypeOrNothing(dart_type, comment=None):
  """Returns string for declaring something with |dart_type| in a context
  where a type may be omitted.
  The string is empty or has a trailing space.
  """
  if dart_type == 'dynamic':
    if comment:
      return '/*%s*/ ' % comment   # Just a comment foo(/*T*/ x)
    else:
      return ''                    # foo(x) looks nicer than foo(var|dynamic x)
  else:
    return dart_type + ' '


def TypeOrVar(dart_type, comment=None):
  """Returns string for declaring something with |dart_type| in a context
  where if a type is omitted, 'var' must be used instead."""
  if dart_type == 'dynamic':
    if comment:
      return 'var /*%s*/' % comment   # e.g.  var /*T*/ x;
    else:
      return 'var'                    # e.g.  var x;
  else:
    return dart_type


class OperationInfo(object):
  """Holder for various derived information from a set of overloaded operations.

  Attributes:
    overloads: A list of IDL operation overloads with the same name.
    name: A string, the simple name of the operation.
    constructor_name: A string, the name of the constructor iff the constructor
       is named, e.g. 'fromList' in  Int8Array.fromList(list).
    type_name: A string, the name of the return type of the operation.
    param_infos: A list of ParamInfo.
    factory_parameters: A list of parameters used for custom designed Factory
        calls.
  """

  def __init__(self):
    self.factory_parameters = None

  def ParametersDeclaration(self, rename_type, force_optional=False):
    def FormatParam(param):
      dart_type = rename_type(param.type_id) if param.type_id else 'dynamic'
      return '%s%s' % (TypeOrNothing(dart_type, param.type_id), param.name)

    required = []
    optional = []
    for param_info in self.param_infos:
      if param_info.is_optional:
        optional.append(param_info)
      else:
        if optional:
          raise Exception('Optional parameters cannot precede required ones: '
                          + str(params))
        required.append(param_info)
    argtexts = map(FormatParam, required)
    if optional:
      needs_named = self.requires_named_arguments and not force_optional
      left_bracket, right_bracket = '{}' if needs_named else '[]'
      argtexts.append(
          left_bracket +
          ', '.join(map(FormatParam, optional)) +
          right_bracket)
    return ', '.join(argtexts)

  def ParametersAsArgumentList(self, parameter_count = None):
    """Returns a string of the parameter names suitable for passing the
    parameters as arguments.
    """
    if parameter_count is None:
      parameter_count = len(self.param_infos)
    return ', '.join(map(
        lambda param_info: param_info.name,
        self.param_infos[:parameter_count]))

  def IsStatic(self):
    is_static = self.overloads[0].is_static
    assert any([is_static == o.is_static for o in self.overloads])
    return is_static

  def _ConstructorFullName(self, rename_type):
    if self.constructor_name:
      return rename_type(self.type_name) + '.' + self.constructor_name
    else:
      return rename_type(self.type_name)

def ConstantOutputOrder(a, b):
  """Canonical output ordering for constants."""
  return cmp(a.id, b.id)


def _FormatNameList(names):
  """Returns JavaScript array literal expression with one name per line."""
  #names = sorted(names)
  if len(names) <= 1:
    expression_string = str(names)  # e.g.  ['length']
  else:
    expression_string = ',\n   '.join(str(names).split(','))
    expression_string = expression_string.replace('[', '[\n    ')
  return expression_string


def IndentText(text, indent):
  """Format lines of text with indent."""
  def FormatLine(line):
    if line.strip():
      return '%s%s\n' % (indent, line)
    else:
      return '\n'
  return ''.join(FormatLine(line) for line in text.split('\n'))

# Given a sorted sequence of type identifiers, return an appropriate type
# name
def TypeName(type_ids, interface):
  # Dynamically type this field for now.
  return 'dynamic'

# ------------------------------------------------------------------------------

class Conversion(object):
  """Represents a way of converting between types."""
  def __init__(self, name, input_type, output_type):
    # input_type is the type of the API input (and the argument type of the
    # conversion function)
    # output_type is the type of the API output (and the result type of the
    # conversion function)
    self.function_name = name
    self.input_type = input_type
    self.output_type = output_type

#  "TYPE DIRECTION INTERFACE.MEMBER" -> conversion
#     Specific member of interface
#  "TYPE DIRECTION INTERFACE.*" -> conversion
#     All members of interface getting (setting) with type.
#  "TYPE DIRECTION" -> conversion
#     All getters (setters) of type.
#
# where DIRECTION is 'get' for getters and operation return values, 'set' for
# setters and operation arguments.  INTERFACE and MEMBER are the idl names.
#

_serialize_SSV = Conversion('convertDartToNative_SerializedScriptValue',
                           'dynamic', 'dynamic')

dart2js_conversions = {
    # Wrap non-local Windows.  We need to check EventTarget (the base type)
    # as well.  Note, there are no functions that take a non-local Window
    # as a parameter / setter.
    'DOMWindow get':
      Conversion('_convertNativeToDart_Window', 'dynamic', 'WindowBase'),
    'EventTarget get':
      Conversion('_convertNativeToDart_EventTarget', 'dynamic',
                 'EventTarget'),
    'EventTarget set':
      Conversion('_convertDartToNative_EventTarget', 'EventTarget',
                 'dynamic'),

    'IDBKey get':
      Conversion('_convertNativeToDart_IDBKey', 'dynamic', 'dynamic'),
    'IDBKey set':
      Conversion('_convertDartToNative_IDBKey', 'dynamic', 'dynamic'),

    'ImageData get':
      Conversion('_convertNativeToDart_ImageData', 'dynamic', 'ImageData'),
    'ImageData set':
      Conversion('_convertDartToNative_ImageData', 'ImageData', 'dynamic'),

    'Dictionary get':
      Conversion('convertNativeToDart_Dictionary', 'dynamic', 'Map'),
    'Dictionary set':
      Conversion('convertDartToNative_Dictionary', 'Map', 'dynamic'),

    'DOMString[] set':
      Conversion('convertDartToNative_StringArray', 'List<String>', 'List'),

    'any set IDBObjectStore.add': _serialize_SSV,
    'any set IDBObjectStore.put': _serialize_SSV,
    'any set IDBCursor.update': _serialize_SSV,

    # postMessage
    'any set DedicatedWorkerContext.postMessage': _serialize_SSV,
    'any set MessagePort.postMessage': _serialize_SSV,
    'SerializedScriptValue set DOMWindow.postMessage': _serialize_SSV,
    'SerializedScriptValue set Worker.postMessage': _serialize_SSV,

    # receiving message via MessageEvent
    '* get MessageEvent.data':
      Conversion('convertNativeToDart_SerializedScriptValue',
                 'dynamic', 'dynamic'),

    '* get History.state':
      Conversion('_convertNativeToDart_SerializedScriptValue',
                 'dynamic', 'dynamic'),

    '* get PopStateEvent.state':
      Conversion('convertNativeToDart_SerializedScriptValue',
                 'dynamic', 'dynamic'),

    # IDBAny is problematic.  Some uses are just a union of other IDB types,
    # which need no conversion..  Others include data values which require
    # serialized script value processing.
    'IDBAny get IDBCursorWithValue.value':
      Conversion('_convertNativeToDart_IDBAny', 'dynamic', 'dynamic'),

    # This is problematic.  The result property of IDBRequest is used for
    # all requests.  Read requests like IDBDataStore.getObject need
    # conversion, but other requests like opening a database return
    # something that does not need conversion.
    'IDBAny get IDBRequest.result':
      Conversion('_convertNativeToDart_IDBAny', 'dynamic', 'dynamic'),

    # "source: On getting, returns the IDBObjectStore or IDBIndex that the
    # cursor is iterating. ...".  So we should not try to convert it.
    'IDBAny get IDBCursor.source': None,

    # Should be either a DOMString, an Array of DOMStrings or null.
    'IDBAny get IDBObjectStore.keyPath': None,
}

def FindConversion(idl_type, direction, interface, member):
  table = dart2js_conversions
  return (table.get('%s %s %s.%s' % (idl_type, direction, interface, member)) or
          table.get('* %s %s.%s' % (direction, interface, member)) or
          table.get('%s %s %s.*' % (idl_type, direction, interface)) or
          table.get('%s %s' % (idl_type, direction)))
  return None

# ------------------------------------------------------------------------------

# Annotations to be placed on native members.  The table is indexed by the IDL
# interface and member name, and by IDL return or field type name.  Both are
# used to assemble the annotations:
#
#   INTERFACE.MEMBER: annotations for member.
#   +TYPE:            add annotations only if there are member annotations.
#   -TYPE:            add annotations only if there are no member annotations.
#   TYPE:             add regardless of member annotations.

dart2js_annotations = {

    'CanvasRenderingContext2D.createImageData': [
      "@Creates('ImageData|=Object')",
    ],

    'CanvasRenderingContext2D.getImageData': [
      "@Creates('ImageData|=Object')",
    ],

    'CanvasRenderingContext2D.webkitGetImageDataHD': [
      "@Creates('ImageData|=Object')",
    ],

    'CanvasRenderingContext2D.fillStyle': [
      "@Creates('String|CanvasGradient|CanvasPattern')",
      "@Returns('String|CanvasGradient|CanvasPattern')",
    ],

    'CanvasRenderingContext2D.strokeStyle': [
      "@Creates('String|CanvasGradient|CanvasPattern')",
      "@Returns('String|CanvasGradient|CanvasPattern')",
    ],

    # Methods returning Window can return a local window, or a cross-frame
    # window (=Object) that needs wrapping.
    'DOMWindow': [
      "@Creates('Window|=Object')",
      "@Returns('Window|=Object')",
    ],

    'DOMWindow.openDatabase': [
      "@Creates('Database')",
      "@Creates('DatabaseSync')",
    ],

    # Cross-frame windows are EventTargets.
    '-EventTarget': [
      "@Creates('EventTarget|=Object')",
      "@Returns('EventTarget|=Object')",
    ],

    # To be in callback with the browser-created Event, we had to have called
    # addEventListener on the target, so we avoid
    'Event.currentTarget': [
      "@Creates('Null')",
      "@Returns('EventTarget|=Object')",
    ],

    # Only nodes in the DOM bubble and have target !== currentTarget.
    'Event.target': [
      "@Creates('Node')",
      "@Returns('EventTarget|=Object')",
    ],

    'MouseEvent.relatedTarget': [
      "@Creates('Node')",
      "@Returns('EventTarget|=Object')",
    ],

    # Touch targets are Elements in a Document, or the Document.
    'Touch.target': [
      "@Creates('Element|Document')",
      "@Returns('Element|Document')",
    ],


    'FileReader.result': ["@Creates('String|ArrayBuffer|Null')"],

    # Rather than have the result of an IDBRequest as a union over all possible
    # results, we mark the result as instantiating any classes, and mark
    # each operation with the classes that it could cause to be asynchronously
    # instantiated.
    'IDBRequest.result': ["@Creates('Null')"],

    # The source is usually a participant in the operation that generated the
    # IDBRequest.
    'IDBRequest.source':  ["@Creates('Null')"],

    'IDBFactory.open': ["@Creates('Database')"],

    'IDBObjectStore.put': ["@_annotation_Creates_IDBKey"],
    'IDBObjectStore.add': ["@_annotation_Creates_IDBKey"],
    'IDBObjectStore.get': ["@annotation_Creates_SerializedScriptValue"],
    'IDBObjectStore.openCursor': ["@Creates('Cursor')"],

    'IDBIndex.get': ["@annotation_Creates_SerializedScriptValue"],
    'IDBIndex.getKey': [
      "@annotation_Creates_SerializedScriptValue",
      # The source is the object store behind the index.
      "@Creates('ObjectStore')",
    ],
    'IDBIndex.openCursor': ["@Creates('Cursor')"],
    'IDBIndex.openKeyCursor': ["@Creates('Cursor')"],

    'IDBCursorWithValue.value': [
      '@annotation_Creates_SerializedScriptValue',
      '@annotation_Returns_SerializedScriptValue',
    ],

    'IDBCursor.key': [
      "@_annotation_Creates_IDBKey",
      "@_annotation_Returns_IDBKey",
    ],

    '+IDBRequest': [
      "@Returns('Request')",
      "@Creates('Request')",
    ],

    '+IDBOpenDBRequest': [
      "@Returns('Request')",
      "@Creates('Request')",
    ],
    '+IDBVersionChangeRequest': [
      "@Returns('Request')",
      "@Creates('Request')",
    ],

    'MessageEvent.ports': ["@Creates('=List')"],

    'MessageEvent.data': [
      "@annotation_Creates_SerializedScriptValue",
      "@annotation_Returns_SerializedScriptValue",
    ],
    'PopStateEvent.state': [
      "@annotation_Creates_SerializedScriptValue",
      "@annotation_Returns_SerializedScriptValue",
    ],
    'SerializedScriptValue': [
      "@annotation_Creates_SerializedScriptValue",
      "@annotation_Returns_SerializedScriptValue",
    ],

    'SQLResultSetRowList.item': ["@Creates('=Object')"],

    'XMLHttpRequest.response': [
      "@Creates('ArrayBuffer|Blob|Document|=Object|=List|String|num')",
    ],
}

# Placeholder to add experimental flag, implementation for this is
# pending in a separate CL.
dart_annotations = {
  'Element.webkitMatchesSelector': ['@Experimental'],
}

_indexed_db_annotations = [
  "@SupportedBrowser(SupportedBrowser.CHROME)",
  "@SupportedBrowser(SupportedBrowser.FIREFOX, '15')",
  "@SupportedBrowser(SupportedBrowser.IE, '10')",
  "@Experimental",
]

_file_system_annotations = [
  "@SupportedBrowser(SupportedBrowser.CHROME)",
  "@Experimental",
]

_all_but_ie9_annotations = [
  "@SupportedBrowser(SupportedBrowser.CHROME)",
  "@SupportedBrowser(SupportedBrowser.FIREFOX)",
  "@SupportedBrowser(SupportedBrowser.IE, '10')",
  "@SupportedBrowser(SupportedBrowser.SAFARI)",
]

_webkit_experimental_annotations = [
  "@SupportedBrowser(SupportedBrowser.CHROME)",
  "@SupportedBrowser(SupportedBrowser.SAFARI)",
  "@Experimental",
]

_history_annotations = _all_but_ie9_annotations

_performance_annotations = [
  "@SupportedBrowser(SupportedBrowser.CHROME)",
  "@SupportedBrowser(SupportedBrowser.FIREFOX)",
  "@SupportedBrowser(SupportedBrowser.IE)",
]

_speech_recognition_annotations = [
  "@SupportedBrowser(SupportedBrowser.CHROME, '25')",
  "@Experimental",
]

_web_sql_annotations = [
  "@SupportedBrowser(SupportedBrowser.CHROME)",
  "@SupportedBrowser(SupportedBrowser.SAFARI)",
  "@Experimental",
]

# Annotations to be placed on generated members.
# The table is indexed as:
#   INTERFACE:     annotations to be added to the interface declaration
#   INTERFACE.MEMBER: annotation to be added to the member declaration
dart_annotations = {
  'ArrayBuffer': _all_but_ie9_annotations,
  'ArrayBufferView': _all_but_ie9_annotations,
  'Database': _web_sql_annotations,
  'DatabaseSync': _web_sql_annotations,
  'DOMApplicationCache': [
    "@SupportedBrowser(SupportedBrowser.CHROME)",
    "@SupportedBrowser(SupportedBrowser.FIREFOX)",
    "@SupportedBrowser(SupportedBrowser.IE, '10')",
    "@SupportedBrowser(SupportedBrowser.OPERA)",
    "@SupportedBrowser(SupportedBrowser.SAFARI)",
  ],
  'DOMWindow.convertPointFromNodeToPage': _webkit_experimental_annotations,
  'DOMWindow.convertPointFromPageToNode': _webkit_experimental_annotations,
  'DOMWindow.indexedDB': _indexed_db_annotations,
  'DOMWindow.openDatabase': _web_sql_annotations,
  'DOMWindow.performance': _performance_annotations,
  'DOMWindow.webkitNotifications': _webkit_experimental_annotations,
  'DOMWindow.webkitRequestFileSystem': _file_system_annotations,
  'DOMWindow.webkitResolveLocalFileSystemURL': _file_system_annotations,
  'Element.webkitCreateShadowRoot': [
    "@SupportedBrowser(SupportedBrowser.CHROME, '25')",
    "@Experimental",
  ],
  'FileSystem': _file_system_annotations,
  'FileSystemSync': _file_system_annotations,
  'HashChangeEvent': [
    "@SupportedBrowser(SupportedBrowser.CHROME)",
    "@SupportedBrowser(SupportedBrowser.FIREFOX)",
    "@SupportedBrowser(SupportedBrowser.SAFARI)",
  ],
  'History.pushState': _history_annotations,
  'History.replaceState': _history_annotations,
  'HTMLContentElement': [
    "@SupportedBrowser(SupportedBrowser.CHROME, '25')",
    "@Experimental",
  ],
  'HTMLDataListElement': _all_but_ie9_annotations,
  'HTMLDetailsElement': _webkit_experimental_annotations,
  'HTMLEmbedElement': [
    "@SupportedBrowser(SupportedBrowser.CHROME)",
    "@SupportedBrowser(SupportedBrowser.IE)",
    "@SupportedBrowser(SupportedBrowser.SAFARI)",
  ],
  'HTMLKeygenElement': _webkit_experimental_annotations,
  'HTMLMeterElement': [
    "@SupportedBrowser(SupportedBrowser.CHROME)",
    "@SupportedBrowser(SupportedBrowser.FIREFOX)",
    "@SupportedBrowser(SupportedBrowser.SAFARI)",
  ],
  'HTMLObjectElement': [
    "@SupportedBrowser(SupportedBrowser.CHROME)",
    "@SupportedBrowser(SupportedBrowser.FIREFOX)",
    "@SupportedBrowser(SupportedBrowser.SAFARI)",
  ],
  'HTMLOutputElement': [
    "@SupportedBrowser(SupportedBrowser.CHROME)",
    "@SupportedBrowser(SupportedBrowser.FIREFOX)",
    "@SupportedBrowser(SupportedBrowser.SAFARI)",
  ],
  'HTMLProgressElement': _all_but_ie9_annotations,
  'HTMLShadowElement': [
    "@SupportedBrowser(SupportedBrowser.CHROME, '25')",
    "@Experimental",
  ],
  'HTMLTrackElement': [
    "@SupportedBrowser(SupportedBrowser.CHROME)",
    "@SupportedBrowser(SupportedBrowser.IE, '10')",
    "@SupportedBrowser(SupportedBrowser.SAFARI)",
  ],
  'IDBFactory': _indexed_db_annotations,
  'IDBDatabase': _indexed_db_annotations,
  'MutationObserver': [
    "@SupportedBrowser(SupportedBrowser.CHROME)",
    "@SupportedBrowser(SupportedBrowser.FIREFOX)",
    "@SupportedBrowser(SupportedBrowser.SAFARI)",
    "@Experimental",
  ],
  'NotificationCenter': _webkit_experimental_annotations,
  'Performance': _performance_annotations,
  'PopStateEvent': _history_annotations,
  'ShadowRoot': [
    "@SupportedBrowser(SupportedBrowser.CHROME, '25')",
    "@Experimental",
  ],
  'SpeechRecognition': _speech_recognition_annotations,
  'SpeechRecognitionAlternative': _speech_recognition_annotations,
  'SpeechRecognitionError': _speech_recognition_annotations,
  'SpeechRecognitionEvent': _speech_recognition_annotations,
  'SpeechRecognitionResult': _speech_recognition_annotations,
  'SQLTransaction': _web_sql_annotations,
  'SQLTransactionSync': _web_sql_annotations,
  'WebKitCSSMatrix': _webkit_experimental_annotations,
  'WebKitPoint': _webkit_experimental_annotations,
  'WebSocket': _all_but_ie9_annotations,
  'WorkerContext.indexedDB': _indexed_db_annotations,
  'WorkerContext.openDatabase': _web_sql_annotations,
  'WorkerContext.openDatabaseSync': _web_sql_annotations,
  'WorkerContext.webkitRequestFileSystem': _file_system_annotations,
  'WorkerContext.webkitRequestFileSystemSync': _file_system_annotations,
  'WorkerContext.webkitResolveLocalFileSystemSyncURL': _file_system_annotations,
  'WorkerContext.webkitResolveLocalFileSystemURL': _file_system_annotations,
  'XMLHttpRequestProgressEvent': _webkit_experimental_annotations,
  'XSLTProcessor': [
    "@SupportedBrowser(SupportedBrowser.CHROME)",
    "@SupportedBrowser(SupportedBrowser.FIREFOX)",
    "@SupportedBrowser(SupportedBrowser.SAFARI)",
  ],
}

def GetComments(interface_name, member_name=None, library_name=None):
  """ Finds all comments for the interface or member and returns a list. """

  # Add documentation from JSON.
  comments = []

  if library_name in _dom_json and interface_name in _dom_json[library_name]:
    if member_name and (member_name in
                        _dom_json[library_name][interface_name]['members']):
      comments = _dom_json[library_name][interface_name]['members'][member_name]
    elif 'comment' in _dom_json[library_name][interface_name]:
      comments = _dom_json[library_name][interface_name]['comment']

  return comments

def GetAnnotationsAndComments(interface_name, member_name=None,
                              library_name=None):
  annotations = GetComments(interface_name, member_name, library_name)
  annotations.extend(FindCommonAnnotations(interface_name, member_name,
                                           library_name))
  return annotations

def FindCommonAnnotations(interface_name, member_name=None, library_name=None):
  """ Finds annotations common between dart2js and dartium.
  """
  if member_name:
    key = '%s.%s' % (interface_name, member_name)
  else:
    key = interface_name

  annotations = ["@DomName('" + key + "')"]

  # Only add this for members, so we don't add DocsEditable to templated classes
  # (they get it from the default class template)
  if member_name:
    annotations.append('@DocsEditable');

  if (dart_annotations.get(key) != None):
    annotations.extend(dart_annotations.get(key))

  return annotations

def FindDart2JSAnnotationsAndComments(idl_type, interface_name, member_name,
                           library_name=None):
  """ Finds all annotations for Dart2JS members- including annotations for
  both dart2js and dartium.
  """
  annotations = GetAnnotationsAndComments(interface_name, member_name,
                                          library_name)

  ann2 = _FindDart2JSSpecificAnnotations(idl_type, interface_name, member_name)
  if ann2:
    if annotations:
      annotations.extend(ann2)
    else:
      annotations = ann2
  return annotations

def AnyConversionAnnotations(idl_type, interface_name, member_name):
  if (dart_annotations.get('%s.%s' % (interface_name, member_name)) or
      _FindDart2JSSpecificAnnotations(idl_type, interface_name, member_name)):
    return True
  else:
    return False

def FormatAnnotationsAndComments(annotations, indentation):
  if annotations:
    newline = '\n%s' % indentation
    result = newline.join(annotations) + newline
    return result
  return ''

def _FindDart2JSSpecificAnnotations(idl_type, interface_name, member_name):
  """ Finds dart2js-specific annotations. This does not include ones shared with
  dartium.
  """
  ann1 = dart2js_annotations.get("%s.%s" % (interface_name, member_name))
  if ann1:
    ann2 = dart2js_annotations.get('+' + idl_type)
    if ann2:
      return ann2 + ann1
    ann2 = dart2js_annotations.get(idl_type)
    if ann2:
      return ann2 + ann1
    return ann1

  ann2 = dart2js_annotations.get('-' + idl_type)
  if ann2:
    return ann2
  ann2 = dart2js_annotations.get(idl_type)
  return ann2

# ------------------------------------------------------------------------------

class IDLTypeInfo(object):
  def __init__(self, idl_type, data):
    self._idl_type = idl_type
    self._data = data

  def idl_type(self):
    return self._idl_type

  def dart_type(self):
    return self._data.dart_type or self._idl_type

  def narrow_dart_type(self):
    return self.dart_type()

  def interface_name(self):
    raise NotImplementedError()

  def implementation_name(self):
    raise NotImplementedError()

  def has_generated_interface(self):
    raise NotImplementedError()

  def list_item_type(self):
    raise NotImplementedError()

  def is_typed_array(self):
    raise NotImplementedError()

  def merged_interface(self):
    return None

  def merged_into(self):
    return None

  def native_type(self):
    return self._data.native_type or self._idl_type

  def bindings_class(self):
    return 'Dart%s' % self.idl_type()

  def vector_to_dart_template_parameter(self):
    return self.bindings_class()

  def to_native_info(self, idl_node, interface_name):
    cls = self.bindings_class()

    if 'Callback' in idl_node.ext_attrs:
      return '%s', 'RefPtr<%s>' % self.native_type(), cls, 'create'

    if self.custom_to_native():
      type = 'RefPtr<%s>' % self.native_type()
      argument_expression_template = '%s.get()'
    else:
      type = '%s*' % self.native_type()
      if isinstance(self, SVGTearOffIDLTypeInfo) and not interface_name.endswith('List'):
        argument_expression_template = '%s->propertyReference()'
      else:
        argument_expression_template = '%s'
    return argument_expression_template, type, cls, 'toNative'

  def pass_native_by_ref(self): return False

  def custom_to_native(self):
    return self._data.custom_to_native

  def parameter_type(self):
    return '%s*' % self.native_type()

  def webcore_includes(self):
    WTF_INCLUDES = [
        'ArrayBuffer',
        'ArrayBufferView',
        'Float32Array',
        'Float64Array',
        'Int8Array',
        'Int16Array',
        'Int32Array',
        'Uint8Array',
        'Uint8ClampedArray',
        'Uint16Array',
        'Uint32Array',
    ]

    if self._idl_type in WTF_INCLUDES:
      return ['<wtf/%s.h>' % self.native_type()]

    if not self._idl_type.startswith('SVG'):
      return ['"%s.h"' % self.native_type()]

    if self._idl_type in ['SVGNumber', 'SVGPoint']:
      return ['"SVGPropertyTearOff.h"']
    if self._idl_type.startswith('SVGPathSeg'):
      include = self._idl_type.replace('Abs', '').replace('Rel', '')
    else:
      include = self._idl_type
    return ['"%s.h"' % include] + _svg_supplemental_includes

  def receiver(self):
    return 'receiver->'

  def conversion_includes(self):
    includes = [self._idl_type] + (self._data.conversion_includes or [])
    return ['"Dart%s.h"' % include for include in includes]

  def to_dart_conversion(self, value, interface_name=None, attributes=None):
    return 'Dart%s::toDart(%s)' % (self._idl_type, value)

  def custom_to_dart(self):
    return self._data.custom_to_dart


class InterfaceIDLTypeInfo(IDLTypeInfo):
  def __init__(self, idl_type, data, dart_interface_name, type_registry):
    super(InterfaceIDLTypeInfo, self).__init__(idl_type, data)
    self._dart_interface_name = dart_interface_name
    self._type_registry = type_registry

  def dart_type(self):
    if self.list_item_type() and not self.has_generated_interface():
      return 'List<%s>' % self._type_registry.TypeInfo(self._data.item_type).dart_type()
    return self._data.dart_type or self._dart_interface_name

  def narrow_dart_type(self):
    if self.list_item_type():
      return self.implementation_name()
    # TODO(podivilov): only primitive and collection types should override
    # dart_type.
    if self._data.dart_type != None:
      return self.dart_type()
    if IsPureInterface(self.idl_type()):
      return self.idl_type()
    return self.interface_name()

  def interface_name(self):
    return self._dart_interface_name

  def implementation_name(self):
    implementation_name = self._dart_interface_name
    if self.merged_into():
      implementation_name = '_%s_Merged' % implementation_name

    if not self.has_generated_interface():
      implementation_name = '_%s' % implementation_name

    return implementation_name

  def has_generated_interface(self):
    return not self._data.suppress_interface

  def list_item_type(self):
    return self._data.item_type

  def is_typed_array(self):
    return self._data.is_typed_array

  def merged_interface(self):
    # All constants, attributes, and operations of merged interface should be
    # added to this interface. Merged idl interface does not have corresponding
    # Dart generated interface, and all references to merged idl interface
    # (e.g. parameter types, return types, parent interfaces) should be replaced
    # with this interface. There are two important restrictions:
    # 1) Merged and target interfaces shouldn't have common members, otherwise
    # there would be duplicated declarations in generated Dart code.
    # 2) Merged interface should be direct child of target interface, so the
    # children of merged interface are not affected by the merge.
    # As a consequence, target interface implementation and its direct children
    # interface implementations should implement merged attribute accessors and
    # operations. For example, SVGElement and Element implementation classes
    # should implement HTMLElement.insertAdjacentElement(),
    # HTMLElement.innerHTML, etc.
    return self._data.merged_interface

  def merged_into(self):
    return self._data.merged_into


class CallbackIDLTypeInfo(IDLTypeInfo):
  def __init__(self, idl_type, data):
    super(CallbackIDLTypeInfo, self).__init__(idl_type, data)


class SequenceIDLTypeInfo(IDLTypeInfo):
  def __init__(self, idl_type, data, item_info):
    super(SequenceIDLTypeInfo, self).__init__(idl_type, data)
    self._item_info = item_info

  def dart_type(self):
    return 'List<%s>' % self._item_info.dart_type()

  def interface_name(self):
    return self.dart_type()

  def implementation_name(self):
    return self.dart_type()

  def vector_to_dart_template_parameter(self):
    raise Exception('sequences of sequences are not supported yet')

  def to_native_info(self, idl_node, interface_name):
    item_native_type = self._item_info.vector_to_dart_template_parameter()
    return '%s', 'Vector<%s>' % item_native_type, 'DartUtilities', 'toNativeVector<%s>' % item_native_type

  def pass_native_by_ref(self): return True

  def to_dart_conversion(self, value, interface_name=None, attributes=None):
    return 'DartDOMWrapper::vectorToDart<%s>(%s)' % (self._item_info.vector_to_dart_template_parameter(), value)

  def conversion_includes(self):
    return self._item_info.conversion_includes()


class DOMStringArrayTypeInfo(SequenceIDLTypeInfo):
  def __init__(self, data, item_info):
    super(DOMStringArrayTypeInfo, self).__init__('DOMString[]', data, item_info)

  def to_native_info(self, idl_node, interface_name):
    return '%s', 'RefPtr<DOMStringList>', 'DartDOMStringList', 'toNative'

  def pass_native_by_ref(self): return False


class PrimitiveIDLTypeInfo(IDLTypeInfo):
  def __init__(self, idl_type, data):
    super(PrimitiveIDLTypeInfo, self).__init__(idl_type, data)

  def vector_to_dart_template_parameter(self):
    # Ugly hack. Usually IDLs floats are treated as C++ doubles, however
    # sequence<float> should map to Vector<float>
    if self.idl_type() == 'float': return 'float'
    return self.native_type()

  def to_native_info(self, idl_node, interface_name):
    type = self.native_type()
    if type == 'SerializedScriptValue':
      type = 'RefPtr<%s>' % type
    if type == 'String':
      type = 'DartStringAdapter'
    return '%s', type, 'DartUtilities', 'dartTo%s' % self._capitalized_native_type()

  def parameter_type(self):
    if self.native_type() == 'String':
      return 'const String&'
    return self.native_type()

  def conversion_includes(self):
    return []

  def to_dart_conversion(self, value, interface_name=None, attributes=None):
    function_name = self._capitalized_native_type()
    function_name = function_name[0].lower() + function_name[1:]
    function_name = 'DartUtilities::%sToDart' % function_name
    if attributes and 'TreatReturnedNullStringAs' in attributes:
      function_name += 'WithNullCheck'
    return '%s(%s)' % (function_name, value)

  def webcore_getter_name(self):
    return self._data.webcore_getter_name

  def webcore_setter_name(self):
    return self._data.webcore_setter_name

  def _capitalized_native_type(self):
    return re.sub(r'(^| )([a-z])', lambda x: x.group(2).upper(), self.native_type())


class SVGTearOffIDLTypeInfo(InterfaceIDLTypeInfo):
  def __init__(self, idl_type, data, interface_name, type_registry):
    super(SVGTearOffIDLTypeInfo, self).__init__(
        idl_type, data, interface_name, type_registry)

  def native_type(self):
    if self._data.native_type:
      return self._data.native_type
    tear_off_type = 'SVGPropertyTearOff'
    if self._idl_type.endswith('List'):
      tear_off_type = 'SVGListPropertyTearOff'
    return '%s<%s>' % (tear_off_type, self._idl_type)

  def receiver(self):
    if self._idl_type.endswith('List'):
      return 'receiver->'
    return 'receiver->propertyReference().'

  def to_dart_conversion(self, value, interface_name, attributes):
    svg_primitive_types = ['SVGAngle', 'SVGLength', 'SVGMatrix',
        'SVGNumber', 'SVGPoint', 'SVGRect', 'SVGTransform']
    conversion_cast = '%s::create(%s)'
    if interface_name.startswith('SVGAnimated'):
      conversion_cast = 'static_cast<%s*>(%s)'
    elif self.idl_type() == 'SVGStringList':
      conversion_cast = '%s::create(receiver, %s)'
    elif interface_name.endswith('List'):
      conversion_cast = 'static_cast<%s*>(%s.get())'
    elif self.idl_type() in svg_primitive_types:
      conversion_cast = '%s::create(%s)'
    else:
      conversion_cast = 'static_cast<%s*>(%s)'
    conversion_cast = conversion_cast % (self.native_type(), value)
    return 'Dart%s::toDart(%s)' % (self._idl_type, conversion_cast)

  def argument_expression(self, name, interface_name):
    return name if interface_name.endswith('List') else '%s->propertyReference()' % name


class TypeData(object):
  def __init__(self, clazz, dart_type=None, native_type=None,
               merged_interface=None, merged_into=None,
               custom_to_dart=False, custom_to_native=False,
               conversion_includes=None,
               webcore_getter_name='getAttribute',
               webcore_setter_name='setAttribute',
               item_type=None, suppress_interface=False, is_typed_array=False):
    self.clazz = clazz
    self.dart_type = dart_type
    self.native_type = native_type
    self.merged_interface = merged_interface
    self.merged_into = merged_into
    self.custom_to_dart = custom_to_dart
    self.custom_to_native = custom_to_native
    self.conversion_includes = conversion_includes
    self.webcore_getter_name = webcore_getter_name
    self.webcore_setter_name = webcore_setter_name
    self.item_type = item_type
    self.suppress_interface = suppress_interface
    self.is_typed_array = is_typed_array


def TypedArrayTypeData(item_type):
  return TypeData(clazz='Interface', item_type=item_type, is_typed_array=True)


_idl_type_registry = {
    'boolean': TypeData(clazz='Primitive', dart_type='bool', native_type='bool',
                        webcore_getter_name='hasAttribute',
                        webcore_setter_name='setBooleanAttribute'),
    'byte': TypeData(clazz='Primitive', dart_type='int', native_type='int'),
    'octet': TypeData(clazz='Primitive', dart_type='int', native_type='int'),
    'short': TypeData(clazz='Primitive', dart_type='int', native_type='int'),
    'unsigned short': TypeData(clazz='Primitive', dart_type='int',
        native_type='int'),
    'int': TypeData(clazz='Primitive', dart_type='int'),
    'unsigned int': TypeData(clazz='Primitive', dart_type='int',
        native_type='unsigned'),
    'long': TypeData(clazz='Primitive', dart_type='int', native_type='int',
        webcore_getter_name='getIntegralAttribute',
        webcore_setter_name='setIntegralAttribute'),
    'unsigned long': TypeData(clazz='Primitive', dart_type='int',
                              native_type='unsigned',
                              webcore_getter_name='getUnsignedIntegralAttribute',
                              webcore_setter_name='setUnsignedIntegralAttribute'),
    'long long': TypeData(clazz='Primitive', dart_type='int'),
    'unsigned long long': TypeData(clazz='Primitive', dart_type='int'),
    'float': TypeData(clazz='Primitive', dart_type='num', native_type='double'),
    'double': TypeData(clazz='Primitive', dart_type='num'),

    'any': TypeData(clazz='Primitive', dart_type='Object', native_type='ScriptValue'),
    'Array': TypeData(clazz='Primitive', dart_type='List'),
    'custom': TypeData(clazz='Primitive', dart_type='dynamic'),
    'Date': TypeData(clazz='Primitive', dart_type='Date', native_type='double'),
    'DOMObject': TypeData(clazz='Primitive', dart_type='Object', native_type='ScriptValue'),
    'DOMString': TypeData(clazz='Primitive', dart_type='String', native_type='String'),
    # TODO(vsm): This won't actually work until we convert the Map to
    # a native JS Map for JS DOM.
    'Dictionary': TypeData(clazz='Primitive', dart_type='Map'),
    # TODO(sra): Flags is really a dictionary: {create:bool, exclusive:bool}
    # http://dev.w3.org/2009/dap/file-system/file-dir-sys.html#the-flags-interface
    'Flags': TypeData(clazz='Primitive', dart_type='Object'),
    'DOMTimeStamp': TypeData(clazz='Primitive', dart_type='int', native_type='unsigned long long'),
    'object': TypeData(clazz='Primitive', dart_type='Object', native_type='ScriptValue'),
    'ObjectArray': TypeData(clazz='Primitive', dart_type='List'),
    'PositionOptions': TypeData(clazz='Primitive', dart_type='Object'),
    # TODO(sra): Come up with some meaningful name so that where this appears in
    # the documentation, the user is made aware that only a limited subset of
    # serializable types are actually permitted.
    'SerializedScriptValue': TypeData(clazz='Primitive', dart_type='dynamic'),
    # TODO(sra): Flags is really a dictionary: {create:bool, exclusive:bool}
    # http://dev.w3.org/2009/dap/file-system/file-dir-sys.html#the-flags-interface
    'WebKitFlags': TypeData(clazz='Primitive', dart_type='Object'),

    'sequence': TypeData(clazz='Primitive', dart_type='List'),
    'void': TypeData(clazz='Primitive', dart_type='void'),

    'CSSRule': TypeData(clazz='Interface', conversion_includes=['CSSImportRule']),
    'DOMException': TypeData(clazz='Interface', native_type='DOMCoreException'),
    'DOMStringMap': TypeData(clazz='Interface', dart_type='Map<String, String>'),
    'DOMWindow': TypeData(clazz='Interface', custom_to_dart=True),
    'Element': TypeData(clazz='Interface', merged_interface='HTMLElement',
        custom_to_dart=True),
    'EventListener': TypeData(clazz='Interface', custom_to_native=True),
    'EventTarget': TypeData(clazz='Interface', custom_to_native=True),
    'HTMLElement': TypeData(clazz='Interface', merged_into='Element',
        custom_to_dart=True),
    'IDBAny': TypeData(clazz='Interface', dart_type='dynamic', custom_to_native=True),
    'IDBKey': TypeData(clazz='Interface', dart_type='dynamic', custom_to_native=True),
    'MutationRecordArray': TypeData(clazz='Interface',  # C++ pass by pointer.
        native_type='MutationRecordArray', dart_type='List<MutationRecord>'),
    'StyleSheet': TypeData(clazz='Interface', conversion_includes=['CSSStyleSheet']),
    'SVGElement': TypeData(clazz='Interface', custom_to_dart=True),

    'ClientRectList': TypeData(clazz='Interface',
        item_type='ClientRect', suppress_interface=True),
    'CSSRuleList': TypeData(clazz='Interface',
        item_type='CSSRule', suppress_interface=True),
    'CSSValueList': TypeData(clazz='Interface',
        item_type='CSSValue', suppress_interface=True),
    'DOMMimeTypeArray': TypeData(clazz='Interface', item_type='DOMMimeType'),
    'DOMPluginArray': TypeData(clazz='Interface', item_type='DOMPlugin'),
    'DOMStringList': TypeData(clazz='Interface', item_type='DOMString',
        dart_type='List<String>', custom_to_native=True),
    'EntryArray': TypeData(clazz='Interface', item_type='Entry',
        suppress_interface=True),
    'EntryArraySync': TypeData(clazz='Interface', item_type='EntrySync',
        suppress_interface=True),
    'FileList': TypeData(clazz='Interface', item_type='File',
        dart_type='List<File>'),
    'GamepadList': TypeData(clazz='Interface', item_type='Gamepad',
        suppress_interface=True),
    'HTMLAllCollection': TypeData(clazz='Interface', item_type='Node'),
    'HTMLCollection': TypeData(clazz='Interface', item_type='Node'),
    'MediaStreamList': TypeData(clazz='Interface',
        item_type='MediaStream', suppress_interface=True),
    'NamedNodeMap': TypeData(clazz='Interface', item_type='Node'),
    'NodeList': TypeData(clazz='Interface', item_type='Node',
                         suppress_interface=False, dart_type='List<Node>'),
    'SVGElementInstanceList': TypeData(clazz='Interface',
        item_type='SVGElementInstance', suppress_interface=True),
    'SourceBufferList': TypeData(clazz='Interface', item_type='SourceBuffer'),
    'SpeechGrammarList': TypeData(clazz='Interface', item_type='SpeechGrammar'),
    'SpeechInputResultList': TypeData(clazz='Interface',
        item_type='SpeechInputResult', suppress_interface=True),
    'SpeechRecognitionResultList': TypeData(clazz='Interface',
        item_type='SpeechRecognitionResult', suppress_interface=True),
    'SQLResultSetRowList': TypeData(clazz='Interface', item_type='Dictionary'),
    'StyleSheetList': TypeData(clazz='Interface',
        item_type='StyleSheet', suppress_interface=True),
    'TextTrackCueList': TypeData(clazz='Interface', item_type='TextTrackCue'),
    'TextTrackList': TypeData(clazz='Interface', item_type='TextTrack'),
    'TouchList': TypeData(clazz='Interface', item_type='Touch'),
    'WebKitAnimationList': TypeData(clazz='Interface',
        item_type='WebKitAnimation', suppress_interface=True),

    'Float32Array': TypedArrayTypeData('double'),
    'Float64Array': TypedArrayTypeData('double'),
    'Int8Array': TypedArrayTypeData('int'),
    'Int16Array': TypedArrayTypeData('int'),
    'Int32Array': TypedArrayTypeData('int'),
    'Uint8Array': TypedArrayTypeData('int'),
    'Uint8ClampedArray': TypedArrayTypeData('int'),
    'Uint16Array': TypedArrayTypeData('int'),
    'Uint32Array': TypedArrayTypeData('int'),

    'SVGAngle': TypeData(clazz='SVGTearOff'),
    'SVGLength': TypeData(clazz='SVGTearOff'),
    'SVGLengthList': TypeData(clazz='SVGTearOff', item_type='SVGLength'),
    'SVGMatrix': TypeData(clazz='SVGTearOff'),
    'SVGNumber': TypeData(clazz='SVGTearOff', native_type='SVGPropertyTearOff<float>'),
    'SVGNumberList': TypeData(clazz='SVGTearOff', item_type='SVGNumber'),
    'SVGPathSegList': TypeData(clazz='SVGTearOff', item_type='SVGPathSeg',
        native_type='SVGPathSegListPropertyTearOff'),
    'SVGPoint': TypeData(clazz='SVGTearOff', native_type='SVGPropertyTearOff<FloatPoint>'),
    'SVGPointList': TypeData(clazz='SVGTearOff'),
    'SVGPreserveAspectRatio': TypeData(clazz='SVGTearOff'),
    'SVGRect': TypeData(clazz='SVGTearOff', native_type='SVGPropertyTearOff<FloatRect>'),
    'SVGStringList': TypeData(clazz='SVGTearOff', item_type='DOMString',
        native_type='SVGStaticListPropertyTearOff<SVGStringList>'),
    'SVGTransform': TypeData(clazz='SVGTearOff'),
    'SVGTransformList': TypeData(clazz='SVGTearOff', item_type='SVGTransform',
        native_type='SVGTransformListPropertyTearOff'),
}

_svg_supplemental_includes = [
    '"SVGAnimatedPropertyTearOff.h"',
    '"SVGAnimatedListPropertyTearOff.h"',
    '"SVGStaticListPropertyTearOff.h"',
    '"SVGAnimatedListPropertyTearOff.h"',
    '"SVGTransformListPropertyTearOff.h"',
    '"SVGPathSegListPropertyTearOff.h"',
]

class TypeRegistry(object):
  def __init__(self, database, renamer=None):
    self._database = database
    self._renamer = renamer
    self._cache = {}

  def TypeInfo(self, type_name):
    if not type_name in self._cache:
      self._cache[type_name] = self._TypeInfo(type_name)
    return self._cache[type_name]

  def DartType(self, type_name):
    return self.TypeInfo(type_name).dart_type()

  def _TypeInfo(self, type_name):
    match = re.match(r'(?:sequence<(\w+)>|(\w+)\[\])$', type_name)
    if match:
      if type_name == 'DOMString[]':
        return DOMStringArrayTypeInfo(TypeData('Sequence'), self.TypeInfo('DOMString'))
      item_info = self.TypeInfo(match.group(1) or match.group(2))
      return SequenceIDLTypeInfo(type_name, TypeData('Sequence'), item_info)

    if not type_name in _idl_type_registry:
      interface = self._database.GetInterface(type_name)
      if 'Callback' in interface.ext_attrs:
        return CallbackIDLTypeInfo(type_name, TypeData('Callback',
            self._renamer.DartifyTypeName(type_name)))
      return InterfaceIDLTypeInfo(
          type_name,
          TypeData('Interface'),
          self._renamer.RenameInterface(interface),
          self)

    type_data = _idl_type_registry.get(type_name)

    if type_data.clazz == 'Interface':
      if self._database.HasInterface(type_name):
        dart_interface_name = self._renamer.RenameInterface(
            self._database.GetInterface(type_name))
      else:
        dart_interface_name = self._renamer.DartifyTypeName(type_name)
      return InterfaceIDLTypeInfo(type_name, type_data, dart_interface_name,
                                  self)

    if type_data.clazz == 'SVGTearOff':
      dart_interface_name = self._renamer.RenameInterface(
          self._database.GetInterface(type_name))
      return SVGTearOffIDLTypeInfo(
          type_name, type_data, dart_interface_name, self)

    class_name = '%sIDLTypeInfo' % type_data.clazz
    return globals()[class_name](type_name, type_data)
