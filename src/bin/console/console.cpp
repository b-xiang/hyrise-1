#include "console.hpp"

#include <readline/history.h>
#include <readline/readline.h>
#include <sys/stat.h>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <chrono>
#include <csetjmp>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "SQLParser.h"
#include "benchmark_utils.hpp"
#include "concurrency/transaction_context.hpp"
#include "concurrency/transaction_manager.hpp"
#include "operators/get_table.hpp"
#include "operators/import_csv.hpp"
#include "operators/print.hpp"
#include "optimizer/optimizer.hpp"
#include "pagination.hpp"
#include "planviz/lqp_visualizer.hpp"
#include "planviz/sql_query_plan_visualizer.hpp"
#include "scheduler/current_scheduler.hpp"
#include "scheduler/node_queue_scheduler.hpp"
#include "scheduler/topology.hpp"
#include "sql/sql_pipeline_builder.hpp"
#include "sql/sql_pipeline_statement.hpp"
#include "sql/sql_translator.hpp"
#include "storage/storage_manager.hpp"
#include "tpcc/tpcc_table_generator.hpp"
#include "utils/filesystem.hpp"
#include "utils/invalid_input_exception.hpp"
#include "utils/load_table.hpp"
#include "utils/plugin_manager.hpp"
#include "utils/string_functions.hpp"

#define ANSI_COLOR_RED "\x1B[31m"
#define ANSI_COLOR_GREEN "\x1B[32m"
#define ANSI_COLOR_RESET "\x1B[0m"

#define ANSI_COLOR_RED_RL "\001\x1B[31m\002"
#define ANSI_COLOR_GREEN_RL "\001\x1B[32m\002"
#define ANSI_COLOR_RESET_RL "\001\x1B[0m\002"

namespace {

/**
 * Buffer for program state
 *
 * We use this to make Ctrl+C work on all platforms by jumping back into main() from the Ctrl+C signal handler. This
 * was the only way to get this to work on all platforms inclusing macOS.
 * See here (https://github.com/hyrise/hyrise/pull/198#discussion_r135539719) for a discussion about this.
 *
 * The known caveats of goto/longjmp aside, this will probably also cause problems (queries continuing to run in the
 * background) when the scheduler/multithreading is enabled.
 */
sigjmp_buf jmp_env;

// Returns a string containing a timestamp of the current date and time
std::string current_timestamp() {
  auto t = std::time(nullptr);
  auto tm = *std::localtime(&t);

  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return oss.str();
}

// Removes the coloring commands (e.g. '\x1B[31m') from input, to have a clean logfile.
// If remove_rl_codes_only is true, then it only removes the Readline specific escape sequences '\001' and '\002'
std::string remove_coloring(const std::string& input, bool remove_rl_codes_only = false) {
  // matches any characters that need to be escaped in RegEx except for '|'
  std::regex special_chars{R"([-[\]{}()*+?.,\^$#\s])"};
  std::string sequences = "\x1B[31m|\x1B[32m|\x1B[0m|\001|\002";
  if (remove_rl_codes_only) {
    sequences = "\001|\002";
  }
  std::string sanitized_sequences = std::regex_replace(sequences, special_chars, R"(\$&)");

  // Remove coloring commands and escape sequences before writing to logfile
  std::regex expression{"(" + sanitized_sequences + ")"};
  return std::regex_replace(input, expression, "");
}
}  // namespace

namespace opossum {

// Console implementation

Console::Console()
    : _prompt("> "),
      _multiline_input(""),
      _out(std::cout.rdbuf()),
      _log("console.log", std::ios_base::app | std::ios_base::out),
      _verbose(false) {
  // Init readline basics, tells readline to use our custom command completion function
  rl_attempted_completion_function = &Console::_command_completion;
  rl_completer_word_break_characters = const_cast<char*>(" \t\n\"\\'`@$><=;|&{(");  // NOLINT (legacy API)

  // Register default commands to Console
  register_command("exit", std::bind(&Console::_exit, this, std::placeholders::_1));
  register_command("quit", std::bind(&Console::_exit, this, std::placeholders::_1));
  register_command("help", std::bind(&Console::_help, this, std::placeholders::_1));
  register_command("generate", std::bind(&Console::_generate_tpcc, this, std::placeholders::_1));
  register_command("load", std::bind(&Console::_load_table, this, std::placeholders::_1));
  register_command("script", std::bind(&Console::_exec_script, this, std::placeholders::_1));
  register_command("print", std::bind(&Console::_print_table, this, std::placeholders::_1));
  register_command("visualize", std::bind(&Console::_visualize, this, std::placeholders::_1));
  register_command("begin", std::bind(&Console::_begin_transaction, this, std::placeholders::_1));
  register_command("rollback", std::bind(&Console::_rollback_transaction, this, std::placeholders::_1));
  register_command("commit", std::bind(&Console::_commit_transaction, this, std::placeholders::_1));
  register_command("txinfo", std::bind(&Console::_print_transaction_info, this, std::placeholders::_1));
  register_command("pwd", std::bind(&Console::_print_current_working_directory, this, std::placeholders::_1));
  register_command("setting", std::bind(&Console::_change_runtime_setting, this, std::placeholders::_1));
  register_command("load_plugin", std::bind(&Console::_load_plugin, this, std::placeholders::_1));
  register_command("stop_plugin", std::bind(&Console::_stop_plugin, this, std::placeholders::_1));

  // Register words specifically for command completion purposes, e.g.
  // for TPC-C table generation, 'CUSTOMER', 'DISTRICT', etc
  auto tpcc_generators = opossum::TpccTableGenerator::table_generator_functions();
  for (const auto& generator : tpcc_generators) {
    _tpcc_commands.push_back(generator.first);
  }

  _prepared_statements = std::make_shared<PreparedStatementCache>(DefaultCacheCapacity);
}

int Console::read() {
  char* buffer;

  // Prompt user for input
  buffer = readline(_prompt.c_str());
  if (buffer == nullptr) {
    return ReturnCode::Quit;
  }

  std::string input(buffer);
  boost::algorithm::trim<std::string>(input);

  // Only save non-empty commands to history
  if (!input.empty()) {
    add_history(buffer);
    // Save command to history file
    if (!_history_file.empty()) {
      if (append_history(1, _history_file.c_str()) != 0) {
        out("Error appending to history file: " + _history_file + "\n");
      }
    }
  }

  // Free buffer, since readline() allocates new string every time
  free(buffer);  // NOLINT (legacy API)

  return _eval(input);
}

int Console::execute_script(const std::string& filepath) { return _exec_script(filepath); }

int Console::_eval(const std::string& input) {
  // Do nothing if no input was given
  if (input.empty() && _multiline_input.empty()) {
    return ReturnCode::Ok;
  }

  // Dump command to logfile, and to the Console if input comes from a script file
  // Also remove Readline specific escape sequences ('\001' and '\002') to make it look normal
  out(remove_coloring(_prompt + input + "\n", true), _verbose);

  // Check if we already are in multiline input
  if (_multiline_input.empty()) {
    // Check if a registered command was entered
    RegisteredCommands::iterator it;
    if ((it = _commands.find(input.substr(0, input.find_first_of(" \n;")))) != std::end(_commands)) {
      return _eval_command(it->second, input);
    }

    // Regard query as complete if input is valid and not already in multiline
    hsql::SQLParserResult parse_result;
    hsql::SQLParser::parse(input, &parse_result);
    if (parse_result.isValid()) {
      return _eval_sql(input);
    }
  }

  // Regard query as complete if last character is semicolon, regardless of multiline or not
  if (input.back() == ';') {
    int return_code = _eval_sql(_multiline_input + input);
    _multiline_input = "";
    return return_code;
  }

  // If query is not complete(/valid), and the last character is not a semicolon, enter/continue multiline
  _multiline_input += input;
  _multiline_input += '\n';
  return ReturnCode::Multiline;
}

int Console::_eval_command(const CommandFunction& func, const std::string& command) {
  std::string cmd = command;
  if (command.back() == ';') {
    cmd = command.substr(0, command.size() - 1);
  }
  boost::algorithm::trim<std::string>(cmd);

  size_t first = cmd.find(' ');
  size_t last = cmd.find('\n');

  // If no whitespace is found, zero arguments are provided
  if (std::string::npos == first) {
    return static_cast<int>(func(""));
  }

  std::string args = cmd.substr(first + 1, last - (first + 1));

  // Remove whitespace duplicates in args
  auto both_are_spaces = [](char left, char right) { return (left == right) && (left == ' '); };
  args.erase(std::unique(args.begin(), args.end(), both_are_spaces), args.end());

  return static_cast<int>(func(args));
}

bool Console::_initialize_pipeline(const std::string& sql) {
  try {
    auto builder = SQLPipelineBuilder{sql}
                       .dont_cleanup_temporaries()  // keep tables for debugging and visualization
                       .with_prepared_statement_cache(_prepared_statements);
    if (_explicitly_created_transaction_context != nullptr) {
      builder.with_transaction_context(_explicitly_created_transaction_context);
    }
    _sql_pipeline = std::make_unique<SQLPipeline>(builder.create_pipeline());
  } catch (const InvalidInputException& exception) {
    out(std::string(exception.what()) + '\n');
    return false;
  }

  return true;
}

int Console::_eval_sql(const std::string& sql) {
  if (!_initialize_pipeline(sql)) return ReturnCode::Error;

  try {
    _sql_pipeline->get_result_table();
  } catch (const InvalidInputException& exception) {
    out(std::string(exception.what()) + "\n");
    if (_handle_rollback() && _explicitly_created_transaction_context == nullptr &&
        _sql_pipeline->statement_count() > 1) {
      out("All previous statements have been committed.\n");
    }
    return ReturnCode::Error;
  }

  const auto& table = _sql_pipeline->get_result_table();
  auto row_count = table ? table->row_count() : 0;

  // Print result (to Console and logfile)
  if (table) {
    out(table);
  }

  out("===\n");
  out(std::to_string(row_count) + " rows total\n");
  out(_sql_pipeline->metrics().to_string());

  return ReturnCode::Ok;
}

void Console::register_command(const std::string& name, const CommandFunction& func) { _commands[name] = func; }

Console::RegisteredCommands Console::commands() { return _commands; }

void Console::set_prompt(const std::string& prompt) {
  if (IS_DEBUG) {
    _prompt = ANSI_COLOR_RED_RL "(debug)" ANSI_COLOR_RESET_RL + prompt;
  } else {
    _prompt = ANSI_COLOR_GREEN_RL "(release)" ANSI_COLOR_RESET_RL + prompt;
  }
}

void Console::set_logfile(const std::string& logfile) {
  _log = std::ofstream(logfile, std::ios_base::app | std::ios_base::out);
}

void Console::load_history(const std::string& history_file) {
  _history_file = history_file;

  // Check if history file exist, create empty history file if not
  std::ifstream file(_history_file);
  if (!file.good()) {
    out("Creating history file: " + _history_file + "\n");
    if (write_history(_history_file.c_str()) != 0) {
      out("Error creating history file: " + _history_file + "\n");
      return;
    }
  }

  if (read_history(_history_file.c_str()) != 0) {
    out("Error reading history file: " + _history_file + "\n");
  }
}

void Console::out(const std::string& output, bool console_print) {
  if (console_print) {
    _out << output;
  }
  // Remove coloring commands like '\x1B[32m' when writing to logfile
  _log << remove_coloring(output);
  _log.flush();
}

void Console::out(const std::shared_ptr<const Table>& table, uint32_t flags) {
  int size_y, size_x;
  rl_get_screen_size(&size_y, &size_x);

  const bool fits_on_one_page = table->row_count() < static_cast<uint64_t>(size_y) - 1;

  static bool pagination_disabled = false;
  if (!fits_on_one_page && !std::getenv("TERM") && !pagination_disabled) {
    out("Your TERM environment variable is not set - most likely because you are running the console from an IDE. "
        "Pagination is disabled.\n\n");
    pagination_disabled = true;
  }

  // Paginate only if table has more rows that fit in the terminal
  if (fits_on_one_page || pagination_disabled) {
    Print::print(table, flags, _out);
  } else {
    std::stringstream stream;
    Print::print(table, flags, stream);
    Pagination(stream).display();
  }
}

// Command functions

int Console::_exit(const std::string&) { return Console::ReturnCode::Quit; }

int Console::_help(const std::string&) {
  out("HYRISE SQL Interface\n\n");
  out("Available commands:\n");
  out("  generate [TABLENAME]             - Generate available TPC-C tables, or a specific table if TABLENAME is "
      "specified\n");
  out("  load FILE TABLENAME              - Load table from disc specified by filepath FILE, store it with name "
      "TABLENAME\n");
  out("  script SCRIPTFILE                - Execute script specified by SCRIPTFILE\n");
  out("  print TABLENAME                  - Fully print the given table (including MVCC columns)\n");
  out("  visualize [options] (noexec) SQL - Visualize a SQL query\n");
  out("                      <if set>        - does not execute the query (only supported with single statements)\n");
  out("             lqp                      - print the raw logical query plans\n");
  out("             lqpopt                   - print the optimized logical query plans\n");
  out("            <not set>                 - print the physical query plan\n");
  out("  begin                            - Manually create a new transaction (Auto-commit is active unless begin is "
      "called)\n");
  out("  rollback                         - Roll back a manually created transaction\n");
  out("  commit                           - Commit a manually created transaction\n");
  out("  txinfo                           - Print information on the current transaction\n");
  out("  pwd                              - Print current working directory\n");
  out("  load_plugin FILE NAME            - Load plugin from disc specified by filepath FILE, sotre it with NAME\n");
  out("  stop_plugin NAME                 - Stop and unload plugin NAME (also clears the query cache)\n");
  out("  quit                             - Exit the HYRISE Console\n");
  out("  help                             - Show this message\n\n");
  out("  setting [property] [value]       - Change a runtime setting\n\n");
  out("           scheduler (on|off)      - Turn the scheduler on (default) or off\n\n");
  out("After TPC-C tables are generated, SQL queries can be executed.\n");
  out("Example:\n");
  out("SELECT * FROM DISTRICT\n");
  return Console::ReturnCode::Ok;
}

int Console::_generate_tpcc(const std::string& tablename) {
  if (tablename.empty() || "ALL" == tablename) {
    out("Generating TPCC tables (this might take a while) ...\n");
    auto tables = opossum::TpccTableGenerator().generate_all_tables();
    for (auto& [table_name, table] : tables) {
      StorageManager::get().add_table(table_name, table);
    }
    return ReturnCode::Ok;
  }

  out("Generating TPCC table: \"" + tablename + "\" ...\n");
  auto table = opossum::TpccTableGenerator().generate_table(tablename);
  if (table == nullptr) {
    out("Error: No TPCC table named \"" + tablename + "\" available.\n");
    return ReturnCode::Error;
  }

  opossum::StorageManager::get().add_table(tablename, table);
  return ReturnCode::Ok;
}

int Console::_load_table(const std::string& args) {
  std::vector<std::string> arguments = trim_and_split(args);

  if (arguments.size() != 2) {
    out("Usage:\n");
    out("  load FILEPATH TABLENAME\n");
    return ReturnCode::Error;
  }

  const std::string& filepath = arguments[0];
  const std::string& tablename = arguments[1];

  std::vector<std::string> file_parts;
  boost::algorithm::split(file_parts, filepath, boost::is_any_of("."));
  const std::string& extension = file_parts.back();

  out("Loading " + filepath + " into table \"" + tablename + "\" ...\n");
  if (extension == "csv") {
    auto importer = std::make_shared<ImportCsv>(filepath, tablename);
    try {
      importer->execute();
    } catch (const std::exception& exception) {
      out("Exception thrown while importing CSV:\n  " + std::string(exception.what()) + "\n");
      return ReturnCode::Error;
    }
  } else if (extension == "tbl") {
    try {
      // We used this chunk size in order to be able to test chunk pruning
      // on sizeable data sets. This should probably be made configurable
      // at some point.
      static constexpr auto DEFAULT_CHUNK_SIZE = 500'000u;
      auto table = opossum::load_table(filepath, DEFAULT_CHUNK_SIZE);
      auto& storage_manager = StorageManager::get();
      if (storage_manager.has_table(tablename)) {
        storage_manager.drop_table(tablename);
        out("Table " + tablename + " already existed. Replaced it.\n");
      }
      StorageManager::get().add_table(tablename, table);
    } catch (const std::exception& exception) {
      out("Exception thrown while importing TBL:\n  " + std::string(exception.what()) + "\n");
      return ReturnCode::Error;
    }
  } else {
    out("Error: Unsupported file extension '" + extension + "'\n");
    return ReturnCode::Error;
  }

  return ReturnCode::Ok;
}

int Console::_print_table(const std::string& args) {
  std::vector<std::string> arguments = trim_and_split(args);

  if (arguments.size() != 1) {
    out("Usage:\n");
    out("  print TABLENAME\n");
    return ReturnCode::Error;
  }

  const std::string& tablename = arguments.at(0);

  auto gt = std::make_shared<GetTable>(tablename);
  try {
    gt->execute();
  } catch (const std::exception& exception) {
    out("Exception thrown while loading table:\n  " + std::string(exception.what()) + "\n");
    return ReturnCode::Error;
  }

  out(gt->get_output(), PrintMvcc);

  return ReturnCode::Ok;
}

int Console::_visualize(const std::string& input) {
  std::vector<std::string> input_words;
  boost::algorithm::split(input_words, input, boost::is_any_of(" \n"));

  const std::string noexec_string = "noexec";
  const std::string lqp_string = "lqp";
  const std::string lqpopt_string = "lqpopt";

  std::string first_word, second_word;
  if (!input_words.empty()) {
    first_word = input_words[0];
  }

  if (input_words.size() > 1) {
    second_word = input_words[1];
  }

  const bool no_execute = (first_word == noexec_string || second_word == noexec_string);

  std::string mode;
  if (first_word == lqp_string || first_word == lqpopt_string)
    mode = first_word;
  else if (second_word == lqp_string || second_word == lqpopt_string)
    mode = second_word;

  // Removes mode and noexec (+ leading whitespace) from sql string. If no mode or noexec is set, does nothing.
  const auto noexec_size = no_execute ? noexec_string.length() : 0u;
  auto sql_begin_pos = mode.size() + noexec_size;

  // If we have both words present, we need to remove additional whitespace
  if (no_execute && !mode.empty()) sql_begin_pos++;

  const auto sql = input.substr(sql_begin_pos, input.size());

  // If no SQL is provided, use the last execution. Else, create a new pipeline.
  if (!sql.empty()) {
    if (!_initialize_pipeline(sql)) return ReturnCode::Error;
  }

  if (no_execute && !sql.empty() && _sql_pipeline->requires_execution()) {
    out("We do not support the visualization of multiple dependant statements in 'noexec' mode.\n");
    return ReturnCode::Error;
  }

  std::string graph_filename, img_filename;

  // Visualize the Logical Query Plan
  if (mode == lqp_string || mode == lqpopt_string) {
    std::vector<std::shared_ptr<AbstractLQPNode>> lqp_roots;

    try {
      if (!no_execute) {
        // Run the query and then collect the LQPs
        _sql_pipeline->get_result_table();
      }

      const auto& lqps = (mode == lqp_string) ? _sql_pipeline->get_unoptimized_logical_plans()
                                              : _sql_pipeline->get_optimized_logical_plans();
      for (const auto& lqp : lqps) {
        lqp_roots.push_back(lqp);
      }
    } catch (const std::exception& exception) {
      out(std::string(exception.what()) + "\n");
      _handle_rollback();
      return ReturnCode::Error;
    }

    graph_filename = "." + mode + ".dot";
    img_filename = mode + ".png";
    LQPVisualizer visualizer;
    visualizer.visualize(lqp_roots, graph_filename, img_filename);

  } else {
    // Visualize the Physical Query Plan
    SQLQueryPlan query_plan{CleanupTemporaries::No};

    try {
      if (!no_execute) {
        _sql_pipeline->get_result_table();
      }

      // Create plan with all roots
      const auto& plans = _sql_pipeline->get_query_plans();
      for (const auto& plan : plans) {
        query_plan.append_plan(*plan);
      }
    } catch (const std::exception& exception) {
      out(std::string(exception.what()) + "\n");
      _handle_rollback();
      return ReturnCode::Error;
    }

    graph_filename = ".queryplan.dot";
    img_filename = "queryplan.png";
    SQLQueryPlanVisualizer visualizer;
    visualizer.visualize(query_plan, graph_filename, img_filename);
  }

  auto ret = system("./scripts/planviz/is_iterm2.sh");
  if (ret != 0) {
    std::string msg{"Currently, only iTerm2 can print the visualization inline. You can find the plan at "};  // NOLINT
    msg += img_filename + "\n";
    out(msg);

    return ReturnCode::Ok;
  }

  auto cmd = std::string("./scripts/planviz/imgcat.sh ") + img_filename;
  ret = system(cmd.c_str());
  Assert(ret == 0, "Printing the image using ./scripts/imgcat.sh failed.");

  return ReturnCode::Ok;
}

int Console::_change_runtime_setting(const std::string& input) {
  auto property = input.substr(0, input.find_first_of(" \n"));
  auto value = input.substr(input.find_first_of(" \n") + 1, input.size());

  if (property == "scheduler") {
    if (value == "on") {
      opossum::CurrentScheduler::set(std::make_shared<opossum::NodeQueueScheduler>());
      out("Scheduler turned on\n");
    } else if (value == "off") {
      opossum::CurrentScheduler::set(nullptr);
      out("Scheduler turned off\n");
    } else {
      out("Usage: scheduler (on|off)\n");
      return 1;
    }
    return 0;
  }

  out("Unknown property\n");
  return 1;
}

int Console::_exec_script(const std::string& script_file) {
  auto filepath = script_file;
  boost::algorithm::trim(filepath);
  std::ifstream script(filepath);

  const auto is_regular_file = [](const std::string& path) {
    struct stat path_stat {};
    stat(path.c_str(), &path_stat);
    return S_ISREG(path_stat.st_mode);  // NOLINT
  };

  if (!script.good()) {
    out("Error: Script file '" + filepath + "' does not exist.\n");
    return ReturnCode::Error;
  } else if (!is_regular_file(filepath)) {
    out("Error: '" + filepath + "' is not a regular file.\n");
    return ReturnCode::Error;
  }

  out("Executing script file: " + filepath + "\n");
  _verbose = true;
  std::string command;
  int return_code = ReturnCode::Ok;
  while (std::getline(script, command)) {
    return_code = _eval(command);
    if (return_code == ReturnCode::Error || return_code == ReturnCode::Quit) {
      break;
    }
  }
  out("Executing script file done\n");
  _verbose = false;
  return return_code;
}

void Console::handle_signal(int sig) {
  if (sig == SIGINT) {
    // Reset console state
    auto& console = Console::get();
    console._out << "\n";
    console._multiline_input = "";
    console.set_prompt("!> ");
    console._verbose = false;
    // Restore program state stored in jmp_env set with sigsetjmp(2).
    // See comment on jmp_env for details
    siglongjmp(jmp_env, 1);
  }
}

int Console::_begin_transaction(const std::string& input) {
  if (_explicitly_created_transaction_context != nullptr) {
    const auto transaction_id = std::to_string(_explicitly_created_transaction_context->transaction_id());
    out("There is already an active transaction (" + transaction_id + "). ");
    out("Type `rollback` or `commit` before beginning a new transaction.\n");
    return ReturnCode::Error;
  }

  _explicitly_created_transaction_context = TransactionManager::get().new_transaction_context();

  const auto transaction_id = std::to_string(_explicitly_created_transaction_context->transaction_id());
  out("New transaction (" + transaction_id + ") started.\n");
  return ReturnCode::Ok;
}

int Console::_rollback_transaction(const std::string& input) {
  if (_explicitly_created_transaction_context == nullptr) {
    out("Console is in auto-commit mode. Type `begin` to start a manual transaction.\n");
    return ReturnCode::Error;
  }

  _explicitly_created_transaction_context->rollback();

  const auto transaction_id = std::to_string(_explicitly_created_transaction_context->transaction_id());
  out("Transaction (" + transaction_id + ") has been rolled back.\n");

  _explicitly_created_transaction_context = nullptr;
  return ReturnCode::Ok;
}

int Console::_commit_transaction(const std::string& input) {
  if (_explicitly_created_transaction_context == nullptr) {
    out("Console is in auto-commit mode. Type `begin` to start a manual transaction.\n");
    return ReturnCode::Error;
  }

  _explicitly_created_transaction_context->commit();

  const auto transaction_id = std::to_string(_explicitly_created_transaction_context->transaction_id());
  out("Transaction (" + transaction_id + ") has been committed.\n");

  _explicitly_created_transaction_context = nullptr;
  return ReturnCode::Ok;
}

int Console::_print_transaction_info(const std::string& input) {
  if (_explicitly_created_transaction_context == nullptr) {
    out("Console is in auto-commit mode. Type `begin` to start a manual transaction.\n");
    return ReturnCode::Error;
  }

  const auto transaction_id = std::to_string(_explicitly_created_transaction_context->transaction_id());
  const auto snapshot_commit_id = std::to_string(_explicitly_created_transaction_context->snapshot_commit_id());
  out("Active transaction: { transaction id = " + transaction_id + ", snapshot commit id = " + snapshot_commit_id +
      " }\n");
  return ReturnCode::Ok;
}

int Console::_print_current_working_directory(const std::string&) {
  out(filesystem::current_path().string() + "\n");
  return ReturnCode::Ok;
}

int Console::_load_plugin(const std::string& args) {
  auto arguments = trim_and_split(args);

  if (arguments.size() != 2) {
    out("Usage:\n");
    out("  load_plugin PLUGINPATH PLUGINNAME\n");
    return ReturnCode::Error;
  }

  const std::string& plugin_path = arguments[0];
  const std::string& plugin_name = arguments[1];

  PluginManager::get().load_plugin(plugin_path, plugin_name);

  std::cout << "Plugin (" << plugin_name << ") successfully loaded." << std::endl;

  return ReturnCode::Ok;
}

int Console::_stop_plugin(const std::string& input) {
  auto arguments = trim_and_split(input);

  if (arguments.size() != 1) {
    out("Usage:\n");
    out("  stop_plugin PLUGINNAME\n");
    return ReturnCode::Error;
  }

  const std::string& plugin_name = arguments[0];

  PluginManager::get().stop_plugin(plugin_name);

  // The presence of some plugins might cause certain query plans to be generated which will not work if the plugin
  // is stopped. Therefore, we clear the cache.
  SQLQueryCache<SQLQueryPlan>::get().clear();

  std::cout << "Plugin (" << plugin_name << ") stopped." << std::endl;

  return ReturnCode::Ok;
}

// GNU readline interface to our commands

char** Console::_command_completion(const char* text, int start, int end) {
  char** completion_matches = nullptr;

  std::string input(rl_line_buffer);

  // Remove whitespace duplicates to not get empty tokens after boost::algorithm::split
  auto both_are_spaces = [](char left, char right) { return (left == right) && (left == ' '); };
  input.erase(std::unique(input.begin(), input.end(), both_are_spaces), input.end());

  std::vector<std::string> tokens;
  boost::algorithm::split(tokens, input, boost::is_space());

  // Choose completion function depending on the input. If it starts with "generate",
  // suggest TPC-C tablenames for completion.
  const std::string& first_word = tokens.at(0);
  if (first_word == "generate") {
    // Completion only for two words, "generate", and the TABLENAME
    if (tokens.size() <= 2) {
      completion_matches = rl_completion_matches(text, &Console::_command_generator_tpcc);
    }
    // Turn off filepath completion for TPC-C table generation
    rl_attempted_completion_over = 1;
  } else if (first_word == "quit" || first_word == "exit" || first_word == "help") {
    // Turn off filepath completion
    rl_attempted_completion_over = 1;
  } else if ((first_word == "load" || first_word == "script") && tokens.size() > 2) {
    // Turn off filepath completion after first argument for "load" and "script"
    rl_attempted_completion_over = 1;
  } else if (start == 0) {
    completion_matches = rl_completion_matches(text, &Console::_command_generator);
  }

  return completion_matches;
}

char* Console::_command_generator(const char* text, int state) {
  static RegisteredCommands::iterator it;
  auto& commands = Console::get()._commands;

  if (state == 0) {
    it = commands.begin();
  }

  while (it != commands.end()) {
    auto& command = it->first;
    ++it;
    if (command.find(text) != std::string::npos) {
      auto completion = new char[command.size()];  // NOLINT (legacy API)
      snprintf(completion, command.size() + 1, "%s", command.c_str());
      return completion;
    }
  }
  return nullptr;
}

char* Console::_command_generator_tpcc(const char* text, int state) {
  static std::vector<std::string>::iterator it;
  auto& commands = Console::get()._tpcc_commands;
  if (state == 0) {
    it = commands.begin();
  }

  while (it != commands.end()) {
    auto& command = *it;
    ++it;
    if (command.find(text) != std::string::npos) {
      auto completion = new char[command.size()];  // NOLINT (legacy API)
      snprintf(completion, command.size() + 1, "%s", command.c_str());
      return completion;
    }
  }
  return nullptr;
}

bool Console::_handle_rollback() {
  auto failed_pipeline = _sql_pipeline->failed_pipeline_statement();
  if (failed_pipeline && failed_pipeline->transaction_context() && failed_pipeline->transaction_context()->aborted()) {
    out("The transaction has been rolled back.\n");
    _explicitly_created_transaction_context = nullptr;
    return true;
  }

  return false;
}

}  // namespace opossum

int main(int argc, char** argv) {
  using Return = opossum::Console::ReturnCode;
  auto& console = opossum::Console::get();

  // Bind CTRL-C to behaviour specified in opossum::Console::_handle_signal
  std::signal(SIGINT, &opossum::Console::handle_signal);

  console.set_prompt("> ");
  console.set_logfile("console.log");

  // Load command history
  console.load_history(".repl_history");

  // Timestamp dump only to logfile
  console.out("--- Session start --- " + current_timestamp() + "\n", false);

  int return_code = Return::Ok;

  // Display Usage if too many arguments are provided
  if (argc > 2) {
    return_code = Return::Quit;
    console.out("Usage:\n");
    console.out("  ./hyriseConsole [SCRIPTFILE] - Start the interactive SQL interface.\n");
    console.out("                                 Execute script if specified by SCRIPTFILE.\n");
  }

  // Execute .sql script if specified
  if (argc == 2) {
    return_code = console.execute_script(std::string(argv[1]));
    // Terminate Console if an error occured during script execution
    if (return_code == Return::Error) {
      return_code = Return::Quit;
    }
  }

  // Display welcome message if Console started normally
  if (argc == 1) {
    console.out("HYRISE SQL Interface\n");
    console.out("Enter 'generate' to generate the TPC-C tables. Then, you can enter SQL queries.\n");
    console.out("Type 'help' for more information.\n\n");

    console.out("Hyrise is running a ");
    if (IS_DEBUG) {
      console.out(ANSI_COLOR_RED "(debug)" ANSI_COLOR_RESET);
    } else {
      console.out(ANSI_COLOR_GREEN "(release)" ANSI_COLOR_RESET);
    }
    console.out(" build.\n\n");
  }

  // Set jmp_env to current program state in preparation for siglongjmp(2)
  // See comment on jmp_env for details
  while (sigsetjmp(jmp_env, 1) != 0) {
  }

  // Main REPL loop
  while (return_code != Return::Quit) {
    return_code = console.read();
    if (return_code == Return::Ok) {
      console.set_prompt("> ");
    } else if (return_code == Return::Multiline) {
      console.set_prompt("... ");
    } else {
      console.set_prompt("!> ");
    }
  }

  console.out("Bye.\n");

  // Timestamp dump only to logfile
  console.out("--- Session end --- " + current_timestamp() + "\n", false);
}
