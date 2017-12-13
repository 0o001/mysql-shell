/*
 * Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2.0,
 * as published by the Free Software Foundation.
 *
 * This program is also distributed with certain software (including
 * but not limited to OpenSSL) that is licensed under separate terms, as
 * designated in a particular file or component or in included license
 * documentation.  The authors of MySQL hereby grant you an additional
 * permission to link the program and your derivative works with the
 * separately licensed software that they have included with MySQL.
 * This program is distributed in the hope that it will be useful,  but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 * the GNU General Public License, version 2.0, for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

// MySQL DB access module, for use by plugins and others
// For the module that implements interactive DB functionality see mod_db

#ifndef MODULES_DEVAPI_DYNAMIC_OBJECT_H_
#define MODULES_DEVAPI_DYNAMIC_OBJECT_H_

#include "scripting/common.h"
#include "scripting/types_cpp.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace mysqlsh {
namespace mysqlx {
#if DOXYGEN_CPP
//! Layer to provide the behavior of objects that can dynamically enable/disable
//! its functions.
#endif
class Dynamic_object : public shcore::Cpp_object_bridge {
  // Overrides to consider enabled/disabled status
  virtual std::vector<std::string> get_members() const;
  virtual shcore::Value get_member(const std::string &prop) const;
  virtual bool has_member(const std::string &prop) const;
  virtual shcore::Value call(const std::string &name,
                             const shcore::Argument_list &args);

  // T the moment will put these since we don't really care about them
  virtual bool operator==(const Object_bridge &) const { return false; }

 protected:
  // Holds the dynamic functions enable/disable status
  std::map<std::string, bool> _enabled_functions;

  // Holds relation of 'enabled' states for every dynamic function
  std::map<std::string, std::set<std::string> > _enable_paths;

  // Registers a dynamic function and it's associated 'enabled' states
  void register_dynamic_function(const std::string &name,
                                 const std::string &enable_after);

  // Enable/disable functions based on the received and registered states
  void update_functions(const std::string &state);
  void enable_function(const char *name, bool enable);

  bool is_enabled(const std::string &name) const;
};
}  // namespace mysqlx
}  // namespace mysqlsh

#endif  // MODULES_DEVAPI_DYNAMIC_OBJECT_H_