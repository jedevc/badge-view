#include <cassert>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "lua.h"
#include <lauxlib.h>
#include <lualib.h>
}

#ifdef __EMSCRIPTEN__
#include <emscripten/bind.h>
using namespace emscripten;
#endif

#define BUDGET 100000
#define BUDGET_STEPS 1000

static int rgb(lua_State *L) {
  int n = lua_gettop(L);
  if (n != 3) {
    luaL_error(L, "rgb expected 3 arguments");
    return 0;
  }

  int value = 0;
  if (lua_isinteger(L, 1) && lua_isinteger(L, 2) && lua_isinteger(L, 3)) {
    value |= (lua_tointeger(L, 1) & 0xff) << 16; // r
    value |= (lua_tointeger(L, 2) & 0xff) << 8;  // g
    value |= (lua_tointeger(L, 3) & 0xff);       // b
  } else {
    value |= ((int)round(255 * lua_tonumber(L, 1)) & 0xff) << 16; // r
    value |= ((int)round(255 * lua_tonumber(L, 2)) & 0xff) << 8;  // g
    value |= ((int)round(255 * lua_tonumber(L, 3)) & 0xff);       // b
  }
  lua_pushinteger(L, value);
  return 1;
}

static int hsl(lua_State *L) {
  int n = lua_gettop(L);
  if (n != 3) {
    luaL_error(L, "hsl expected 3 arguments");
    return 0;
  }

  double h, s, l;
  if (lua_isinteger(L, 1)) {
    h = (lua_tointeger(L, 1) % 360);
  } else {
    h = fmod(lua_tonumber(L, 1), 1) * 360;
  }
  if (lua_isinteger(L, 2)) {
    s = lua_tointeger(L, 2) / 100.0;
  } else {
    s = lua_tonumber(L, 2);
  }
  if (lua_isinteger(L, 2)) {
    l = lua_tointeger(L, 3) / 100.0;
  } else {
    l = lua_tonumber(L, 3);
  }

  // https://css-tricks.com/converting-color-spaces-in-javascript/#aa-hsl-to-rgb
  double c = (1 - fabs(2 * l - 1)) * s;
  double x = c * (1 - fabs(fmod((h / 60), 2) - 1));
  double m = l - c / 2;
  double r = 0, g = 0, b = 0;

  if (0 <= h && h < 60) {
    r = c;
    g = x;
    b = 0;
  } else if (60 <= h && h < 120) {
    r = x;
    g = c;
    b = 0;
  } else if (120 <= h && h < 180) {
    r = 0;
    g = c;
    b = x;
  } else if (180 <= h && h < 240) {
    r = 0;
    g = x;
    b = c;
  } else if (240 <= h && h < 300) {
    r = x;
    g = 0;
    b = c;
  } else if (300 <= h && h < 360) {
    r = c;
    g = 0;
    b = x;
  }

  int value = 0;
  value |= ((int)round(255 * (r + m)) & 0xff) << 16; // r
  value |= ((int)round(255 * (g + m)) & 0xff) << 8;  // g
  value |= ((int)round(255 * (b + m)) & 0xff);       // b

  lua_pushinteger(L, value);
  return 1;
}

class LuaResult {
public:
  std::string err;

  std::string title;
  std::string content;

  std::vector<unsigned int> image;
  unsigned int image_width;
  unsigned int image_height;

  unsigned int pixel(int x, int y) { return image[x * image_height + y]; }
};

class Lua {
public:
  Lua(std::string code) {
    state = luaL_newstate();

    lua_pushlightuserdata(state, (void *)state);
    lua_pushlightuserdata(state, (void *)this);
    lua_settable(state, LUA_REGISTRYINDEX);

    lua_pushlightuserdata(state, (void *)((long long)state + 1));
    luaL_loadstring(state, code.c_str());
    lua_settable(state, LUA_REGISTRYINDEX);

    lua_pushstring(state, "HackTheMidlands v7");
    lua_setglobal(state, "title");

    lua_pushstring(state, "Lorem Ipsum");
    lua_setglobal(state, "content");

    lua_pushinteger(state, image_width);
    lua_setglobal(state, "width");

    lua_pushinteger(state, image_height);
    lua_setglobal(state, "height");

    lua_pushcfunction(state, rgb);
    lua_setglobal(state, "rgb");
    lua_pushcfunction(state, hsl);
    lua_setglobal(state, "hsl");

    lua_newtable(state);
    for (int i = 0; i < image_width; i++) {
      lua_pushinteger(state, i);
      lua_newtable(state);
      for (int j = 0; j < image_height; j++) {
        lua_pushinteger(state, j);
        lua_pushinteger(state, 0);
        lua_settable(state, -3);
      }
      lua_settable(state, -3);
    }
    lua_setglobal(state, "image");

    luaL_openlibs(state);
    lua_sethook(state, hook, LUA_MASKCOUNT, BUDGET_STEPS);
  }

  ~Lua() {
    lua_close(state);
    state = NULL;
  }

  LuaResult run() {
    lua_pushlightuserdata(state, (void *)((long long)state + 1));
    lua_gettable(state, LUA_REGISTRYINDEX);

    cost = 0;

    int code = lua_pcall(state, 0, LUA_MULTRET, 0);
    if (code != 0) {
      size_t len = 0;
      const char *value = lua_tolstring(state, lua_gettop(state), &len);
      return LuaResult{.err{value}};
    }

    LuaResult result = get_result();
    lua_settop(state, 0);
    lua_gc(state, LUA_GCCOLLECT, 0);
    return result;
  }

private:
  static void hook(lua_State *L, lua_Debug *ar) {
    lua_pushlightuserdata(L, (void *)L);
    lua_gettable(L, LUA_REGISTRYINDEX);
    Lua *lua = (Lua *)lua_touserdata(L, -1);
    assert(lua->state == L);

    lua->cost += BUDGET_STEPS;
    if (lua->cost > BUDGET) {
      luaL_error(L, "too many instructions");
    }
  }

  LuaResult get_result() {
    lua_getglobal(state, "title");
    std::string title = lua_tostring(state, -1);
    lua_pop(state, 1);

    lua_getglobal(state, "content");
    std::string content = lua_tostring(state, -1);
    lua_pop(state, 1);

    std::vector<unsigned int> image;
    lua_getglobal(state, "image");
    for (int i = 0; i < image_width; i++) {
      lua_pushinteger(state, i);
      lua_gettable(state, -2);
      for (int j = 0; j < image_height; j++) {
        lua_pushinteger(state, j);
        lua_gettable(state, -2);
        image.push_back(lua_tointeger(state, -1));
        lua_pop(state, 1);
      }
      lua_pop(state, 1);
    }

    return LuaResult{
        .title{title},
        .content{content},
        .image{image},
        .image_width = image_width,
        .image_height = image_height,
    };
  }

  lua_State *state = NULL;
  int cost = 0;

  unsigned int image_width = 64;
  unsigned int image_height = 36;
};

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_BINDINGS(my_module) {
  register_vector<unsigned int>("VectorImage");
  register_vector<std::vector<unsigned int>>("VectorVectorImage");
  class_<LuaResult>("LuaResult")
      .property("title", &LuaResult::title)
      .property("content", &LuaResult::content)
      .property("err", &LuaResult::err)
      .function("pixel", &LuaResult::pixel);
  class_<Lua>("Lua").constructor<std::string>().function("run", &Lua::run);
}
#endif
