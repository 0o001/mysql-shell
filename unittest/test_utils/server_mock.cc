/* Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include <vector>
#include "unittest/test_utils/server_mock.h"
#include "unittest/test_utils/shell_base_test.h"
#include "mysqlshdk/libs/db/column.h"
#include "utils/utils_general.h"
#include "utils/utils_json.h"
#include "utils/utils_file.h"
#include <fstream>
#include <random>

namespace tests {

// TODO(rennox) This function should be deleted and a UUID should be used
// instead
std::string random_json_name(std::string::size_type length)
{
  std::string alphanum =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::random_device seed;
  std::mt19937 rng{seed()};
  std::uniform_int_distribution<std::string::size_type> dist(0, alphanum.size() - 1);

  std::string result;
  result.reserve(length);
  while (length--)
    result += alphanum[dist(rng)];

  return result + ".json";
}

Server_mock::Server_mock():_server_listening(false) {
}

std::string Server_mock::create_data_file(const std::vector<testing::Fake_result_data> &data) {
  shcore::JSON_dumper dumper;

  dumper.start_object();
  dumper.append_string("stmts");
  dumper.start_array();

  for (auto result : data) {
    dumper.start_object();
    dumper.append_string("stmt");
    dumper.append_string(result.sql);
    dumper.append_string("result");
    dumper.start_object();
    dumper.append_string("columns");
    dumper.start_array();
    for (size_t index = 0; index < result.names.size(); index++) {
      dumper.start_object();
      dumper.append_string("type");
      dumper.append_string(map_column_type(result.types[index]));
      dumper.append_string("name");
      dumper.append_string(result.names[index]);
      dumper.end_object();
    }
    dumper.end_array();

    dumper.append_string("rows");
    dumper.start_array();
    for (auto row : result.rows) {
      dumper.start_array();
      for(size_t field_index=0; field_index < row.size(); field_index++) {
        auto type = map_column_type(result.types[field_index]);
        if (type == "STRING")
          dumper.append_string(row[field_index]);
        else
          dumper.append_int64(std::stoi(row[field_index]));
      }
      dumper.end_array();
    }
    dumper.end_array();

    dumper.end_object();

    dumper.end_object();
  }

  dumper.end_array();
  dumper.end_object();

  std::string prefix = shcore::get_binary_folder();

#ifdef _WIN32
  std::string name = prefix + "\\" + random_json_name(15);
#else
  std::string name = prefix + "/" + random_json_name(15);
#endif

  if (!shcore::create_file(name, dumper.str()))
    throw std::runtime_error("Error creating Mock Server data file");

  return name;
}

std::string Server_mock::map_column_type(mysqlshdk::db::Type type) {
  switch (type) {
    case mysqlshdk::db::Type::Null:
      return "null";
    case mysqlshdk::db::Type::Date:
    case mysqlshdk::db::Type::NewDate:
    case mysqlshdk::db::Type::Time:
    case mysqlshdk::db::Type::VarChar:
    case mysqlshdk::db::Type::String:
    case mysqlshdk::db::Type::VarString:
    case mysqlshdk::db::Type::TinyBlob:
    case mysqlshdk::db::Type::MediumBlob:
    case mysqlshdk::db::Type::LongBlob:
    case mysqlshdk::db::Type::Blob:
    case mysqlshdk::db::Type::Geometry:
    case mysqlshdk::db::Type::Json:
    case mysqlshdk::db::Type::DateTime:
    case mysqlshdk::db::Type::Timestamp:
    case mysqlshdk::db::Type::Enum:
    case mysqlshdk::db::Type::Set:
      return "STRING";
    case mysqlshdk::db::Type::NewDecimal:
    case mysqlshdk::db::Type::Float:
    case mysqlshdk::db::Type::LongLong:
    case mysqlshdk::db::Type::Double:
      return "LONGLONG";
    case mysqlshdk::db::Type::Decimal:
    case mysqlshdk::db::Type::Year:
    case mysqlshdk::db::Type::Short:
    case mysqlshdk::db::Type::Int24:
    case mysqlshdk::db::Type::Long:
      return "LONG";
    case mysqlshdk::db::Type::Tiny:
    case mysqlshdk::db::Type::Bit:
      return "TINY";
  }

  throw std::runtime_error("Invalid column type found");

  return "";
}

std::string Server_mock::get_path_to_binary() {

  std::string command;

  std::string prefix = shcore::get_binary_folder();

#ifdef _WIN32
  command = prefix + "\\" + "mysql_server_mock.exe";
#else
  command = prefix + "/" + "mysql_server_mock";
#endif

  return command;
}

void Server_mock::start(int port, const std::vector<testing::Fake_result_data> &data) {
  std::string binary_path = get_path_to_binary();
  std::string data_path = create_data_file(data);
  std::string strport = std::to_string(port);

  std::vector<const char *> args = {
    binary_path.c_str(),
    data_path.c_str(),
    strport.c_str(),
    NULL
  };

  _thread = std::shared_ptr<std::thread>(
    new std::thread([this, args](){
      try {
        _server.lock();

        _process.reset(new shcore::Process_launcher(&args[0]));
        _process->start();

        char c;
        while (_process->read(&c, 1) > 0) {
          _server_output += c;
          if (_server_output.find("Starting to handle connections") !=
              std::string::npos) {
            if (!_server_listening) {
              _server_listening = true;
              _server.unlock();
            }
          }
        }

        _process->wait();

        // If the server is not listening, it is still locked
        if (!_server_listening)
          _server.unlock();
      }
      catch (const std::exception& e) {
        std::cout << e.what() << std::endl;
      }
    }));

  // This delay is required to guarantee the _server is locked first on the
  // mock server thread
#ifdef _WIN32
  Sleep(5);
#else
  usleep(5000);
#endif
  _server.lock();

  // Deletes the temporary data file
  shcore::delete_file(data_path);

  if (!_server_listening)
    throw std::runtime_error(_server_output);
  _server.unlock();
}

void Server_mock::stop() {
  _thread->join();
}

}
