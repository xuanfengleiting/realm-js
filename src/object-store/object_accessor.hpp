////////////////////////////////////////////////////////////////////////////
//
// Copyright 2015 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#ifndef REALM_OBJECT_ACCESSOR_HPP
#define REALM_OBJECT_ACCESSOR_HPP

#include <string>
#include "shared_realm.hpp"

namespace realm {
    template<typename ValueType, typename ContextType>
    class NativeAccessor {
    public:
        //
        // Value converters - template specializations must be implemented for each platform
        //
        static bool dict_has_value_for_key(ContextType ctx, ValueType dict, const std::string &prop_name);
        static ValueType dict_value_for_key(ContextType ctx, ValueType dict, const std::string &prop_name);

        static bool has_default_value_for_property(ContextType ctx, const ObjectSchema &object_schema, const std::string &prop_name);
        static ValueType default_value_for_property(ContextType ctx, const ObjectSchema &object_schema, const std::string &prop_name);

        static bool to_bool(ContextType ctx, ValueType &val);
        static long long to_long(ContextType ctx, ValueType &val);
        static float to_float(ContextType ctx, ValueType &val);
        static double to_double(ContextType ctx, ValueType &val);
        static std::string to_string(ContextType ctx, ValueType &val);
        static DateTime to_datetime(ContextType ctx, ValueType &val);

        static bool is_null(ContextType ctx, ValueType &val);

        // convert value to persisted object
        // for existing objects return the existing row index
        // for new/updated objects return the row index
        static size_t to_object_index(ContextType ctx, SharedRealm &realm, ValueType &val, std::string &type, bool try_update);

        // array value acessors
        static size_t array_size(ContextType ctx, ValueType &val);
        static ValueType array_value_at_index(ContextType ctx, ValueType &val, size_t index);

        //
        // Deprecated
        //
        static Mixed to_mixed(ContextType ctx, ValueType &val) { throw std::runtime_error("'Any' type is unsupported"); }
    };

    class Object {
    public:
        Object(SharedRealm &r, ObjectSchema &s, Row o) : realm(r), object_schema(s), row(o) {}
        // FIXME - all should be const
        SharedRealm realm;
        ObjectSchema &object_schema;
        Row row;

        // property setter
        template<typename ValueType, typename ContextType>
        inline void set_property_value(ContextType ctx, std::string prop_name, ValueType value, bool try_update);

        // create an Object from a native representation
        template<typename ValueType, typename ContextType>
        static inline Object create(ContextType ctx, SharedRealm realm, ObjectSchema &object_schema, ValueType value, bool try_update);
        
    private:
        template<typename ValueType, typename ContextType>
        inline void set_property_value_impl(ContextType ctx, Property &property, ValueType value, bool try_update);
    };

    //
    // template method implementations
    //
    template <typename ValueType, typename ContextType>
    inline void Object::set_property_value(ContextType ctx, std::string prop_name, ValueType value, bool try_update)
    {
        Property *prop = object_schema.property_for_name(prop_name);
        if (!prop) {
            throw std::runtime_error("Setting invalid property '" + prop_name + "' on object '" + object_schema.name + "'.");
        }
        set_property_value_impl(ctx, *prop, value, try_update);
    };

    template <typename ValueType, typename ContextType>
    inline void Object::set_property_value_impl(ContextType ctx, Property &property, ValueType value, bool try_update)
    {
        using Accessor = NativeAccessor<ValueType, ContextType>;

        size_t column = property.table_column;
        switch (property.type) {
            case PropertyTypeBool:
                row.set_bool(column, Accessor::to_bool(ctx, value));
                break;
            case PropertyTypeInt:
                row.set_int(column, Accessor::to_long(ctx, value));
                break;
            case PropertyTypeFloat:
                row.set_float(column, Accessor::to_float(ctx, value));
                break;
            case PropertyTypeDouble:
                row.set_double(column, Accessor::to_double(ctx, value));
                break;
            case PropertyTypeString:
                row.set_string(column, Accessor::to_string(ctx, value));
                break;
            case PropertyTypeData:
                row.set_binary(column, BinaryData(Accessor::to_string(ctx, value)));
                break;
            case PropertyTypeAny:
                row.set_mixed(column, Accessor::to_mixed(ctx, value));
                break;
            case PropertyTypeDate:
                row.set_datetime(column, Accessor::to_datetime(ctx, value));
                break;
            case PropertyTypeObject: {
                if (Accessor::is_null(ctx, value)) {
                    row.nullify_link(column);
                }
                else {
                    row.set_link(column, Accessor::to_object_index(ctx, realm, value, property.object_type, try_update));
                }
                break;
            }
            case PropertyTypeArray: {
                realm::LinkViewRef link_view = row.get_linklist(column);
                link_view->clear();
                size_t count = Accessor::array_size(ctx, value);
                for (size_t i = 0; i < count; i++) {
                    ValueType element = Accessor::array_value_at_index(ctx, value, i);
                    link_view->add(Accessor::to_object_index(ctx, realm, element, property.object_type, try_update));
                }
                break;
            }
        }
    }

    template<typename ValueType, typename ContextType>
    inline Object Object::create(ContextType ctx, SharedRealm realm, ObjectSchema &object_schema, ValueType value, bool try_update)
    {
        using Accessor = NativeAccessor<ValueType, ContextType>;

        if (!realm->is_in_transaction()) {
            throw std::runtime_error("Can only create objects within a transaction.");
        }

        // get or create our accessor
        bool created;

        // try to get existing row if updating
        size_t row_index = realm::not_found;
        realm::TableRef table = ObjectStore::table_for_object_type(realm->read_group(), object_schema.name);
        Property *primary_prop = object_schema.primary_key_property();
        if (primary_prop) {
            // search for existing object based on primary key type
            ValueType primary_value = Accessor::dict_value_for_key(ctx, value, object_schema.primary_key);
            if (primary_prop->type == PropertyTypeString) {
                row_index = table->find_first_string(primary_prop->table_column, Accessor::to_string(ctx, primary_value));
            }
            else {
                row_index = table->find_first_int(primary_prop->table_column, Accessor::to_long(ctx, primary_value));
            }

            if (!try_update && row_index != realm::not_found) {
                throw std::runtime_error("Attempting to create an object of type '" + object_schema.name + "' with an exising primary key value.");
            }
        }

        // if no existing, create row
        created = false;
        if (row_index == realm::not_found) {
            row_index = table->add_empty_row();
            created = true;
        }

        // populate
        Object object(realm, object_schema, table->get(row_index));
        for (Property &prop : object_schema.properties) {
            if (created || !prop.is_primary) {
                if (Accessor::dict_has_value_for_key(ctx, value, prop.name)) {
                    object.set_property_value_impl(ctx, prop, Accessor::dict_value_for_key(ctx, value, prop.name), try_update);
                }
                else if (created) {
                    if (Accessor::has_default_value_for_property(ctx, object_schema, prop.name)) {
                        object.set_property_value_impl(ctx, prop, Accessor::default_value_for_property(ctx, object_schema, prop.name), try_update);
                    }
                    else {
                        throw std::runtime_error("Missing property value for property " + prop.name);
                    }
                }
            }
        }
        return object;
    }
}

#endif /* defined(REALM_OBJECT_ACCESSOR_HPP) */