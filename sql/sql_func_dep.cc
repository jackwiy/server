#include "mariadb.h"
#include "sql_base.h"
#include "sql_select.h"

/**
  @file
   Check if SELECT list and HAVING fields are used in GROUP BY
   or are functionally dependent on fields used in GROUP BY.

   Let's call fields that are used in GROUP BY 'gb' fields and
   fields that are functionally dependent on 'gb' fields 'fd'
   fields. Fields that are either 'gb' or 'fd' or functionally
   dependent on 'fd' will be called 'allowed' fields. 'Allowed'
   fields are allowed to be used in SELECT list and HAVING.

   Field F2 is called functionally dependent on some other field F1
   if such a rule holds: if two values of F1 are equal (or both NULL)
   then two corresponding values of F2 are also equal or both NULL.
   F1 and F2 can also be groups of fields:
   (F11, ..., F1n) and (F21, ..., F2m).

   Functionally dependent fields can be extracted from the WHERE clause
   equalities. Current implementation is limited to the following equalities:

   F2 = g(E11, ... E1n), where

   (E11, ..., E1n) are some functions of 'allowed' fields and/or 'allowed'
                   fields and/or constants.
   g               is some function. It can be identity function.
   F2              is some non-'allowed' field.

   Work if 'only_full_group_by' mode is set only.
*/


class Item_equal_fd :public Sql_alloc
{
public:
  Item_func_eq *equal;
  List<Field> fields1;
  List<Field> fields2;
  Item_equal_fd(Item_func_eq *eq, List<Field> flds1, List<Field> flds2)
    : equal(eq), fields1(flds1), fields2(flds2) {}
};


/**
  Check if all key parts of 'key' are 'allowed' fields.
  If so return true.
*/

static bool all_key_parts_in_gb(KEY *key)
{
  Item *item_arg= 0;
  List<Item> gb_items;
  for (uint i= 0; i < key->user_defined_key_parts; ++i)
  {
    if (!key->key_part[i].field->
         excl_func_dep_on_grouping_fields(0, &gb_items, &item_arg))
      return false;
  }
  return true;
}


/**
  @brief
    Check if either PRIMARY key or UNIQUE key fields are 'allowed'

  @param
    sl  current select

  @details
    For each table used in the FROM list of SELECT sl check
    its PRIMARY and UNIQUE keys.
    If key contains 'allowed' fields only all fields of the table
    where this key is defined are also 'allowed'.

  @retval
    true   if new 'allowed' fields are extracted
    false  otherwise
*/

static
bool check_allowed_unique_keys(st_select_lex *sl)
{
  List_iterator<TABLE_LIST> it(sl->leaf_tables);
  TABLE_LIST *tbl;
  bool fields_extracted= false;
  while ((tbl= it++))
  {
    if (!tbl->table)
      continue;
    /* Check if all fields of this table are already said to be 'allowed'. */
    if (bitmap_is_set_all(&tbl->table->tmp_set))
      continue;
    /* Check PRIMARY key fields if they are 'allowed'. */
    if (tbl->table->s->primary_key < MAX_KEY)
    {
      KEY *pk= &tbl->table->key_info[tbl->table->s->primary_key];
      if (all_key_parts_in_gb(pk))
      {
        bitmap_set_all(&tbl->table->tmp_set);
        fields_extracted= true;
        continue;
      }
    }
    /* Check UNIQUE keys fields if they are 'allowed' */
    KEY *end= tbl->table->key_info + tbl->table->s->keys;
    for (KEY *k= tbl->table->key_info; k < end; k++)
      if ((k->flags & HA_NOSAME) && all_key_parts_in_gb(k))
      {
        bitmap_set_all(&tbl->table->tmp_set);
        fields_extracted= true;
        break;
      }
  }
  return fields_extracted;
}


/**
  @brief
    Check if materialized derived tables and views fields are 'allowed'

  @param
    mat_derived  list of materialized derived tables and views

  @details
    SELECTs that define materialized derived tables and views (MDV)
    are checked before SELECTs where they are used. So on the step
    when this method is called it can be said that if to run MDV as a separate
    query it will return deterministic result set.
    Based on the fact above such a rule holds: if some MDV field is
    'allowed' in SELECT sl where MDV is used then all fields of this MDV
    are 'allowed' in sl.

  @retval
    true   if new 'allowed' fields are extracted
    false  otherwise
*/

static
bool check_allowed_materialized_derived(List<TABLE_LIST> *mat_derived)
{
  if (mat_derived->is_empty())
    return false;
  List_iterator<TABLE_LIST> it(*mat_derived);
  TABLE_LIST *tbl;
  uint tabs_cnt= mat_derived->elements;
  while ((tbl= it++))
  {
    /* Check if at least one field was found as 'allowed'. */
    if (bitmap_is_clear_all(&tbl->table->tmp_set))
      continue;
    bitmap_set_all(&tbl->table->tmp_set);
    it.remove();
  }
  return tabs_cnt != mat_derived->elements;
}


/**
  @brief
    Collect fields used in GROUP BY

  @param
    mat_derived  list of materialized derived tables and views
    gb_items     list of non-field GROUP BY items

  @details
    For each table used in the FROM clause collect its fields used
    in the GROUP BY.
    Mark them in tmp_set map.
    If GROUP BY item is not a field store it in gb_items list.

  @retval
    true   if an error occurs
    false  otherwise
*/

bool st_select_lex::collect_gb_fields(List<TABLE_LIST> *mat_derived,
                                      List<Item> &gb_items)
{
  THD *thd= join->thd;
  if (!group_list.elements)
    return false;

  for (ORDER *ord= group_list.first; ord; ord= ord->next)
  {
    Item *ord_item= *ord->item;
    if (ord_item->type() == Item::FIELD_ITEM ||
        (ord_item->type() == Item::REF_ITEM &&
        ord_item->real_item()->type() == Item::FIELD_ITEM))
    {
      Item_field *real_it= (Item_field *)(ord_item->real_item());
      bitmap_set_bit(&real_it->field->table->tmp_set,
                     real_it->field->field_index);
    }
    else if (gb_items.push_back(ord_item, thd->mem_root))
      return true;
  }

  check_allowed_unique_keys(this);
  check_allowed_materialized_derived(mat_derived);
  return false;
}


/**
  Set subqueries places in the SELECT sl.
  Place here: where this subquery is used (in SELECT list, WHERE or
  HAVING clause of sl).
*/

static
void set_subqueries_context(st_select_lex *sl)
{
  List_iterator_fast<Item> it(sl->item_list);
  Item *item;

  enum_parsing_place ctx= SELECT_LIST;
  while ((item= it++))
  {
    if (item->with_subquery())
      item->walk(&Item::set_subquery_ctx, 0, &ctx);
  }

  Item *cond= sl->join->conds;
  if (cond && cond->with_subquery())
  {
    ctx= IN_WHERE;
    cond->walk(&Item::set_subquery_ctx, 0, &ctx);
  }

  Item *having= sl->join->having;
  if (having && having->with_subquery())
  {
    ctx= IN_HAVING;
    having->walk(&Item::set_subquery_ctx, 0, &ctx);
  }
}


/**
  Check if all fields used in SELECT list are 'allowed'.
*/

bool st_select_lex::are_select_list_fields_allowed(List<Item> *gb_items)
{
  Item *item;
  List_iterator<Item> li(item_list);
  while ((item=li++))
  {
    Item *item_arg= NULL;;
    if (item->excl_func_dep_on_grouping_fields(this, gb_items,
                                               &item_arg))
      continue;
    my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
             item_arg->real_item()->full_name(), "SELECT list");
    return false;
  }
  return true;
}


/**
  Check if all fields used in HAVING clause are 'allowed'.
*/

static
bool are_having_fields_allowed(st_select_lex *sl,
                               Item *having,
                               List<Item> *gb_items)
{
  if (!having)
    return true;

  Item *item_arg= NULL;
  if (having->excl_func_dep_on_grouping_fields(sl, gb_items, &item_arg))
    return true;
  my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
           item_arg->real_item()->full_name(), "HAVING clause");
  return false;
}


/**
  Mark non-'allowed' field in 'allowed' fields map if possible.
*/

static
bool extract_new_func_dep_field(Item *dp_item, Item *nd_item,
                                Item_func_eq *eq)
{
  /*
    Non-'allowed' equality part should be a single field.

    'Allowed' fields equality part should have the same comparison
    type as the equality to avoid additional conversion to the equality type.
    This conversion can lead to the situation when there will be no
    functional dependency between equality parts anymore.
  */
  if (nd_item->real_item()->type() != Item::FIELD_ITEM ||
      (dp_item->type_handler_for_comparison() !=
       eq->compare_type_handler()))
    return false;

  Field *fld= ((Item_field *)nd_item)->field;
  /* Mark nd_item field as 'allowed' */
  bitmap_set_bit(&fld->table->tmp_set, fld->field_index);
  /* Field is a materialized derived table field */
  if (fld->table->pos_in_table_list->is_materialized_derived())
    bitmap_set_all(&fld->table->tmp_set);
  return true;
}


/**
  @brief
    Check if a new 'allowed' field can be extracted from the equality

  @param
    eq_item   equality that needs to be checked
    sl        current select
    eq_items  list of WHERE clause equalities items information

  @details
    Divide equality into two parts (left and right) and check if left
    and right are functionally dependent on 'allowed' fields.

    There can be several cases:

    1. Both parts of the equality depend on 'allowed' fields only.
       Then no new 'allowed' field can be extracted from this equality.
    2. Both parts of the equality don't depend on 'allowed' fields only.
         a. There is a chance that after processing some other equality
            new 'allowed' field will be extracted. This field will make
            left or right dependent on 'allowed' fields only.
            If so, new 'allowed' field can be extracted from the other
            part of the equality.
         b. Information about this equality (left and right details)
            is saved in eq_items so it can be used in future processing.
    3. One part (let it be left part) depends on allowed fields only and
       the other part (right) depends on non-'allowed' fields only.
       Right part should be a single field.
       Then it can be said that non-'allowed' field is equal to some
       function of 'allowed' fields. Therefore, this field can be functionally
       dependent on 'allowed' fields and is also 'allowed'.

  @retval
    true   if an error occurs
    false  otherwise
*/

static bool check_equality_on_new_func_dep(Item_func_eq *eq_item,
                                           st_select_lex *sl,
                                           List<Item_equal_fd> &eq_items)
{
  THD *thd= sl->join->thd;
  Item *item_arg= 0;

  Item *item1= eq_item->arguments()[0];  /* Left part of the equality */
  Item *item2= eq_item->arguments()[1];  /* Right part of the equality */
  List<Field> fields1;  /* Fields used in the left part of the equality */
  List<Field> fields2;  /* Fields used in the right part of the equality */

  bool dep1=
    item1->excl_func_dep_from_equalities(sl, &item_arg, &fields1);

  /*
    Left part of the equality can't be used for the extraction of a
    new 'allowed' field.
  */
  if (!dep1 && fields1.is_empty())
  {
    if (item_arg)
    {
      my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
               item_arg->real_item()->full_name(), "WHERE clause");
      return true;
    }
    return false;
  }

  bool dep2=
    item2->excl_func_dep_from_equalities(sl, &item_arg, &fields2);

  /*
    1. Both parts of the equality depend on 'allowed' fields only.
    or
    a. Right part of the equality can't be used for the extraction of
       a new 'allowed' field
    or
    b. Both left and right parts don't contain exactly one field.
       So the equality is of the form:
       (F11,...,F1n) = (F11,...,F1m)
  */
  if ((dep1 && dep2) ||
      (!dep2 && fields2.is_empty()) ||
      (fields1.elements != 1 && fields2.elements != 1))
  {
    /*
      Non-'allowed' field is used in WHERE.
      Example:

      SELECT (                        <------------- sl1
        SELECT inner1.a           <----------------- sl2
        FROM t1 AS inner1
        WHERE (outer.b > 1)
        GROUP BY inner1.a
      ) FROM t1 AS outer
      GROUP BY outer.a;

      Here outer.b can't be used in the WHERE clause of the inner
      SELECT sl2. sl2 is located in the SELECT list of sl1 where
      it is forbidden to use non-'allowed' sl1 fields.
    */
    if (item_arg)
    {
      my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
               item_arg->real_item()->full_name(), "WHERE clause");
      return true;
    }
    return false;
  }
  /* 2. Both parts don't depend on 'allowed' fields only. */
  if (!dep1 && !dep2)
  {
    Item_equal_fd *new_eqctx=
      new (thd->mem_root) Item_equal_fd(eq_item, fields1, fields2);
    if (eq_items.push_back(new_eqctx, thd->mem_root))
      return true;
    return false;
  }
  /*
    3. One part depends on allowed fields only and the other part depends
       on non-'allowed' fields only.
  */
  if (dep1)
    extract_new_func_dep_field(item1, item2, eq_item);
  else if (dep2)
    extract_new_func_dep_field(item2, item1, eq_item);
  return false;
}


/**
  @brief
    Get information about fields used in the WHERE clause.

  @param
    sl           current select
    mat_derived  list of materialized derived tables and views

  @details
    This method can be divided into several stages:

    1. Traverse WHERE clause and check if it doesn't depend on non-'allowed'
       fields of outer SELECTs.

       If WHERE clause is an equality or it is an AND-condition that
       contains some equalities then check_equality_on_new_func_dep() method
       is called to check if some new 'allowed' field can be extracted
       from these equality.
       If ‘allowed’ field can’t be extracted from the equality on this step
       its internal information is saved into eq_items list.
    2. If there are no items in eq_items list then no new 'allowed' fields
       can be extracted.

       This happens because:
       a. There are no equalities in the WHERE clause from which some new
          'allowed' fields can be extracted.
       b. All equalities are already processed and all ‘allowed’ fields are
          extracted.
    3. If the count of equalities that can be used for extraction is equal
       to the eq_items list elements count then no new 'allowed' fields can
       be extracted.

       This means that for every equality that can be used for extraction
       the following is true: its parts don't depend on 'allowed' fields only.
    4. Go through the eq_items list trying to extract new 'allowed' fields.
       Stop if no new 'allowed' fields were extracted on the previous step
       or there are no equalities from which ‘allowed’ fields can be extracted.

  @retval
    true   if an error occurs
    false  otherwise
*/

static
bool check_where_and_get_new_dependencies(st_select_lex *sl,
                                          List<TABLE_LIST> *mat_derived)
{
  Item *cond= sl->join->conds;
  if (!cond)
    return false;

  List<Item_equal_fd> eq_items;
  List<Item> gb_items;
  Item *item_arg= 0;
  uint eq_count= 0;

  /*
    1. Traverse WHERE clause and check if it doesn't depend on non-'allowed'
       fields of outer SELECTs.
  */
  if (cond && cond->type() == Item::COND_ITEM &&
      ((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC)
  {
    List_iterator_fast<Item> li(*((Item_cond*) cond)->argument_list());
    Item *item;
    while ((item=li++))
    {
      if (item->type() == Item::FUNC_ITEM &&
          ((Item_func_eq *) item)->functype() == Item_func::EQ_FUNC)
      {
        eq_count++;
        if (check_equality_on_new_func_dep((Item_func_eq *)item, sl, eq_items))
          return true;
      }
      else
      {
        /*
           How to change it?
           Want to allow to use all fields of this select.
           Can't use IN_WHERE context.
        */
        enum_parsing_place ctx= sl->context_analysis_place;
        sl->context_analysis_place= IN_GROUP_BY;
        if (!item->excl_func_dep_on_grouping_fields(sl, &gb_items, &item_arg) &&
            item_arg)
        {
          my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
                   item_arg->real_item()->full_name(), "WHERE clause");
          return true;
        }
        sl->context_analysis_place= ctx;
      }
    }
  }
  else if (cond->type() == Item::FUNC_ITEM &&
           ((Item_func*) cond)->functype() == Item_func::EQ_FUNC)
  {
    eq_count++;
    if (check_equality_on_new_func_dep((Item_func_eq *)cond, sl, eq_items))
      return true;
  }
  else
  {
    enum_parsing_place ctx= sl->context_analysis_place;
    sl->context_analysis_place= IN_GROUP_BY;
    if (!cond->excl_func_dep_on_grouping_fields(sl, &gb_items, &item_arg) &&
           item_arg)
    {
      my_error(ER_NON_GROUPING_FIELD_USED, MYF(0),
               item_arg->real_item()->full_name(), "WHERE clause");
      return true;
    }
    sl->context_analysis_place= ctx;
  }
  /*
    2. If there are no items in eq_items list then no new 'allowed' fields
       can be extracted.
  */
  if (eq_items.is_empty())
  {
    check_allowed_unique_keys(sl);
    return false;
  }
  /*
    3. If the count of equalities that can be used for extraction is equal
       to the eq_items list elements count then no new 'allowed' fields can
       be extracted.
  */
  if (eq_count == eq_items.elements)
    return false;

  List_iterator<Item_equal_fd> li(eq_items);
  Item_equal_fd *eq_it;
  bool extracted= true;

  /*
    4. Go through the eq_items list trying to extract new 'allowed' fields.
  */
  while (extracted && !eq_items.is_empty())
  {
    extracted= false;
    li.rewind();
    while ((eq_it= li++))
    {
      List_iterator_fast<Field> it1(eq_it->fields1);
      List_iterator_fast<Field> it2(eq_it->fields2);
      Field *fld;
      bool dep1= true;
      bool dep2= true;

      while ((fld= it1++))
        dep1&= fld->excl_func_dep_on_grouping_fields(sl, &gb_items, &item_arg);
      while ((fld= it2++))
        dep2&= fld->excl_func_dep_on_grouping_fields(sl, &gb_items, &item_arg);

      if (!dep1 && !dep2)
        continue;
      if (!(dep1 && dep2) &&
          ((dep1 && extract_new_func_dep_field(eq_it->equal->arguments()[0],
                                               eq_it->equal->arguments()[1],
                                               eq_it->equal)) ||
          (dep2 && extract_new_func_dep_field(eq_it->equal->arguments()[1],
                                              eq_it->equal->arguments()[0],
                                              eq_it->equal))))
        extracted= true;
      li.remove();
    }
    if (!extracted || eq_items.is_empty())
    {
      /* Check if any keys fields become 'allowed'. */
      if (check_allowed_unique_keys(sl))
        extracted= true;
    }
  }
  return false;
}


/**
  If UPDATE query is used mark all fields of the updated table as allowed.
*/

void set_update_table_fields(st_select_lex *sl)
{
  if (!sl->master_unit()->item ||
      !sl->master_unit()->outer_select() ||
      sl->master_unit()->outer_select()->join)
    return;
  List_iterator<TABLE_LIST> it(sl->master_unit()->outer_select()->leaf_tables);
  TABLE_LIST *tbl;
  while ((tbl= it++))
    bitmap_set_all(&tbl->table->tmp_set);
}


/**
  @brief
    Check if this SELECT returns deterministic result

  @details
    Check if SELECT list and HAVING clause depend on 'allowed'
    fields only.
    'Allowed' fields list is formed this way:
    a. GROUP BY fields
    b. Fields that are functionally dependent on GROUP BY fields
       (extracted from the WHERE clause equalities).
    c. Fields that are functionally dependent on fields, got from 'b.'
       and 'c.' (also extracted from WHERE clause equalities).

  @note
    If this SELECT is a subquery and there are outer references
    to the parent SELECTs tables, check that all these references
    are also 'allowed'. Fields of SELECT list, HAVING clause and
    WHERE clause are checked.

  @retval
    true   if an error occurs
    false  otherwise
*/

bool st_select_lex::check_func_dep()
{
  THD *thd= join->thd;
  /* Stop if no tables are used or fake SELECT is processed. */
  if (leaf_tables.is_empty() ||
      select_number == UINT_MAX ||
      select_number == INT_MAX)
    return false;

  bool need_check= (group_list.elements > 0) ||
                    (master_unit()->outer_select() &&
                     master_unit()->outer_select()->join) ||
                     having;

  List<TABLE_LIST> mat_derived;
  List_iterator<TABLE_LIST> it(leaf_tables);
  TABLE_LIST *tbl;
  while ((tbl= it++))
  {
    if (!tbl->table)
      continue;
    bitmap_clear_all(&tbl->table->tmp_set);
    if (tbl->is_materialized_derived())
    {
      /*
        Collect materialized derived tables used in the FROM clause
        of this SELECT.
      */
      if (mat_derived.push_back(tbl, thd->mem_root))
        return true;
      continue;
    }
  }
  set_update_table_fields(this); /* UPDATE query processing. */
  set_subqueries_context(this); /* Set subqueries places in this SELECT. */

  /*
    This SELECT has no GROUP BY clause and HAVING.
    If so all FROM clause tables fields are 'allowed'.
  */
  if (group_list.elements == 0 && !having)
  {
    List_iterator<TABLE_LIST> it(leaf_tables);
    TABLE_LIST *tbl;

    while ((tbl= it++))
      bitmap_set_all(&tbl->table->tmp_set);
    if (!need_check)
      return false;
  }

  List<Item> gb_items;
  /* Collect fields from GROUP BY. */
  if (collect_gb_fields(&mat_derived, gb_items))
    return true;

  /*
    Try to find new fields that are functionally dependent on 'allowed'
    fields and check if WHERE depends on 'allowed' fields only.
  */
  if (check_where_and_get_new_dependencies(this, &mat_derived))
    return true;
  /*
    Check if SELECT list and HAVING clause depend on 'allowed' fields only.
  */
  if (!are_select_list_fields_allowed(&gb_items) ||
      !are_having_fields_allowed(this, join->having, &gb_items))
    return true;

  return false;
}
