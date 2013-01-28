library WebGL1Test;
import '../../pkg/unittest/lib/unittest.dart';
import '../../pkg/unittest/lib/html_config.dart';
import 'dart:html';

// Test that WebGL is present in dart:html API

main() {
  useHtmlConfiguration();

  test('simple', () {
      var canvas = document.createElement("canvas");
      var gl = canvas.getContext("experimental-webgl");
      var shader = gl.createShader(WebGLRenderingContext.VERTEX_SHADER);
      gl.shaderSource(shader, "void main() { }");
      gl.compileShader(shader);
      var success =
          gl.getShaderParameter(shader, WebGLRenderingContext.COMPILE_STATUS);
      expect(success, isTrue);
  });
}
