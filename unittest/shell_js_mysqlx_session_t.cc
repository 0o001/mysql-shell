/*
* Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License as
* published by the Free Software Foundation; version 2 of the
* License.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
* 02110-1301  USA
*/

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "gtest/gtest.h"
#include "test_utils.h"
#include "base_session.h"

namespace shcore {
  class Shell_js_mysqlx_session_tests : public Shell_core_test_wrapper
  {
  protected:
    // You can define per-test set-up and tear-down logic as usual.
    virtual void SetUp()
    {
      Shell_core_test_wrapper::SetUp();

      bool initilaized(false);
      _shell_core->switch_mode(Shell_core::Mode_JScript, initilaized);
    }
  };

  // Tests session.getDefaultSchema()
  TEST_F(Shell_js_mysqlx_session_tests, mysqlx_base_session_get_uri)
  {
    exec_and_out_equals("var mysqlx = require('mysqlx').mysqlx;");

    std::string uri = mysh::strip_password(_uri);

    exec_and_out_equals("var session = mysqlx.getSession('" + _uri + "');");

    exec_and_out_equals("print(session.getUri());", uri);

    exec_and_out_equals("session.close();");
  }

  // Tests session.getDefaultSchema()
  TEST_F(Shell_js_mysqlx_session_tests, mysqlx_base_session_uri)
  {
    exec_and_out_equals("var mysqlx = require('mysqlx').mysqlx;");

    std::string uri = mysh::strip_password(_uri);

    exec_and_out_equals("var session = mysqlx.getSession('" + _uri + "');");

    exec_and_out_equals("print(session.uri);", uri);

    exec_and_out_equals("session.close();");
  }

  // Tests session.getDefaultSchema()
  TEST_F(Shell_js_mysqlx_session_tests, mysqlx_base_session_get_default_schema)
  {
    {
      SCOPED_TRACE("retrieving the default schema for first time");
      exec_and_out_equals("var mysqlx = require('mysqlx').mysqlx;");

      exec_and_out_equals("var session = mysqlx.getNodeSession('" + _uri + "');");

      // Attempts to get the default schema
      exec_and_out_equals("var schema = session.getDefaultSchema();");

      exec_and_out_equals("print(schema);", "null");
    };

    {
      SCOPED_TRACE("Setting/getting default schema.");
      exec_and_out_equals("session.setDefaultSchema('mysql');");

      // Now uses the formal method
      exec_and_out_equals("var schema = session.getDefaultSchema();");
      exec_and_out_equals("print(schema);", "<Schema:mysql>");
    };

    {
      // Sets a different default database
      exec_and_out_equals("session.setDefaultSchema('information_schema');");

      // Now uses the formal method
      exec_and_out_equals("var schema = session.getDefaultSchema();");
      exec_and_out_equals("print(schema);", "<Schema:information_schema>");
    };

    exec_and_out_equals("session.close();");
  }

  // Tests session.defaultSchema
  TEST_F(Shell_js_mysqlx_session_tests, mysqlx_base_session_default_schema)
  {
    {
      SCOPED_TRACE("retrieving the default schema for first time");
      exec_and_out_equals("var mysqlx = require('mysqlx').mysqlx;");

      exec_and_out_equals("var session = mysqlx.getNodeSession('" + _uri + "');");

      // Attempts to get the default schema
      exec_and_out_equals("print(session.defaultSchema);", "null");
    };

    {
      SCOPED_TRACE("Setting/getting default schema.");
      exec_and_out_equals("session.setDefaultSchema('mysql');");

      // Now uses the formal method
      exec_and_out_equals("print(session.defaultSchema);", "<Schema:mysql>");
    };

    {
      // Sets a different default database
      exec_and_out_equals("session.setDefaultSchema('information_schema');");

      // Now uses the formal method
      exec_and_out_equals("print(session.defaultSchema);", "<Schema:information_schema>");
    };

    exec_and_out_equals("session.close();");
  }

  // Tests session.getSchemas()
  TEST_F(Shell_js_mysqlx_session_tests, mysqlx_base_session_get_schemas)
  {
    exec_and_out_equals("var mysqlx = require('mysqlx').mysqlx;");

    exec_and_out_equals("var session = mysqlx.getNodeSession('" + _uri + "');");

    // Triggers the schema load
    exec_and_out_equals("var schemas = session.getSchemas();");

    // Now checks the objects can be accessed directly
    // because has been already set as a session attributes
    exec_and_out_equals("print(schemas.mysql);", "<Schema:mysql>");
    exec_and_out_equals("print(schemas.information_schema);", "<Schema:information_schema>");

    exec_and_out_equals("session.close();");
  }

  // Tests session.schemas
  TEST_F(Shell_js_mysqlx_session_tests, mysqlx_base_session_schemas)
  {
    exec_and_out_equals("var mysqlx = require('mysqlx').mysqlx;");

    exec_and_out_equals("var session = mysqlx.getNodeSession('" + _uri + "');");

    // Ensures the schemas have not been loaded
    exec_and_out_equals("print(session.schemas.mysql);", "<Schema:mysql>");
    exec_and_out_equals("print(session.schemas.information_schema)", "<Schema:information_schema>");

    exec_and_out_equals("session.close();");
  }

  // Tests session.getSchema()
  TEST_F(Shell_js_mysqlx_session_tests, mysqlx_base_session_get_schema)
  {
    exec_and_out_equals("var mysqlx = require('mysqlx').mysqlx;");

    exec_and_out_equals("var session = mysqlx.getSession('" + _uri + "');");

    // Checks schema retrieval
    exec_and_out_equals("var schema = session.getSchema('mysql');");
    exec_and_out_equals("print(schema);", "<Schema:mysql>");

    exec_and_out_equals("var schema = session.getSchema('information_schema');");
    exec_and_out_equals("print(schema);", "<Schema:information_schema>");

    // Checks schema retrieval with invalid schema
    exec_and_out_contains("var schema = session.getSchema('unexisting_schema');", "", "Unknown database 'unexisting_schema'");

    exec_and_out_equals("session.close();");
  }

  // Tests session.<schema>
  TEST_F(Shell_js_mysqlx_session_tests, mysqlx_base_session_schema)
  {
    exec_and_out_equals("var mysqlx = require('mysqlx').mysqlx;");

    exec_and_out_equals("var session = mysqlx.getSession('" + _uri + "');");

    // Now direct and indirect access
    exec_and_out_equals("print(session.mysql);", "<Schema:mysql>");

    exec_and_out_equals("print(session.information_schema);", "<Schema:information_schema>");

    // TODO: add test case with schema that is named as another property

    // Now direct and indirect access
    exec_and_out_contains("print(session.unexisting_schema);;", "", "Unknown database 'unexisting_schema'");

    exec_and_out_equals("session.close();");
  }

  // Tests session.<schema>
  TEST_F(Shell_js_mysqlx_session_tests, set_fetch_warnings)
  {
    exec_and_out_equals("var mysqlx = require('mysqlx').mysqlx;");

    exec_and_out_equals("var session = mysqlx.getNodeSession('" + _uri + "');");

    // Now direct and indirect access
    exec_and_out_equals("session.setFetchWarnings(true);");

    exec_and_out_equals("var result = session.sql('drop database if exists unexisting;').execute();");

    exec_and_out_equals("print(result.warningCount);", "1");

    exec_and_out_equals("print(result.warnings.length);", "1");

    exec_and_out_equals("print(result.getWarnings().length);", "1");

    exec_and_out_contains("print(result.warnings[0].Message);", "Can't drop database 'unexisting'; database doesn't exist");

    exec_and_out_equals("session.setFetchWarnings(false);");

    exec_and_out_equals("var result = session.sql('drop database if exists unexisting;').execute();");

    exec_and_out_equals("print(result.warningCount);", "0");

    exec_and_out_equals("print(result.warnings.length);", "0");

    exec_and_out_equals("print(result.getWarnings().length);", "0");

    exec_and_out_equals("session.close();");
  }
}