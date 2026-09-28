// The Lua C API lua_api.hpp names, as inert stubs: the harness drives the API's state directly (api_ops.inc) and
// never enters a Lua function, so these only have to compile.
#pragma once

struct lua_State;
using lua_CFunction = int (*)(lua_State*);
using lua_Number = double;
using lua_Integer = long long;

#define LUA_REGISTRYINDEX (-1001000)
#define LUA_RIDX_MAINTHREAD 1
#define LUA_OK 0
#define LUA_TNIL 0
#define LUA_TNUMBER 3
#define LUA_TSTRING 4

inline int lua_rawgeti(lua_State*, int, lua_Integer) { return 0; }
inline lua_State* lua_tothread(lua_State*, int) { return nullptr; }
inline void lua_settop(lua_State*, int) {}
#define lua_pop(L, n) lua_settop(L, -(n) - 1)
inline void lua_pushnil(lua_State*) {}
inline const char* lua_pushstring(lua_State*, const char* s) { return s; }
inline void lua_createtable(lua_State*, int, int) {}
#define lua_newtable(L) lua_createtable(L, 0, 0)
inline void lua_pushnumber(lua_State*, lua_Number) {}
inline void lua_pushinteger(lua_State*, lua_Integer) {}
inline void lua_pushboolean(lua_State*, int) {}
inline void lua_setfield(lua_State*, int, const char*) {}
inline int lua_getfield(lua_State*, int, const char*) { return LUA_TNIL; }
inline int lua_type(lua_State*, int) { return LUA_TNIL; }
#define lua_isnil(L, n) (lua_type(L, (n)) == LUA_TNIL)
#define lua_isnoneornil(L, n) (lua_type(L, (n)) <= 0)
#define lua_istable(L, n) (lua_type(L, (n)) == 5)
inline lua_Number lua_tonumber(lua_State*, int) { return 0; }
inline int lua_toboolean(lua_State*, int) { return 0; }
inline const char* lua_tostring(lua_State*, int) { return ""; }
inline int lua_gettop(lua_State*) { return 0; }
inline void lua_rawseti(lua_State*, int, lua_Integer) {}
inline void lua_pushcclosure(lua_State*, lua_CFunction, int) {}
#define lua_pushcfunction(L, f) lua_pushcclosure(L, (f), 0)
inline void lua_setglobal(lua_State*, const char*) {}
inline int lua_getglobal(lua_State*, const char*) { return LUA_TNIL; }
inline void lua_pushvalue(lua_State*, int) {}
inline int luaL_dostring(lua_State*, const char*) { return LUA_OK; }
