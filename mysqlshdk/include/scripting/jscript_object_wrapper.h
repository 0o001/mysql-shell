/*
 * Copyright (c) 2014, 2017, Oracle and/or its affiliates. All rights reserved.
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

// Provides a generic wrapper for shcore::Object_bridge objects so that they
// can be used from JavaScript

// new_instance() can be used to create a JS object instance per class

#ifndef _JSCRIPT_OBJECT_WRAPPER_H_
#define _JSCRIPT_OBJECT_WRAPPER_H_

#include <memory>
#include <string>
#include "scripting/types.h"
#include "scripting/include_v8.h"

namespace shcore {
class JScript_context;

class JScript_method_wrapper {
 public:
  explicit JScript_method_wrapper(JScript_context* context);
  ~JScript_method_wrapper();

  v8::Handle<v8::Object> wrap(std::shared_ptr<Object_bridge> object,
                              const std::string& method);

 private:
  struct Collectable;
  static void call(const v8::FunctionCallbackInfo<v8::Value>& args);

  static void wrapper_deleted(
      const v8::WeakCallbackData<v8::Object, Collectable>& data);

 private:
  JScript_context* _context;
  v8::Persistent<v8::ObjectTemplate> _object_template;
};

class JScript_object_wrapper {
public:
  JScript_object_wrapper(JScript_context *context, bool indexed = false);
  ~JScript_object_wrapper();

  v8::Handle<v8::Object> wrap(std::shared_ptr<Object_bridge> object);

  static bool unwrap(v8::Handle<v8::Object> value, std::shared_ptr<Object_bridge> &ret_object);

  static bool is_object(v8::Handle<v8::Object> value);
  static bool is_method(v8::Handle<v8::Object> value);

private:
  struct Collectable;
  static void handler_getter(v8::Local<v8::String> property, const v8::PropertyCallbackInfo<v8::Value>& info);
  static void handler_setter(v8::Local<v8::String> property, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<v8::Value>& info);
  static void handler_enumerator(const v8::PropertyCallbackInfo<v8::Array>& info);
  static void handler_query(v8::Local<v8::String> property, const v8::PropertyCallbackInfo<v8::Integer>& info);

  static void handler_igetter(uint32_t i, const v8::PropertyCallbackInfo<v8::Value>& info);
  static void handler_isetter(uint32_t i, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<v8::Value>& info);
  static void handler_ienumerator(const v8::PropertyCallbackInfo<v8::Array>& info);

  static void wrapper_deleted(const v8::WeakCallbackData<v8::Object, Collectable>& data);

private:
  JScript_context *_context;
  v8::Persistent<v8::ObjectTemplate> _object_template;
  JScript_method_wrapper _method_wrapper;
};
}  // namespace shcore

#endif