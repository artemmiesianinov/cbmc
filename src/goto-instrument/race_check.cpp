/*******************************************************************\

Module: Race Detection for Threaded Goto Programs

Author: Daniel Kroening

Date: February 2006

\*******************************************************************/

/// \file
/// Race Detection for Threaded Goto Programs

#include "race_check.h"

#include <util/prefix.h>

#include <goto-programs/remove_skip.h>

#include <linking/static_lifetime_init.h>
#include <util/pointer_predicates.h>

#include "rw_set.h"

#ifdef LOCAL_MAY
#include <analyses/local_may_alias.h>
#define L_M_ARG(x) x,
#define L_M_LAST_ARG(x) , x
#else
#define L_M_ARG(x)
#define L_M_LAST_ARG(x)
#endif

class w_guardst
{
public:
  explicit w_guardst(symbol_table_baset &_symbol_table)
    : symbol_table(_symbol_table)
  {
  }

  std::list<irep_idt> w_guards;

  const symbolt &get_guard_symbol(const irep_idt &object);

  const exprt get_guard_symbol_expr(const irep_idt &object)
  {
    return get_guard_symbol(object).symbol_expr();
  }

  const exprt get_w_guard_expr(const rw_set_baset::entryt &entry)
  {
    return get_guard_symbol_expr(entry.object);
  }

  const exprt get_assertion(const rw_set_baset::entryt &entry)
  {
    return not_exprt(get_guard_symbol_expr(entry.object));
  }

  void add_initialization(goto_programt &goto_program) const;

protected:
  symbol_table_baset &symbol_table;
};

const symbolt &w_guardst::get_guard_symbol(const irep_idt &object)
{
  const irep_idt identifier=id2string(object)+"$w_guard";

  const symbol_table_baset::symbolst::const_iterator it =
    symbol_table.symbols.find(identifier);

  if(it!=symbol_table.symbols.end())
    return it->second;

  w_guards.push_back(identifier);

  symbolt new_symbol{
    identifier, bool_typet(), symbol_table.lookup_ref(object).mode};
  new_symbol.base_name=identifier;
  new_symbol.is_static_lifetime=true;
  new_symbol.value=false_exprt();

  symbolt *symbol_ptr;
  symbol_table.move(new_symbol, symbol_ptr);
  return *symbol_ptr;
}

void w_guardst::add_initialization(goto_programt &goto_program) const
{
  goto_programt::targett t=goto_program.instructions.begin();
  const namespacet ns(symbol_table);

  for(std::list<irep_idt>::const_iterator
      it=w_guards.begin();
      it!=w_guards.end();
      it++)
  {
    exprt symbol=ns.lookup(*it).symbol_expr();

    t = goto_program.insert_before(
      t, goto_programt::make_assignment(symbol, false_exprt()));

    t++;
  }
}

static std::string comment(const rw_set_baset::entryt &entry, bool write)
{
  std::string result;
  if(write)
    result="W/W";
  else
    result="R/W";

  result+=" data race on ";
  result+=id2string(entry.object);
  return result;
}

static bool is_shared(const namespacet &ns, const symbol_exprt &symbol_expr)
{
  const irep_idt &identifier=symbol_expr.get_identifier();

  if(
    identifier == CPROVER_PREFIX "alloc" ||
    identifier == CPROVER_PREFIX "alloc_size" || identifier == "stdin" ||
    identifier == "stdout" || identifier == "stderr" ||
    identifier == "sys_nerr" ||
    has_prefix(id2string(identifier), "symex::invalid_object") ||
    has_prefix(id2string(identifier), SYMEX_DYNAMIC_PREFIX "::dynamic_object"))
    return false; // no race check

  const symbolt &symbol=ns.lookup(identifier);
  return symbol.is_shared();
}

static bool has_shared_entries(const namespacet &ns, const rw_set_baset &rw_set)
{
  for(rw_set_baset::entriest::const_iterator
      it=rw_set.r_entries.begin();
      it!=rw_set.r_entries.end();
      it++)
    if(is_shared(ns, it->second.symbol_expr))
      return true;

  for(rw_set_baset::entriest::const_iterator
      it=rw_set.w_entries.begin();
      it!=rw_set.w_entries.end();
      it++)
    if(is_shared(ns, it->second.symbol_expr))
      return true;

  return false;
}

// clang-format off
// clang-format is confused by the L_M_ARG macro and wants to indent the line
// after
static void race_check(
  value_setst &value_sets,
  symbol_table_baset &symbol_table,
  const irep_idt &function_id,
  L_M_ARG(const goto_functionst::goto_functiont &goto_function)
  goto_programt &goto_program,
  w_guardst &w_guards,
  message_handlert &message_handler)
// clang-format on
{
  namespacet ns(symbol_table);

#ifdef LOCAL_MAY
  local_may_aliast local_may(goto_function);
#endif

  Forall_goto_program_instructions(i_it, goto_program)
  {
    goto_programt::instructiont &instruction=*i_it;

    if(instruction.is_assign())
    {
      rw_set_loct rw_set(
        ns,
        value_sets,
        function_id,
        i_it L_M_LAST_ARG(local_may),
        message_handler);

      if(!has_shared_entries(ns, rw_set))
        continue;

      goto_programt::instructiont original_instruction;
      original_instruction.swap(instruction);

      instruction =
        goto_programt::make_skip(original_instruction.source_location());
      i_it++;

      // now add assignments for what is written -- set
      for(const auto &w_entry : rw_set.w_entries)
      {
        if(!is_shared(ns, w_entry.second.symbol_expr))
          continue;

        goto_program.insert_before(
          i_it,
          goto_programt::make_assignment(
            w_guards.get_w_guard_expr(w_entry.second),
            w_entry.second.guard,
            original_instruction.source_location()));
      }

      // insert original statement here
      {
        goto_programt::targett t=goto_program.insert_before(i_it);
        *t=original_instruction;
      }

      // now add assignments for what is written -- reset
      for(const auto &w_entry : rw_set.w_entries)
      {
        if(!is_shared(ns, w_entry.second.symbol_expr))
          continue;

        goto_program.insert_before(
          i_it,
          goto_programt::make_assignment(
            w_guards.get_w_guard_expr(w_entry.second),
            false_exprt(),
            original_instruction.source_location()));
      }

      // now add assertions for what is read and written
      for(const auto &r_entry : rw_set.r_entries)
      {
        if(!is_shared(ns, r_entry.second.symbol_expr))
          continue;

        source_locationt annotated_location =
          original_instruction.source_location();
        annotated_location.set_comment(comment(r_entry.second, false));
        goto_program.insert_before(
          i_it,
          goto_programt::make_assertion(
            w_guards.get_assertion(r_entry.second), annotated_location));
      }

      for(const auto &w_entry : rw_set.w_entries)
      {
        if(!is_shared(ns, w_entry.second.symbol_expr))
          continue;

        source_locationt annotated_location =
          original_instruction.source_location();
        annotated_location.set_comment(comment(w_entry.second, true));
        goto_program.insert_before(
          i_it,
          goto_programt::make_assertion(
            w_guards.get_assertion(w_entry.second), annotated_location));
      }

      i_it--; // the for loop already counts us up
    }
  }

  remove_skip(goto_program);
}

void race_check(
  value_setst &value_sets,
  symbol_table_baset &symbol_table,
  const irep_idt &function_id,
#ifdef LOCAL_MAY
  const goto_functionst::goto_functiont &goto_function,
#endif
  goto_programt &goto_program,
  message_handlert &message_handler)
{
  w_guardst w_guards(symbol_table);

  race_check(
    value_sets,
    symbol_table,
    function_id,
    L_M_ARG(goto_function) goto_program,
    w_guards,
    message_handler);

  w_guards.add_initialization(goto_program);
  goto_program.update();
}

void race_check(
  value_setst &value_sets,
  goto_modelt &goto_model,
  message_handlert &message_handler)
{
  w_guardst w_guards(goto_model.symbol_table);

  for(auto &gf_entry : goto_model.goto_functions.function_map)
  {
    if(
      gf_entry.first != goto_functionst::entry_point() &&
      gf_entry.first != INITIALIZE_FUNCTION)
    {
      race_check(
        value_sets,
        goto_model.symbol_table,
        gf_entry.first,
        L_M_ARG(gf_entry.second) gf_entry.second.body,
        w_guards,
        message_handler);
    }
  }

  // get "main"
  goto_functionst::function_mapt::iterator
    m_it=goto_model.goto_functions.function_map.find(
      goto_model.goto_functions.entry_point());

  if(m_it==goto_model.goto_functions.function_map.end())
    throw "race check instrumentation needs an entry point";

  goto_programt &main=m_it->second.body;
  w_guards.add_initialization(main);
  goto_model.goto_functions.update();
}
