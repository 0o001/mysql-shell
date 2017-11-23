// Assumptions: ensure_schema_does_not_exist is available
// Assumes __uripwd is defined as <user>:<pwd>@<host>:<plugin_port>
// validateMemer and validateNotMember are defined on the setup script
var mysqlx = require('mysqlx');

//@ Session: validating members
var mySession = mysqlx.getSession(__uripwd);
var mySessionMembers = dir(mySession);
validateMember(mySessionMembers, 'close');
validateMember(mySessionMembers, 'createSchema');
validateMember(mySessionMembers, 'getCurrentSchema');
validateMember(mySessionMembers, 'getDefaultSchema');
validateMember(mySessionMembers, 'getSchema');
validateMember(mySessionMembers, 'getSchemas');
validateMember(mySessionMembers, 'getUri');
validateMember(mySessionMembers, 'setCurrentSchema');
validateMember(mySessionMembers, 'setFetchWarnings');
validateMember(mySessionMembers, 'sql');
validateMember(mySessionMembers, 'defaultSchema');
validateMember(mySessionMembers, 'uri');
validateMember(mySessionMembers, 'currentSchema');
validateMember(mySessionMembers, 'setSavepoint');
validateMember(mySessionMembers, 'releaseSavepoint');
validateMember(mySessionMembers, 'rollbackTo');

//@ Session: accessing Schemas
var schemas = mySession.getSchemas();
print(getSchemaFromList(schemas, 'mysql'));
print(getSchemaFromList(schemas, 'information_schema'));

//@ Session: accessing individual schema
var schema;
schema = mySession.getSchema('mysql');
print(schema.name);
schema = mySession.getSchema('information_schema');
print(schema.name);

//@ Session: accessing unexisting schema
schema = mySession.getSchema('unexisting_schema');

//@ Session: current schema validations: nodefault
var dschema;
var cschema;
dschema = mySession.getDefaultSchema();
cschema = mySession.getCurrentSchema();
print(dschema);
print(cschema);

//@ Session: create schema success
ensure_schema_does_not_exist(mySession, 'node_session_schema');

ss = mySession.createSchema('node_session_schema');
print(ss)

//@ Session: create schema failure
var sf = mySession.createSchema('node_session_schema');

//@ Session: Transaction handling: rollback
var collection = ss.createCollection('sample');
mySession.startTransaction();
var res1 = collection.add({ name: 'john', age: 15 }).execute();
var res2 = collection.add({ name: 'carol', age: 16 }).execute();
var res3 = collection.add({ name: 'alma', age: 17 }).execute();
mySession.rollback();

var result = collection.find().execute();
print('Inserted Documents:', result.fetchAll().length);

//@ Session: Transaction handling: commit
mySession.startTransaction();
var res1 = collection.add({ name: 'john', age: 15 }).execute();
var res2 = collection.add({ name: 'carol', age: 16 }).execute();
var res3 = collection.add({ name: 'alma', age: 17 }).execute();
mySession.commit();

var result = collection.find().execute();
print('Inserted Documents:', result.fetchAll().length);


//@ Transaction Savepoints Initialization
mySession.dropSchema('testSP');
var schema = mySession.createSchema('testSP');
var coll = schema.createCollection('collSP');

//@# Savepoint Error Conditions (WL10869-ET1_2)
var sp = mySession.setSavepoint(null);
var sp = mySession.setSavepoint('mysql3306', 'extraParam');

//@ Create a savepoint without specifying a name (WL10869-SR1_1)
mySession.startTransaction();
coll.add({ _id: '5C514FF38144B714E7119BCF48B4CA98', name : 'test1'});
var sp = mySession.setSavepoint();
print ("Autogenerated Savepoint: " + sp);
coll.add({ _id: '5C514FF38144B714E7119BCF48B4CA99', name : 'test2'});

//@<OUT> WL10869-SR1_1: Documents
coll.find();

//@<OUT> Rollback a savepoint using a savepoint with auto generated name (WL10869-SR1_2)
mySession.rollbackTo(sp);
coll.find();

//@ Release a savepoint using a savepoint with auto generated name (WL10869-SR1_3)
var sp = mySession.setSavepoint();
print ("Autogenerated Savepoint: " + sp);
mySession.releaseSavepoint(sp);

//@ Create multiple savepoints with auto generated name and verify are unique in the session (WL10869-SR1_4)
var sp = mySession.setSavepoint();
print ("Autogenerated Savepoint: " + sp);
coll.add({ _id: '5C514FF38144B714E7119BCF48B4CBA0', name : 'test3'});

//@<OUT> WL10869-SR1_4: Documents
coll.find();

//@ Create a savepoint specifying a name (WL10869-SR1_5)
var sp = mySession.setSavepoint('mySavedPoint');
print ("Savepoint: " + sp);
coll.add({ _id: '5C514FF38144B714E7119BCF48B4CBA1', name : 'test4'});

//@<OUT> WL10869-SR1_5: Documents
coll.find();

//@<OUT> Rollback a savepoint using a savepoint created with a custom name (WL10869-SR1_6)
mySession.rollbackTo(sp);
coll.find();

//@ Release a savepoint using a savepoint with a custom name (WL10869-SR1_6)
var sp = mySession.setSavepoint('anotherSP');
print ("Savepoint: " + sp);
mySession.releaseSavepoint(sp);
var sp = mySession.rollback();

//@<OUT> Create a savepoint several times with the same name, the savepoint must be overwritten (WL10869-ET1_3)
mySession.startTransaction();
var sp = mySession.setSavepoint('anotherSP');
coll.add({ _id: '5C514FF38144B714E7119BCF48B4CBA1', name : 'test4'});
var sp = mySession.setSavepoint('anotherSP');
coll.add({ _id: '5C514FF38144B714E7119BCF48B4CBA2', name : 'test5'});
var sp = mySession.setSavepoint('anotherSP');
coll.add({ _id: '5C514FF38144B714E7119BCF48B4CBA3', name : 'test6'});
mySession.rollbackTo(sp);
coll.find();

//@ Releasing the savepoint
mySession.releaseSavepoint(sp);

//@ Release the same savepoint multiple times, an error must be thrown (WL10869-ET1_4)
mySession.releaseSavepoint(sp);

//@ Rollback a non existing savepoint, exception must be thrown (WL10869-ET1_7)
mySession.rollbackTo('unexisting');

//@ Final rollback
mySession.rollback();
coll.find();

//@<OUT> Rollback and Release a savepoint after a transaction commit, error must be thrown
mySession.startTransaction();
var sp = mySession.setSavepoint();
coll.add({ _id: '5C514FF38144B714E7119BCF48B4CBA4', name : 'test7'});
mySession.commit()
coll.find();

//@ rollbackTo after commit (WL10869-ET1_8)
mySession.rollbackTo(sp);

//@ release after commit (WL10869-ET1_8)
mySession.releaseSavepoint(sp);

//@<OUT> Rollback and Release a savepoint after a transaction rollback, error must be thrown
mySession.startTransaction();
var sp = mySession.setSavepoint();
coll.add({ _id: '5C514FF38144B714E7119BCF48B4CBA5', name : 'test8'});
mySession.rollback()
coll.find();

//@ rollbackTo after rollback (WL10869-ET1_9)
mySession.rollbackTo(sp);

//@ release after rollback (WL10869-ET1_9)
mySession.releaseSavepoint(sp);

//@ Session: test for drop schema functions
mySession.dropCollection('node_session_schema', 'coll');
mySession.dropTable('node_session_schema', 'table');
mySession.dropView('node_session_schema', 'view');

//@ Session: Testing dropping existing schema
print(mySession.dropSchema('node_session_schema'));

//@ Session: Testing if the schema is actually dropped
mySession.getSchema('node_session_schema');

//@ Session: Testing dropping non-existing schema
print(mySession.dropSchema('non_existing'));

//@ Session: current schema validations: nodefault, mysql
mySession.setCurrentSchema('mysql');
dschema = mySession.getDefaultSchema();
cschema = mySession.getCurrentSchema();
print(dschema);
print(cschema);

//@ Session: current schema validations: nodefault, information_schema
mySession.setCurrentSchema('information_schema');
dschema = mySession.getDefaultSchema();
cschema = mySession.getCurrentSchema();
print(dschema);
print(cschema);

//@ Session: current schema validations: default
mySession.close()
mySession = mysqlx.getSession(__uripwd + '/mysql');
dschema = mySession.getDefaultSchema();
cschema = mySession.getCurrentSchema();
print(dschema);
print(cschema);

//@ Session: current schema validations: default, information_schema
mySession.setCurrentSchema('information_schema');
dschema = mySession.getDefaultSchema();
cschema = mySession.getCurrentSchema();
print(dschema);
print(cschema);

//@ Session: setFetchWarnings(false)
mySession.setFetchWarnings(false);
var result = mySession.sql('drop database if exists unexisting').execute();
print(result.warningCount);

//@ Session: setFetchWarnings(true)
mySession.setFetchWarnings(true);
var result = mySession.sql('drop database if exists unexisting').execute();
print(result.warningCount);
print(result.warnings[0].message);

//@ Session: quoteName no parameters
print(mySession.quoteName());

//@ Session: quoteName wrong param type
print(mySession.quoteName(5));

//@ Session: quoteName with correct parameters
print(mySession.quoteName('sample'));
print(mySession.quoteName('sam`ple'));
print(mySession.quoteName('`sample`'));
print(mySession.quoteName('`sample'));
print(mySession.quoteName('sample`'));

//@# Session: bad params
mysqlx.getSession()
mysqlx.getSession(42)
mysqlx.getSession(["bla"])
mysqlx.getSession(null)

// Cleanup
mySession.close();
