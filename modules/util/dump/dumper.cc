/*
 * Copyright (c) 2020, 2022, Oracle and/or its affiliates.
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

#include "modules/util/dump/dumper.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iterator>
#include <type_traits>
#include <utility>

#include <mysqld_error.h>

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include "mysqlshdk/include/shellcore/console.h"
#include "mysqlshdk/include/shellcore/interrupt_handler.h"
#include "mysqlshdk/include/shellcore/shell_init.h"
#include "mysqlshdk/include/shellcore/shell_options.h"
#include "mysqlshdk/libs/db/mysql/session.h"
#include "mysqlshdk/libs/db/mysqlx/session.h"
#include "mysqlshdk/libs/storage/compressed_file.h"
#include "mysqlshdk/libs/storage/idirectory.h"
#include "mysqlshdk/libs/storage/utils.h"
#include "mysqlshdk/libs/textui/textui.h"
#include "mysqlshdk/libs/utils/debug.h"
#include "mysqlshdk/libs/utils/fault_injection.h"
#include "mysqlshdk/libs/utils/profiling.h"
#include "mysqlshdk/libs/utils/rate_limit.h"
#include "mysqlshdk/libs/utils/std.h"
#include "mysqlshdk/libs/utils/strformat.h"
#include "mysqlshdk/libs/utils/utils_general.h"
#include "mysqlshdk/libs/utils/utils_net.h"
#include "mysqlshdk/libs/utils/utils_sqlstring.h"
#include "mysqlshdk/libs/utils/utils_string.h"

#include "modules/mod_utils.h"
#include "modules/util/dump/compatibility_option.h"
#include "modules/util/dump/console_with_progress.h"
#include "modules/util/dump/decimal.h"
#include "modules/util/dump/dialect_dump_writer.h"
#include "modules/util/dump/dump_errors.h"
#include "modules/util/dump/dump_manifest.h"
#include "modules/util/dump/dump_utils.h"
#include "modules/util/dump/schema_dumper.h"
#include "modules/util/dump/text_dump_writer.h"

namespace mysqlsh {
namespace dump {

using mysqlshdk::storage::Mode;
using mysqlshdk::storage::backend::Memory_file;
using mysqlshdk::utils::Version;

using Row = std::vector<std::string>;

namespace {

static constexpr auto k_dump_in_progress_ext = ".dumping";

static constexpr const int k_mysql_server_net_write_timeout = 30 * 60;
static constexpr const int k_mysql_server_wait_timeout = 365 * 24 * 60 * 60;

FI_DEFINE(dumper, [](const mysqlshdk::utils::FI::Args &args) {
  throw std::runtime_error(args.get_string("msg"));
});

enum class Issue_status {
  NONE,
  FIXED,
  FIXED_CREATE_PKS,
  FIXED_IGNORE_PKS,
  ERROR,
  PK_ERROR,
};

std::string quote_value(const std::string &value, mysqlshdk::db::Type type) {
  if (is_string_type(type)) {
    return shcore::quote_sql_string(value);
  } else if (mysqlshdk::db::Type::Decimal == type) {
    return "'" + value + "'";
  } else {
    return value;
  }
}

std::string trim_in_progress_extension(const std::string &s) {
  if (shcore::str_iendswith(s, k_dump_in_progress_ext)) {
    return s.substr(0, s.length() - strlen(k_dump_in_progress_ext));
  } else {
    return s;
  }
}

auto refs(const std::string &s) {
  return rapidjson::StringRef(s.c_str(), s.length());
}

std::string to_string(rapidjson::Document *doc) {
  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
  doc->Accept(writer);
  return std::string{buffer.GetString(), buffer.GetSize()};
}

void write_json(std::unique_ptr<mysqlshdk::storage::IFile> file,
                rapidjson::Document *doc) {
  const auto json = to_string(doc);
  file->open(Mode::WRITE);
  file->write(json.c_str(), json.length());
  file->close();
}

Issue_status show_issues(const std::vector<Schema_dumper::Issue> &issues) {
  const auto console = current_console();
  Issue_status status = Issue_status::NONE;
  const auto set_status = [&status](Issue_status s) {
    status = std::max(status, s);
  };

  for (const auto &issue : issues) {
    if (issue.status <= Schema_dumper::Issue::Status::FIXED) {
      if (issue.status ==
          Schema_dumper::Issue::Status::FIXED_BY_CREATE_INVISIBLE_PKS) {
        set_status(Issue_status::FIXED_CREATE_PKS);
      } else if (issue.status ==
                 Schema_dumper::Issue::Status::FIXED_BY_IGNORE_MISSING_PKS) {
        set_status(Issue_status::FIXED_IGNORE_PKS);
      } else {
        set_status(Issue_status::FIXED);
      }

      console->print_note(issue.description);
    } else {
      set_status(Issue_status::ERROR);

      std::string hint;

      if (Schema_dumper::Issue::Status::FIX_MANUALLY == issue.status) {
        hint = "this issue needs to be fixed manually";
      } else if (Schema_dumper::Issue::Status::USE_CREATE_OR_IGNORE_PKS ==
                 issue.status) {
        set_status(Issue_status::PK_ERROR);
      } else {
        hint = "fix this with '" +
               to_string(to_compatibility_option(issue.status)) +
               "' compatibility option";
      }

      console->print_error(issue.description +
                           (hint.empty() ? "" : " (" + hint + ")"));
    }
  }

  return status;
}

std::string quote(const std::string &schema, const std::string &table) {
  return shcore::quote_identifier(schema) + "." +
         shcore::quote_identifier(table);
}

Row fetch_row(const mysqlshdk::db::IRow *row) {
  Row result;

  if (row) {
    for (uint32_t i = 0, size = row->num_fields(); i < size; ++i) {
      assert(!row->is_null(i));
      result.emplace_back(row->get_as_string(i));
    }
  }

  return result;
}

int64_t to_int64_t(const std::string &s) { return std::stoll(s); }

uint64_t to_uint64_t(const std::string &s) { return std::stoull(s); }

}  // namespace

class Dumper::Synchronize_workers final {
 public:
  Synchronize_workers() = default;
  ~Synchronize_workers() = default;

  void wait_for(const uint16_t count) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_condition.wait(lock, [this, count]() { return m_count >= count; });
    m_count -= count;
  }

  void notify() {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      ++m_count;
    }
    m_condition.notify_one();
  }

 private:
  std::mutex m_mutex;
  std::condition_variable m_condition;
  uint16_t m_count = 0;
};

class Dumper::Table_worker final {
 public:
  enum class Exception_strategy { ABORT, CONTINUE };

  Table_worker() = delete;

  Table_worker(std::size_t id, Dumper *dumper, Exception_strategy strategy)
      : m_id(id), m_dumper(dumper), m_strategy(strategy) {}

  Table_worker(const Table_worker &) = delete;
  Table_worker(Table_worker &&) = default;

  Table_worker &operator=(const Table_worker &) = delete;
  Table_worker &operator=(Table_worker &&) = delete;

  ~Table_worker() = default;

  void run() {
    std::string context;

    try {
#ifndef NDEBUG
      try {
        FI_TRIGGER_TRAP(dumper, mysqlshdk::utils::FI::Trigger_options(
                                    {{"op", "WORKER_START"},
                                     {"id", std::to_string(m_id)}}));
      } catch (const std::exception &e) {
        if (shcore::str_beginswith(e.what(), "sleep=")) {
          shcore::sleep_ms(shcore::lexical_cast<uint32_t>(e.what() + 6));
        } else {
          throw;
        }
      }
#endif
      mysqlsh::Mysql_thread mysql_thread;
      shcore::on_leave_scope close_session([this]() {
        if (m_session) {
          m_session->close();
        }
      });

      open_session();

      m_rate_limit =
          mysqlshdk::utils::Rate_limit(m_dumper->m_options.max_rate());

      while (true) {
        auto work = m_dumper->m_worker_tasks.pop();

        if (m_dumper->m_worker_interrupt) {
          return;
        }

        if (!work.task) {
          break;
        }

        context = std::move(work.info);

        work.task(this);

        if (m_dumper->m_worker_interrupt) {
          return;
        }
      }

      m_dumper->assert_transaction_is_open(m_session);
    } catch (const mysqlshdk::db::Error &e) {
      handle_exception(context, e.format().c_str());
    } catch (const std::exception &e) {
      handle_exception(context, e.what());
    } catch (...) {
      handle_exception(context, "Unknown exception");
    }
  }

 private:
  friend class Dumper;

  void open_session() {
    // notify dumper that the session has been established
    shcore::on_leave_scope notify_dumper(
        [this]() { m_dumper->m_worker_synchronization->notify(); });

    m_session =
        establish_session(m_dumper->session()->get_connection_options(), false);

    m_dumper->start_transaction(m_session);
    m_dumper->on_init_thread_session(m_session);
  }

  inline std::shared_ptr<mysqlshdk::db::IResult> query(
      const std::string &sql) const {
    return Dumper::query(m_session, sql);
  }

  std::string prepare_query(
      const Table_data_task &table,
      std::vector<Dump_writer::Encoding_type> *out_pre_encoded_columns) const {
    const auto base64 = m_dumper->m_options.use_base64();
    std::string query = "SELECT SQL_NO_CACHE ";

    for (const auto &column : table.info->columns) {
      if (column->csv_unsafe) {
        query += (base64 ? "TO_BASE64(" : "HEX(") + column->quoted_name + ")";

        out_pre_encoded_columns->push_back(
            base64 ? Dump_writer::Encoding_type::BASE64
                   : Dump_writer::Encoding_type::HEX);
      } else {
        query += column->quoted_name;

        out_pre_encoded_columns->push_back(Dump_writer::Encoding_type::NONE);
      }

      query += ",";
    }

    // remove last comma
    query.pop_back();

    query += " FROM " + table.quoted_name;

    if (table.partition) {
      query += " PARTITION (" + table.partition->quoted_name + ")";
    }

    query += where(table.where);

    if (table.info->index.valid()) {
      query += " ORDER BY " + table.info->index.columns_sql();
    }

    query += " " + m_dumper->get_query_comment(table, "dumping");

    return query;
  }

  void dump_table_data(const Table_data_task &table) {
    Dump_write_result bytes_written_per_file;
    Dump_write_result bytes_written_per_update{table.schema, table.name};
    uint64_t rows_written_per_update = 0;
    uint64_t rows_written_per_file = 0;
    uint64_t max_row_size = 0;
    const uint64_t update_every = 2000;
    uint64_t bytes_written_per_idx = 0;
    const uint64_t write_idx_every = 1024 * 1024;  // bytes
    Dump_write_result bytes_written;
    mysqlshdk::utils::Duration duration;
    std::vector<Dump_writer::Encoding_type> pre_encoded_columns;

    log_debug("Dumping %s (chunk %s) using condition: %s",
              table.task_name.c_str(), table.id.c_str(), table.where.c_str());

    duration.start();

    const auto result = query(prepare_query(table, &pre_encoded_columns));

    shcore::on_leave_scope close_index_file([&table]() {
      try {
        if (table.index_file) {
          if (table.index_file->is_open()) {
            table.index_file->close();
          }
        }
      } catch (const std::runtime_error &error) {
        log_error("%s", error.what());
      }
    });

    table.writer->open();
    if (table.index_file) {
      table.index_file->open(Mode::WRITE);
    }
    bytes_written = table.writer->write_preamble(result->get_metadata(),
                                                 pre_encoded_columns);
    bytes_written_per_file += bytes_written;
    bytes_written_per_update += bytes_written;

    while (const auto row = result->fetch_one()) {
      if (m_dumper->m_worker_interrupt) {
        return;
      }

      bytes_written = table.writer->write_row(row);
      bytes_written_per_file += bytes_written;
      bytes_written_per_update += bytes_written;
      bytes_written_per_idx += bytes_written.data_bytes();
      ++rows_written_per_update;
      ++rows_written_per_file;

      if (table.index_file && bytes_written_per_idx >= write_idx_every) {
        // the idx file contains offsets to the data stream, not to binary one
        const auto offset = mysqlshdk::utils::host_to_network(
            bytes_written_per_file.data_bytes());
        table.index_file->write(&offset, sizeof(uint64_t));

        // make sure offsets are written when close to the write_idx_every
        bytes_written_per_idx %= write_idx_every;
      }

      if (update_every == rows_written_per_update) {
        m_dumper->update_progress(rows_written_per_update,
                                  bytes_written_per_update);

        // we don't know how much data was read from the server, number of
        // bytes written to the dump file is a good approximation
        if (m_rate_limit.enabled()) {
          m_rate_limit.throttle(bytes_written_per_update.data_bytes());
        }

        rows_written_per_update = 0;
        bytes_written_per_update.reset();
      }

      if (bytes_written.data_bytes() > max_row_size) {
        max_row_size = bytes_written.data_bytes();
      }
    }

    bytes_written = table.writer->write_postamble();
    bytes_written_per_file += bytes_written;
    bytes_written_per_update += bytes_written;

    duration.finish();

    if (table.index_file) {
      const auto total = mysqlshdk::utils::host_to_network(
          bytes_written_per_file.data_bytes());
      table.index_file->write(&total, sizeof(uint64_t));
      table.index_file->close();
    }

    log_info("Dump of %s into '%s' (chunk %s) took %f seconds, written %" PRIu64
             " rows (%" PRIu64 " bytes), longest row has %" PRIu64 " bytes",
             table.task_name.c_str(),
             table.writer->output()->full_path().masked().c_str(),
             table.id.c_str(), duration.seconds_elapsed(),
             rows_written_per_file, bytes_written_per_file.data_bytes(),
             max_row_size);

    const auto file_size = m_dumper->finish_writing(
        table.writer, bytes_written_per_file.data_bytes());
    m_dumper->update_progress(rows_written_per_update,
                              bytes_written_per_update);

    // if file is compressed, the final file size may differ from the bytes
    // written so far, as i.e. bytes were not flushed until the whole block was
    // ready
    if (file_size > bytes_written_per_file.bytes_written()) {
      m_dumper->update_bytes_written(file_size -
                                     bytes_written_per_file.bytes_written());
    }
  }

  void push_table_data_task(Table_data_task &&task) {
    std::string info = "dumping " + task.task_name;
    // Table_data_task contains an unique_ptr, it's moved into shared_ptr and
    // then move-captured by lambda, so that this lambda can be stored,
    // otherwise compiler will complain about a call to implicitly-deleted
    // copy constructor
    auto t = std::make_shared<Table_data_task>(std::move(task));

    m_dumper->m_worker_tasks.push({std::move(info),
                                   [task = std::move(t)](Table_worker *worker) {
                                     ++worker->m_dumper->m_num_threads_dumping;

                                     worker->dump_table_data(*task);

                                     --worker->m_dumper->m_num_threads_dumping;
                                   }},
                                  shcore::Queue_priority::LOW);
  }

  Table_data_task create_table_data_task(const Table_task &table,
                                         const std::string &filename) {
    Table_data_task data_task;

    data_task.task_name = table.task_name;
    data_task.name = table.name;
    data_task.quoted_name = table.quoted_name;
    data_task.schema = table.schema;
    data_task.info = table.info;
    data_task.partition = table.partition;
    data_task.writer = m_dumper->get_table_data_writer(filename);

    if (!m_dumper->m_options.is_export_only()) {
      data_task.index_file = m_dumper->make_file(filename + ".idx");
    }

    return data_task;
  }

  void create_and_push_table_data_task(const Table_task &table) {
    Table_data_task data_task = create_table_data_task(
        table, m_dumper->get_table_data_filename(table.basename));

    data_task.id = "1";

    push_table_data_task(std::move(data_task));
  }

  void create_and_push_table_data_task(const Table_task &table,
                                       const std::string &where,
                                       const std::string &id, std::size_t idx,
                                       bool last_chunk) {
    Table_data_task data_task = create_table_data_task(
        table,
        m_dumper->get_table_data_filename(table.basename, idx, last_chunk));

    data_task.id = id;

    if (!where.empty()) {
      data_task.where = "(" + where + ")";
    }

    if (0 == idx) {
      for (const auto &c : table.info->index.columns()) {
        if (c->nullable) {
          data_task.where += "OR(" + c->quoted_name + " IS NULL)";
        }
      }
    }

    push_table_data_task(std::move(data_task));
  }

  void write_schema_metadata(const Schema_info &schema) const {
    log_info("Writing metadata for schema %s", schema.quoted_name.c_str());

    m_dumper->write_schema_metadata(schema);

    ++m_dumper->m_schema_metadata_written;

    m_dumper->validate_dump_consistency(m_session);
  }

  void write_table_metadata(const Table_task &table) {
    log_info("Writing metadata for table %s", table.quoted_name.c_str());

    m_dumper->write_table_metadata(table, m_session);

    ++m_dumper->m_table_metadata_written;

    m_dumper->validate_dump_consistency(m_session);
  }

  void create_table_data_tasks(const Table_task &table) {
    auto ranges = create_ranged_tasks(table);

    if (0 == ranges) {
      create_and_push_table_data_task(table);
      ++ranges;
    }

    log_info("Data dump for table %s will be written to %zu file%s",
             table.task_name.c_str(), ranges, ranges > 1 ? "s" : "");

    m_dumper->chunking_task_finished();
  }

  template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
  static std::string quote(const T &v) {
    return std::to_string(v);
  }

  static std::string quote(const Decimal &d) {
    return '\'' + d.to_string() + '\'';
  }

  template <typename T>
  static std::string between(const std::string &column, T begin, T end) {
    return column + " BETWEEN " + quote(begin) + " AND " + quote(end);
  }

  struct Chunking_info {
    const Table_task *table;
    uint64_t row_count;
    uint64_t rows_per_chunk;
    uint64_t accuracy;
    int explain_rows_idx;
    std::string partition;
    std::string where;
    std::string order_by;
    std::string order_by_desc;
    std::size_t index_column;
  };

  static std::string compare(const Chunking_info &info, const Row &value,
                             const std::string &op, bool eq) {
    const auto &columns = info.table->info->index.columns();
    const auto size = columns.size() - info.index_column;

    assert(size == value.size());

    auto column = columns.rbegin();
    const auto end = column + size;
    std::size_t idx = size;

    const auto get_value = [&column, &value, &idx]() {
      return quote_value(value[--idx], (*column)->type);
    };

    std::string result = (*column)->quoted_name + op;

    if (eq) {
      result += "=";
    }

    result += get_value();

    while (++column != end) {
      const auto val = get_value();
      result = (*column)->quoted_name + op + val + " OR(" +
               (*column)->quoted_name + "=" + val + " AND(" + result + "))";
    }

    return "(" + result + ")";
  }

  static std::string compare_ge(const Chunking_info &info, const Row &value) {
    return compare(info, value, ">", true);
  }

  static std::string compare_le(const Chunking_info &info, const Row &value) {
    return compare(info, value, "<", true);
  }

  static std::string ge(const Chunking_info &info, const Row &value) {
    std::string result = info.where;

    if (!result.empty()) {
      result += " AND";
    }

    result += compare_ge(info, value);

    return result;
  }

  static std::string between(const Chunking_info &info, const Row &begin,
                             const Row &end) {
    return ge(info, begin) + "AND" + compare_le(info, end);
  }

  template <typename T>
  static std::string between(const Chunking_info &info, T begin, T end) {
    std::string result = info.where;

    if (!result.empty()) {
      result += " AND ";
    }

    return result +
           between(info.table->info->index.columns()[info.index_column]
                       ->quoted_name,
                   begin, end);
  }

  static std::string where(const std::string &condition) {
    std::string result;

    if (!condition.empty()) {
      result = " WHERE " + condition;
    }

    return result;
  }

  template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
  static uint64_t distance(const T &min, const T &max) {
    // it should be (max - min + 1), but this can potentially overflow, check
    // for overflow first
    uint64_t result = max - min;

    if (result < std::numeric_limits<uint64_t>::max()) {
      ++result;
    }

    return result;
  }

  static Decimal distance(const Decimal &min, const Decimal &max) {
    return max - min + 1;
  }

  template <typename T>
  static T ensure_not_zero(T &&v) {
    T t = std::forward<T>(v);

    if (0 == t) {
      ++t;
    }

    return t;
  }

  template <typename T, typename U>
  static T cast(const U &value) {
    // if estimated_step is greater than maximum possible value, casting will
    // overflow, use max instead
    return value < static_cast<U>(std::numeric_limits<T>::max())
               ? static_cast<T>(value)
               : std::numeric_limits<T>::max();
  }

  template <typename T>
  static T sum(const T &value, const T &addend) {
    // if sum is greater than max, use max instead
    return std::numeric_limits<T>::max() - addend <= value
               ? std::numeric_limits<T>::max()
               : value + addend;
  }

  template <typename T>
  static T constant_step(const T & /* from */, const T &step) {
    return step;
  }

  template <typename T>
  T adaptive_step(const T &from, const T &step, const T &max,
                  const Chunking_info &info, const std::string &chunk_id) {
    static constexpr int k_chunker_retries = 10;
    static constexpr int k_chunker_iterations = 20;

    const auto double_step = 2 * step;
    auto middle = from;

    auto rows = info.rows_per_chunk;
    const auto comment = this->get_query_comment(*info.table, chunk_id);

    int retry = 0;
    uint64_t delta = info.accuracy + 1;

    const auto row_count = [&info, &comment, this](const auto begin,
                                                   const auto end) {
      return query("EXPLAIN SELECT COUNT(*) FROM " + info.table->quoted_name +
                   info.partition + where(between(info, begin, end)) +
                   info.order_by + comment)
          ->fetch_one_or_throw()
          ->get_uint(info.explain_rows_idx);
    };

    while (delta > info.accuracy && retry < k_chunker_retries) {
      if (max - retry * double_step <= from) {
        // if left boundary is greater than max, stop here
        middle = max;
        break;
      }

      // each time search in a different range, we didn't find the answer in the
      // previous one
      auto left = from + retry * double_step;
      auto right = sum(left, double_step);

      assert(left < right);

      for (int i = 0; i < k_chunker_iterations; ++i) {
        middle = left + (right - left) / 2;

        if (middle >= right || middle <= left) {
          break;
        }

        rows = row_count(from, middle);

        if (0 == i && rows < info.rows_per_chunk) {
          // if in the first iteration there's not enough rows, check the whole
          // range, if there's still not enough rows we can skip this range
          const auto total_rows = row_count(from, right);

          if (total_rows < info.rows_per_chunk) {
            middle = right;
            delta = info.rows_per_chunk - total_rows;
            break;
          }
        }

        if (rows > info.rows_per_chunk) {
          right = middle;
          delta = rows - info.rows_per_chunk;
        } else {
          left = middle;
          delta = info.rows_per_chunk - rows;
        }

        if (delta <= info.accuracy) {
          // we're close enough
          break;
        }
      }

      if (delta > info.accuracy) {
        if (rows >= info.rows_per_chunk) {
          // we have too many rows, but that's OK...
          retry = k_chunker_retries;
        } else {
          if (middle >= max) {
            // we've reached the upper boundary, stop here
            retry = k_chunker_retries;
          } else {
            // we didn't find enough rows here, move farther to
            // the right
            ++retry;
          }
        }
      }
    }

    return ensure_not_zero(middle - from);
  }

  template <typename T>
  std::size_t chunk_integer_column(const Chunking_info &info, const T &min,
                                   const T &max) {
    std::size_t ranges_count = 0;

    // if rows_per_chunk <= 1 it may mean that the rows are bigger than chunk
    // size, which means we # chunks ~= # rows
    const auto estimated_chunks =
        info.rows_per_chunk > 0
            ? std::max(info.row_count / info.rows_per_chunk, UINT64_C(1))
            : info.row_count;

    using step_t = std20::remove_cvref_t<decltype(min)>;
    const auto index_range = distance(min, max);
    const auto row_count_accuracy = std::max(info.row_count / 10, UINT64_C(1));
    const auto estimated_step =
        cast<step_t>(ensure_not_zero(index_range / estimated_chunks));
    // use constant step if number of chunks is small or index range is close to
    // the number of rows
    const bool use_constant_step =
        estimated_chunks < 2 ||
        (index_range > info.row_count
             ? index_range - info.row_count
             : info.row_count - index_range) <= row_count_accuracy;

    std::string chunk_id;
    const auto next_step =
        use_constant_step
            ? std::function<step_t(const step_t &, const step_t &)>(
                  constant_step<T>)
            // using the default capture [&] below results in problems with
            // GCC 5.4.0 (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80543)
            : [&info, &max, &chunk_id, this](const auto &from,
                                             const auto &step) {
                return this->adaptive_step(from, step, max, info, chunk_id);
              };

    auto current = min;
    const auto step = estimated_step;

    log_info("Chunking %s using integer algorithm with %s step",
             info.table->task_name.c_str(),
             use_constant_step ? "constant" : "adaptive");

    bool last_chunk = false;

    while (!last_chunk) {
      if (m_dumper->m_worker_interrupt) {
        return ranges_count;
      }

      chunk_id = std::to_string(ranges_count);
      const auto begin = current;
      auto new_step = next_step(current, step);

      // ensure that there's no integer overflow
      --new_step;
      current = (current > max - new_step ? max : current + new_step);

      const auto end = current;

      last_chunk = (current >= max);

      create_and_push_table_data_task(*info.table, between(info, begin, end),
                                      chunk_id, ranges_count++, last_chunk);

      ++current;
    }

    return ranges_count;
  }

  std::size_t chunk_integer_column(const Chunking_info &info, const Row &begin,
                                   const Row &end) {
    log_info("Chunking %s using integer algorithm",
             info.table->task_name.c_str());

    const auto type =
        info.table->info->index.columns()[info.index_column]->type;

    if (mysqlshdk::db::Type::Integer == type) {
      return chunk_integer_column(info, to_int64_t(begin[info.index_column]),
                                  to_int64_t(end[info.index_column]));
    } else if (mysqlshdk::db::Type::UInteger == type) {
      return chunk_integer_column(info, to_uint64_t(begin[info.index_column]),
                                  to_uint64_t(end[info.index_column]));
    } else if (mysqlshdk::db::Type::Decimal == type) {
      return chunk_integer_column(info, Decimal{begin[info.index_column]},
                                  Decimal{end[info.index_column]});
    }

    throw std::logic_error(
        "Unsupported column type for the integer algorithm: " +
        mysqlshdk::db::to_string(type));
  }

  std::size_t chunk_non_integer_column(const Chunking_info &info,
                                       const Row &begin, const Row &end) {
    log_info("Chunking %s using non-integer algorithm",
             info.table->task_name.c_str());

    std::size_t ranges_count = 0;

    Row range_begin = begin;
    Row range_end;

    const auto &columns = info.table->info->index.columns();
    const auto index =
        shcore::str_join(columns.begin() + info.index_column, columns.end(),
                         ",", [](const auto &c) { return c->quoted_name; });

    const auto select = "SELECT SQL_NO_CACHE " + index + " FROM " +
                        info.table->quoted_name + info.partition + " ";
    const auto order_by_and_limit = info.order_by + " LIMIT " +
                                    std::to_string(info.rows_per_chunk - 1) +
                                    ",2 ";

    const auto fetch =
        [&end](const std::shared_ptr<mysqlshdk::db::IResult> &res) {
          const auto row = res->fetch_one();
          return row ? fetch_row(row) : end;
        };

    std::shared_ptr<mysqlshdk::db::IResult> result;

    do {
      const auto condition = where(ge(info, range_begin));
      const auto chunk_id = std::to_string(ranges_count);
      const auto comment = get_query_comment(*info.table, chunk_id);

      result = query(select + condition + order_by_and_limit + comment);

      if (m_dumper->m_worker_interrupt) {
        return 0;
      }

      range_end = fetch(result);

      create_and_push_table_data_task(
          *info.table, between(info, range_begin, range_end), chunk_id,
          ranges_count++, range_end == end);

      range_begin = fetch(result);
    } while (range_end != end);

    return ranges_count;
  }

  std::size_t chunk_column(const Chunking_info &info) {
    const auto sql =
        "SELECT SQL_NO_CACHE " + info.table->info->index.columns_sql() +
        " FROM " + info.table->quoted_name + info.partition + where(info.where);

    auto result = query(sql + info.order_by + " LIMIT 1");
    auto row = result->fetch_one();

    const auto handle_empty_table = [&info, this]() {
      create_and_push_table_data_task(*info.table, info.where, "0", 0, true);
      return 1;
    };

    if (!row) {
      return handle_empty_table();
    }

    const Row begin = fetch_row(row);

    result = query(sql + info.order_by_desc + " LIMIT 1");
    row = result->fetch_one();

    if (!row) {
      return handle_empty_table();
    }

    const Row end = fetch_row(row);

    const auto type =
        info.table->info->index.columns()[info.index_column]->type;

    log_info("Chunking %s, using columns: %s, min: [%s], max: [%s]",
             info.table->task_name.c_str(),
             shcore::str_join(info.table->info->index.columns(), ", ",
                              [](const auto &c) {
                                return c->quoted_name + " (" +
                                       mysqlshdk::db::to_string(c->type) + ")";
                              })
                 .c_str(),
             shcore::str_join(begin, ", ").c_str(),
             shcore::str_join(end, ", ").c_str());

    if (mysqlshdk::db::Type::Integer == type ||
        mysqlshdk::db::Type::UInteger == type ||
        mysqlshdk::db::Type::Decimal == type) {
      return chunk_integer_column(info, begin, end);
    } else {
      return chunk_non_integer_column(info, begin, end);
    }
  }

  std::size_t create_ranged_tasks(const Table_task &table) {
    if (!m_dumper->is_chunked(table)) {
      return 0;
    }

    mysqlshdk::utils::Duration duration;
    duration.start();

    const auto &task_name = table.task_name;

    // default row size to use when there's no known row size
    constexpr const uint64_t k_default_row_size = 256;

    auto average_row_length = table.partition
                                  ? table.partition->average_row_length
                                  : table.info->average_row_length;
    auto partition = table.partition
                         ? " PARTITION (" + table.partition->quoted_name + ")"
                         : "";

    if (0 == average_row_length) {
      average_row_length = k_default_row_size;

      const auto result =
          query("SELECT " + table.info->index.columns_sql() + " FROM " +
                table.quoted_name + partition + " LIMIT 1");

      // print the message only if table is not empty
      if (result->fetch_one()) {
        auto msg = "Table statistics not available for " + task_name +
                   ", chunking operation may be not optimal. "
                   "Please consider running 'ANALYZE TABLE " +
                   table.quoted_name + ";'";

        if (table.partition) {
          msg += " or 'ALTER TABLE " + table.quoted_name +
                 " ANALYZE PARTITION " + table.partition->quoted_name + ";'";
        }

        msg += " first.";

        current_console()->print_note(msg);
      }
    }

    Chunking_info info;

    info.table = &table;
    info.row_count =
        table.partition ? table.partition->row_count : table.info->row_count;
    info.rows_per_chunk =
        m_dumper->m_options.bytes_per_chunk() / average_row_length;
    info.accuracy = std::max(info.rows_per_chunk / 10, UINT64_C(10));
    info.explain_rows_idx = m_dumper->m_cache.explain_rows_idx;
    info.partition = std::move(partition);

    for (const auto &c : table.info->index.columns()) {
      if (c->nullable) {
        if (!info.where.empty()) {
          info.where += "AND";
        }

        info.where += "(" + c->quoted_name + " IS NOT NULL)";
      }
    }

    info.order_by = " ORDER BY " + table.info->index.columns_sql();
    info.order_by_desc =
        " ORDER BY " +
        shcore::str_join(table.info->index.columns(), ",", [](const auto &c) {
          return c->quoted_name + " DESC";
        });
    info.index_column = 0;

    log_info("Chunking %s, rows: %" PRIu64 ", average row length: %" PRIu64
             ", rows per chunk: %" PRIu64,
             task_name.c_str(), info.row_count, average_row_length,
             info.rows_per_chunk);

    const auto ranges_count = chunk_column(info);

    duration.finish();
    log_debug("Chunking of %s took %f seconds", task_name.c_str(),
              duration.seconds_elapsed());

    return ranges_count;
  }

  void handle_exception(const std::string &context, const char *msg) {
    m_dumper->m_worker_exceptions[m_id] = std::current_exception();
    current_console()->print_error(
        shcore::str_format("[Worker%03zu]: ", m_id) +
        (context.empty() ? "" : "Error while " + context + ": ") + msg);

    if (Exception_strategy::ABORT == m_strategy) {
      m_dumper->emergency_shutdown();
    }
  }

  void dump_schema_ddl(const Schema_info &schema) const {
    log_info("Writing DDL for schema %s", schema.quoted_name.c_str());

    const auto dumper = m_dumper->schema_dumper(m_session);

    m_dumper->write_ddl(*m_dumper->dump_schema(dumper.get(), schema.name),
                        get_schema_filename(schema.basename));

    ++m_dumper->m_ddl_written;

    m_dumper->validate_dump_consistency(m_session);
  }

  void dump_table_ddl(const Schema_info &schema,
                      const Table_info &table) const {
    log_info("Writing DDL for table %s", table.quoted_name.c_str());

    const auto dumper = m_dumper->schema_dumper(m_session);

    m_dumper->write_ddl(
        *m_dumper->dump_table(dumper.get(), schema.name, table.name),
        get_table_filename(table.basename));

    if (m_dumper->m_options.dump_triggers() &&
        dumper->count_triggers_for_table(schema.name, table.name) > 0) {
      m_dumper->write_ddl(
          *m_dumper->dump_triggers(dumper.get(), schema.name, table.name),
          dump::get_table_data_filename(table.basename, "triggers.sql"));
    }

    ++m_dumper->m_ddl_written;

    m_dumper->validate_dump_consistency(m_session);
  }

  void dump_view_ddl(const Schema_info &schema, const View_info &view) const {
    log_info("Writing DDL for view %s", view.quoted_name.c_str());

    const auto dumper = m_dumper->schema_dumper(m_session);

    // DDL file with the temporary table
    m_dumper->write_ddl(
        *m_dumper->dump_temporary_view(dumper.get(), schema.name, view.name),
        dump::get_table_data_filename(view.basename, "pre.sql"));

    // DDL file with the view structure
    m_dumper->write_ddl(
        *m_dumper->dump_view(dumper.get(), schema.name, view.name),
        get_table_filename(view.basename));

    ++m_dumper->m_ddl_written;

    m_dumper->validate_dump_consistency(m_session);
  }

  std::string get_query_comment(const Table_task &table,
                                const std::string &id) const {
    return m_dumper->get_query_comment(table.task_name, id, "chunking");
  }

  const std::size_t m_id;
  Dumper *m_dumper;
  Exception_strategy m_strategy;
  mysqlshdk::utils::Rate_limit m_rate_limit;
  std::shared_ptr<mysqlshdk::db::ISession> m_session;
};

// template specialization of a static method must be defined outside of a class
template <>
Decimal Dumper::Table_worker::cast(const Decimal &value) {
  return value;
}

template <>
Decimal Dumper::Table_worker::sum(const Decimal &value, const Decimal &delta) {
  return value + delta;
}

class Dumper::Memory_dumper final {
 public:
  Memory_dumper() = delete;

  explicit Memory_dumper(Schema_dumper *dumper)
      : m_dumper(dumper), m_file("/dev/null") {}

  Memory_dumper(const Memory_dumper &) = delete;
  Memory_dumper(Memory_dumper &&) = default;

  Memory_dumper &operator=(const Memory_dumper &) = delete;
  Memory_dumper &operator=(Memory_dumper &&) = default;

  ~Memory_dumper() = default;

  const std::vector<Schema_dumper::Issue> &dump(
      const std::function<void(Memory_dumper *)> &func) {
    m_issues.clear();

    m_file.open(Mode::WRITE);
    func(this);
    m_file.close();

    return issues();
  }

  const std::vector<Schema_dumper::Issue> &issues() const { return m_issues; }

  const std::string &content() const { return m_file.content(); }

  template <typename... Args>
  void dump(std::vector<Schema_dumper::Issue> (Schema_dumper::*func)(
                IFile *, const std20::remove_cvref_t<Args> &...),
            Args &&... args) {
    auto issues = (m_dumper->*func)(&m_file, std::forward<Args>(args)...);

    m_issues.insert(m_issues.end(), std::make_move_iterator(issues.begin()),
                    std::make_move_iterator(issues.end()));
  }

  template <typename... Args>
  void dump(void (Schema_dumper::*func)(IFile *,
                                        const std20::remove_cvref_t<Args> &...),
            Args &&... args) {
    (m_dumper->*func)(&m_file, std::forward<Args>(args)...);
  }

 private:
  Schema_dumper *m_dumper;
  Memory_file m_file;
  std::vector<Schema_dumper::Issue> m_issues;
};

Dumper::Dumper(const Dump_options &options)
    : m_options(options), m_progress_thread("Dump", options.show_progress()) {
  m_options.validate();

  if (m_options.use_single_file()) {
    using mysqlshdk::storage::make_file;

    {
      using mysqlshdk::storage::utils::get_scheme;
      using mysqlshdk::storage::utils::scheme_matches;
      using mysqlshdk::storage::utils::strip_scheme;

      const auto scheme = get_scheme(m_options.output_url());

      if (!scheme.empty() && !scheme_matches(scheme, "file")) {
        throw std::invalid_argument("File handling for " + scheme +
                                    " protocol is not supported.");
      }

      if (m_options.output_url().empty() ||
          (!scheme.empty() &&
           strip_scheme(m_options.output_url(), scheme).empty())) {
        throw std::invalid_argument(
            "The name of the output file cannot be empty.");
      }
    }

    m_output_file = make_file(m_options.output_url(), m_options.oci_options());
    m_output_dir = m_output_file->parent();

    if (!m_output_dir->exists()) {
      throw std::invalid_argument(
          "Cannot proceed with the dump, the directory containing '" +
          m_options.output_url() + "' does not exist at the target location " +
          m_output_dir->full_path().masked() + ".");
    }
  } else {
    using mysqlshdk::storage::make_directory;
    if (m_options.oci_options().oci_par_manifest.get_safe()) {
      m_output_dir = std::make_unique<Dump_manifest>(
          Manifest_mode::WRITE, m_options.oci_options(), nullptr,
          m_options.output_url());
    } else {
      m_output_dir =
          make_directory(m_options.output_url(), m_options.oci_options());
    }

    if (m_output_dir->exists()) {
      const auto files = m_output_dir->list_files_sorted(true);

      if (!files.empty()) {
        std::vector<std::string> file_data;
        const auto full_path = m_output_dir->full_path();

        for (const auto &file : files) {
          file_data.push_back(shcore::str_format(
              "%s [size %zu]",
              m_output_dir->join_path(full_path.masked(), file.name()).c_str(),
              file.size()));
        }

        log_error(
            "Unable to dump to %s, the directory exists and is not empty:\n  "
            "%s",
            full_path.masked().c_str(),
            shcore::str_join(file_data, "\n  ").c_str());

        if (m_options.oci_options()) {
          throw std::invalid_argument(
              "Cannot proceed with the dump, bucket '" +
              *m_options.oci_options().os_bucket_name +
              "' already contains files with the specified prefix '" +
              m_options.output_url() + "'.");
        } else {
          throw std::invalid_argument(
              "Cannot proceed with the dump, the specified directory '" +
              m_options.output_url() +
              "' already exists at the target location " + full_path.masked() +
              " and is not empty.");
        }
      }
    }
  }
}

// needs to be defined here due to Dumper::Synchronize_workers being
// incomplete type
Dumper::~Dumper() = default;

void Dumper::run() {
  if (m_options.is_dry_run()) {
    current_console()->print_info(
        "dryRun enabled, no locks will be acquired and no files will be "
        "created.");
  }

  try {
    do_run();
  } catch (...) {
    kill_workers();
    translate_current_exception(m_progress_thread);
  }

  if (m_worker_interrupt) {
    // m_worker_interrupt is also used to signal exceptions from workers,
    // if we're here, then no exceptions were thrown and user pressed ^C
    throw std::runtime_error("Interrupted by user");
  }
}

void Dumper::do_run() {
  // helper:
  // lock_instance():
  //   1. if LIFB is available to the user
  //     1.1. LOCK INSTANCE FOR BACKUP
  //   2. if LIFB is not available to the user
  //     2.1. if user was able to execute FTWRL:
  //       2.1.1. continue
  //     2.2. if user was not able to execute FTWRL:
  //       2.2.1. if (gtid_mode is ON or ON_PERMISSIVE) or (log_bin is ON):
  //         2.2.1.1. continue
  //       2.2.2. else
  //         2.2.2.1. error and abort
  // start up sequence (consistent dump):
  // 1. acquire read locks:
  //   1.1. if user is able to execute FTWRL:
  //     1.1.1. FLUSH TABLES WITH READ LOCK
  //   1.2. if user is not able to execute FTWRL:
  //     1.2.1. execute lock_instance()
  //     1.2.2. create a new session and LOCK mysql TABLES using that session
  //     1.2.3. fetch schemas, tables and views to be dumped
  //     1.2.4. create one or more new sessions and LOCK dumped TABLES
  //     1.2.5. if there are any errors, abort
  // 2. start a transaction
  // 3. create worker threads, each thread:
  //   3.1. creates a new session
  //   3.2. starts a transaction
  // 4. gather information about objects to be dumped:
  //   4.1. if 1.2.3. was not executed, do this now
  //   4.2. fetch server metadata (i.e. gtid_executed, binlog file and position)
  //   4.3. fetch information about tables, views, routines, users, etc.
  // 5. wait for all worker threads to finish their initialization
  // 6. execute lock_instance()
  // 7. release read locks:
  //   7.1. if user was able to execute FTWRL:
  //     7.1.1 UNLOCK TABLES
  //   7.2. if user was not able to execute FTWRL:
  //     7.2.1 close all sessions which executed LOCK TABLES
  // 8. workers start to execute their tasks
  // 9. once all DDL-related files are written (metadata, SQL, etc.):
  //   9.1. if both FTWRL and LIFB were not available to the user:
  //     9.1.1. if GTID: fetch gtid_executed, if different from the 4.2. value:
  //       9.1.1.1. if dumpInstance(): error and abort
  //       9.1.1.2. else: warning and continue
  //     9.1.2. else SHOW MASTER STATUS, if different from the 4.2. value:
  //       9.1.2.1. if dumpInstance(): error and abort
  //       9.1.2.2. else: warning and continue

  m_worker_interrupt = false;
  m_progress_thread.start();
  m_current_stage = m_progress_thread.start_stage("Initializing");

  shcore::Interrupt_handler intr_handler([this]() -> bool {
    current_console()->print_warning("Interrupted by user. Canceling...");
    emergency_shutdown();
    kill_query();
    return false;
  });

  open_session();

  shcore::on_leave_scope terminate_session([this]() { close_session(); });

  fetch_user_privileges();

  {
    shcore::on_leave_scope read_locks([this]() { release_read_locks(); });

    acquire_read_locks();

    if (m_worker_interrupt) {
      return;
    }

    create_worker_threads();

    m_current_stage->finish();

    // initialize cache while threads are starting up
    initialize_instance_cache();

    wait_for_workers();

    if (m_options.consistent_dump() && !m_worker_interrupt) {
      current_console()->print_info("All transactions have been started");
      lock_instance();
    }

    if (!m_worker_interrupt && !m_options.is_export_only() &&
        is_gtid_executed_inconsistent()) {
      current_console()->print_warning(
          "The dumped value of gtid_executed is not guaranteed to be "
          "consistent");
    }
  }

  create_schema_tasks();

  validate_privileges();
  initialize_counters();
  validate_mds();

  initialize_dump();

  dump_ddl();

  create_schema_metadata_tasks();
  create_schema_ddl_tasks();
  create_table_tasks();

  if (!m_options.is_dry_run() && !m_worker_interrupt) {
    current_console()->print_status(
        "Running data dump using " + std::to_string(m_options.threads()) +
        " thread" + (m_options.threads() > 1 ? "s" : "") + ".");

    if (m_options.show_progress()) {
      current_console()->print_note(
          "Progress information uses estimated values and may not be "
          "accurate.");
    }
  }

  initialize_progress();

  maybe_push_shutdown_tasks();
  wait_for_all_tasks();

  if (!m_options.is_dry_run() && !m_worker_interrupt) {
    write_dump_finished_metadata();
    close_output_directory();
    shutdown_progress();
    summarize();
  }

  rethrow();

#ifndef NDEBUG
  if (m_server_version < Version(8, 0, 21) ||
      m_server_version > Version(8, 0, 23) || !dump_users()) {
    // SHOW CREATE USER auto-commits transaction in some 8.0 versions, we don't
    // check if transaction is still open in such case if users were dumped
    assert_transaction_is_open(session());
  }
#endif  // !NDEBUG
}

const std::shared_ptr<mysqlshdk::db::ISession> &Dumper::session() const {
  return m_session;
}

std::unique_ptr<Schema_dumper> Dumper::schema_dumper(
    const std::shared_ptr<mysqlshdk::db::ISession> &session) const {
  auto dumper = std::make_unique<Schema_dumper>(session);

  dumper->use_cache(&m_cache);

  dumper->opt_comments = true;
  dumper->opt_drop_database = false;
  dumper->opt_drop_table = false;
  dumper->opt_drop_view = true;
  dumper->opt_drop_event = true;
  dumper->opt_drop_routine = true;
  dumper->opt_drop_trigger = true;
  dumper->opt_reexecutable = true;
  dumper->opt_tz_utc = m_options.use_timezone_utc();
  dumper->opt_mysqlaas = static_cast<bool>(m_options.mds_compatibility());
  dumper->opt_character_set_results = m_options.character_set();
  dumper->opt_column_statistics = false;

  return dumper;
}

void Dumper::on_init_thread_session(
    const std::shared_ptr<mysqlshdk::db::ISession> &session) const {
  // transaction cannot be started here, as the main thread has to acquire read
  // locks first
  execute(session, "SET SQL_MODE = '';");
  executef(session, "SET NAMES ?;", m_options.character_set());

  // The amount of time the server should wait for us to read data from it
  // like resultsets. Result reading can be delayed by slow uploads.
  executef(session, "SET SESSION net_write_timeout = ?",
           k_mysql_server_net_write_timeout);

  // Amount of time before server disconnects idle clients.
  executef(session, "SET SESSION wait_timeout = ?",
           k_mysql_server_wait_timeout);

  if (m_options.use_timezone_utc()) {
    execute(session, "SET TIME_ZONE = '+00:00';");
  }
}

void Dumper::open_session() {
  auto co = get_classic_connection_options(m_options.session());

  // set read timeout, if not already set by user
  if (!co.has(mysqlshdk::db::kNetReadTimeout)) {
    const auto k_one_day = "86400000";
    co.set(mysqlshdk::db::kNetReadTimeout, k_one_day);
  }

  // set size of max packet (~size of 1 row) we can get from server
  if (!co.has(mysqlshdk::db::kMaxAllowedPacket)) {
    const auto k_one_gb = "1073741824";
    co.set(mysqlshdk::db::kMaxAllowedPacket, k_one_gb);
  }

  m_session = establish_session(co, false);
  on_init_thread_session(m_session);

  fetch_server_information();

  log_server_version();
}

void Dumper::close_session() {
  if (m_session) {
    m_session->close();
  }
}

void Dumper::fetch_user_privileges() {
  using mysqlshdk::mysql::Instance;
  using mysqlshdk::mysql::User_privileges;

  const auto instance = Instance(session());
  std::string user;
  std::string host;

  instance.get_current_user(&user, &host);

  m_user_privileges = instance.get_user_privileges(user, host, true);
  m_user_account = shcore::make_account(user, host);
  m_skip_grant_tables_active =
      "'skip-grants user'@'skip-grants host'" == m_user_account;

  if (m_server_version >= Version(8, 0, 0)) {
    m_user_has_backup_admin =
        !m_user_privileges->validate({"BACKUP_ADMIN"}).has_missing_privileges();
  }

  warn_about_backup_lock();
}

void Dumper::warn_about_backup_lock() const {
  if (!m_options.consistent_dump()) {
    return;
  }

  if (!m_user_has_backup_admin) {
    current_console()->print_note(
        "Backup lock is not " + why_backup_lock_is_missing() +
        " and DDL changes will not be blocked. The dump may fail with an error "
        "if schema changes are made while dumping.");
  }
}

std::string Dumper::why_backup_lock_is_missing() const {
  return m_server_version >= Version(8, 0, 0)
             ? "available to the account " + m_user_account
             : "supported in MySQL " + m_server_version.get_short();
}

void Dumper::lock_all_tables() {
  lock_instance();

  // find out the max query size we can send
  uint64_t max_packet_size = 0;

  {
    const auto r = query("select @@max_allowed_packet");
    // 1024 is the minimum allowed value
    max_packet_size = r->fetch_one_or_throw()->get_uint(0, 1024);
  }

  const auto console = current_console();
  const std::string k_lock_tables = "LOCK TABLES ";

  using mysqlshdk::mysql::User_privileges;
  const std::set<std::string> privileges = {"LOCK TABLES", "SELECT"};

  const auto validate_privileges = [this, &privileges](
                                       const std::string &schema,
                                       const std::string &table =
                                           User_privileges::k_wildcard) {
    const auto result = m_user_privileges->validate(privileges, schema, table);

    if (result.has_missing_privileges()) {
      const std::string object =
          User_privileges::k_wildcard == table
              ? "schema " + shcore::quote_identifier(schema)
              : "table " + quote(schema, table);

      THROW_ERROR(SHERR_DUMP_LOCK_TABLES_MISSING_PRIVILEGES,
                  m_user_account.c_str(), object.c_str(),
                  shcore::str_join(result.missing_privileges(), ", ").c_str());
    }
  };

  const auto lock_tables = [this](std::string *lock_statement) {
    if (',' == lock_statement->back()) {
      // remove last comma
      lock_statement->pop_back();
    }

    log_info("Locking tables: %s", lock_statement->c_str());

    if (!m_options.is_dry_run()) {
      // initialize session
      auto s = establish_session(session()->get_connection_options(), false);
      on_init_thread_session(s);
      execute(s, *lock_statement);
      m_lock_sessions.emplace_back(std::move(s));
    }
  };

  // lock relevant tables in mysql so that grants, views, routines etc. are
  // consistent too, use the main thread to lock these tables, as it is going to
  // read from them
  try {
    const std::string mysql = "mysql";

    // if user doesn't have the SELECT privilege on mysql.*, it's not going to
    // be possible to list tables
    validate_privileges(mysql);

    const auto result = query(
        "SHOW TABLES IN mysql WHERE Tables_in_mysql IN"
        "('columns_priv', 'db', 'default_roles', 'func', 'global_grants', "
        "'proc', 'procs_priv', 'proxies_priv', 'role_edges', 'tables_priv', "
        "'user')");

    auto stmt = k_lock_tables;

    while (auto row = result->fetch_one()) {
      const auto table = row->get_string(0);

      validate_privileges(mysql, table);

      stmt.append("mysql." + shcore::quote_identifier(table) + " READ,");
    }

    lock_tables(&stmt);
  } catch (const mysqlshdk::db::Error &e) {
    console->print_error("Could not lock mysql system tables: " + e.format());
    throw;
  } catch (const std::runtime_error &e) {
    console->print_warning("Could not lock mysql system tables: " +
                           std::string{e.what()});
    console->print_warning(
        "The dump will continue, but the dump may not be completely consistent "
        "if changes to accounts or routines are made during it.");
  }

  initialize_instance_cache_minimal();

  // get tables to be locked
  std::vector<std::string> tables;

  for (const auto &schema : m_cache.schemas) {
    for (const auto &table : schema.second.tables) {
      validate_privileges(schema.first, table.first);

      tables.emplace_back(quote(schema.first, table.first) + " READ,");
    }
  }

  // iterate all tables that are going to be dumped and LOCK TABLES them, use
  // separate sessions to run the queries as executing LOCK TABLES implicitly
  // unlocks tables locked before
  try {
    const auto k_lock_tables_size = k_lock_tables.size();
    auto size = std::string::npos;
    auto stmt = k_lock_tables;

    for (const auto &table : tables) {
      size = stmt.size();

      // check if we're overflowing the SQL packet (256B slack is probably
      // enough)
      if (size + table.size() >= max_packet_size - 256 &&
          size > k_lock_tables_size) {
        lock_tables(&stmt);

        stmt = k_lock_tables;
      }

      stmt.append(table);
    }

    if (stmt.size() > k_lock_tables_size) {
      // flush the rest
      lock_tables(&stmt);
    }
  } catch (const mysqlshdk::db::Error &e) {
    console->print_error("Error locking tables: " + e.format());
    throw;
  }
}

void Dumper::acquire_read_locks() {
  if (!m_options.consistent_dump()) {
    return;
  }

  // This will block until lock_wait_timeout if there are any sessions with open
  // transactions/locks.
  current_console()->print_info("Acquiring global read lock");

  // user can execute FLUSH statements if has RELOAD privilege or, starting with
  // 8.0.23, FLUSH_TABLES privilege
  const auto execute_ftwrl =
      !m_user_privileges->validate({"RELOAD"}).has_missing_privileges() ||
      (m_server_version >= Version(8, 0, 23) &&
       !m_user_privileges->validate({"FLUSH_TABLES"}).has_missing_privileges());
  m_ftwrl_used = execute_ftwrl;

  if (execute_ftwrl) {
    if (m_options.is_dry_run()) {
      // in case of a dry run we need to attempt to acquire FTWRL, because it
      // may require some other privileges, set the lock_wait_timeout to
      // smallest possible value to avoid waiting for long periods of time
      execute("SET @current_lock_wait_timeout = @@session.lock_wait_timeout");
      execute("SET @@session.lock_wait_timeout = 1");
    }

    bool timeout = false;

    try {
      // no need to flush in case of a dry run, we're only interested if FTWRL
      // succeeds
      if (!m_options.is_dry_run()) {
        // We do first a FLUSH TABLES. If a long update is running, the FLUSH
        // TABLES will wait but will not stall the whole mysqld, and when the
        // long update is done the FLUSH TABLES WITH READ LOCK will start and
        // succeed quickly. So, FLUSH TABLES is to lower the probability of a
        // stage where both shell and most client connections are stalled. Of
        // course, if a second long update starts between the two FLUSHes, we
        // have that bad stall.
        execute("FLUSH NO_WRITE_TO_BINLOG TABLES;");
      }

      execute("FLUSH TABLES WITH READ LOCK;");
    } catch (const mysqlshdk::db::Error &e) {
      if (ER_LOCK_WAIT_TIMEOUT == e.code() && m_options.is_dry_run()) {
        // it's a dry run and FTWRL failed due to a timeout, treat this as a
        // success - user had privileges required to execute FTWRL
        timeout = true;
      } else if (ER_SPECIFIC_ACCESS_DENIED_ERROR == e.code() ||
                 ER_DBACCESS_DENIED_ERROR == e.code() ||
                 ER_ACCESS_DENIED_ERROR == e.code()) {
        // FTWRL failed due to an access denied error, fall back to LOCK TABLES
        m_ftwrl_used = false;
      } else {
        current_console()->print_error("Failed to acquire global read lock: " +
                                       e.format());
        THROW_ERROR0(SHERR_DUMP_GLOBAL_READ_LOCK_FAILED);
      }
    }

    if (m_options.is_dry_run()) {
      // restore previous value of lock_wait_timeout
      execute("SET @@session.lock_wait_timeout = @current_lock_wait_timeout");

      if (!timeout && m_ftwrl_used) {
        // we've successfully acquired FTWRL, but it's a dry run and we don't
        // want to interfere with the live server, release it immediately
        execute("UNLOCK TABLES");
      }
    }

    if (m_ftwrl_used) {
      current_console()->print_info("Global read lock acquired");
    }
  }

  if (!m_ftwrl_used) {
    current_console()->print_warning(
        "The current user lacks privileges to acquire a global read lock "
        "using 'FLUSH TABLES WITH READ LOCK'. Falling back to LOCK "
        "TABLES...");

    const auto handle_error = [](const std::string &msg) {
      current_console()->print_error(
          "Unable to acquire global read lock neither table read locks.");

      THROW_ERROR(SHERR_DUMP_LOCK_TABLES_FAILED, msg.c_str());
    };

    // if FTWRL isn't executable by the user, try LOCK TABLES instead
    try {
      lock_all_tables();

      current_console()->print_info("Table locks acquired");
    } catch (const mysqlshdk::db::Error &e) {
      handle_error(e.format());
    } catch (const std::runtime_error &e) {
      handle_error(e.what());
    }
  }

  // if FTWRL was used to lock the tables, it is safe to start a transaction, as
  // it will not release the global read lock; if LOCK TABLES was used instead,
  // tables were locked in separate sessions, so it's safe to start the
  // transaction in the main session
  start_transaction(session());
}

void Dumper::release_read_locks() const {
  if (!m_options.consistent_dump()) {
    return;
  }

  if (!m_ftwrl_used) {
    log_info("Releasing the table locks");

    // close the locking sessions, this will release the table locks
    for (const auto &session : m_lock_sessions) {
      session->close();
    }
  } else {
    // FTWRL has succeeded, transaction is active, UNLOCK TABLES will not
    // automatically commit the transaction
    execute("UNLOCK TABLES;");
  }

  if (!m_worker_interrupt) {
    // we've been interrupted, we still need to release locks, but don't
    // inform the user
    current_console()->print_info("Global read lock has been released");
  }
}

void Dumper::start_transaction(
    const std::shared_ptr<mysqlshdk::db::ISession> &session) const {
  if (m_options.consistent_dump() && !m_options.is_dry_run()) {
    execute(session,
            "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ;");
    execute(session, "START TRANSACTION WITH CONSISTENT SNAPSHOT;");
  }
}

void Dumper::assert_transaction_is_open(
    const std::shared_ptr<mysqlshdk::db::ISession> &session) const {
#ifdef NDEBUG
  (void)session;
#else  // !NDEBUG
  if (m_options.consistent_dump() && !m_options.is_dry_run()) {
    try {
      // if there's an active transaction, this will throw
      execute(session, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
      // no exception -> transaction is not active
      assert(false);
    } catch (const mysqlshdk::db::Error &e) {
      // make sure correct error is reported
      assert(e.code() == ER_CANT_CHANGE_TX_CHARACTERISTICS);
    } catch (...) {
      // any other exception means that something else went wrong
      assert(false);
    }
  }
#endif
}

void Dumper::lock_instance() {
  if (!m_options.consistent_dump() || m_instance_locked) {
    return;
  }

  if (m_user_has_backup_admin) {
    const auto console = current_console();

    console->print_info("Locking instance for backup");

    if (!m_options.is_dry_run()) {
      try {
        execute("LOCK INSTANCE FOR BACKUP");
      } catch (const shcore::Error &e) {
        console->print_error("Could not acquire the backup lock: " +
                             e.format());

        throw;
      }
    }
  } else if (!m_ftwrl_used && !m_gtid_enabled &&
             (!m_binlog_enabled ||
              2 == m_user_privileges->validate({"REPLICATION CLIENT", "SUPER"})
                       .missing_privileges()
                       .size())) {
    const auto msg =
        "The current user does not have required privileges to execute FLUSH "
        "TABLES WITH READ LOCK.\n    Backup lock is not " +
        why_backup_lock_is_missing() +
        " and DDL changes cannot be blocked.\n    The gtid_mode system "
        "variable is set to OFF or OFF_PERMISSIVE.\n    The log_bin system "
        "variable is set to OFF or the current user does not have required "
        "privileges to execute SHOW MASTER STATUS.\nThe consistency of the "
        "dump cannot be guaranteed.";
    const auto console = current_console();

    if (shcore::str_caseeq(name(), "dumpInstance")) {
      console->print_error(msg);
      THROW_ERROR0(SHERR_DUMP_CONSISTENCY_CHECK_FAILED);
    } else {
      console->print_warning(msg);
    }
  }

  m_instance_locked = true;
}

void Dumper::initialize_instance_cache_minimal() {
  m_cache = Instance_cache_builder(session(), m_options.included_schemas(),
                                   m_options.included_tables(),
                                   m_options.excluded_schemas(),
                                   m_options.excluded_tables(), {}, false)
                .build();

  validate_schemas_list();
}

void Dumper::initialize_instance_cache() {
  m_current_stage = m_progress_thread.start_stage("Gathering information");
  shcore::on_leave_scope finish_stage([this]() { m_current_stage->finish(); });

  auto builder = Instance_cache_builder(
      session(), m_options.included_schemas(), m_options.included_tables(),
      m_options.excluded_schemas(), m_options.excluded_tables(),
      std::move(m_cache));

  if (dump_users()) {
    builder.users(m_options.included_users(), m_options.excluded_users());
  }

  if (m_options.dump_ddl()) {
    if (m_options.dump_events()) {
      builder.events(m_options.included_events(), m_options.excluded_events());
    }

    if (m_options.dump_routines()) {
      builder.routines(m_options.included_routines(),
                       m_options.excluded_routines());
    }

    if (m_options.dump_triggers()) {
      builder.triggers(m_options.included_triggers(),
                       m_options.excluded_triggers());
    }
  }

  if (m_options.dump_binlog_info()) {
    builder.binlog_info();
  }

  m_cache = builder.build();

  validate_schemas_list();

  print_object_stats();
}

void Dumper::create_schema_tasks() {
  bool has_partitions = false;

  for (const auto &s : m_cache.schemas) {
    Schema_info schema;
    schema.name = s.first;
    schema.quoted_name = shcore::quote_identifier(schema.name);
    schema.basename = get_basename(encode_schema_basename(schema.name));

    for (const auto &t : s.second.tables) {
      Table_info table;
      table.name = t.first;
      table.quoted_name = quote(schema.name, table.name);
      table.basename =
          get_basename(encode_table_basename(schema.name, table.name));
      table.info = &t.second;

      for (const auto &p : t.second.partitions) {
        has_partitions = true;

        Partition_info partition;
        partition.info = &p;
        partition.basename = get_basename(
            encode_partition_basename(schema.name, table.name, p.name));

        table.partitions.emplace_back(std::move(partition));
      }

      schema.tables.emplace_back(std::move(table));
    }

    for (const auto &v : s.second.views) {
      View_info view;
      view.name = v.first;
      view.quoted_name = quote(schema.name, view.name);
      view.basename =
          get_basename(encode_table_basename(schema.name, view.name));

      schema.views.emplace_back(std::move(view));
    }

    m_schema_infos.emplace_back(std::move(schema));
  }

  if (has_partitions) {
    m_used_capabilities.emplace(Capability::PARTITION_AWARENESS);
  }
}

void Dumper::validate_mds() const {
  if (!m_options.mds_compatibility() ||
      (!m_options.dump_ddl() && !dump_users())) {
    return;
  }

  const auto console = current_console();
  const auto version = m_options.mds_compatibility()->get_base();

  console->print_info(
      "Checking for compatibility with MySQL Database Service " + version);

  if (!m_cache.server_is_8_0) {
    console->print_note("MySQL Server " + m_cache.server_version.get_short() +
                        " detected, please consider upgrading to 8.0 first.");

    if (m_cache.server_is_5_7) {
      console->print_note(
          "You can check for potential upgrade issues using util." +
          shcore::get_member_name("checkForServerUpgrade",
                                  shcore::current_naming_style()) +
          "().");
    }
  }

  Progress_thread::Progress_config config;
  std::atomic<uint64_t> objects_checked{0};

  config.current = [&objects_checked]() -> uint64_t { return objects_checked; };
  config.total = [this]() { return m_total_objects; };

  m_current_stage = m_progress_thread.start_stage(
      "Validating MDS compatibility", std::move(config));
  shcore::on_leave_scope finish_stage([this]() { m_current_stage->finish(); });

  Issue_status status = Issue_status::NONE;

  const auto issues = [&status](const auto &memory) {
    status = std::max(status, show_issues(memory->issues()));
  };

  const auto dumper = schema_dumper(session());

  if (dump_users()) {
    issues(dump_users(dumper.get()));
  }

  if (m_options.dump_ddl()) {
    for (const auto &schema : m_schema_infos) {
      issues(dump_schema(dumper.get(), schema.name));
      ++objects_checked;
    }

    for (const auto &schema : m_schema_infos) {
      for (const auto &table : schema.tables) {
        issues(dump_table(dumper.get(), schema.name, table.name));

        if (m_options.dump_triggers() &&
            dumper->count_triggers_for_table(schema.name, table.name) > 0) {
          issues(dump_triggers(dumper.get(), schema.name, table.name));
        }

        ++objects_checked;
      }

      for (const auto &view : schema.views) {
        issues(dump_temporary_view(dumper.get(), schema.name, view.name));
        issues(dump_view(dumper.get(), schema.name, view.name));

        ++objects_checked;
      }
    }
  }

  switch (status) {
    case Issue_status::PK_ERROR:
      console->print_info();
      console->print_error(
          "One or more tables without Primary Keys were found.");
      console->print_info(R"(
       MySQL Database Service High Availability (MDS HA) requires Primary Keys to be present in all tables.
       To continue with the dump you must do one of the following:

       * Create PRIMARY keys in all tables before dumping them.
         MySQL 8.0.23 supports the creation of invisible columns to allow creating Primary Key columns with no impact to applications. For more details, see https://dev.mysql.com/doc/refman/en/invisible-columns.html.
         This is considered a best practice for both performance and usability and will work seamlessly with MDS.

       * Add the "create_invisible_pks" to the "compatibility" option.
         The dump will proceed and loader will automatically add Primary Keys to tables that don't have them when loading into MDS.
         This will make it possible to enable HA in MDS without application impact.
         However, Inbound Replication into an MDS HA instance (at the time of the release of MySQL Shell 8.0.24) will still not be possible.

       * Add the "ignore_missing_pks" to the "compatibility" option.
         This will disable this check and the dump will be produced normally, Primary Keys will not be added automatically.
         It will not be possible to load the dump in an HA enabled MDS instance.
)");
      // fallthrough

    case Issue_status::ERROR:
      console->print_info(
          "Compatibility issues with MySQL Database Service " + version +
          " were found. Please use the 'compatibility' option to apply "
          "compatibility adaptations to the dumped DDL.");
      THROW_ERROR0(SHERR_DUMP_COMPATIBILITY_ISSUES_FOUND);

    case Issue_status::FIXED:
    case Issue_status::FIXED_CREATE_PKS:
    case Issue_status::FIXED_IGNORE_PKS:
      if (Issue_status::FIXED != status) {
        console->print_info();
        console->print_note(
            "One or more tables without Primary Keys were found.");
      }

      if (Issue_status::FIXED_CREATE_PKS == status) {
        console->print_info(R"(
      Missing Primary Keys will be created automatically when this dump is loaded.
      This will make it possible to enable High Availability in MySQL Database Service instance without application impact.
      However, Inbound Replication into an MDS HA instance (at the time of the release of MySQL Shell 8.0.24) will still not be possible.
)");
      }

      if (Issue_status::FIXED_IGNORE_PKS == status) {
        console->print_info(R"(
      This issue is ignored.
      This dump cannot be loaded into an MySQL Database Service instance with High Availability.
)");
      }

      console->print_info("Compatibility issues with MySQL Database Service " +
                          version +
                          " were found and repaired. Please review the changes "
                          "made before loading them.");
      break;

    case Issue_status::NONE:
      console->print_info("Compatibility checks finished.");
      break;
  }
}

void Dumper::initialize_counters() {
  m_total_rows = 0;
  m_total_tables = 0;
  m_total_views = 0;
  m_total_schemas = m_schema_infos.size();

  for (auto &schema : m_schema_infos) {
    m_total_tables += schema.tables.size();
    m_total_views += schema.views.size();

    for (auto &table : schema.tables) {
      m_total_rows += table.info->row_count;
    }
  }

  m_total_objects = m_total_schemas + m_total_tables + m_total_views;

  m_rows_written = 0;
  m_bytes_written = 0;
  m_data_bytes = 0;
  m_table_data_bytes.clear();

  m_data_throughput = std::make_unique<mysqlshdk::textui::Throughput>();
  m_bytes_throughput = std::make_unique<mysqlshdk::textui::Throughput>();

  m_num_threads_chunking = 0;
  m_num_threads_dumping = 0;

  m_ddl_written = 0;
  m_schema_metadata_written = 0;
  m_table_metadata_to_write = 0;
  m_table_metadata_written = 0;
}

void Dumper::initialize_dump() {
  if (m_options.is_dry_run()) {
    return;
  }

  create_output_directory();
  write_metadata();
}

void Dumper::create_output_directory() {
  const auto dir = directory();

  if (!dir->exists()) {
    dir->create();
  }
}

void Dumper::close_output_directory() {
  if (m_options.is_dry_run()) {
    return;
  }

  Progress_thread::Stage *stage = nullptr;
  shcore::on_leave_scope finish_stage([&stage]() {
    if (stage) {
      stage->finish();
    }
  });

  if (m_options.oci_options().oci_par_manifest.get_safe()) {
    stage = m_current_stage = m_progress_thread.start_stage("Writing manifest");
  }

  directory()->close();
}

void Dumper::create_worker_threads() {
  m_worker_exceptions.clear();
  m_worker_exceptions.resize(m_options.threads());
  m_worker_synchronization = std::make_unique<Synchronize_workers>();

  for (std::size_t i = 0; i < m_options.threads(); ++i) {
    auto t = mysqlsh::spawn_scoped_thread(
        &Table_worker::run,
        Table_worker{i, this, Table_worker::Exception_strategy::ABORT});
    m_workers.emplace_back(std::move(t));
  }
}

void Dumper::wait_for_workers() {
  m_worker_synchronization->wait_for(m_workers.size());
}

void Dumper::maybe_push_shutdown_tasks() {
  if (0 == m_chunking_tasks &&
      m_main_thread_finished_producing_chunking_tasks) {
    m_worker_tasks.shutdown(m_workers.size());
  }
}

void Dumper::chunking_task_finished() {
  --m_chunking_tasks;
  maybe_push_shutdown_tasks();
}

void Dumper::wait_for_all_tasks() {
  for (auto &worker : m_workers) {
    worker.join();
  }

  // when using a single file as an output, it's not closed until the whole
  // dump is done
  if (m_options.use_single_file()) {
    for (const auto &writer : m_worker_writers) {
      close_file(*writer);
    }
  }

  m_workers.clear();
  m_worker_writers.clear();

  if (m_data_dump_stage && !m_worker_interrupt) {
    m_data_dump_stage->finish();
  }
}

void Dumper::dump_ddl() const {
  if (!m_options.dump_ddl()) {
    return;
  }

  dump_global_ddl();
  dump_users_ddl();
}

void Dumper::dump_global_ddl() const {
  current_console()->print_status("Writing global DDL files");

  if (m_options.is_dry_run()) {
    return;
  }

  const auto dumper = schema_dumper(session());

  {
    // file with the DDL setup
    const auto output = make_file("@.sql");
    output->open(Mode::WRITE);

    dumper->write_comment(output.get());

    output->close();
  }

  {
    // post DDL file (cleanup)
    const auto output = make_file("@.post.sql");
    output->open(Mode::WRITE);

    dumper->write_comment(output.get());

    output->close();
  }
}

void Dumper::dump_users_ddl() const {
  if (!dump_users()) {
    return;
  }

  current_console()->print_status("Writing users DDL");

  const auto dumper = schema_dumper(session());

  write_ddl(*dump_users(dumper.get()), "@.users.sql");
}

void Dumper::write_ddl(const Memory_dumper &in_memory,
                       const std::string &file) const {
  if (!m_options.mds_compatibility()) {
    // if MDS is on, changes done by compatibility options were printed earlier
    if (show_issues(in_memory.issues()) >= Issue_status::ERROR) {
      THROW_ERROR0(SHERR_DUMP_COMPATIBILITY_OPTIONS_FAILED);
    }
  }

  if (m_options.is_dry_run()) {
    return;
  }

  const auto output = make_file(file);
  output->open(Mode::WRITE);

  const auto &content = in_memory.content();
  output->write(content.c_str(), content.length());

  output->close();
}

std::unique_ptr<Dumper::Memory_dumper> Dumper::dump_ddl(
    Schema_dumper *dumper,
    const std::function<void(Memory_dumper *)> &func) const {
  auto memory = std::make_unique<Memory_dumper>(dumper);

  memory->dump(func);

  return memory;
}

std::unique_ptr<Dumper::Memory_dumper> Dumper::dump_schema(
    Schema_dumper *dumper, const std::string &schema) const {
  return dump_ddl(dumper, [&schema, this](Memory_dumper *m) {
    m->dump(&Schema_dumper::write_comment, schema, std::string{});
    m->dump(&Schema_dumper::dump_schema_ddl, schema);

    if (m_options.dump_events()) {
      m->dump(&Schema_dumper::dump_events_ddl, schema);
    }

    if (m_options.dump_routines()) {
      m->dump(&Schema_dumper::dump_routines_ddl, schema);
    }
  });
}

std::unique_ptr<Dumper::Memory_dumper> Dumper::dump_table(
    Schema_dumper *dumper, const std::string &schema,
    const std::string &table) const {
  return dump_ddl(dumper, [&schema, &table](Memory_dumper *m) {
    m->dump(&Schema_dumper::write_comment, schema, table);
    m->dump(&Schema_dumper::dump_table_ddl, schema, table);
  });
}

std::unique_ptr<Dumper::Memory_dumper> Dumper::dump_triggers(
    Schema_dumper *dumper, const std::string &schema,
    const std::string &table) const {
  return dump_ddl(dumper, [&schema, &table](Memory_dumper *m) {
    m->dump(&Schema_dumper::write_comment, schema, table);
    m->dump(&Schema_dumper::dump_triggers_for_table_ddl, schema, table);
  });
}

std::unique_ptr<Dumper::Memory_dumper> Dumper::dump_temporary_view(
    Schema_dumper *dumper, const std::string &schema,
    const std::string &view) const {
  return dump_ddl(dumper, [&schema, &view](Memory_dumper *m) {
    m->dump(&Schema_dumper::write_comment, schema, view);
    m->dump(&Schema_dumper::dump_temporary_view_ddl, schema, view);
  });
}

std::unique_ptr<Dumper::Memory_dumper> Dumper::dump_view(
    Schema_dumper *dumper, const std::string &schema,
    const std::string &view) const {
  return dump_ddl(dumper, [&schema, &view](Memory_dumper *m) {
    m->dump(&Schema_dumper::write_comment, schema, view);
    m->dump(&Schema_dumper::dump_view_ddl, schema, view);
  });
}

std::unique_ptr<Dumper::Memory_dumper> Dumper::dump_users(
    Schema_dumper *dumper) const {
  return dump_ddl(dumper, [this](Memory_dumper *m) {
    m->dump(&Schema_dumper::write_comment, std::string{}, std::string{});
    m->dump(&Schema_dumper::dump_grants, m_options.included_users(),
            m_options.excluded_users());
  });
}

void Dumper::create_schema_metadata_tasks() {
  if (m_options.is_dry_run() || m_options.is_export_only()) {
    return;
  }

  Progress_thread::Progress_config config;
  config.current = [this]() -> uint64_t { return m_schema_metadata_written; };
  config.total = [this]() { return m_total_schemas; };

  m_current_stage = m_progress_thread.start_stage("Writing schema metadata",
                                                  std::move(config));

  for (const auto &schema : m_schema_infos) {
    m_worker_tasks.push({"writing metadata of " + schema.quoted_name,
                         [&schema](Table_worker *worker) {
                           worker->write_schema_metadata(schema);
                         }},
                        shcore::Queue_priority::HIGH);
  }
}

void Dumper::create_schema_ddl_tasks() {
  if (!m_options.dump_ddl()) {
    return;
  }

  Progress_thread::Progress_config config;

  config.current = [this]() -> uint64_t { return m_ddl_written; };
  config.total = [this]() { return m_total_objects; };

  m_current_stage =
      m_progress_thread.start_stage("Writing DDL", std::move(config));

  for (const auto &schema : m_schema_infos) {
    m_worker_tasks.push(
        {"writing DDL of " + schema.quoted_name,
         [&schema](Table_worker *worker) { worker->dump_schema_ddl(schema); }},
        shcore::Queue_priority::HIGH);

    for (const auto &view : schema.views) {
      m_worker_tasks.push({"writing DDL of " + view.quoted_name,
                           [&schema, &view](Table_worker *worker) {
                             worker->dump_view_ddl(schema, view);
                           }},
                          shcore::Queue_priority::HIGH);
    }

    for (const auto &table : schema.tables) {
      m_worker_tasks.push({"writing DDL of " + table.quoted_name,
                           [&schema, &table](Table_worker *worker) {
                             worker->dump_table_ddl(schema, table);
                           }},
                          shcore::Queue_priority::HIGH);
    }
  }
}

void Dumper::create_table_tasks() {
  m_chunking_tasks = 0;

  m_main_thread_finished_producing_chunking_tasks = false;

  m_all_table_metadata_tasks_scheduled = false;

  const auto write_metadata =
      !m_options.is_dry_run() && !m_options.is_export_only();

  if (write_metadata) {
    Progress_thread::Progress_config config;

    config.current = [this]() -> uint64_t { return m_table_metadata_written; };
    config.total = [this]() -> uint64_t { return m_table_metadata_to_write; };
    config.is_total_known = [this]() {
      return m_all_table_metadata_tasks_scheduled;
    };

    m_current_stage = m_progress_thread.start_stage("Writing table metadata",
                                                    std::move(config));
  }

  for (const auto &schema : m_schema_infos) {
    for (const auto &table : schema.tables) {
      auto task = create_table_task(schema, table);

      if (write_metadata && should_dump_data(task)) {
        ++m_table_metadata_to_write;

        m_worker_tasks.push({"writing metadata of " + table.quoted_name,
                             [task](Table_worker *worker) {
                               worker->write_table_metadata(task);
                             }},
                            shcore::Queue_priority::HIGH);
      }

      if (m_options.dump_data()) {
        push_table_task(std::move(task));
      }
    }
  }

  m_all_table_metadata_tasks_scheduled = true;

  m_main_thread_finished_producing_chunking_tasks = true;
}

Dumper::Table_task Dumper::create_table_task(const Schema_info &schema,
                                             const Table_info &table) {
  Table_task task;
  task.task_name = table.quoted_name;
  task.name = table.name;
  task.quoted_name = table.quoted_name;
  task.schema = schema.name;
  task.basename = table.basename;
  task.info = table.info;
  task.partitions = table.partitions;

  on_create_table_task(task.schema, task.name, task.info);

  return task;
}

void Dumper::push_table_task(Table_task &&task) {
  const auto &task_name = task.task_name;

  if (!should_dump_data(task)) {
    current_console()->print_warning("Skipping data dump for table " +
                                     task_name);
    return;
  }

  log_info("Preparing data dump for table %s", task_name.c_str());

  const auto &index = task.info->index;

  const auto get_index = [&index]() {
    return std::string{"column"} + (index.columns().size() > 1 ? "s" : "") +
           " " + index.columns_sql();
  };

  if (m_options.split()) {
    if (!index.valid()) {
      current_console()->print_note(
          "Could not select columns to be used as an index for table " +
          task_name +
          ". Chunking has been disabled for this table, data will be dumped to "
          "a single file.");
    } else {
      log_info("Data dump for table %s will be chunked using %s",
               task_name.c_str(), get_index().c_str());
    }
  } else {
    const auto index_info =
        (!index.valid() ? "will not use an index"
                        : "will use " + get_index() + " as an index");
    log_info("Data dump for table %s %s", task_name.c_str(),
             index_info.c_str());
  }

  if (m_options.is_dry_run()) {
    return;
  }

  if (m_options.is_export_only() || task.partitions.empty()) {
    push_table_chunking_task(std::move(task));
  } else {
    for (const auto &partition : task.partitions) {
      auto copy = task;

      copy.task_name =
          task.task_name + " partition " + partition.info->quoted_name;
      copy.basename = partition.basename;
      copy.partition = partition.info;

      push_table_chunking_task(std::move(copy));
    }
  }
}

void Dumper::push_table_chunking_task(Table_task &&task) {
  ++m_chunking_tasks;

  std::string info = "chunking " + task.task_name;
  m_worker_tasks.push({std::move(info),
                       [task = std::move(task)](Table_worker *worker) {
                         ++worker->m_dumper->m_num_threads_chunking;

                         worker->create_table_data_tasks(task);

                         --worker->m_dumper->m_num_threads_chunking;
                       }},
                      shcore::Queue_priority::MEDIUM);
}

Dump_writer *Dumper::get_table_data_writer(const std::string &filename) {
  // TODO(pawel): in the future, it's going to be possible to dump into a single
  //              SQL file: use a different type of writer, return the same
  //              pointer each time
  std::lock_guard<std::mutex> lock(m_worker_writers_mutex);

  // create new writer if we're writing to multiple files, or to a single file
  // and writer hasn't been created yet
  if (!m_options.use_single_file() || m_worker_writers.empty()) {
    // if we're writing to a single file, simply use the provided name
    auto file = m_options.use_single_file()
                    ? std::move(m_output_file)
                    : make_file(filename + k_dump_in_progress_ext, true);
    auto compressed_file =
        mysqlshdk::storage::make_file(std::move(file), m_options.compression());
    std::unique_ptr<Dump_writer> writer;

    if (import_table::Dialect::default_() == m_options.dialect()) {
      writer =
          std::make_unique<Default_dump_writer>(std::move(compressed_file));
    } else if (import_table::Dialect::json() == m_options.dialect()) {
      writer = std::make_unique<Json_dump_writer>(std::move(compressed_file));
    } else if (import_table::Dialect::csv() == m_options.dialect()) {
      writer = std::make_unique<Csv_dump_writer>(std::move(compressed_file));
    } else if (import_table::Dialect::tsv() == m_options.dialect()) {
      writer = std::make_unique<Tsv_dump_writer>(std::move(compressed_file));
    } else if (import_table::Dialect::csv_unix() == m_options.dialect()) {
      writer =
          std::make_unique<Csv_unix_dump_writer>(std::move(compressed_file));
    } else {
      writer = std::make_unique<Text_dump_writer>(std::move(compressed_file),
                                                  m_options.dialect());
    }

    m_worker_writers.emplace_back(std::move(writer));
  }

  return m_worker_writers.back().get();
}

std::size_t Dumper::finish_writing(Dump_writer *writer, uint64_t total_bytes) {
  std::size_t file_size = 0;

  // close the file if we're writing to multiple files, otherwise the single
  // file is going to be closed when all tasks are finished
  if (!m_options.use_single_file()) {
    std::string final_filename = close_file(*writer);

    {
      std::lock_guard<std::mutex> lock(m_table_data_bytes_mutex);
      m_chunk_file_bytes[final_filename] = total_bytes;
    }

    file_size = writer->output()->file_size();

    {
      std::lock_guard<std::mutex> lock(m_worker_writers_mutex);

      m_worker_writers.erase(
          std::remove_if(m_worker_writers.begin(), m_worker_writers.end(),
                         [writer](const auto &w) { return w.get() == writer; }),
          m_worker_writers.end());
    }
  }

  return file_size;
}

std::string Dumper::close_file(const Dump_writer &writer) const {
  const auto output = writer.output();

  if (output->is_open()) {
    output->close();
  }

  const auto filename = output->filename();
  const auto trimmed = trim_in_progress_extension(filename);

  if (trimmed != filename) {
    output->rename(trimmed);
  }
  return trimmed;
}

void Dumper::write_metadata() const {
  if (m_options.is_export_only()) {
    return;
  }

  write_dump_started_metadata();
}

void Dumper::write_dump_started_metadata() const {
  if (m_options.is_export_only()) {
    return;
  }

  using rapidjson::Document;
  using rapidjson::StringRef;
  using rapidjson::Type;
  using rapidjson::Value;

  Document doc{Type::kObjectType};
  auto &a = doc.GetAllocator();

  const auto mysqlsh = std::string("mysqlsh ") + shcore::get_long_version();
  doc.AddMember(StringRef("dumper"), refs(mysqlsh), a);
  doc.AddMember(StringRef("version"), StringRef(Schema_dumper::version()), a);
  doc.AddMember(StringRef("origin"), StringRef(name()), a);

  {
    // list of schemas
    Value schemas{Type::kArrayType};

    for (const auto &schema : m_schema_infos) {
      schemas.PushBack(refs(schema.name), a);
    }

    doc.AddMember(StringRef("schemas"), std::move(schemas), a);
  }

  {
    // map of basenames
    Value basenames{Type::kObjectType};

    for (const auto &schema : m_schema_infos) {
      basenames.AddMember(refs(schema.name), refs(schema.basename), a);
    }

    doc.AddMember(StringRef("basenames"), std::move(basenames), a);
  }

  if (dump_users()) {
    const auto dumper = schema_dumper(session());

    // list of users
    Value users{Type::kArrayType};

    for (const auto &user : dumper->get_users(m_options.included_users(),
                                              m_options.excluded_users())) {
      users.PushBack({shcore::make_account(user).c_str(), a}, a);
    }

    doc.AddMember(StringRef("users"), std::move(users), a);
  }

  doc.AddMember(StringRef("defaultCharacterSet"),
                refs(m_options.character_set()), a);
  doc.AddMember(StringRef("tzUtc"), m_options.use_timezone_utc(), a);
  doc.AddMember(StringRef("bytesPerChunk"), m_options.bytes_per_chunk(), a);

  doc.AddMember(StringRef("user"), refs(m_cache.user), a);
  doc.AddMember(StringRef("hostname"), refs(m_cache.hostname), a);
  doc.AddMember(StringRef("server"), refs(m_cache.server), a);
  doc.AddMember(StringRef("serverVersion"),
                {m_cache.server_version.get_full().c_str(), a}, a);

  if (m_options.dump_binlog_info()) {
    doc.AddMember(StringRef("binlogFile"), refs(m_cache.binlog.file), a);
    doc.AddMember(StringRef("binlogPosition"), m_cache.binlog.position, a);
  }

  doc.AddMember(StringRef("gtidExecuted"), refs(m_cache.gtid_executed), a);
  doc.AddMember(StringRef("gtidExecutedInconsistent"),
                is_gtid_executed_inconsistent(), a);
  doc.AddMember(StringRef("consistent"), m_options.consistent_dump(), a);

  if (m_options.mds_compatibility()) {
    bool compat = static_cast<bool>(m_options.mds_compatibility());
    doc.AddMember(StringRef("mdsCompatibility"), compat, a);
  }

  {
    // list of compatibility options
    Value compatibility{Type::kArrayType};

    for (const auto &option : m_options.compatibility_options().values()) {
      compatibility.PushBack({to_string(option).c_str(), a}, a);
    }

    doc.AddMember(StringRef("compatibilityOptions"), std::move(compatibility),
                  a);
  }

  {
    // list of used capabilities
    Value capabilities{Type::kArrayType};

    for (const auto &c : m_used_capabilities) {
      Value capability{Type::kObjectType};

      capability.AddMember(StringRef("id"), {capability::id(c).c_str(), a}, a);
      capability.AddMember(StringRef("description"),
                           {capability::description(c).c_str(), a}, a);
      capability.AddMember(
          StringRef("versionRequired"),
          {capability::version_required(c).get_base().c_str(), a}, a);

      capabilities.PushBack(std::move(capability), a);
    }

    doc.AddMember(StringRef("capabilities"), std::move(capabilities), a);
  }

  doc.AddMember(StringRef("begin"),
                refs(m_progress_thread.duration().started_at()), a);

  write_json(make_file("@.json"), &doc);
}

void Dumper::write_dump_finished_metadata() const {
  if (m_options.is_export_only()) {
    return;
  }

  using rapidjson::Document;
  using rapidjson::StringRef;
  using rapidjson::Type;
  using rapidjson::Value;

  Document doc{Type::kObjectType};
  auto &a = doc.GetAllocator();

  // We cannot use m_progress_thread.duration().finished_at() here, because
  // progress thread was not terminated yet, as it needs to optionally show
  // progress when closing the output directory, if ociParManifest was enabled.
  // We cannot close it before writing the @.done.json, because manifest will be
  // missing that file, and loader will assume that the dump is not complete.
  // We're writing the current time here, it's a good approximation, which will
  // be off only if writing the manifest takes a lot of time. On the other hand,
  // dump is now complete, writing the manifest is an extra step, and the
  // summary will include the time it takes.
  doc.AddMember(StringRef("end"),
                {Progress_thread::Duration::current_time().c_str(), a}, a);
  doc.AddMember(StringRef("dataBytes"), m_data_bytes.load(), a);

  {
    Value schemas{Type::kObjectType};

    for (const auto &schema : m_table_data_bytes) {
      Value tables{Type::kObjectType};

      for (const auto &table : schema.second) {
        tables.AddMember(refs(table.first), table.second, a);
      }

      schemas.AddMember(refs(schema.first), std::move(tables), a);
    }

    doc.AddMember(StringRef("tableDataBytes"), std::move(schemas), a);
  }

  {
    Value files{Type::kObjectType};

    for (const auto &file : m_chunk_file_bytes) {
      Value tables{Type::kObjectType};

      files.AddMember(refs(file.first), file.second, a);
    }
    doc.AddMember(StringRef("chunkFileBytes"), std::move(files), a);
  }

  write_json(make_file("@.done.json"), &doc);
}

void Dumper::write_schema_metadata(const Schema_info &schema) const {
  if (m_options.is_export_only()) {
    return;
  }

  using rapidjson::Document;
  using rapidjson::StringRef;
  using rapidjson::Type;
  using rapidjson::Value;

  Document doc{Type::kObjectType};
  auto &a = doc.GetAllocator();

  doc.AddMember(StringRef("schema"), refs(schema.name), a);
  doc.AddMember(StringRef("includesDdl"), m_options.dump_ddl(), a);
  doc.AddMember(StringRef("includesViewsDdl"), m_options.dump_ddl(), a);
  doc.AddMember(StringRef("includesData"), m_options.dump_data(), a);

  {
    // list of tables
    Value tables{Type::kArrayType};

    for (const auto &table : schema.tables) {
      tables.PushBack(refs(table.name), a);
    }

    doc.AddMember(StringRef("tables"), std::move(tables), a);
  }

  if (m_options.dump_ddl()) {
    // list of views
    Value views{Type::kArrayType};

    for (const auto &view : schema.views) {
      views.PushBack(refs(view.name), a);
    }

    doc.AddMember(StringRef("views"), std::move(views), a);
  }

  if (m_options.dump_ddl()) {
    const auto dumper = schema_dumper(session());

    if (m_options.dump_events()) {
      // list of events
      Value events{Type::kArrayType};

      for (const auto &event : dumper->get_events(schema.name)) {
        events.PushBack({event.c_str(), a}, a);
      }

      doc.AddMember(StringRef("events"), std::move(events), a);
    }

    if (m_options.dump_routines()) {
      // list of functions
      Value functions{Type::kArrayType};

      for (const auto &function :
           dumper->get_routines(schema.name, "FUNCTION")) {
        functions.PushBack({function.c_str(), a}, a);
      }

      doc.AddMember(StringRef("functions"), std::move(functions), a);
    }

    if (m_options.dump_routines()) {
      // list of stored procedures
      Value procedures{Type::kArrayType};

      for (const auto &procedure :
           dumper->get_routines(schema.name, "PROCEDURE")) {
        procedures.PushBack({procedure.c_str(), a}, a);
      }

      doc.AddMember(StringRef("procedures"), std::move(procedures), a);
    }
  }

  {
    // map of basenames
    Value basenames{Type::kObjectType};

    for (const auto &table : schema.tables) {
      basenames.AddMember(refs(table.name), refs(table.basename), a);
    }

    for (const auto &view : schema.views) {
      basenames.AddMember(refs(view.name), refs(view.basename), a);
    }

    doc.AddMember(StringRef("basenames"), std::move(basenames), a);
  }

  write_json(make_file(get_schema_filename(schema.basename, "json")), &doc);
}

void Dumper::write_table_metadata(
    const Table_task &table,
    const std::shared_ptr<mysqlshdk::db::ISession> &session) const {
  if (m_options.is_export_only()) {
    return;
  }

  using rapidjson::Document;
  using rapidjson::StringRef;
  using rapidjson::Type;
  using rapidjson::Value;

  Document doc{Type::kObjectType};
  auto &a = doc.GetAllocator();

  {
    // options - to be used by importer
    Value options{Type::kObjectType};

    options.AddMember(StringRef("schema"), refs(table.schema), a);
    options.AddMember(StringRef("table"), refs(table.name), a);

    Value cols{Type::kArrayType};
    Value decode{Type::kObjectType};

    for (const auto &c : table.info->columns) {
      cols.PushBack(refs(c->name), a);

      if (c->csv_unsafe) {
        decode.AddMember(
            refs(c->name),
            StringRef(m_options.use_base64() ? "FROM_BASE64" : "UNHEX"), a);
      }
    }

    options.AddMember(StringRef("columns"), std::move(cols), a);

    if (!decode.ObjectEmpty()) {
      options.AddMember(StringRef("decodeColumns"), std::move(decode), a);
    }

    options.AddMember(StringRef("defaultCharacterSet"),
                      refs(m_options.character_set()), a);

    options.AddMember(StringRef("fieldsTerminatedBy"),
                      refs(m_options.dialect().fields_terminated_by), a);
    options.AddMember(StringRef("fieldsEnclosedBy"),
                      refs(m_options.dialect().fields_enclosed_by), a);
    options.AddMember(StringRef("fieldsOptionallyEnclosed"),
                      m_options.dialect().fields_optionally_enclosed, a);
    options.AddMember(StringRef("fieldsEscapedBy"),
                      refs(m_options.dialect().fields_escaped_by), a);
    options.AddMember(StringRef("linesTerminatedBy"),
                      refs(m_options.dialect().lines_terminated_by), a);

    doc.AddMember(StringRef("options"), std::move(options), a);
  }

  const auto dumper = schema_dumper(session);

  if (m_options.dump_triggers() && m_options.dump_ddl()) {
    // list of triggers
    Value triggers{Type::kArrayType};

    for (const auto &trigger : dumper->get_triggers(table.schema, table.name)) {
      triggers.PushBack({trigger.c_str(), a}, a);
    }

    doc.AddMember(StringRef("triggers"), std::move(triggers), a);
  }

  const auto all_histograms = dumper->get_histograms(table.schema, table.name);

  if (!all_histograms.empty()) {
    // list of histograms
    Value histograms{Type::kArrayType};

    for (const auto &histogram : all_histograms) {
      Value h{Type::kObjectType};

      h.AddMember(StringRef("column"), refs(histogram.column), a);
      h.AddMember(StringRef("buckets"),
                  static_cast<uint64_t>(histogram.buckets), a);

      histograms.PushBack(std::move(h), a);
    }

    doc.AddMember(StringRef("histograms"), std::move(histograms), a);
  }

  doc.AddMember(StringRef("includesData"), m_options.dump_data(), a);
  doc.AddMember(StringRef("includesDdl"), m_options.dump_ddl(), a);

  doc.AddMember(StringRef("extension"), {get_table_data_ext().c_str(), a}, a);
  doc.AddMember(StringRef("chunking"), is_chunked(table), a);
  doc.AddMember(
      StringRef("compression"),
      {mysqlshdk::storage::to_string(m_options.compression()).c_str(), a}, a);

  {
    Value primary{Type::kArrayType};

    if (table.info->index.primary()) {
      for (const auto &column : table.info->index.columns()) {
        primary.PushBack(refs(column->name), a);
      }
    }

    doc.AddMember(StringRef("primaryIndex"), std::move(primary), a);
  }

  {
    // list of partitions
    Value partitions{Type::kArrayType};

    for (const auto &partition : table.partitions) {
      partitions.PushBack(refs(partition.info->name), a);
    }

    doc.AddMember(StringRef("partitions"), std::move(partitions), a);
  }

  {
    // map of partition basenames
    Value basenames{Type::kObjectType};

    for (const auto &partition : table.partitions) {
      basenames.AddMember(refs(partition.info->name), refs(partition.basename),
                          a);
    }

    doc.AddMember(StringRef("basenames"), std::move(basenames), a);
  }

  write_json(make_file(dump::get_table_data_filename(table.basename, "json")),
             &doc);
}

void Dumper::summarize() const {
  const auto console = current_console();

  if (!m_options.consistent_dump() && m_options.threads() > 1) {
    const auto gtid_executed = schema_dumper(session())->gtid_executed(true);

    if (gtid_executed != m_cache.gtid_executed) {
      console->print_warning(
          "The value of the gtid_executed system variable has changed during "
          "the dump, from: '" +
          m_cache.gtid_executed + "' to: '" + gtid_executed + "'.");
    }
  }

  console->print_status("Dump duration: " +
                        m_data_dump_stage->duration().to_string());
  console->print_status("Total duration: " +
                        m_progress_thread.duration().to_string());

  if (!m_options.is_export_only()) {
    console->print_status("Schemas dumped: " + std::to_string(m_total_schemas));
    console->print_status("Tables dumped: " + std::to_string(m_total_tables));
  }

  console->print_status(
      std::string{compressed() ? "Uncompressed d" : "D"} +
      "ata size: " + mysqlshdk::utils::format_bytes(m_data_bytes));

  if (compressed()) {
    console->print_status("Compressed data size: " +
                          mysqlshdk::utils::format_bytes(m_bytes_written));
    console->print_status(shcore::str_format(
        "Compression ratio: %.1f",
        m_data_bytes / std::max(static_cast<double>(m_bytes_written), 1.0)));
  }

  console->print_status("Rows written: " + std::to_string(m_rows_written));
  console->print_status("Bytes written: " +
                        mysqlshdk::utils::format_bytes(m_bytes_written));
  console->print_status(
      std::string{"Average "} + (compressed() ? "uncompressed " : "") +
      "throughput: " +
      mysqlshdk::utils::format_throughput_bytes(
          m_data_bytes, m_data_dump_stage->duration().seconds()));

  if (compressed()) {
    console->print_status(
        "Average compressed throughput: " +
        mysqlshdk::utils::format_throughput_bytes(
            m_bytes_written, m_data_dump_stage->duration().seconds()));
  }

  summary();
}

void Dumper::rethrow() const {
  for (const auto &exc : m_worker_exceptions) {
    if (exc) {
      THROW_ERROR0(SHERR_DUMP_WORKER_THREAD_FATAL_ERROR);
    }
  }
}

void Dumper::emergency_shutdown() {
  m_worker_interrupt = true;

  const auto workers = m_workers.size();

  if (workers > 0) {
    m_worker_tasks.shutdown(workers);
  }
}

void Dumper::kill_workers() {
  emergency_shutdown();
  wait_for_all_tasks();
}

std::string Dumper::get_table_data_filename(const std::string &basename) const {
  return dump::get_table_data_filename(basename, get_table_data_ext());
}

std::string Dumper::get_table_data_filename(const std::string &basename,
                                            const std::size_t idx,
                                            const bool last_chunk) const {
  return dump::get_table_data_filename(basename, get_table_data_ext(), idx,
                                       last_chunk);
}

std::string Dumper::get_table_data_ext() const {
  using import_table::Dialect;

  const auto dialect = m_options.dialect();
  auto extension = "txt";

  if (dialect == Dialect::default_() || dialect == Dialect::tsv()) {
    extension = "tsv";
  } else if (dialect == Dialect::csv() || dialect == Dialect::csv_unix()) {
    extension = "csv";
  } else if (dialect == Dialect::json()) {
    extension = "json";
  }

  return extension + mysqlshdk::storage::get_extension(m_options.compression());
}

void Dumper::initialize_progress() {
  Progress_thread::Throughput_config config;

  config.items_full = "rows";
  config.items_abbrev = "rows";
  config.item_singular = "row";
  config.item_plural = "rows";
  config.space_before_item = true;
  config.total_is_approx = true;
  config.current = [this]() -> uint64_t { return m_rows_written; };
  config.total = [this]() { return m_total_rows; };

  if (!m_options.is_export_only()) {
    config.left_label = [this]() {
      const uint64_t chunking = m_num_threads_chunking;
      const uint64_t dumping = m_num_threads_dumping;
      std::string label;

      if (0 != chunking || 0 != dumping) {
        if (0 == chunking) {
          label = std::to_string(dumping) + " thds";
        } else {
          label = std::to_string(chunking) + " thds chunking, " +
                  std::to_string(dumping);
        }

        label += " dumping - ";
      }

      return label;
    };
  }

  config.right_label = [this]() { return ", " + throughput(); };
  config.on_display_started = []() {
    current_console()->print_status("Starting data dump");
  };

  m_data_dump_stage = m_current_stage =
      m_progress_thread.start_stage("Dumping data", std::move(config));
}

void Dumper::update_bytes_written(uint64_t new_bytes) {
  m_bytes_written += new_bytes;
}

void Dumper::update_progress(uint64_t new_rows,
                             const Dump_write_result &new_bytes) {
  m_rows_written += new_rows;
  update_bytes_written(new_bytes.bytes_written());
  m_data_bytes += new_bytes.data_bytes();

  {
    std::lock_guard<std::mutex> lock(m_table_data_bytes_mutex);
    m_table_data_bytes[new_bytes.schema()][new_bytes.table()] +=
        new_bytes.data_bytes();
  }

  {
    std::unique_lock<std::recursive_mutex> lock(m_throughput_mutex,
                                                std::try_to_lock);
    if (lock.owns_lock()) {
      m_data_throughput->push(m_data_bytes);
      m_bytes_throughput->push(m_bytes_written);
    }
  }
}

void Dumper::shutdown_progress() { m_progress_thread.finish(); }

std::string Dumper::throughput() const {
  std::lock_guard<std::recursive_mutex> lock(m_throughput_mutex);

  return mysqlshdk::utils::format_throughput_bytes(m_data_throughput->rate(),
                                                   1.0) +
         (compressed() ? " uncompressed, " +
                             mysqlshdk::utils::format_throughput_bytes(
                                 m_bytes_throughput->rate(), 1.0) +
                             " compressed"
                       : "");
}

mysqlshdk::storage::IDirectory *Dumper::directory() const {
  return m_output_dir.get();
}

std::unique_ptr<mysqlshdk::storage::IFile> Dumper::make_file(
    const std::string &filename, bool use_mmap) const {
  static const char *s_mmap_mode = nullptr;
  if (s_mmap_mode == nullptr) {
    s_mmap_mode = getenv("MYSQLSH_MMAP");
    if (!s_mmap_mode) s_mmap_mode = "on";
  }

  mysqlshdk::storage::File_options options;
  if (use_mmap) options["file.mmap"] = s_mmap_mode;
  return directory()->file(filename, options);
}

std::string Dumper::get_basename(const std::string &basename) {
  // 255 characters total:
  // - 225 - base name
  // -   5 - base name ordinal number
  // -   2 - '@@'
  // -   5 - chunk ordinal number
  // -  10 - extension
  // -   8 - '.dumping' extension
  static const std::size_t max_length = 225;
  const auto wbasename = shcore::utf8_to_wide(basename);
  const auto wtruncated = shcore::truncate(wbasename, max_length);

  if (wbasename.length() != wtruncated.length()) {
    const auto truncated = shcore::wide_to_utf8(wtruncated);
    const auto ordinal = m_truncated_basenames[truncated]++;
    return truncated + std::to_string(ordinal);
  } else {
    return basename;
  }
}

bool Dumper::compressed() const {
  return mysqlshdk::storage::Compression::NONE != m_options.compression();
}

void Dumper::kill_query() const {
  const auto &s = session();

  if (s) {
    try {
      // establish_session() cannot be used here, as it's going to create
      // interrupt handler of its own
      const auto &co = s->get_connection_options();
      std::shared_ptr<mysqlshdk::db::ISession> kill_session;

      switch (co.get_session_type()) {
        case mysqlsh::SessionType::X:
          kill_session = mysqlshdk::db::mysqlx::Session::create();
          break;

        case mysqlsh::SessionType::Classic:
          kill_session = mysqlshdk::db::mysql::Session::create();
          break;

        default:
          throw std::runtime_error("Unsupported session type.");
      }

      kill_session->connect(co);
      executef(kill_session, "KILL QUERY ?", s->get_connection_id());
      kill_session->close();
    } catch (const std::exception &e) {
      log_warning("Error canceling SQL query: %s", e.what());
    }
  }
}

std::string Dumper::get_query_comment(const std::string &quoted_name,
                                      const std::string &id,
                                      const char *context) const {
  return "/* mysqlsh " +
         shcore::get_member_name(name(), shcore::current_naming_style()) +
         ", " + context + " table " +
         // sanitize schema/table names in case they contain a '*/'
         // *\/ isn't really a valid escape, but it doesn't matter because we
         // just want the lexer to not see */
         shcore::str_replace(quoted_name, "*/", "*\\/") + ", chunk ID: " + id +
         " */";
}

std::string Dumper::get_query_comment(const Table_data_task &task,
                                      const char *context) const {
  return get_query_comment(task.task_name, task.id, context);
}

bool Dumper::is_chunked(const Table_task &task) const {
  return m_options.split() && task.info->index.valid();
}

bool Dumper::should_dump_data(const Table_task &table) {
  if (table.schema == "mysql" &&
      (table.name == "apply_status" || table.name == "general_log" ||
       table.name == "schema" || table.name == "slow_log")) {
    return false;
  } else {
    return true;
  }
}

void Dumper::validate_privileges() const {
  std::set<std::string> all_required;
  std::set<std::string> global_required;
  std::set<std::string> schema_required;
  std::set<std::string> table_required;

  if (m_options.dump_events()) {
    std::string event{"EVENT"};
    all_required.emplace(event);
    schema_required.emplace(std::move(event));
  }

  if (m_options.dump_triggers()) {
    std::string trigger{"TRIGGER"};
    all_required.emplace(trigger);
    table_required.emplace(std::move(trigger));
  }

  if (!m_cache.server_is_8_0) {
    // need to explicitly check for SELECT privilege, otherwise some queries
    // will return empty results
    std::string select{"SELECT"};
    all_required.emplace(select);
    table_required.emplace(std::move(select));
  }

  if (m_cache.server_is_5_6 && dump_users()) {
    // on 5.6, SUPER is required to get the real hash value from SHOW GRANTS
    std::string super{"SUPER"};
    all_required.emplace(super);
    global_required.emplace(std::move(super));
  }

  if (!all_required.empty()) {
    using mysqlshdk::mysql::User_privileges_result;

    const auto get_missing = [](const User_privileges_result &result,
                                const std::set<std::string> &required) {
      std::set<std::string> missing;

      std::set_intersection(result.missing_privileges().begin(),
                            result.missing_privileges().end(), required.begin(),
                            required.end(),
                            std::inserter(missing, missing.begin()));

      return missing;
    };

    const auto global_result = m_user_privileges->validate(all_required);
    const auto global_missing = get_missing(global_result, global_required);

    if (!global_missing.empty()) {
      THROW_ERROR(SHERR_DUMP_MISSING_GLOBAL_PRIVILEGES, m_user_account.c_str(),
                  shcore::str_join(global_missing, ", ").c_str());
    }

    if (global_result.has_missing_privileges()) {
      {
        // global privileges can be safely removed from the all_required set
        std::set<std::string> temporary;

        std::set_difference(all_required.begin(), all_required.end(),
                            global_required.begin(), global_required.end(),
                            std::inserter(temporary, temporary.begin()));

        all_required = std::move(temporary);
      }

      // user has all required global privileges
      // user doesn't have *.* schema/table-level privileges, check schemas
      for (const auto &schema : m_schema_infos) {
        const auto schema_result =
            m_user_privileges->validate(all_required, schema.name);
        const auto schema_missing = get_missing(schema_result, schema_required);

        if (!schema_missing.empty()) {
          THROW_ERROR(SHERR_DUMP_MISSING_SCHEMA_PRIVILEGES,
                      m_user_account.c_str(), schema.quoted_name.c_str(),
                      shcore::str_join(schema_missing, ", ").c_str());
        }

        if (schema_result.has_missing_privileges()) {
          // user has all required schema-level privileges for this schema
          // user doesn't have schema.* table-level privileges, check tables
          for (const auto &table : schema.tables) {
            const auto table_result = m_user_privileges->validate(
                all_required, schema.name, table.name);

            // if at this stage there are any missing privileges, they are all
            // table-level ones
            if (table_result.has_missing_privileges()) {
              THROW_ERROR(
                  SHERR_DUMP_MISSING_TABLE_PRIVILEGES, m_user_account.c_str(),
                  table.quoted_name.c_str(),
                  shcore::str_join(table_result.missing_privileges(), ", ")
                      .c_str());
            }
          }
        }
      }
    }
  }
}

bool Dumper::is_gtid_executed_inconsistent() const {
  return !m_options.consistent_dump();
}

void Dumper::validate_schemas_list() const {
  if (!dump_users() && m_cache.schemas.empty()) {
    THROW_ERROR0(SHERR_DUMP_NO_SCHEMAS_SELECTED);
  }
}

void Dumper::log_server_version() const {
  log_info("Source server: %s",
           query("SELECT CONCAT(@@version, ' ', @@version_comment)")
               ->fetch_one()
               ->get_string(0)
               .c_str());
}

void Dumper::print_object_stats() const {
  auto stats = object_stats(m_cache.filtered, m_cache.total);

  if (stats.empty()) {
    return;
  }

#define FORMAT_OBJECT_STATS(type)                                         \
  do {                                                                    \
    if (m_options.dump_##type() && 0 != m_cache.total.type) {             \
      stats.emplace_back(format_object_stats(m_cache.filtered.type,       \
                                             m_cache.total.type, #type)); \
    }                                                                     \
  } while (false)

  if (m_options.dump_ddl()) {
    FORMAT_OBJECT_STATS(events);
    FORMAT_OBJECT_STATS(routines);
    FORMAT_OBJECT_STATS(triggers);
  }

#undef FORMAT_OBJECT_STATS

  std::string msg = stats.front() + " will be dumped";

  auto current = std::next(stats.begin());
  const auto end = stats.end();

  if (end != current) {
    msg += " and within them";
  } else {
    msg += '.';
  }

  for (; end != current; ++current) {
    msg += ' ' + (*current) + ',';
  }

  msg.back() = '.';

  const auto console = current_console();

  console->print_status(msg);

  if (m_options.dump_users()) {
    if (m_skip_grant_tables_active) {
      console->print_warning(
          "Server is running with the --skip-grant-tables option active, "
          "dumping users, roles and grants has been disabled.");
    } else {
      console->print_status(format_object_stats(m_cache.filtered.users,
                                                m_cache.total.users, "users") +
                            " will be dumped.");
    }
  }
}

std::string Dumper::format_object_stats(uint64_t value, uint64_t total,
                                        const std::string &object) {
  std::string format;

  if (value != total) {
    format += std::to_string(value) + " out of ";
  }

  format += std::to_string(total) + " " + object;

  if (1 == total && 's' == format.back()) {
    format.pop_back();
  }

  return format;
}

bool Dumper::dump_users() const {
  return m_options.dump_users() && !m_skip_grant_tables_active;
}

void Dumper::validate_dump_consistency(
    const std::shared_ptr<mysqlshdk::db::ISession> &session) const {
  if (!m_options.consistent_dump() || m_ftwrl_used || m_user_has_backup_admin) {
    return;
  }

  // if there are still metadata or DDL files to be written, skip this check
  if (!(m_all_table_metadata_tasks_scheduled &&
        m_table_metadata_to_write == m_table_metadata_written &&
        m_total_schemas == m_schema_metadata_written &&
        m_total_objects == m_ddl_written)) {
    return;
  }

  bool consistent = false;
  std::string context;
  auto dumper = schema_dumper(session);

  if (m_gtid_enabled) {
    const auto gtid_executed = dumper->gtid_executed(true);
    consistent = gtid_executed == m_cache.gtid_executed;

    if (!consistent) {
      context =
          "The value of the gtid_executed "
          "system variable has changed during the dump, from: '" +
          m_cache.gtid_executed + "' to: '" + gtid_executed + "'.";
    }
  } else {
    // gtid_mode is not enabled, check binlog
    const auto binlog = dumper->binlog(true);
    consistent = m_cache.binlog == binlog;

    if (!consistent) {
      context = "The binlog position has changed during the dump, from: '" +
                m_cache.binlog.to_string() + "' to: '" + binlog.to_string() +
                "'.";
    }
  }

  const auto console = current_console();

  if (!consistent) {
    const auto msg = "Backup lock is not " + why_backup_lock_is_missing() +
                     " and DDL changes were not blocked. " + context +
                     " The consistency of the dump cannot be guaranteed.";

    if (shcore::str_caseeq(name(), "dumpInstance")) {
      console->print_error(msg);
      console->print_note(
          "In order to overcome this issue, use a read-only replica with "
          "replication stopped, or, if dumping from a primary, then enable "
          "super_read_only system variable and ensure that any inbound "
          "replication channels are stopped.");
      THROW_ERROR0(SHERR_DUMP_CONSISTENCY_CHECK_FAILED);
    } else {
      console->print_warning(msg);
    }
  } else {
    console->print_note(
        "Backup lock was not acquired, but DDL is consistent, the world may "
        "resume now.");
  }
}

void Dumper::fetch_server_information() {
  const auto instance = mysqlshdk::mysql::Instance(session());

  m_server_version = instance.get_version();
  m_binlog_enabled = instance.get_sysvar_bool("log_bin").get_safe(false);
  m_gtid_enabled = shcore::str_ibeginswith(
      instance.get_sysvar_string("gtid_mode").get_safe("OFF"), "ON");

  DBUG_EXECUTE_IF("dumper_binlog_disabled", { m_binlog_enabled = false; });
  DBUG_EXECUTE_IF("dumper_gtid_disabled", { m_gtid_enabled = false; });
}

}  // namespace dump
}  // namespace mysqlsh
