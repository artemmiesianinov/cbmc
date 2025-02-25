/*******************************************************************\

Module: Compile and link source and object files.

Author: CM Wintersteiger

Date: June 2006

\*******************************************************************/

/// \file
/// Compile and link source and object files.

#include "compile.h"

#include <util/cmdline.h>
#include <util/config.h>
#include <util/get_base_name.h>
#include <util/prefix.h>
#include <util/run.h>
#include <util/symbol_table_builder.h>
#include <util/tempdir.h>
#include <util/tempfile.h>
#include <util/unicode.h>
#include <util/version.h>

#include <goto-programs/goto_convert_functions.h>
#include <goto-programs/name_mangler.h>
#include <goto-programs/read_goto_binary.h>
#include <goto-programs/write_goto_binary.h>

#include <ansi-c/ansi_c_entry_point.h>
#include <ansi-c/c_object_factory_parameters.h>
#include <langapi/language.h>
#include <langapi/language_file.h>
#include <langapi/mode.h>
#include <linking/linking.h>
#include <linking/static_lifetime_init.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#define DOTGRAPHSETTINGS  "color=black;" \
                          "orientation=portrait;" \
                          "fontsize=20;"\
                          "compound=true;"\
                          "size=\"30,40\";"\
                          "ratio=compress;"

/// reads and source and object files, compiles and links them into goto program
/// objects.
/// \return true on error, false otherwise
bool compilet::doit()
{
  add_compiler_specific_defines();

  // Parse command line for source and object file names
  for(const auto &arg : cmdline.args)
    if(add_input_file(arg))
      return true;

  for(const auto &library : libraries)
  {
    if(!find_library(library))
      // GCC is going to complain if this doesn't exist
      log.debug() << "Library not found: " << library << " (ignoring)"
                  << messaget::eom;
  }

  log.statistics() << "No. of source files: " << source_files.size()
                   << messaget::eom;
  log.statistics() << "No. of object files: " << object_files.size()
                   << messaget::eom;

  // Work through the given source files

  if(source_files.empty() && object_files.empty())
  {
    log.error() << "no input files" << messaget::eom;
    return true;
  }

  if(mode==LINK_LIBRARY && !source_files.empty())
  {
    log.error() << "cannot link source files" << messaget::eom;
    return true;
  }

  if(mode==PREPROCESS_ONLY && !object_files.empty())
  {
    log.error() << "cannot preprocess object files" << messaget::eom;
    return true;
  }

  const unsigned warnings_before =
    log.get_message_handler().get_message_count(messaget::M_WARNING);

  auto symbol_table_opt = compile();
  if(!symbol_table_opt.has_value())
    return true;

  if(mode==LINK_LIBRARY ||
     mode==COMPILE_LINK ||
     mode==COMPILE_LINK_EXECUTABLE)
  {
    if(link(*symbol_table_opt))
      return true;
  }

  return warning_is_fatal && log.get_message_handler().get_message_count(
                               messaget::M_WARNING) != warnings_before;
}

enum class file_typet
{
  FAILED_TO_OPEN_FILE,
  UNKNOWN,
  SOURCE_FILE,
  NORMAL_ARCHIVE,
  THIN_ARCHIVE,
  GOTO_BINARY,
  ELF_OBJECT
};

static file_typet detect_file_type(
  const std::string &file_name,
  message_handlert &message_handler)
{
  // first of all, try to open the file
  std::ifstream in(file_name);
  if(!in)
    return file_typet::FAILED_TO_OPEN_FILE;

  const std::string::size_type r = file_name.rfind('.');

  const std::string ext =
    r == std::string::npos ? "" : file_name.substr(r + 1, file_name.length());

  if(
    ext == "c" || ext == "cc" || ext == "cp" || ext == "cpp" || ext == "CPP" ||
    ext == "c++" || ext == "C" || ext == "i" || ext == "ii" || ext == "class" ||
    ext == "jar")
  {
    return file_typet::SOURCE_FILE;
  }

  char hdr[8];
  in.get(hdr, 8);
  if((ext == "a" || ext == "o") && strncmp(hdr, "!<thin>", 8) == 0)
    return file_typet::THIN_ARCHIVE;

  if(ext == "a")
    return file_typet::NORMAL_ARCHIVE;

  if(is_goto_binary(file_name, message_handler))
    return file_typet::GOTO_BINARY;

  if(hdr[0] == 0x7f && memcmp(hdr + 1, "ELF", 3) == 0)
    return file_typet::ELF_OBJECT;

  return file_typet::UNKNOWN;
}

/// puts input file names into a list and does preprocessing for libraries.
/// \return false on success, true on error.
bool compilet::add_input_file(const std::string &file_name)
{
  switch(detect_file_type(file_name, log.get_message_handler()))
  {
  case file_typet::FAILED_TO_OPEN_FILE:
    log.warning() << "failed to open file '" << file_name
                  << "': " << std::strerror(errno) << messaget::eom;
    return warning_is_fatal; // generously ignore unless -Werror

  case file_typet::UNKNOWN:
    // unknown extension, not a goto binary, will silently ignore
    log.debug() << "unknown file type in '" << file_name << "'"
                << messaget::eom;
    return false;

  case file_typet::ELF_OBJECT:
    // ELF file without goto-cc section, silently ignore
    log.debug() << "ELF object without goto-cc section: '" << file_name << "'"
                << messaget::eom;
    return false;

  case file_typet::SOURCE_FILE:
    source_files.push_back(file_name);
    return false;

  case file_typet::NORMAL_ARCHIVE:
    return add_files_from_archive(file_name, false);

  case file_typet::THIN_ARCHIVE:
    return add_files_from_archive(file_name, true);

  case file_typet::GOTO_BINARY:
    object_files.push_back(file_name);
    return false;
  }

  UNREACHABLE;
}

/// extracts goto binaries from AR archive and add them as input files.
/// \return false on success, true on error.
bool compilet::add_files_from_archive(
  const std::string &file_name,
  bool thin_archive)
{
  std::string tstr = working_directory;

  if(!thin_archive)
  {
    tstr = get_temporary_directory("goto-cc.XXXXXX");

    tmp_dirs.push_back(tstr);
    std::filesystem::current_path(tmp_dirs.back());

    // unpack now
    int ret = run(
      "ar",
      {"ar",
       "x",
       std::filesystem::path(working_directory).append(file_name).string()});
    if(ret != 0)
    {
      log.error() << "Failed to extract archive " << file_name << messaget::eom;
      return true;
    }
  }

  // add the files from "ar t"
  temporary_filet tmp_file_out("", "");
  int ret = run(
    "ar",
    {"ar",
     "t",
     std::filesystem::path(working_directory).append(file_name).string()},
    "",
    tmp_file_out(),
    "");
  if(ret != 0)
  {
    log.error() << "Failed to list archive " << file_name << messaget::eom;
    return true;
  }

  std::ifstream in(tmp_file_out());
  std::string line;

  while(!in.fail() && std::getline(in, line))
  {
    std::string t = std::filesystem::path(tstr).append(line).string();

    if(is_goto_binary(t, log.get_message_handler()))
      object_files.push_back(t);
    else
      log.debug() << "Object file is not a goto binary: " << line
                  << messaget::eom;
  }

  if(!thin_archive)
    std::filesystem::current_path(working_directory);

  return false;
}

/// tries to find a library object file that matches the given library name.
/// \par parameters: library name
/// \return true if found, false otherwise
bool compilet::find_library(const std::string &name)
{
  std::string library_file_name;

  for(const auto &library_path : library_paths)
  {
    library_file_name =
      std::filesystem::path(library_path).append("lib" + name + ".a").string();

    std::ifstream in(library_file_name);

    if(in.is_open())
      return !add_input_file(library_file_name);
    else
    {
      library_file_name = std::filesystem::path(library_path)
                            .append("lib" + name + ".so")
                            .string();

      switch(detect_file_type(library_file_name, log.get_message_handler()))
      {
      case file_typet::GOTO_BINARY:
        return !add_input_file(library_file_name);

      case file_typet::ELF_OBJECT:
        log.warning() << "Warning: Cannot read ELF library "
                      << library_file_name << messaget::eom;
        return warning_is_fatal;

      case file_typet::THIN_ARCHIVE:
      case file_typet::NORMAL_ARCHIVE:
      case file_typet::SOURCE_FILE:
      case file_typet::FAILED_TO_OPEN_FILE:
      case file_typet::UNKNOWN:
        break;
      }
    }
  }

  return false;
}

/// parses object files and links them
/// \return true on error, false otherwise
bool compilet::link(std::optional<symbol_tablet> &&symbol_table)
{
  // "compile" hitherto uncompiled functions
  log.statistics() << "Compiling functions" << messaget::eom;
  goto_modelt goto_model;
  if(symbol_table.has_value())
    goto_model.symbol_table = std::move(*symbol_table);
  convert_symbols(goto_model);

  // parse object files
  if(read_objects_and_link(object_files, goto_model, log.get_message_handler()))
    return true;

  // produce entry point?

  if(mode==COMPILE_LINK_EXECUTABLE)
  {
    // new symbols may have been added to a previously linked file
    // make sure a new entry point is created that contains all
    // static initializers
    goto_model.goto_functions.function_map.erase(INITIALIZE_FUNCTION);

    goto_model.symbol_table.remove(goto_functionst::entry_point());
    goto_model.goto_functions.function_map.erase(
      goto_functionst::entry_point());

    const bool error = ansi_c_entry_point(
      goto_model.symbol_table,
      log.get_message_handler(),
      c_object_factory_parameterst());

    if(error)
      return true;

    // entry_point may (should) add some more functions.
    convert_symbols(goto_model);
  }

  if(keep_file_local)
  {
    function_name_manglert<file_name_manglert> mangler(
      log.get_message_handler(), goto_model, file_local_mangle_suffix);
    mangler.mangle();
  }

  if(write_bin_object_file(output_file_executable, goto_model))
    return true;

  return add_written_cprover_symbols(goto_model.symbol_table);
}

/// Parses source files and writes object files, or keeps the symbols in the
/// symbol_table if not compiling/assembling only.
/// \return Symbol table, if parsing and type checking succeeded, else empty
std::optional<symbol_tablet> compilet::compile()
{
  symbol_tablet symbol_table;

  while(!source_files.empty())
  {
    std::string file_name=source_files.front();
    source_files.pop_front();

    // Visual Studio always prints the name of the file it's doing
    // onto stdout. The name of the directory is stripped.
    if(echo_file_name)
      std::cout << get_base_name(file_name, false) << '\n' << std::flush;

    auto file_symbol_table = parse_source(file_name);

    if(!file_symbol_table.has_value())
    {
      const std::string &debug_outfile=
        cmdline.get_value("print-rejected-preprocessed-source");
      if(!debug_outfile.empty())
      {
        std::ifstream in(file_name, std::ios::binary);
        std::ofstream out(debug_outfile, std::ios::binary);
        out << in.rdbuf();
        log.warning() << "Failed sources in " << debug_outfile << messaget::eom;
      }

      return {}; // parser/typecheck error
    }

    if(mode==COMPILE_ONLY || mode==ASSEMBLE_ONLY)
    {
      // output an object file for every source file

      // "compile" functions
      goto_modelt file_goto_model;
      file_goto_model.symbol_table = std::move(*file_symbol_table);
      convert_symbols(file_goto_model);

      std::string cfn;

      if(output_file_object.empty())
      {
        const std::string file_name_with_obj_ext =
          get_base_name(file_name, true) + "." + object_file_extension;

        if(!output_directory_object.empty())
        {
          cfn = std::filesystem::path(output_directory_object)
                  .append(file_name_with_obj_ext)
                  .string();
        }
        else
          cfn = file_name_with_obj_ext;
      }
      else
        cfn = output_file_object;

      if(keep_file_local)
      {
        function_name_manglert<file_name_manglert> mangler(
          log.get_message_handler(), file_goto_model, file_local_mangle_suffix);
        mangler.mangle();
      }

      if(write_bin_object_file(cfn, file_goto_model))
        return {};

      if(add_written_cprover_symbols(file_goto_model.symbol_table))
        return {};
    }
    else
    {
      if(linking(symbol_table, *file_symbol_table, log.get_message_handler()))
      {
        return {};
      }
    }
  }

  return std::move(symbol_table);
}

/// parses a source file (low-level parsing)
/// \return true on error, false otherwise
bool compilet::parse(
  const std::string &file_name,
  language_filest &language_files)
{
  std::unique_ptr<languaget> languagep;

  // Using '-x', the type of a file can be overridden;
  // otherwise, it's guessed from the extension.

  if(!override_language.empty())
  {
    if(override_language=="c++" || override_language=="c++-header")
      languagep = get_language_from_mode(ID_cpp);
    else
      languagep = get_language_from_mode(ID_C);
  }
  else if(file_name != "-")
    languagep=get_language_from_filename(file_name);

  if(languagep==nullptr)
  {
    log.error() << "failed to figure out type of file '" << file_name << "'"
                << messaget::eom;
    return true;
  }

  if(file_name == "-")
    return parse_stdin(*languagep);

  std::ifstream infile(widen_if_needed(file_name));

  if(!infile)
  {
    log.error() << "failed to open input file '" << file_name << "'"
                << messaget::eom;
    return true;
  }

  language_filet &lf=language_files.add_file(file_name);
  lf.language=std::move(languagep);

  if(mode==PREPROCESS_ONLY)
  {
    log.statistics() << "Preprocessing: " << file_name << messaget::eom;

    std::ostream *os = &std::cout;
    std::ofstream ofs;

    if(cmdline.isset('o'))
    {
      ofs.open(cmdline.get_value('o'));
      os = &ofs;

      if(!ofs.is_open())
      {
        log.error() << "failed to open output file '" << cmdline.get_value('o')
                    << "'" << messaget::eom;
        return true;
      }
    }

    lf.language->preprocess(infile, file_name, *os, log.get_message_handler());
  }
  else
  {
    log.statistics() << "Parsing: " << file_name << messaget::eom;

    if(lf.language->parse(infile, file_name, log.get_message_handler()))
    {
      log.error() << "PARSING ERROR" << messaget::eom;
      return true;
    }
  }

  lf.get_modules();
  return false;
}

/// parses a source file (low-level parsing)
/// \param language: source language processor
/// \return true on error, false otherwise
bool compilet::parse_stdin(languaget &language)
{
  log.statistics() << "Parsing: (stdin)" << messaget::eom;

  if(mode==PREPROCESS_ONLY)
  {
    std::ostream *os = &std::cout;
    std::ofstream ofs;

    if(cmdline.isset('o'))
    {
      ofs.open(cmdline.get_value('o'));
      os = &ofs;

      if(!ofs.is_open())
      {
        log.error() << "failed to open output file '" << cmdline.get_value('o')
                    << "'" << messaget::eom;
        return true;
      }
    }

    language.preprocess(std::cin, "", *os, log.get_message_handler());
  }
  else
  {
    if(language.parse(std::cin, "", log.get_message_handler()))
    {
      log.error() << "PARSING ERROR" << messaget::eom;
      return true;
    }
  }

  return false;
}

bool compilet::write_bin_object_file(
  const std::string &file_name,
  const goto_modelt &src_goto_model,
  bool validate_goto_model,
  message_handlert &message_handler)
{
  messaget log(message_handler);

  if(validate_goto_model)
  {
    log.status() << "Validating goto model" << messaget::eom;
    src_goto_model.validate();
  }

  log.statistics() << "Writing binary format object '" << file_name << "'"
                   << messaget::eom;

  // symbols
  log.statistics() << "Symbols in table: "
                   << src_goto_model.symbol_table.symbols.size()
                   << messaget::eom;

  std::ofstream outfile(file_name, std::ios::binary);

  if(!outfile.is_open())
  {
    log.error() << "Error opening file '" << file_name << "'" << messaget::eom;
    return true;
  }

  if(write_goto_binary(outfile, src_goto_model))
    return true;

  const auto cnt = function_body_count(src_goto_model.goto_functions);

  log.statistics() << "Functions: "
                   << src_goto_model.goto_functions.function_map.size() << "; "
                   << cnt << " have a body." << messaget::eom;

  outfile.close();

  return false;
}

/// Parses and type checks a source file located at \p file_name.
/// \return A symbol table if, and only if, parsing and type checking succeeded.
std::optional<symbol_tablet>
compilet::parse_source(const std::string &file_name)
{
  language_filest language_files;

  if(parse(file_name, language_files))
    return {};

  // we just typecheck one file here
  symbol_tablet file_symbol_table;
  if(language_files.typecheck(
       file_symbol_table, keep_file_local, log.get_message_handler()))
  {
    log.error() << "CONVERSION ERROR" << messaget::eom;
    return {};
  }

  if(language_files.final(file_symbol_table))
  {
    log.error() << "CONVERSION ERROR" << messaget::eom;
    return {};
  }

  return std::move(file_symbol_table);
}

/// constructor
compilet::compilet(cmdlinet &_cmdline, message_handlert &mh, bool Werror)
  : log(mh),
    cmdline(_cmdline),
    warning_is_fatal(Werror),
    keep_file_local(
      // function-local is the old name and is still in use, but is misleading
      cmdline.isset("export-function-local-symbols") ||
      cmdline.isset("export-file-local-symbols")),
    file_local_mangle_suffix(
      cmdline.isset("mangle-suffix") ? cmdline.get_value("mangle-suffix") : "")
{
  mode=COMPILE_LINK_EXECUTABLE;
  echo_file_name=false;
  wrote_object=false;
  working_directory = std::filesystem::current_path().string();

  if(cmdline.isset("export-function-local-symbols"))
  {
    log.warning()
      << "The `--export-function-local-symbols` flag is deprecated. "
         "Please use `--export-file-local-symbols` instead."
      << messaget::eom;
  }
}

/// cleans up temporary files
compilet::~compilet()
{
  // clean up temp dirs

  for(const auto &dir : tmp_dirs)
    std::filesystem::remove_all(dir);
}

std::size_t compilet::function_body_count(const goto_functionst &functions)
{
  std::size_t count = 0;

  for(const auto &f : functions.function_map)
    if(f.second.body_available())
      count++;

  return count;
}

void compilet::add_compiler_specific_defines() const
{
  config.ansi_c.defines.push_back(
    std::string("__GOTO_CC_VERSION__=") + CBMC_VERSION);
}

void compilet::convert_symbols(goto_modelt &goto_model)
{
  symbol_table_buildert symbol_table_builder =
    symbol_table_buildert::wrap(goto_model.symbol_table);

  goto_convert_functionst converter(
    symbol_table_builder, log.get_message_handler());

  // the compilation may add symbols!

  symbol_tablet::symbolst::size_type before=0;

  while(before != symbol_table_builder.symbols.size())
  {
    before = symbol_table_builder.symbols.size();

    typedef std::set<irep_idt> symbols_sett;
    symbols_sett symbols;

    for(const auto &named_symbol : symbol_table_builder.symbols)
      symbols.insert(named_symbol.first);

    // the symbol table iterators aren't stable
    for(const auto &symbol : symbols)
    {
      symbol_tablet::symbolst::const_iterator s_it =
        symbol_table_builder.symbols.find(symbol);
      CHECK_RETURN(s_it != symbol_table_builder.symbols.end());

      if(
        s_it->second.is_function() && !s_it->second.is_compiled() &&
        s_it->second.value.is_not_nil())
      {
        log.debug() << "Compiling " << s_it->first << messaget::eom;
        converter.convert_function(
          s_it->first, goto_model.goto_functions.function_map[s_it->first]);
        symbol_table_builder.get_writeable_ref(symbol).set_compiled();
      }
    }
  }
}

bool compilet::add_written_cprover_symbols(const symbol_tablet &symbol_table)
{
  for(const auto &pair : symbol_table.symbols)
  {
    const irep_idt &name=pair.second.name;
    const typet &new_type=pair.second.type;
    if(!(has_prefix(id2string(name), CPROVER_PREFIX) && new_type.id()==ID_code))
      continue;

    if(has_prefix(id2string(name), FILE_LOCAL_PREFIX))
      continue;

    bool inserted;
    std::map<irep_idt, symbolt>::iterator old;
    std::tie(old, inserted)=written_macros.insert({name, pair.second});

    if(!inserted && old->second.type!=new_type)
    {
      log.error() << "Incompatible CPROVER macro symbol types:" << '\n'
                  << old->second.type.pretty() << "(at " << old->second.location
                  << ")\n"
                  << "and\n"
                  << new_type.pretty() << "(at " << pair.second.location << ")"
                  << messaget::eom;
      return true;
    }
  }
  return false;
}
