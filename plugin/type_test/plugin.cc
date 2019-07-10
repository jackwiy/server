/*
   Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2009, 2019, MariaDB

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

#include <my_global.h>
#include <sql_class.h>          // THD
#include <mysql/plugin.h>
#include "sql_type.h"


class Type_handler_test_int64: public Type_handler_longlong
{
public:
  const Name name() const override
  {
    static Name name(STRING_WITH_LEN("test_int64"));
    return name;
  }
};

static Type_handler_test_int64 type_handler_test_int64;


static struct st_mariadb_data_type data_type_test_plugin=
{
  MariaDB_DATA_TYPE_INTERFACE_VERSION,
  &type_handler_test_int64
};


maria_declare_plugin(type_geom)
{
  MariaDB_DATA_TYPE_PLUGIN,     // the plugin type (see include/mysql/plugin.h)
  &data_type_test_plugin,       // pointer to type-specific plugin descriptor
  "TEST_INT64",                 // plugin name
  "MariaDB",                    // plugin author
  "Provides data type TEST_INT64",// the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB veriosn
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_ALPHA // Maturity (see include/mysql/plugin.h)*/
}
maria_declare_plugin_end;
