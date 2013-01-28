library dromaeo;
import 'dart:html';
import '../common/common.dart';
import 'dart:math' as Math;
part 'Common.dart';
part 'RunnerSuite.dart';

void main() {
  final int num = 40;

  // Try to force real results.
  var ret;

  String html = document.body.innerHtml;

  new Suite(window, 'dom-query')
    .prep(() {
      html = BenchUtil.replaceAll(html, 'id="test(\\w).*?"', (Match match) {
        final group = match.group(1);
        return 'id="test${group}${num}"';
      });
      html = BenchUtil.replaceAll(html, 'name="test.*?"', (Match match) {
        return 'name="test${num}"';
      });
      html = BenchUtil.replaceAll(html, 'class="foo.*?"', (Match match) {
        return 'class="foo test${num} bar"';
      });
      final div = new Element.tag('div');
      div.innerHtml = html;
      document.body.$dom_appendChild(div);
    })
    .test('getElementById', () {
      for (int i = 0; i < num * 30; i++) {
        ret = document.$dom_getElementById('testA$num').nodeType;
        ret = document.$dom_getElementById('testB$num').nodeType;
        ret = document.$dom_getElementById('testC$num').nodeType;
        ret = document.$dom_getElementById('testD$num').nodeType;
        ret = document.$dom_getElementById('testE$num').nodeType;
        ret = document.$dom_getElementById('testF$num').nodeType;
      }
    })
    .test('getElementById (not in document)', () {
      for (int i = 0; i < num * 30; i++) {
        ret = document.$dom_getElementById('testA');
        ret = document.$dom_getElementById('testB');
        ret = document.$dom_getElementById('testC');
        ret = document.$dom_getElementById('testD');
        ret = document.$dom_getElementById('testE');
        ret = document.$dom_getElementById('testF');
      }
    })
    .test('getElementsByTagName(div)', () {
      for (int i = 0; i < num; i++) {
        List<Element> elems = document.$dom_getElementsByTagName('div');
        ret = elems.last.nodeType;
      }
    })
    .test('getElementsByTagName(p)', () {
      for (int i = 0; i < num; i++) {
        List<Element> elems = document.$dom_getElementsByTagName('p');
        ret = elems.last.nodeType;
      }
    })
    .test('getElementsByTagName(a)', () {
      for (int i = 0; i < num; i++) {
        List<Element> elems = document.$dom_getElementsByTagName('a');
        ret = elems.last.nodeType;
      }
    })
    .test('getElementsByTagName(*)', () {
      for (int i = 0; i < num; i++) {
        List<Element> elems = document.$dom_getElementsByTagName('*');
        ret = elems.last.nodeType;
      }
    })
    .test('getElementsByTagName (not in document)', () {
      for (int i = 0; i < num; i++) {
        List<Element> elems = document.$dom_getElementsByTagName('strong');
        ret = elems.length == 0;
      }
    })
    .test('getElementsByName', () {
      for (int i = 0; i < num * 20; i++) {
        List<Element> elems = document.$dom_getElementsByName('test$num');
        ret = elems[elems.length-1].nodeType;
        elems = document.$dom_getElementsByName('test$num');
        ret = elems[elems.length-1].nodeType;
        elems = document.$dom_getElementsByName('test$num');
        ret = elems[elems.length-1].nodeType;
        elems = document.$dom_getElementsByName('test$num');
        ret = elems[elems.length-1].nodeType;
      }
    })
    .test('getElementsByName (not in document)', () {
      for (int i = 0; i < num * 20; i++) {
        ret = document.$dom_getElementsByName('test').length == 0;
        ret = document.$dom_getElementsByName('test').length == 0;
        ret = document.$dom_getElementsByName('test').length == 0;
        ret = document.$dom_getElementsByName('test').length == 0;
        ret = document.$dom_getElementsByName('test').length == 0;
      }
    })
    .end();
}
