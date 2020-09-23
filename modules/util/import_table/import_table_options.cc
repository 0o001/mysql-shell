/*
 * Copyright (c) 2018, 2020, Oracle and/or its affiliates.
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

#include "modules/util/import_table/import_table_options.h"

#include <errno.h>
#include <algorithm>
#include <limits>
#include <stack>
#include <utility>

#include "modules/mod_utils.h"
#include "modules/util/import_table/helpers.h"
#include "mysqlshdk/include/scripting/types.h"
#include "mysqlshdk/include/shellcore/base_session.h"
#include "mysqlshdk/include/shellcore/console.h"
#include "mysqlshdk/include/shellcore/shell_options.h"
#include "mysqlshdk/libs/db/connection_options.h"
#include "mysqlshdk/libs/oci/oci_options.h"
#include "mysqlshdk/libs/storage/backend/oci_object_storage.h"
#include "mysqlshdk/libs/storage/compressed_file.h"
#include "mysqlshdk/libs/storage/ifile.h"
#include "mysqlshdk/libs/utils/strformat.h"
#include "mysqlshdk/libs/utils/utils_file.h"
#include "mysqlshdk/libs/utils/utils_path.h"

namespace {
template <typename FwdIter>
FwdIter consume_string(FwdIter first, FwdIter last, const char qchar,
                       const char escchar) {
  if (first == last) return last;
  if (*first != qchar) return last;
  ++first;
  while (first != last) {
    if (*first == qchar) {
      return first;
    } else if (*first == escchar) {
      ++first;
      if (first == last) {
        return last;
      }
    }
    ++first;
  }
  return last;
}

bool transformation_validation(const std::string &expr) {
  std::stack<char> s;
  auto first = expr.begin();
  auto last = expr.end();
  while (first != last) {
    switch (*first) {
      case '[':
      case '(':
      case '{':
        s.push(*first);
        break;
      case ']':
        if (s.empty() || s.top() != '[') {
          return false;
        }
        s.pop();
        break;
      case ')':
        if (s.empty() || s.top() != '(') {
          return false;
        }
        s.pop();
        break;
      case '}':
        if (s.empty() || s.top() != '{') {
          return false;
        }
        s.pop();
        break;
      case '\'':
      case '"':
      case '`':
        first = consume_string(first, last, *first, '\\');
        if (first == last) return false;
        break;
    }
    ++first;
  }
  return s.empty();
}

bool has_wildcard(const std::string &s) {
  return s.find('*') != std::string::npos || s.find('?') != std::string::npos;
}
}  // namespace

namespace mysqlsh {
namespace import_table {

Import_table_options::Import_table_options(std::vector<std::string> &&filenames,
                                           const shcore::Dictionary_t &options)
    : m_filelist_from_user(std::move(filenames)),
      m_oci_options(mysqlshdk::oci::Oci_options::Unpack_target::
                        OBJECT_STORAGE_NO_PAR_OPTIONS) {
  if (!is_multifile()) {
    m_table = std::get<0>(shcore::path::split_extension(
        shcore::path::basename(m_filelist_from_user[0])));
  }
  unpack(options);
}

Import_table_options::Import_table_options(const shcore::Dictionary_t &options)
    : m_oci_options(mysqlshdk::oci::Oci_options::Unpack_target::
                        OBJECT_STORAGE_NO_PAR_OPTIONS) {
  unpack(options);
}

void Import_table_options::validate() {
  if (m_table.empty()) {
    throw shcore::Exception::runtime_error(
        "Target table is not set. The target table for the import operation "
        "must be provided in the options.");
  }

  m_dialect.validate();

  // remove empty paths provided by user
  m_filelist_from_user.erase(
      std::remove_if(m_filelist_from_user.begin(), m_filelist_from_user.end(),
                     [](const auto &s) { return s.empty(); }),
      m_filelist_from_user.end());

  if (m_filelist_from_user.empty()) {
    throw shcore::Exception::runtime_error("File list cannot be empty.");
  }

  if (m_schema.empty()) {
    auto res = m_base_session->query("SELECT schema()");
    auto row = res->fetch_one_or_throw();
    m_schema = row->is_null(0) ? "" : row->get_string(0);
    if (m_schema.empty()) {
      throw std::runtime_error(
          "There is no active schema on the current session, the target "
          "schema for the import operation must be provided in the options.");
    }
  }

  {
    auto result =
        m_base_session->query("SHOW GLOBAL VARIABLES LIKE 'local_infile'");
    auto row = result->fetch_one();
    auto local_infile_value = row->get_string(1);

    if (shcore::str_caseeq(local_infile_value, "off")) {
      mysqlsh::current_console()->print_error(
          "The 'local_infile' global system variable must be set to ON in "
          "the target server, after the server is verified to be trusted.");
      throw shcore::Exception::runtime_error("Invalid preconditions");
    }
  }

  if (m_oci_options) {
    m_oci_options.check_option_values();
  }

  if (!is_multifile()) {
    if (!m_filelist_from_user[0].empty()) {
      if (m_oci_options) {
        // this call is here to verify if filename does not have a scheme
        mysqlshdk::oci::parse_oci_options(m_filelist_from_user[0], {},
                                          &m_oci_options);
        m_oci_options.check_option_values();
      }

      m_file_handle = create_file_handle(m_filelist_from_user[0]);
      m_file_handle->open(mysqlshdk::storage::Mode::READ);
      m_full_path = m_file_handle->full_path();
      m_file_size = m_file_handle->file_size();
      m_file_handle->close();
    }
  }
  m_threads_size = calc_thread_size();
}

bool Import_table_options::is_multifile() const {
  if (m_filelist_from_user.size() == 1) {
    if (has_wildcard(m_filelist_from_user[0])) {
      return true;
    }
    // const auto &extension = std::get<1>(shcore::path::split_extension(path));
    // if (extension == ".gz" || extension == ".zst") {
    //   return true;
    // }
    return false;
  }
  return true;
}

std::unique_ptr<mysqlshdk::storage::IFile>
Import_table_options::create_file_handle(const std::string &filepath) const {
  std::unique_ptr<mysqlshdk::storage::IFile> file;

  if (!m_oci_options.os_bucket_name.is_null()) {
    file = mysqlshdk::storage::make_file(filepath, m_oci_options);
  } else {
    file = mysqlshdk::storage::make_file(filepath);
  }
  return create_file_handle(std::move(file));
}

std::unique_ptr<mysqlshdk::storage::IFile>
Import_table_options::create_file_handle(
    std::unique_ptr<mysqlshdk::storage::IFile> file_handler) const {
  mysqlshdk::storage::Compression compr;
  try {
    compr = mysqlshdk::storage::from_extension(
        std::get<1>(shcore::path::split_extension(file_handler->filename())));
  } catch (...) {
    compr = mysqlshdk::storage::Compression::NONE;
  }
  return mysqlshdk::storage::make_file(std::move(file_handler), compr);
}

size_t Import_table_options::calc_thread_size() {
  // We need at least one thread
  int64_t threads_size = std::max(static_cast<int64_t>(1), m_threads_size);

  if (!is_multifile()) {
    // We do not need to spawn more threads than file chunks
    const size_t calculated_threads = (m_file_size / bytes_per_chunk()) + 1;
    if (calculated_threads <
        static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
      threads_size =
          std::min(threads_size, static_cast<int64_t>(calculated_threads));
    }
  }
  return threads_size;
}

size_t Import_table_options::max_rate() const {
  if (!m_max_rate.empty()) {
    return std::max(static_cast<size_t>(0),
                    mysqlshdk::utils::expand_to_bytes(m_max_rate));
  }
  return 0;
}

Connection_options Import_table_options::connection_options() const {
  Connection_options connection_options =
      m_base_session->get_connection_options();

  if (connection_options.has(mysqlshdk::db::kLocalInfile)) {
    connection_options.remove(mysqlshdk::db::kLocalInfile);
  }
  connection_options.set(mysqlshdk::db::kLocalInfile, "true");

  // Set long timeouts by default
  const std::string timeout =
      std::to_string(24 * 3600 * 1000);  // 1 day in milliseconds
  if (!connection_options.has(mysqlshdk::db::kNetReadTimeout)) {
    connection_options.set(mysqlshdk::db::kNetReadTimeout, timeout);
  }
  if (!connection_options.has(mysqlshdk::db::kNetWriteTimeout)) {
    connection_options.set(mysqlshdk::db::kNetWriteTimeout, timeout);
  }

  return connection_options;
}

size_t Import_table_options::bytes_per_chunk() const {
  constexpr const size_t min_bytes_per_chunk = 2 * BUFFER_SIZE;
  return std::max(mysqlshdk::utils::expand_to_bytes(m_bytes_per_chunk),
                  min_bytes_per_chunk);
}

std::string Import_table_options::target_import_info() const {
  auto connection_options = m_base_session->get_connection_options();

  if (!is_multifile()) {
    std::string info_msg = "Importing from file '" + full_path() +
                           "' to table `" + schema() + "`.`" + table() +
                           "` in MySQL Server at " +
                           connection_options.as_uri(
                               mysqlshdk::db::uri::formats::only_transport()) +
                           " using " + std::to_string(threads_size());
    info_msg += threads_size() == 1 ? " thread" : " threads";
    return info_msg;
  }
  std::string info_msg =
      "Importing from multiple files to table `" + schema() + "`.`" + table() +
      "` in MySQL Server at " +
      connection_options.as_uri(mysqlshdk::db::uri::formats::only_transport()) +
      " using " + std::to_string(threads_size());
  info_msg += threads_size() == 1 ? " thread" : " threads";
  return info_msg;
}

void Import_table_options::unpack(const shcore::Dictionary_t &options) {
  auto unpack_options = Unpack_options(options);

  unpack_options.unpack(&m_oci_options);

  m_dialect = Dialect::unpack(&unpack_options);
  shcore::Dictionary_t decode_columns = nullptr;

  unpack_options.optional("table", &m_table)
      .optional("schema", &m_schema)
      .optional("threads", &m_threads_size)
      .optional("columns", &m_columns)
      .optional("replaceDuplicates", &m_replace_duplicates)
      .optional("maxRate", &m_max_rate)
      .optional("showProgress", &m_show_progress)
      .optional("skipRows", &m_skip_rows_count)
      .optional("decodeColumns", &decode_columns)
      .optional("characterSet", &m_character_set);

  if (!is_multifile()) {
    unpack_options.optional("bytesPerChunk", &m_bytes_per_chunk);
  }

  unpack_options.end();

  if (decode_columns) {
    for (const auto &it : *decode_columns) {
      if (it.second.type != shcore::Null) {
        auto transformation = it.second.descr();
        if (shcore::str_caseeq(transformation, std::string{"UNHEX"}) ||
            shcore::str_caseeq(transformation, std::string{"FROM_BASE64"})) {
          m_decode_columns[it.first] = shcore::str_upper(transformation);
        } else {
          // Try to initially validate user input, i.e. check if brackets are
          // balanced in provided user input.
          if (!transformation_validation(transformation)) {
            throw std::runtime_error(
                "Invalid SQL expression in decodeColumns option for column '" +
                it.first + "'");
          }
          m_decode_columns[it.first] = std::move(transformation);
        }
      }
    }
  }
}

}  // namespace import_table
}  // namespace mysqlsh
