library IndexedDB1Test;
import '../../pkg/unittest/lib/unittest.dart';
import '../../pkg/unittest/lib/html_config.dart';
import 'dart:html' as html;
import 'dart:indexed_db' as idb;
import 'dart:collection';
import 'utils.dart';

// Write and re-read Maps: simple Maps; Maps with DAGs; Maps with cycles.

const String DB_NAME = 'Test';
const String STORE_NAME = 'TEST';
const int VERSION = 1;

testReadWrite(key, value, check,
              [dbName = DB_NAME,
               storeName = STORE_NAME,
               version = VERSION]) => () {
  var db;

  fail(e) {
    guardAsync(() {
      throw new Exception('IndexedDB failure');
    });
  }

  createObjectStore(db) {
    var store = db.createObjectStore(storeName);
    expect(store, isNotNull);
  }

  step2(e) {
    var transaction = db.transaction(storeName, 'readonly');
    var request = transaction.objectStore(storeName).getObject(key);
    request.onSuccess.listen(expectAsync1((e) {
      var object = e.target.result;
      db.close();
      check(value, object);
    }));
    request.onError.listen(fail);
  }

  step1() {
    var transaction = db.transaction([storeName], 'readwrite');
    var request = transaction.objectStore(storeName).put(value, key);
    request.onSuccess.listen(expectAsync1(step2));
    request.onError.listen(fail);
  }

  initDb(e) {
    db = e.target.result;
    if (version != db.version) {
      // Legacy 'setVersion' upgrade protocol.
      var request = db.setVersion('$version');
      request.onSuccess.listen(
        expectAsync1((e) {
          createObjectStore(db);
          var transaction = e.target.result;
          transaction.onComplete.listen(expectAsync1((e) => step1()));
          transaction.onError.listen(fail);
        })
      );
      request.onError.listen(fail);
    } else {
      step1();
    }
  }

  openDb(e) {
    var request = html.window.indexedDB.open(dbName, version);
    expect(request, isNotNull);
    request.onSuccess.listen(expectAsync1(initDb));
    request.onError.listen(fail);
    if (request is idb.OpenDBRequest) {
      // New upgrade protocol.  Old API has no 'upgradeNeeded' and uses
      // setVersion instead.
      request.onUpgradeNeeded.listen((e) {
          guardAsync(() {
              createObjectStore(e.target.result);
            });
        });
    }
  }

  // Delete any existing DB.
  var deleteRequest = html.window.indexedDB.deleteDatabase(dbName);
  deleteRequest.onSuccess.listen(expectAsync1(openDb));
  deleteRequest.onError.listen(fail);
};


main() {
  useHtmlConfiguration();

  var obj1 = {'a': 100, 'b': 's'};
  var obj2 = {'x': obj1, 'y': obj1};  // DAG.

  var obj3 = {};
  obj3['a'] = 100;
  obj3['b'] = obj3;  // Cycle.

  var obj4 = new SplayTreeMap<String, dynamic>();  // Different implementation.
  obj4['a'] = 100;
  obj4['b'] = 's';

  var cyclic_list = [1, 2, 3];
  cyclic_list[1] = cyclic_list;

  go(name, data) => test(name, testReadWrite(123, data, verifyGraph));

  test('test_verifyGraph', () {
      // Nice to know verifyGraph is working before we rely on it.
      verifyGraph(obj4, obj4);
      verifyGraph(obj1, new Map.from(obj1));
      verifyGraph(obj4, new Map.from(obj4));

      var l1 = [1,2,3];
      var l2 = [const [1, 2, 3], const [1, 2, 3]];
      verifyGraph([l1, l1], l2);
      expect(() => verifyGraph([[1, 2, 3], [1, 2, 3]], l2), throws);

      verifyGraph(cyclic_list, cyclic_list);
    });

  // Don't bother with these tests if it's unsupported.
  // Support is tested in indexeddb_1_test
  if (idb.IdbFactory.supported) {
    go('test_simple', obj1);
    go('test_DAG', obj2);
    go('test_cycle', obj3);
    go('test_simple_splay', obj4);
    go('const_array_1', const [const [1], const [2]]);
    go('const_array_dag', const [const [1], const [1]]);
    go('array_deferred_copy', [1,2,3, obj3, obj3, 6]);
    go('array_deferred_copy_2', [1,2,3, [4, 5, obj3], [obj3, 6]]);
    go('cyclic_list', cyclic_list);
  }
}
