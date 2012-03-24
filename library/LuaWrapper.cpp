/*
https://github.com/peterix/dfhack
Copyright (c) 2009-2011 Petr Mrázek (peterix@gmail.com)

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must
not claim that you wrote the original software. If you use this
software in a product, an acknowledgment in the product documentation
would be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/

#include "Internal.h"

#include <string>
#include <vector>
#include <map>

#include "MemAccess.h"
#include "Core.h"
#include "VersionInfo.h"
#include "tinythread.h"
// must be last due to MS stupidity
#include "DataDefs.h"
#include "DataIdentity.h"

#include "MiscUtils.h"

#include <lua.h>
#include <lauxlib.h>

using namespace DFHack;

static luaL_Reg no_functions[] = { { NULL, NULL } };

inline void lua_dup(lua_State *state) { lua_pushvalue(state, -1); }
inline void lua_swap(lua_State *state) { lua_insert(state, -2); }

/*
 * Registry name: hash of type metatables <-> type identities.
 */
#define DFHACK_TYPETABLE_NAME "DFHack::DFTypes"

/*
 * Registry name: hash of type identity -> node in df.etc...
 */
#define DFHACK_TYPEID_TABLE_NAME "DFHack::DFTypeIds"

/*
 * Registry name: hash of enum/bitfield identity -> index lookup table
 */
#define DFHACK_ENUM_TABLE_NAME "DFHack::DFEnums"

/*
 * Registry name: hash of pointer target identity <-> adhoc pointer identity userdata.
 */
#define DFHACK_PTR_IDTABLE_NAME "DFHack::PtrDFTypes"

// Function registry names
#define DFHACK_CHANGEERROR_NAME "DFHack::ChangeError"
#define DFHACK_COMPARE_NAME "DFHack::ComparePtrs"
#define DFHACK_TYPE_TOSTRING_NAME "DFHack::TypeToString"
#define DFHACK_SIZEOF_NAME "DFHack::Sizeof"
#define DFHACK_DISPLACE_NAME "DFHack::Displace"
#define DFHACK_NEW_NAME "DFHack::New"
#define DFHACK_ASSIGN_NAME "DFHack::Assign"

/*
 * Upvalue: contents of DFHACK_TYPETABLE_NAME
 */
#define UPVAL_TYPETABLE lua_upvalueindex(1)

/*
 * Expected metatable of the current object.
 */
#define UPVAL_METATABLE lua_upvalueindex(2)

/*
 * Table mapping field names to indices or data structure pointers.
 * Enum index table is linked into here via getmetatable($).__index.
 * Fields that are actually in UPVAL_METATABLE are marked with NULL light udata.
 */
#define UPVAL_FIELDTABLE lua_upvalueindex(3)

/*
 * Only for containers: light udata with container identity.
 */
#define UPVAL_CONTAINER_ID lua_upvalueindex(4)

/*
 * Only for containers: light udata with item identity.
 */
#define UPVAL_ITEM_ID lua_upvalueindex(5)

/*
 * Only for containers: if not nil, overrides the item count.
 */
#define UPVAL_ITEM_COUNT lua_upvalueindex(6)

namespace {
    /**
     * Object references are represented as userdata instances
     * with an appropriate metatable; the payload of userdata is
     * this structure:
     */
    struct DFRefHeader {
        void *ptr;
    };

    /*
     * The system might be extended to carry some simple
     * objects inline inside the reference buffer.
     */
    inline bool is_self_contained(DFRefHeader *ptr) {
        void **pp = &ptr->ptr;
        return **(void****)pp == (pp + 1);
    }
}

/**
 * Report an error while accessing a field (index = field name).
 */
static void field_error(lua_State *state, int index, const char *err, const char *mode)
{
    lua_getfield(state, UPVAL_METATABLE, "__metatable");
    const char *cname = lua_tostring(state, -1);
    const char *fname = index ? lua_tostring(state, index) : "*";
    luaL_error(state, "Cannot %s field %s.%s: %s.",
               mode, (cname ? cname : "?"), (fname ? fname : "?"), err);
}

/*
 * If is_method is true, these use UPVAL_TYPETABLE to save a hash lookup.
 */
static void push_object_internal(lua_State *state, type_identity *type, void *ptr, bool in_method = true);
static void *get_object_internal(lua_State *state, type_identity *type, int val_index, bool exact_type, bool in_method = true);

void DFHack::PushDFObject(lua_State *state, type_identity *type, void *ptr)
{
    push_object_internal(state, type, ptr, false);
}

void *DFHack::GetDFObject(lua_State *state, type_identity *type, int val_index, bool exact_type)
{
    return get_object_internal(state, type, val_index, exact_type, false);
}

static void push_adhoc_pointer(lua_State *state, void *ptr, type_identity *target);

/**************************************
 * Identity object read/write methods *
 **************************************/

void constructed_identity::lua_read(lua_State *state, int fname_idx, void *ptr)
{
    push_object_internal(state, this, ptr);
}

void constructed_identity::lua_write(lua_State *state, int fname_idx, void *ptr, int val_index)
{
    field_error(state, fname_idx, "complex object", "write");
}

void enum_identity::lua_read(lua_State *state, int fname_idx, void *ptr)
{
    base_type->lua_read(state, fname_idx, ptr);
}

void enum_identity::lua_write(lua_State *state, int fname_idx, void *ptr, int val_index)
{
    base_type->lua_write(state, fname_idx, ptr, val_index);
}

void df::number_identity_base::lua_read(lua_State *state, int fname_idx, void *ptr)
{
    lua_pushnumber(state, read(ptr));
}

void df::number_identity_base::lua_write(lua_State *state, int fname_idx, void *ptr, int val_index)
{
    if (!lua_isnumber(state, val_index))
        field_error(state, fname_idx, "number expected", "write");

    write(ptr, lua_tonumber(state, val_index));
}

void df::bool_identity::lua_read(lua_State *state, int fname_idx, void *ptr)
{
    lua_pushboolean(state, *(bool*)ptr);
}

void df::bool_identity::lua_write(lua_State *state, int fname_idx, void *ptr, int val_index)
{
    char *pb = (char*)ptr;

    if (lua_isboolean(state, val_index) || lua_isnil(state, val_index))
        *pb = lua_toboolean(state, val_index);
    else if (lua_isnumber(state, val_index))
        *pb = lua_tointeger(state, val_index);
    else
        field_error(state, fname_idx, "boolean or number expected", "write");
}

void df::stl_string_identity::lua_read(lua_State *state, int fname_idx, void *ptr)
{
    auto pstr = (std::string*)ptr;
    lua_pushlstring(state, pstr->data(), pstr->size());
}

void df::stl_string_identity::lua_write(lua_State *state, int fname_idx, void *ptr, int val_index)
{
    size_t size;
    const char *bytes = lua_tolstring(state, val_index, &size);
    if (!bytes)
        field_error(state, fname_idx, "string expected", "write");

    *(std::string*)ptr = std::string(bytes, size);
}

void df::pointer_identity::lua_read(lua_State *state, int fname_idx, void *ptr, type_identity *target)
{
    push_object_internal(state, target, *(void**)ptr);
}

void df::pointer_identity::lua_read(lua_State *state, int fname_idx, void *ptr)
{
    lua_read(state, fname_idx, ptr, target);
}

void df::pointer_identity::lua_write(lua_State *state, int fname_idx, void *ptr,
                                     type_identity *target, int val_index)
{
    auto pptr = (void**)ptr;

    if (lua_isnil(state, val_index))
        *pptr = NULL;
    else
    {
        void *nval = get_object_internal(state, target, val_index, false);
        if (nval)
            *pptr = nval;
        else
            field_error(state, fname_idx, "incompatible pointer type", "write");
    }
}

void df::pointer_identity::lua_write(lua_State *state, int fname_idx, void *ptr, int val_index)
{
    lua_write(state, fname_idx, ptr, target, val_index);
}

int container_identity::lua_item_count(lua_State *state, void *ptr)
{
    if (lua_isnumber(state, UPVAL_ITEM_COUNT))
        return lua_tointeger(state, UPVAL_ITEM_COUNT);
    else
        return item_count(ptr);
}

void container_identity::lua_item_reference(lua_State *state, int fname_idx, void *ptr, int idx)
{
    auto id = (type_identity*)lua_touserdata(state, UPVAL_ITEM_ID);
    void *pitem = item_pointer(id, ptr, idx);
    push_object_internal(state, id, pitem);
}

void container_identity::lua_item_read(lua_State *state, int fname_idx, void *ptr, int idx)
{
    auto id = (type_identity*)lua_touserdata(state, UPVAL_ITEM_ID);
    void *pitem = item_pointer(id, ptr, idx);
    id->lua_read(state, fname_idx, pitem);
}

void container_identity::lua_item_write(lua_State *state, int fname_idx, void *ptr, int idx, int val_index)
{
    auto id = (type_identity*)lua_touserdata(state, UPVAL_ITEM_ID);
    void *pitem = item_pointer(id, ptr, idx);
    id->lua_write(state, fname_idx, pitem, val_index);
}

void ptr_container_identity::lua_item_reference(lua_State *state, int fname_idx, void *ptr, int idx)
{
    auto id = (type_identity*)lua_touserdata(state, UPVAL_ITEM_ID);
    void *pitem = item_pointer(id, ptr, idx);
    push_adhoc_pointer(state, pitem, id);
}

void ptr_container_identity::lua_item_read(lua_State *state, int fname_idx, void *ptr, int idx)
{
    auto id = (type_identity*)lua_touserdata(state, UPVAL_ITEM_ID);
    void *pitem = item_pointer(&df::identity_traits<void*>::identity, ptr, idx);
    df::pointer_identity::lua_read(state, fname_idx, pitem, id);
}

void ptr_container_identity::lua_item_write(lua_State *state, int fname_idx, void *ptr, int idx, int val_index)
{
    auto id = (type_identity*)lua_touserdata(state, UPVAL_ITEM_ID);
    void *pitem = item_pointer(&df::identity_traits<void*>::identity, ptr, idx);
    df::pointer_identity::lua_write(state, fname_idx, pitem, id, val_index);
}

void bit_container_identity::lua_item_reference(lua_State *state, int, void *, int)
{
    lua_pushnil(state);
}

void bit_container_identity::lua_item_read(lua_State *state, int fname_idx, void *ptr, int idx)
{
    lua_pushboolean(state, get_item(ptr, idx));
}

void bit_container_identity::lua_item_write(lua_State *state, int fname_idx, void *ptr, int idx, int val_index)
{
    if (lua_isboolean(state, val_index) || lua_isnil(state, val_index))
        set_item(ptr, idx, lua_toboolean(state, val_index));
    else if (lua_isnumber(state, val_index))
        set_item(ptr, idx, lua_tointeger(state, val_index) != 0);
    else
        field_error(state, fname_idx, "boolean or number expected", "write");
}

/* */

static int change_error(lua_State *state)
{
    luaL_error(state, "Attempt to change a read-only table.\n");
    return 0;
}

/**
 * Wrap a table so that it can't be modified.
 */
static void freeze_table(lua_State *state, bool leave_metatable = false, const char *name = NULL)
{
    // rv = {}; setmetatable(rv, { __index = in, __newindex = change_error, __metatable = name })
    int base = lua_gettop(state);
    lua_newtable(state);
    lua_swap(state);
    lua_setfield(state, base, "__index");
    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_CHANGEERROR_NAME);
    lua_setfield(state, base, "__newindex");
    lua_newtable(state);
    lua_swap(state);
    lua_dup(state);
    lua_setmetatable(state, base);
    if (name)
    {
        lua_pushstring(state, name);
        lua_setfield(state, -2, "__metatable");
    }
    // result: [frozen table] [metatable]
    if (!leave_metatable)
        lua_pop(state, 1);
}

static void LookupInTable(lua_State *state, const char *tname)
{
    lua_getfield(state, LUA_REGISTRYINDEX, tname);
    lua_swap(state);
    lua_rawget(state, -2);
    lua_remove(state, -2);
}

/**
 * Look up the key on the stack in DFHACK_TYPETABLE;
 * if found, put result on the stack and return true.
 */
static bool LookupTypeInfo(lua_State *state, bool in_method)
{
    // stack: [lookup key]

    if (in_method)
    {
        lua_rawget(state, UPVAL_TYPETABLE);
    }
    else
    {
        LookupInTable(state, DFHACK_TYPETABLE_NAME);
    }

    // stack: [info]

    if (!lua_islightuserdata(state, -1))
    {
        lua_pop(state, 1);
        return false;
    }
    else
        return true;
}

static void LookupInTable(lua_State *state, void *id, const char *tname)
{
    lua_getfield(state, LUA_REGISTRYINDEX, tname);
    lua_pushlightuserdata(state, id);
    lua_rawget(state, -2);
    lua_remove(state, -2);
}

static void SaveInTable(lua_State *state, void *node, const char *tname)
{
    // stack: [info]
    lua_getfield(state, LUA_REGISTRYINDEX, tname);

    lua_pushlightuserdata(state, node);
    lua_pushvalue(state, -3);
    lua_rawset(state, -3);

    lua_pushvalue(state, -2);
    lua_pushlightuserdata(state, node);
    lua_rawset(state, -3);

    lua_pop(state, 1);
    // stack: [info]
}

static void SaveTypeInfo(lua_State *state, void *node)
{
    SaveInTable(state, node, DFHACK_TYPETABLE_NAME);
}

static void BuildTypeMetatable(lua_State *state, type_identity *type);

/**
 * Push the pointer as DF object ref using metatable on the stack.
 */
static void push_object_ref(lua_State *state, void *ptr)
{
    // stack: [metatable]
    auto ref = (DFRefHeader*)lua_newuserdata(state, sizeof(DFRefHeader));
    ref->ptr = ptr;

    lua_swap(state);
    lua_setmetatable(state, -2);
    // stack: [userdata]
}

static void *get_object_ref(lua_State *state, int val_index)
{
    assert(!lua_islightuserdata(state, val_index));

    auto ref = (DFRefHeader*)lua_touserdata(state, val_index);
    return ref->ptr;
}

/**
 * Push the pointer using given identity.
 */
static void push_object_internal(lua_State *state, type_identity *type, void *ptr, bool in_method)
{
    /*
     * If NULL pointer or no type, push something simple
     */

    if (!ptr || !type)
    {
        if (!ptr)
            lua_pushnil(state);
        else
            lua_pushlightuserdata(state, ptr);

        return;
    }

    /*
     * Resolve actual class using vtable
     */

    if (type->type() == IDTYPE_CLASS)
    {
        virtual_identity *class_vid = virtual_identity::get(virtual_ptr(ptr));
        if (class_vid)
            type = class_vid;
    }

    /*
     * Resolve metatable by identity, and push the object
     */

    lua_pushlightuserdata(state, type); // () -> type

    if (!LookupTypeInfo(state, in_method)) // type -> metatable?
        BuildTypeMetatable(state, type); // () -> metatable

    push_object_ref(state, ptr); // metatable -> userdata
}

static void fetch_container_details(lua_State *state, int meta, type_identity **pitem, int *pcount)
{
    if (!meta) return;

    lua_getfield(state, meta, "_field_identity");
    *pitem = (type_identity*)lua_touserdata(state, -1);
    lua_pop(state, 1);

    if (pcount)
    {
        lua_getfield(state, meta, "_count");
        if (lua_isnumber(state, -1))
            *pcount = lua_tointeger(state, -1);
        lua_pop(state, 1);
    }
}

/**
 * Check if type1 and type2 are compatible, possibly using additional metatable data.
 */
static bool is_type_compatible(lua_State *state, type_identity *type1, int meta1,
                               type_identity *type2, int meta2, bool exact_equal)
{
    if (type1 == type2)
        return true;
    if (!exact_equal && !type1)
        return true;
    if (!type1 || !type2)
        return false;

    auto t1 = type1->type();
    if (t1 != type2->type())
        return false;

    switch (t1)
    {
    case IDTYPE_POINTER:
        return is_type_compatible(state,
                                  ((pointer_identity*)type1)->getTarget(), 0,
                                  ((pointer_identity*)type2)->getTarget(), 0,
                                  exact_equal);
        break;

    case IDTYPE_BUFFER:
    {
        auto b1 = (df::buffer_container_identity*)type1;
        auto b2 = (df::buffer_container_identity*)type2;
        type_identity *item1 = b1->getItemType(), *item2 = b2->getItemType();
        int count1 = b1->getSize(), count2 = b2->getSize();

        fetch_container_details(state, meta1, &item1, &count1);
        fetch_container_details(state, meta2, &item2, &count2);

        return item1 && item2 && count1 == count2 &&
               is_type_compatible(state, item1, 0, item2, 0, true);
    }

    case IDTYPE_STL_PTR_VECTOR:
    {
        auto b1 = (df::stl_ptr_vector_identity*)type1;
        auto b2 = (df::stl_ptr_vector_identity*)type2;
        type_identity *item1 = b1->getItemType(), *item2 = b2->getItemType();

        fetch_container_details(state, meta1, &item1, NULL);
        fetch_container_details(state, meta1, &item2, NULL);

        return is_type_compatible(state, item1, 0, item2, 0, exact_equal);
    }

    case IDTYPE_STRUCT:
    case IDTYPE_CLASS:
    {
        auto b1 = (struct_identity*)type1;
        auto b2 = (struct_identity*)type2;

        return (!exact_equal && b1->is_subclass(b2));
    }

    default:
        return false;
    }
}

static bool is_type_compatible(lua_State *state, type_identity *type1, int meta1,
                               int meta2, bool exact_equal)
{
    lua_getfield(state, meta2, "_identity");
    auto type2 = (type_identity*)lua_touserdata(state, -1);
    lua_pop(state, 1);

    return is_type_compatible(state, type1, meta1, type2, meta2, exact_equal);
}

static bool is_type_compatible(lua_State *state, int meta1, int meta2, bool exact_equal)
{
    if (lua_rawequal(state, meta1, meta2))
        return true;

    lua_getfield(state, meta1, "_identity");
    auto type1 = (type_identity*)lua_touserdata(state, -1);
    lua_pop(state, 1);

    return is_type_compatible(state, type1, meta1, meta2, exact_equal);
}

/**
 * Verify that the value matches the identity, and return ptr if so.
 */
static void *get_object_internal(lua_State *state, type_identity *type, int val_index, bool exact_type, bool in_method)
{
    /*
     * Non-userdata results in NULL; nil for NULL gets handled here too.
     */
    if (!lua_isuserdata(state, val_index))
        return NULL;

    /*
     * Light user data is allowed with null type; otherwise bail out.
     */
    if (!lua_getmetatable(state, val_index)) // () -> metatable?
    {
        if (!type && lua_islightuserdata(state, val_index))
            return lua_touserdata(state, val_index);

        return NULL;
    }

    /*
     * Verify that the metatable is known, and refers to the correct type.
     * Here doing reverse lookup of identity by metatable.
     */
    if (!LookupTypeInfo(state, in_method)) // metatable -> type?
        return NULL;

    if (type && lua_touserdata(state, -1) != type)
    {
        /*
         * If valid but different type, do an intelligent comparison.
         */
        lua_pop(state, 1); // type -> ()
        lua_getmetatable(state, val_index);

        if (!is_type_compatible(state, type, 0, lua_gettop(state), exact_type))
        {
            lua_pop(state, 1); // metatable -> ()
            return NULL;
        }
    }

    lua_pop(state, 1); // type -> ()

    /*
     * Finally decode the reference.
     */
    return get_object_ref(state, val_index);
}

/**
 * Check if the object and metatable are a valid DF reference or type.
 */
static bool is_valid_metatable(lua_State *state, int objidx, int metaidx)
{
    // Verify object type validity
    if (lua_isuserdata(state, objidx))
    {
        lua_pushvalue(state, metaidx);
        lua_rawget(state, UPVAL_TYPETABLE);
    }
    else
    {
        lua_pushvalue(state, objidx);
        LookupInTable(state, DFHACK_TYPEID_TABLE_NAME);
    }

    bool ok = !lua_isnil(state, -1);
    lua_pop(state, 1);
    return ok;
}

/**
 * Given a DF object reference or type, safely retrieve its identity pointer.
 */
static type_identity *get_object_identity(lua_State *state, int objidx,
                                          const char *ctx, bool allow_type = false,
                                          bool keep_metatable = false)
{
    if (!lua_getmetatable(state, objidx))
        luaL_error(state, "Invalid object in %s", ctx);

    if (!allow_type && !lua_isuserdata(state, objidx))
        luaL_error(state, "Object expected in %s", ctx);

    if (!is_valid_metatable(state, objidx, -1))
        luaL_error(state, "Invalid object metatable in %s", ctx);

    // Extract identity from metatable
    lua_getfield(state, -1, "_identity");

    type_identity *id = (type_identity*)lua_touserdata(state, -1);
    if (!id)
        luaL_error(state, "Invalid object identity in %s", ctx);

    lua_pop(state, keep_metatable ? 1 : 2);
    return id;
}

static void check_type_compatible(lua_State *state, int obj1, int obj2,
                                  type_identity **type1, type_identity **type2,
                                  const char *ctx, bool allow_type, bool exact)
{
    int base = lua_gettop(state);

    *type1 = get_object_identity(state, obj1, ctx, allow_type, true);
    *type2 = get_object_identity(state, obj2, ctx, allow_type, true);

    if (!is_type_compatible(state, *type1, base+1, *type2, base+2, exact))
    {
        lua_getfield(state, base+1, "__metatable");
        const char *cname1 = lua_tostring(state, -1);
        lua_getfield(state, base+2, "__metatable");
        const char *cname2 = lua_tostring(state, -1);

        luaL_error(state, "Types %s and %s incompatible in %s", cname1, cname2, ctx);
    }

    lua_pop(state, 2);
}

/**
 * Metamethod: compare two DF object references.
 *
 * Equal if same pointer and same metatable.
 */
static int meta_ptr_compare(lua_State *state)
{
    if (!lua_isuserdata(state, 1) || !lua_isuserdata(state, 2) ||
        !lua_getmetatable(state, 1) || !lua_getmetatable(state, 2) ||
        get_object_ref(state, 1) != get_object_ref(state, 2) ||
        !is_type_compatible(state, 3, 4, true))
    {
        lua_pushboolean(state, false);
        return 1;
    }

    lua_pushboolean(state, true);
    return 1;
}

/**
 * Method: sizeof for DF object references.
 *
 * Returns: size[, address]
 */
static int meta_sizeof(lua_State *state)
{
    int argc = lua_gettop(state);

    if (argc != 1)
        luaL_error(state, "Usage: object:sizeof() or df.sizeof(object)");

    // Two special cases: nil and lightuserdata for NULL and void*
    if (lua_isnil(state, 1))
    {
        lua_pushnil(state);
        lua_pushinteger(state, 0);
        return 2;
    }

    if (lua_islightuserdata(state, 1))
    {
        lua_pushnil(state);
        lua_pushnumber(state, (size_t)lua_touserdata(state, 1));
        return 2;
    }

    type_identity *id = get_object_identity(state, 1, "df.sizeof()", true);

    lua_pushinteger(state, id->byte_size());

    if (lua_isuserdata(state, 1))
    {
        lua_pushnumber(state, (size_t)get_object_ref(state, 1));
        return 2;
    }
    else
        return 1;
}

/**
 * Method: displace for DF object references.
 *
 * Returns: a reference with the same type, but modified address
 */
static int meta_displace(lua_State *state)
{
    int argc = lua_gettop(state);

    bool has_step = (argc >= 3);
    if ((argc < 2 || argc > 3) ||
        !lua_isnumber(state, 2) ||
        (has_step && !lua_isnumber(state, 3)))
    {
        luaL_error(state, "Usage: object:_displace(index[,step]) or df._displace(object,...)");
    }

    int index = lua_tointeger(state, 2);
    int step = has_step ? lua_tointeger(state, 3) : 1;

    // Two special cases: nil and lightuserdata for NULL and void*
    if (lua_isnil(state, 1))
    {
        lua_pushnil(state);
        return 1;
    }

    if (lua_islightuserdata(state, 1))
    {
        if (!has_step)
            luaL_error(state, "Step is mandatory in _displace of void*");

        auto ptr = (uint8_t*)lua_touserdata(state, 1);
        lua_pushlightuserdata(state, ptr + index*step);
        return 1;
    }

    type_identity *id = get_object_identity(state, 1, "df._displace()");

    if (!has_step)
        step = id->byte_size();

    if (index == 0 || step == 0)
    {
        lua_pushvalue(state, 1);
    }
    else
    {
        auto ptr = (uint8_t*)get_object_ref(state, 1);
        lua_getmetatable(state, 1);
        push_object_ref(state, ptr + index*step);
    }

    return 1;
}

/**
 * Method: allocation for DF object references.
 */
static int meta_new(lua_State *state)
{
    int argc = lua_gettop(state);

    if (argc != 1)
        luaL_error(state, "Usage: object:new() or df.new(object)");

    type_identity *id = get_object_identity(state, 1, "df.new()", true);

    void *ptr = id->allocate();
    if (!ptr)
        return 0;

    if (lua_isuserdata(state, 1))
    {
        lua_getmetatable(state, 1);
        push_object_ref(state, ptr);

        id->copy(ptr, get_object_ref(state, 1));
    }
    else
        push_object_internal(state, id, ptr);

    return 1;
}

/**
 * Method: assign data between objects.
 */
static int meta_assign(lua_State *state)
{
    int argc = lua_gettop(state);

    if (argc != 2)
        luaL_error(state, "Usage: target:assign(src) or df.assign(target,src)");

    type_identity *id1, *id2;
    check_type_compatible(state, 1, 2, &id1, &id2, "df.assign()", false, false);

    if (!id1->copy(get_object_ref(state, 1), get_object_ref(state, 2)))
        luaL_error(state, "No copy support for %s", id1->getFullName().c_str());

    return 0;
}

/**
 * Resolve the field name in UPVAL_FIELDTABLE, die if not found.
 */
static void lookup_field(lua_State *state, int index, const char *mode)
{
    lua_pushvalue(state, index);
    lua_gettable(state, UPVAL_FIELDTABLE); // uses metatable with enum keys

    if (lua_isnil(state, -1))
        field_error(state, index, "not found", mode);
}

static void *find_field(lua_State *state, int index, const char *mode)
{
    lookup_field(state, index, mode);

    void *p = lua_touserdata(state, -1);
    lua_pop(state, 1);
    return p;
}

/**
 * Verify that the object is a DF ref with UPVAL_METATABLE.
 * If everything ok, extract the address.
 */
static uint8_t *get_object_addr(lua_State *state, int obj, int field, const char *mode)
{
    if (!lua_isuserdata(state, obj) ||
        !lua_getmetatable(state, obj))
        field_error(state, field, "invalid object", mode);

    if (!lua_rawequal(state, -1, UPVAL_METATABLE))
        field_error(state, field, "invalid object metatable", mode);

    lua_pop(state, 1);

    return (uint8_t*)get_object_ref(state, obj);
}

static void GetAdHocMetatable(lua_State *state, const struct_field_info *field);

static void read_field(lua_State *state, const struct_field_info *field, void *ptr)
{
    switch (field->mode)
    {
        case struct_field_info::STATIC_STRING:
        {
            int len = strnlen((char*)ptr, field->count);
            lua_pushlstring(state, (char*)ptr, len);
            return;
        }

        case struct_field_info::PRIMITIVE:
        case struct_field_info::SUBSTRUCT:
            field->type->lua_read(state, 2, ptr);
            return;

        case struct_field_info::POINTER:
            df::pointer_identity::lua_read(state, 2, ptr, field->type);
            return;

        case struct_field_info::CONTAINER:
            if (!field->eid || !field->type->isContainer() ||
                field->eid == ((container_identity*)field->type)->getIndexEnumType())
            {
                field->type->lua_read(state, 2, ptr);
                return;
            }
            // fallthrough

        case struct_field_info::STATIC_ARRAY:
        case struct_field_info::STL_VECTOR_PTR:
            GetAdHocMetatable(state, field);
            push_object_ref(state, ptr);
            return;

        case struct_field_info::END:
            break;
    }

    lua_pushnil(state);
}

static void field_reference(lua_State *state, const struct_field_info *field, void *ptr)
{
    switch (field->mode)
    {
        case struct_field_info::PRIMITIVE:
        case struct_field_info::SUBSTRUCT:
            push_object_internal(state, field->type, ptr);
            return;

        case struct_field_info::POINTER:
            push_adhoc_pointer(state, ptr, field->type);
            return;

        case struct_field_info::CONTAINER:
            read_field(state, field, ptr);
            return;

        case struct_field_info::STATIC_STRING:
        case struct_field_info::STATIC_ARRAY:
        case struct_field_info::STL_VECTOR_PTR:
            GetAdHocMetatable(state, field);
            push_object_ref(state, ptr);
            return;

        case struct_field_info::END:
            break;
    }

    lua_pushnil(state);
}

static void write_field(lua_State *state, const struct_field_info *field, void *ptr, int value_idx)
{
    switch (field->mode)
    {
        case struct_field_info::STATIC_STRING:
        {
            size_t size;
            const char *str = lua_tolstring(state, value_idx, &size);
            if (!str)
                field_error(state, 2, "string expected", "write");
            memcpy(ptr, str, std::min(size+1, size_t(field->count)));
            return;
        }

        case struct_field_info::PRIMITIVE:
        case struct_field_info::SUBSTRUCT:
        case struct_field_info::CONTAINER:
            field->type->lua_write(state, 2, ptr, value_idx);
            return;

        case struct_field_info::POINTER:
            df::pointer_identity::lua_write(state, 2, ptr, field->type, value_idx);

        case struct_field_info::STATIC_ARRAY:
        case struct_field_info::STL_VECTOR_PTR:
            field_error(state, 2, "complex object", "write");

        case struct_field_info::END:
            return;
    }
}

/**
 * Metamethod: represent a type node as string.
 */
static int meta_type_tostring(lua_State *state)
{
    if (!lua_getmetatable(state, 1))
        return 0;

    lua_getfield(state, -1, "__metatable");
    const char *cname = lua_tostring(state, -1);

    lua_pushstring(state, stl_sprintf("<type: %s>", cname).c_str());
    return 1;
}

/**
 * Metamethod: represent a DF object reference as string.
 */
static int meta_ptr_tostring(lua_State *state)
{
    uint8_t *ptr = get_object_addr(state, 1, 0, "access");

    lua_getfield(state, UPVAL_METATABLE, "__metatable");
    const char *cname = lua_tostring(state, -1);

    lua_pushstring(state, stl_sprintf("<%s: 0x%08x>", cname, (unsigned)ptr).c_str());
    return 1;
}

// Resolve the field in the metatable and return
static int get_metafield(lua_State *state)
{
    lua_rawget(state, UPVAL_METATABLE);
    return 1;
}

/**
 * Metamethod: __index for structures.
 */
static int meta_struct_index(lua_State *state)
{
    uint8_t *ptr = get_object_addr(state, 1, 2, "read");
    auto field = (struct_field_info*)find_field(state, 2, "read");
    if (!field)
        return get_metafield(state);
    read_field(state, field, ptr + field->offset);
    return 1;
}

/**
 * Method: _field for structures.
 */
static int meta_struct_field_reference(lua_State *state)
{
    if (lua_gettop(state) != 2)
        luaL_error(state, "Usage: object._field(name)");
    uint8_t *ptr = get_object_addr(state, 1, 2, "reference");
    auto field = (struct_field_info*)find_field(state, 2, "reference");
    if (!field)
        field_error(state, 2, "builtin property", "reference");
    field_reference(state, field, ptr + field->offset);
    return 1;
}

/**
 * Metamethod: __newindex for structures.
 */
static int meta_struct_newindex(lua_State *state)
{
    uint8_t *ptr = get_object_addr(state, 1, 2, "write");
    auto field = (struct_field_info*)find_field(state, 2, "write");
    if (!field)
        field_error(state, 2, "builtin property", "write");
    write_field(state, field, ptr + field->offset, 3);
    return 0;
}

/**
 * Metamethod: __index for primitives, i.e. simple object references.
 *   Fields point to identity, or NULL for metafields.
 */
static int meta_primitive_index(lua_State *state)
{
    uint8_t *ptr = get_object_addr(state, 1, 2, "read");
    auto type = (type_identity*)find_field(state, 2, "read");
    if (!type)
        return get_metafield(state);
    type->lua_read(state, 2, ptr);
    return 1;
}

/**
 * Metamethod: __newindex for primitives.
 */
static int meta_primitive_newindex(lua_State *state)
{
    uint8_t *ptr = get_object_addr(state, 1, 2, "write");
    auto type = (type_identity*)find_field(state, 2, "write");
    if (!type)
        field_error(state, 2, "builtin property", "write");
    type->lua_write(state, 2, ptr, 3);
    return 0;
}

/**
 * Metamethod: __len for containers.
 */
static int meta_container_len(lua_State *state)
{
    uint8_t *ptr = get_object_addr(state, 1, 0, "get length");
    auto id = (container_identity*)lua_touserdata(state, UPVAL_CONTAINER_ID);
    int len = id->lua_item_count(state, ptr);
    lua_pushinteger(state, len);
    return 1;
}

/**
 * Field lookup for containers:
 *
 *   - Numbers are indices and handled directly.
 *   - NULL userdata are metafields; push and exit;
 */
static int lookup_container_field(lua_State *state, int field, const char *mode = NULL)
{
    if (lua_type(state, field) == LUA_TNUMBER)
        return field;

    lookup_field(state, field, mode ? mode : "read");

    if (lua_isuserdata(state, -1) && !lua_touserdata(state, -1))
    {
        if (mode)
            field_error(state, field, "builtin property", mode);

        lua_pop(state, 1);
        get_metafield(state);
        return 0;
    }

    return -1;
}

/**
 * Index verification: number and in range.
 */
static int check_container_index(lua_State *state, int len,
                                 int fidx, int iidx, const char *mode)
{
    if (!lua_isnumber(state, iidx))
        field_error(state, fidx, "invalid index", mode);

    int idx = lua_tointeger(state, iidx);
    if (idx < 0 || idx >= len)
        field_error(state, fidx, "index out of bounds", mode);

    return idx;
}

/**
 * Metamethod: __index for containers.
 */
static int meta_container_index(lua_State *state)
{
    uint8_t *ptr = get_object_addr(state, 1, 2, "read");
    int iidx = lookup_container_field(state, 2);
    if (!iidx)
        return 1;

    auto id = (container_identity*)lua_touserdata(state, UPVAL_CONTAINER_ID);
    int len = id->lua_item_count(state, ptr);
    int idx = check_container_index(state, len, 2, iidx, "read");
    id->lua_item_read(state, 2, ptr, idx);
    return 1;
}

/**
 * Method: _field for containers.
 */
static int meta_container_field_reference(lua_State *state)
{
    if (lua_gettop(state) != 2)
        luaL_error(state, "Usage: object._field(index)");
    uint8_t *ptr = get_object_addr(state, 1, 2, "reference");
    int iidx = lookup_container_field(state, 2, "reference");

    auto id = (container_identity*)lua_touserdata(state, UPVAL_CONTAINER_ID);
    int len = id->lua_item_count(state, ptr);
    int idx = check_container_index(state, len, 2, iidx, "reference");
    id->lua_item_reference(state, 2, ptr, idx);
    return 1;
}

/**
 * Metamethod: __index for containers.
 */
static int meta_container_newindex(lua_State *state)
{
    uint8_t *ptr = get_object_addr(state, 1, 2, "write");
    int iidx = lookup_container_field(state, 2, "write");

    auto id = (container_identity*)lua_touserdata(state, UPVAL_CONTAINER_ID);
    int len = id->lua_item_count(state, ptr);
    int idx = check_container_index(state, len, 2, iidx, "write");
    id->lua_item_write(state, 2, ptr, idx, 3);
    return 0;
}

/**
 * Metamethod: __len for bitfields.
 */
static int meta_bitfield_len(lua_State *state)
{
    uint8_t *ptr = get_object_addr(state, 1, 0, "get size");
    auto id = (bitfield_identity*)lua_touserdata(state, UPVAL_CONTAINER_ID);
    lua_pushinteger(state, id->getNumBits());
    return 1;
}

/**
 * Metamethod: __index for bitfields.
 */
static int meta_bitfield_index(lua_State *state)
{
    uint8_t *ptr = get_object_addr(state, 1, 2, "read");
    int iidx = lookup_container_field(state, 2);
    if (!iidx)
        return 1;

    auto id = (bitfield_identity*)lua_touserdata(state, UPVAL_CONTAINER_ID);

    // whole
    if (lua_isuserdata(state, iidx) && lua_touserdata(state, iidx) == id)
    {
        size_t intv = 0;
        memcpy(&intv, ptr, std::min(sizeof(intv), size_t(id->byte_size())));
        lua_pushnumber(state, intv);
        return 1;
    }

    int idx = check_container_index(state, id->getNumBits(), 2, iidx, "read");
    int size = id->getBits()[idx].size;

    int value = getBitfieldField(ptr, idx, size);
    if (size <= 1)
        lua_pushboolean(state, value != 0);
    else
        lua_pushinteger(state, value);
    return 1;
}

/**
 * Metamethod: __newindex for bitfields.
 */
static int meta_bitfield_newindex(lua_State *state)
{
    uint8_t *ptr = get_object_addr(state, 1, 2, "write");
    int iidx = lookup_container_field(state, 2, "write");

    auto id = (bitfield_identity*)lua_touserdata(state, UPVAL_CONTAINER_ID);

    // whole
    if (lua_isuserdata(state, iidx) && lua_touserdata(state, iidx) == id)
    {
        if (!lua_isnumber(state, 3))
            field_error(state, 2, "number expected", "write");

        size_t intv = (size_t)lua_tonumber(state, 3);
        memcpy(ptr, &intv, std::min(sizeof(intv), size_t(id->byte_size())));
        return 0;
    }

    int idx = check_container_index(state, id->getNumBits(), 2, iidx, "write");
    int size = id->getBits()[idx].size;

    if (lua_isboolean(state, 3) || lua_isnil(state, 3))
        setBitfieldField(ptr, idx, size, lua_toboolean(state, 3));
    else if (lua_isnumber(state, 3))
        setBitfieldField(ptr, idx, size, lua_tointeger(state, 3));
    else
        field_error(state, 2, "number or boolean expected", "write");
    return 0;
}

/**
 * Metamethod: __index for df.global
 */
static int meta_global_index(lua_State *state)
{
    auto field = (struct_field_info*)find_field(state, 2, "read");
    if (!field)
        return get_metafield(state);
    void *ptr = *(void**)field->offset;
    if (!ptr)
        field_error(state, 2, "global address not known", "read");
    read_field(state, field, ptr);
    return 1;
}

/**
 * Metamethod: __newindex for df.global
 */
static int meta_global_newindex(lua_State *state)
{
    auto field = (struct_field_info*)find_field(state, 2, "write");
    if (!field)
        field_error(state, 2, "builtin property", "write");
    void *ptr = *(void**)field->offset;
    if (!ptr)
        field_error(state, 2, "global address not known", "write");
    write_field(state, field, ptr, 3);
    return 0;
}

/**
 * Add fields in the array to the UPVAL_FIELDTABLE candidates on the stack.
 */
static void IndexFields(lua_State *state, struct_identity *pstruct)
{
    // stack: fieldtable

    int base = lua_gettop(state);

    for (struct_identity *p = pstruct; p; p = p->getParent())
    {
        auto fields = p->getFields();

        for (; fields; ++fields)
        {
            if (fields->mode == struct_field_info::END)
                break;

            lua_pushstring(state,fields->name);
            lua_pushlightuserdata(state,(void*)fields);
            lua_rawset(state,base);
        }
    }
}

/**
 * Enable a metafield by injecting an entry into a UPVAL_FIELDTABLE.
 */
static void EnableMetaField(lua_State *state, int ftable_idx, const char *name, void *id = NULL)
{
    lua_pushlightuserdata(state, id);
    lua_setfield(state, ftable_idx, name);
}

/**
 * Set metatable properties common to all actual DF object references.
 */
static void SetPtrMethods(lua_State *state, int meta_idx, int read_idx)
{
    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_COMPARE_NAME);
    lua_setfield(state, meta_idx, "__eq");

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPETABLE_NAME);
    lua_pushvalue(state, meta_idx);
    lua_pushcclosure(state, meta_ptr_tostring, 2);
    lua_setfield(state, meta_idx, "__tostring");

    EnableMetaField(state, read_idx, "_type");
    EnableMetaField(state, read_idx, "_kind");

    EnableMetaField(state, read_idx, "_field");

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_SIZEOF_NAME);
    lua_setfield(state, meta_idx, "sizeof");
    EnableMetaField(state, read_idx, "sizeof");

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_NEW_NAME);
    lua_setfield(state, meta_idx, "new");
    EnableMetaField(state, read_idx, "new");

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_ASSIGN_NAME);
    lua_setfield(state, meta_idx, "assign");
    EnableMetaField(state, read_idx, "assign");

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_DISPLACE_NAME);
    lua_setfield(state, meta_idx, "_displace");
    EnableMetaField(state, read_idx, "_displace");
}

/**
 * Add a struct-style (3 upvalues) metamethod to the metatable.
 */
static void SetStructMethod(lua_State *state, int meta_idx, int ftable_idx,
                            lua_CFunction function, const char *name)
{
    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPETABLE_NAME);
    lua_pushvalue(state, meta_idx);
    lua_pushvalue(state, ftable_idx);
    lua_pushcclosure(state, function, 3);
    lua_setfield(state, meta_idx, name);
}

/**
 * Make a metatable with most common fields, and an empty table for UPVAL_FIELDTABLE.
 */
static void MakeMetatable(lua_State *state, type_identity *type, const char *kind)
{
    int base = lua_gettop(state);
    lua_newtable(state); // metatable

    lua_pushstring(state, type->getFullName().c_str());
    lua_setfield(state, base+1, "__metatable");

    lua_pushlightuserdata(state, type);
    lua_setfield(state, base+1, "_identity");

    LookupInTable(state, type, DFHACK_TYPEID_TABLE_NAME);
    if (lua_isnil(state, -1))
    {
        // Copy the string from __metatable if no real type
        lua_pop(state, 1);
        lua_getfield(state, base+1, "__metatable");
    }
    lua_setfield(state, base+1, "_type");

    lua_pushstring(state, kind);
    lua_setfield(state, base+1, "_kind");

    lua_newtable(state); // fieldtable
}

/**
 * Make a struct-style object metatable.
 */
static void MakeFieldMetatable(lua_State *state, struct_identity *pstruct,
                               lua_CFunction reader, lua_CFunction writer)
{
    int base = lua_gettop(state);

    MakeMetatable(state, pstruct, "struct"); // meta, fields

    IndexFields(state, pstruct);

    SetStructMethod(state, base+1, base+2, reader, "__index");
    SetStructMethod(state, base+1, base+2, writer, "__newindex");

    // returns: [metatable readfields writefields];
}

/**
 * Make a primitive-style metatable
 */
static void MakePrimitiveMetatable(lua_State *state, type_identity *type)
{
    int base = lua_gettop(state);

    MakeMetatable(state, type, "primitive");

    SetPtrMethods(state, base+1, base+2);

    EnableMetaField(state, base+2, "value", type);

    SetStructMethod(state, base+1, base+2, meta_primitive_index, "__index");
    SetStructMethod(state, base+1, base+2, meta_primitive_newindex, "__newindex");
}

/**
 * Add a 6 upvalue metamethod to the metatable.
 */
static void SetContainerMethod(lua_State *state, int meta_idx, int ftable_idx,
                               lua_CFunction function, const char *name,
                               type_identity *container, type_identity *item, int count)
{
    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPETABLE_NAME);
    lua_pushvalue(state, meta_idx);
    lua_pushvalue(state, ftable_idx);

    lua_pushlightuserdata(state, container);
    lua_pushlightuserdata(state, item);
    if (count < 0)
        lua_pushnil(state);
    else
        lua_pushinteger(state, count);

    lua_pushcclosure(state, function, 6);
    lua_setfield(state, meta_idx, name);
}

/**
 * If ienum refers to a valid enum, attach its keys to UPVAL_FIELDTABLE,
 * and the enum itself to the _enum metafield.
 */
static void AttachEnumKeys(lua_State *state, int base, type_identity *ienum)
{
    LookupInTable(state, ienum, DFHACK_ENUM_TABLE_NAME);

    if (!lua_isnil(state, -1))
    {
        lua_newtable(state);
        lua_swap(state);
        lua_setfield(state, -2, "__index");
        lua_setmetatable(state, base+2);
    }
    else
        lua_pop(state, 1);

    LookupInTable(state, ienum, DFHACK_TYPEID_TABLE_NAME);
    lua_setfield(state, base+1, "_enum");

    EnableMetaField(state, base+2, "_enum");
}

/**
 * Make a container-style object metatable.
 */
static void MakeContainerMetatable(lua_State *state, container_identity *type,
                                   type_identity *item, int count, type_identity *ienum)
{
    int base = lua_gettop(state);

    MakeMetatable(state, type, "container");
    SetPtrMethods(state, base+1, base+2);

    // Update the type name using full info
    lua_pushstring(state, type->getFullName(item).c_str());
    lua_dup(state);
    lua_setfield(state, base+1, "__metatable");
    lua_setfield(state, base+1, "_type");

    lua_pushlightuserdata(state, item);
    lua_setfield(state, base+1, "_field_identity");

    if (count >= 0)
    {
        lua_pushinteger(state, count);
        lua_setfield(state, base+1, "_count");
    }

    SetContainerMethod(state, base+1, base+2, meta_container_len, "__len", type, item, count);
    SetContainerMethod(state, base+1, base+2, meta_container_index, "__index", type, item, count);
    SetContainerMethod(state, base+1, base+2, meta_container_newindex, "__newindex", type, item, count);

    SetContainerMethod(state, base+1, base+2, meta_container_field_reference, "_field", type, item, count);

    AttachEnumKeys(state, base, ienum);
}

/*
 * Metatable construction identity methods.
 */
void type_identity::build_metatable(lua_State *state)
{
    MakePrimitiveMetatable(state, this);
}

void container_identity::build_metatable(lua_State *state)
{
    MakeContainerMetatable(state, this, getItemType(), -1, getIndexEnumType());
}

void bitfield_identity::build_metatable(lua_State *state)
{
    int base = lua_gettop(state);

    MakeMetatable(state, this, "bitfield");

    SetPtrMethods(state, base+1, base+2);

    SetContainerMethod(state, base+1, base+2, meta_bitfield_len, "__len", this, NULL, -1);
    SetContainerMethod(state, base+1, base+2, meta_bitfield_index, "__index", this, NULL, -1);
    SetContainerMethod(state, base+1, base+2, meta_bitfield_newindex, "__newindex", this, NULL, -1);

    AttachEnumKeys(state, base, this);

    EnableMetaField(state, base+2, "whole", this);
}

void struct_identity::build_metatable(lua_State *state)
{
    int base = lua_gettop(state);
    MakeFieldMetatable(state, this, meta_struct_index, meta_struct_newindex);
    SetStructMethod(state, base+1, base+2, meta_struct_field_reference, "_field");
    SetPtrMethods(state, base+1, base+2);
}

void global_identity::build_metatable(lua_State *state)
{
    MakeFieldMetatable(state, this, meta_global_index, meta_global_newindex);
}

static void BuildTypeMetatable(lua_State *state, type_identity *type)
{
    type->build_metatable(state);

    lua_pop(state, 1);

    SaveTypeInfo(state, type);
}

/**
 * Construct a metatable for an object type folded into the field descriptor.
 * This is done to reduce compile-time symbol table bloat due to templates.
 */
static void GetAdHocMetatable(lua_State *state, const struct_field_info *field)
{
    lua_pushlightuserdata(state, (void*)field);

    if (!LookupTypeInfo(state, true))
    {
        switch (field->mode)
        {
        case struct_field_info::CONTAINER:
        {
            auto ctype = (container_identity*)field->type;
            MakeContainerMetatable(state, ctype, ctype->getItemType(), -1, field->eid);
            break;
        }

        case struct_field_info::STATIC_STRING:
            MakeContainerMetatable(state, &df::buffer_container_identity::base_instance,
                                   &df::identity_traits<char>::identity, field->count, NULL);
            break;

        case struct_field_info::STATIC_ARRAY:
            MakeContainerMetatable(state, &df::buffer_container_identity::base_instance,
                                   field->type, field->count, field->eid);
            break;

        case struct_field_info::STL_VECTOR_PTR:
            MakeContainerMetatable(state, &df::identity_traits<std::vector<void*> >::identity,
                                   field->type, -1, field->eid);
            break;

        default:
            luaL_error(state, "Invalid ad-hoc field: %d", field->mode);
        }

        lua_pop(state, 1);

        SaveTypeInfo(state, (void*)field);
    }
}

static void push_adhoc_pointer(lua_State *state, void *ptr, type_identity *target)
{
    if (!target)
    {
        push_object_internal(state, &df::identity_traits<void*>::identity, ptr);
        return;
    }

    LookupInTable(state, target, DFHACK_PTR_IDTABLE_NAME);

    type_identity *id = (type_identity*)lua_touserdata(state, -1);
    lua_pop(state, 1);

    if (!id)
    {
        /*
         * HACK: relies on
         *   1) pointer_identity destructor being no-op
         *   2) lua gc never moving objects in memory
         */

        void *newobj = lua_newuserdata(state, sizeof(pointer_identity));
        id = new (newobj) pointer_identity(target);

        SaveInTable(state, target, DFHACK_PTR_IDTABLE_NAME);
        lua_pop(state, 1);
    }

    push_object_internal(state, id, ptr);
}

/*
 * Recursive walk of scopes to construct the df... tree.
 */

static void RenderTypeChildren(lua_State *state, const std::vector<compound_identity*> &children);

static void AssociateId(lua_State *state, int table, int val, const char *name)
{
    lua_pushinteger(state, val);
    lua_pushstring(state, name);
    lua_dup(state);
    lua_pushinteger(state, val);

    lua_rawset(state, table);
    lua_rawset(state, table);
}

static void RenderType(lua_State *state, compound_identity *node)
{
    assert(node->getName());
    std::string name = node->getFullName();

    lua_newtable(state);
    if (!lua_checkstack(state, 20))
        return;

    int base = lua_gettop(state);

    switch (node->type())
    {
    case IDTYPE_STRUCT:
        lua_pushstring(state, "struct-type");
        lua_setfield(state, base, "_kind");
        break;

    case IDTYPE_CLASS:
        lua_pushstring(state, "class-type");
        lua_setfield(state, base, "_kind");
        break;

    case IDTYPE_ENUM:
        {
            lua_pushstring(state, "enum-type");
            lua_setfield(state, base, "_kind");

            enum_identity *eid = (enum_identity*)node;
            const char *const *keys = eid->getKeys();

            // For enums, set mapping between keys and values
            for (int64_t i = eid->getFirstItem(), j = 0; i <= eid->getLastItem(); i++, j++)
            {
                if (keys[j])
                    AssociateId(state, base, i, keys[j]);
            }

            if (eid->getFirstItem() <= eid->getLastItem())
            {
                lua_pushinteger(state, eid->getFirstItem());
                lua_setfield(state, base, "_first_item");

                lua_pushinteger(state, eid->getLastItem());
                lua_setfield(state, base, "_last_item");
            }

            SaveInTable(state, node, DFHACK_ENUM_TABLE_NAME);
        }
        break;

    case IDTYPE_BITFIELD:
        {
            lua_pushstring(state, "bitfield-type");
            lua_setfield(state, base, "_kind");

            bitfield_identity *eid = (bitfield_identity*)node;
            auto bits = eid->getBits();

            for (int i = 0; i < eid->getNumBits(); i++)
            {
                if (bits[i].name)
                    AssociateId(state, base, i, bits[i].name);
                if (bits[i].size > 1)
                    i += bits[i].size-1;
            }

            lua_pushinteger(state, 0);
            lua_setfield(state, base, "_first_item");

            lua_pushinteger(state, eid->getNumBits()-1);
            lua_setfield(state, base, "_last_item");

            SaveInTable(state, node, DFHACK_ENUM_TABLE_NAME);
        }
        break;

    default:
        break;
    }

    RenderTypeChildren(state, node->getScopeChildren());

    assert(base == lua_gettop(state));

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_SIZEOF_NAME);
    lua_setfield(state, base, "sizeof");
    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_NEW_NAME);
    lua_setfield(state, base, "new");

    if (node->type() == IDTYPE_GLOBAL)
    {
        BuildTypeMetatable(state, node);

        // Set metatable for the inner table
        lua_dup(state);
        lua_setmetatable(state, base);
        lua_swap(state); // -> meta curtable

        freeze_table(state, true, "global");

        // Copy __newindex to the outer metatable
        lua_getfield(state, base, "__newindex");
        lua_setfield(state, -2, "__newindex");

        lua_remove(state, base);
    }
    else
    {
        freeze_table(state, true, name.c_str());
    }

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPE_TOSTRING_NAME);
    lua_setfield(state, -2, "__tostring");

    lua_pushlightuserdata(state, node);
    lua_setfield(state, -2, "_identity");

    lua_pop(state, 1);

    SaveInTable(state, node, DFHACK_TYPEID_TABLE_NAME);
}

static void RenderTypeChildren(lua_State *state, const std::vector<compound_identity*> &children)
{
    for (size_t i = 0; i < children.size(); i++)
    {
        RenderType(state, children[i]);
        lua_pushstring(state, children[i]->getName());
        lua_swap(state);
        lua_rawset(state, -3);
    }
}

static void DoAttach(lua_State *state)
{
    int base = lua_gettop(state);

    lua_newtable(state);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_PTR_IDTABLE_NAME);

    lua_newtable(state);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_TYPEID_TABLE_NAME);

    lua_newtable(state);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_ENUM_TABLE_NAME);

    lua_pushcfunction(state, change_error);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_CHANGEERROR_NAME);

    lua_pushcfunction(state, meta_ptr_compare);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_COMPARE_NAME);

    lua_pushcfunction(state, meta_type_tostring);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_TYPE_TOSTRING_NAME);

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPETABLE_NAME);
    lua_pushcclosure(state, meta_sizeof, 1);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_SIZEOF_NAME);

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPETABLE_NAME);
    lua_pushcclosure(state, meta_displace, 1);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_DISPLACE_NAME);

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPETABLE_NAME);
    lua_pushcclosure(state, meta_new, 1);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_NEW_NAME);

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPETABLE_NAME);
    lua_pushcclosure(state, meta_assign, 1);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_ASSIGN_NAME);

    luaL_register(state, "df", no_functions);

    {
        // Assign df a metatable with read-only contents
        lua_newtable(state);

        // Render the type structure
        RenderTypeChildren(state, compound_identity::getTopScope());

        lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_SIZEOF_NAME);
        lua_setfield(state, -2, "sizeof");
        lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_NEW_NAME);
        lua_setfield(state, -2, "new");
        lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_DISPLACE_NAME);
        lua_setfield(state, -2, "_displace");
        lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_ASSIGN_NAME);
        lua_setfield(state, -2, "assign");

        freeze_table(state, true, "df");
        lua_remove(state, -2);
        lua_setmetatable(state, -2);
    }

    lua_pop(state, 1);
}

/**
 * Initialize access to DF objects from the interpreter
 * context, unless it has already been done.
 */
void DFHack::AttachDFGlobals(lua_State *state)
{
    if (luaL_newmetatable(state, DFHACK_TYPETABLE_NAME))
        DoAttach(state);

    lua_pop(state, 1);
}